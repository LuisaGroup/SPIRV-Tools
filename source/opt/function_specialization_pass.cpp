// Copyright (c) 2024 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "source/opt/function_specialization_pass.h"

#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "source/opt/def_use_manager.h"
#include "source/opt/ir_context.h"
#include "source/opt/type_manager.h"

namespace spvtools {
namespace opt {

Pass::Status FunctionSpecializationPass::Process() {
  bool modified = false;

  // Collect all OpFunctionCall instructions and their callee functions.
  std::vector<std::pair<Instruction*, Function*>> worklist;

  for (auto& func : *get_module()) {
    if (func.IsDeclaration()) continue;

    func.ForEachInst([&](Instruction* inst) {
      if (inst->opcode() != spv::Op::OpFunctionCall) return;

      // Get the called function id (first in-operand).
      uint32_t func_id = inst->GetSingleWordInOperand(0);
      Function* callee = context()->GetFunction(func_id);
      if (callee == nullptr || callee->IsDeclaration()) return;

      // Skip if already specialized (prevent recursive specialization).
      // Get the function name from debug info.
      auto names = context()->GetNames(callee->result_id());
      for (auto& name_entry : names) {
        Instruction* name_inst = name_entry.second;
        if (name_inst->opcode() == spv::Op::OpName) {
          std::string func_name = name_inst->GetInOperand(1).AsString();
          if (func_name.find(".specialized") != std::string::npos) {
            return;  // Already specialized, skip.
          }
        }
      }

      worklist.push_back({inst, callee});
    });
  }

  // Process the worklist.
  for (auto& entry : worklist) {
    Instruction* call_inst = entry.first;
    Function* callee = entry.second;

    // Check if all arguments are constants.
    std::vector<std::pair<uint32_t, uint32_t>> const_args;
    if (!GetAllConstantArgs(call_inst, &const_args)) {
      continue;
    }

    // No constant arguments? Nothing to specialize.
    if (const_args.empty()) continue;

    // Build a unique key for this specialization.
    SpecializationKey key =
        BuildSpecializationKey(callee->result_id(), const_args);

    // Check if we already have this specialization.
    auto existing = specialized_keys_.find(key);
    if (existing != specialized_keys_.end()) {
      // Already specialized.  We need to find the specialized function.
      // We stored the specialized function id in the key lookup, but since
      // our key is just a string, we need another mechanism.  For the
      // simplified approach, we search by the specialized name.
      std::string func_name;
      auto names = context()->GetNames(callee->result_id());
      for (auto& name_entry : names) {
        if (name_entry.second->opcode() == spv::Op::OpName) {
          func_name = name_entry.second->GetInOperand(1).AsString();
          break;
        }
      }
      if (func_name.empty()) {
        func_name = "func_" + std::to_string(callee->result_id());
      }
      std::string spec_name =
          GetSpecializedName(callee->result_id(), func_name, const_args);

      // Search for the function by name.
      bool found = false;
      for (auto& f : *get_module()) {
        auto f_names = context()->GetNames(f.result_id());
        for (auto& n_entry : f_names) {
          if (n_entry.second->opcode() == spv::Op::OpName &&
              n_entry.second->GetInOperand(1).AsString() == spec_name) {
            // Found it! Rewrite the call.
            RewriteCall(call_inst, f.result_id());
            modified = true;
            found = true;
            break;
          }
        }
        if (found) break;
      }
      continue;
    }

    // Clone the function.
    Function* cloned_func = callee->Clone(context());
    if (cloned_func == nullptr) continue;

    // Give the clone a new result id.
    uint32_t new_func_id = TakeNextId();
    cloned_func->DefInst().SetResultId(new_func_id);

    // Build the specialized name.
    std::string func_name;
    auto names = context()->GetNames(callee->result_id());
    for (auto& name_entry : names) {
      if (name_entry.second->opcode() == spv::Op::OpName) {
        func_name = name_entry.second->GetInOperand(1).AsString();
        break;
      }
    }
    if (func_name.empty()) {
      func_name = "func_" + std::to_string(callee->result_id());
    }
    std::string spec_name =
        GetSpecializedName(callee->result_id(), func_name, const_args);

    // Clone the OpName for the original function to use the new id.
    context()->CloneNames(callee->result_id(), new_func_id, 0);

    // Replace all parameters with their corresponding constants in the clone.
    uint32_t param_idx = 0;
    cloned_func->ForEachParam([&const_args, &param_idx,
                               this](Instruction* param_inst) {
      if (param_idx < const_args.size()) {
        uint32_t const_id = const_args[param_idx].second;
        // Replace all uses of the parameter with the constant id.
        context()->ReplaceAllUsesWith(param_inst->result_id(), const_id);
        param_idx++;
      }
    });

    // Remove all parameters from the cloned function.
    std::vector<uint32_t> param_ids;
    cloned_func->ForEachParam(
        [&param_ids](Instruction* p) { param_ids.push_back(p->result_id()); });
    for (uint32_t pid : param_ids) {
      cloned_func->RemoveParameter(pid);
      context()->KillDef(pid);
    }

    // Create a new function type with 0 parameters (just return type).
    uint32_t return_type_id = cloned_func->type_id();
    uint32_t new_func_type_id = TakeNextId();

    // Build OpTypeFunction: %new_func_type_id %return_type_id (no params).
    std::unique_ptr<Instruction> func_type_inst(new Instruction(
        context(), spv::Op::OpTypeFunction, 0, new_func_type_id,
        {{SPV_OPERAND_TYPE_ID, {return_type_id}}}));
    context()->AddType(std::move(func_type_inst));

    // Update the cloned function's OpFunction to use the new function type.
    // OpFunction in-operand layout: [0]=control_mask, [1]=function_type
    cloned_func->DefInst().SetInOperand(1, {new_func_type_id});
    context()->UpdateDefUse(&cloned_func->DefInst());

    // Add the cloned function to the module.
    context()->AddFunction(std::unique_ptr<Function>(cloned_func));

    // Mark this key as seen.
    specialized_keys_.insert(key);

    // Rewrite the original call to call the specialized function with no args.
    RewriteCall(call_inst, new_func_id);

    modified = true;
  }

  return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
}

void FunctionSpecializationPass::RewriteCall(Instruction* call_inst,
                                              uint32_t new_func_id) {
  // Replace all in-operands with just the function id (no arguments).
  call_inst->SetInOperands({{SPV_OPERAND_TYPE_ID, {new_func_id}}});
  context()->UpdateDefUse(call_inst);
}

FunctionSpecializationPass::SpecializationKey
FunctionSpecializationPass::BuildSpecializationKey(
    uint32_t func_id,
    const std::vector<std::pair<uint32_t, uint32_t>>& const_args) const {
  std::ostringstream oss;
  oss << func_id;
  for (const auto& arg : const_args) {
    oss << ":" << arg.first << ":" << arg.second;
  }
  return oss.str();
}

std::string FunctionSpecializationPass::GetSpecializedName(
    uint32_t func_id, const std::string& func_name,
    const std::vector<std::pair<uint32_t, uint32_t>>& const_args) const {
  std::string name = func_name + ".specialized";
  for (const auto& arg : const_args) {
    name += "." + std::to_string(arg.first) + "." + std::to_string(arg.second);
  }
  return name;
}

bool FunctionSpecializationPass::GetAllConstantArgs(
    Instruction* call_inst,
    std::vector<std::pair<uint32_t, uint32_t>>* const_args) const {
  // OpFunctionCall operands:
  //   in-operand 0: function ID
  //   in-operand 1..N: arguments
  if (call_inst->NumInOperands() <= 1) {
    return true;  // No arguments, trivially all constants.
  }

  for (uint32_t i = 1; i < call_inst->NumInOperands(); ++i) {
    uint32_t arg_id = call_inst->GetSingleWordInOperand(i);
    Instruction* arg_def = get_def_use_mgr()->GetDef(arg_id);
    if (arg_def == nullptr) return false;

    // Check if it's any kind of constant.
    if (arg_def->opcode() == spv::Op::OpConstant ||
        arg_def->opcode() == spv::Op::OpConstantComposite ||
        arg_def->opcode() == spv::Op::OpSpecConstant ||
        arg_def->opcode() == spv::Op::OpConstantNull ||
        arg_def->opcode() == spv::Op::OpConstantTrue ||
        arg_def->opcode() == spv::Op::OpConstantFalse) {
      const_args->push_back({i - 1, arg_id});  // 0-based argument index.
    } else {
      return false;  // Not a constant.
    }
  }

  return true;
}

}  // namespace opt
}  // namespace spvtools
