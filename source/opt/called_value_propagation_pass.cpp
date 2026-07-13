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

#include "source/opt/called_value_propagation_pass.h"

#include <vector>

#include "source/opt/def_use_manager.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"

namespace spvtools {
namespace opt {

namespace {
// InOperand index for the callee function id in an OpFunctionCall.
constexpr uint32_t kFunctionCallCalleeInOperand = 0u;
}  // namespace

Pass::Status CalledValuePropagationPass::Process() {
  bool modified = false;

  // Process all functions in the module
  for (auto& func : *get_module()) {
    for (auto& bb : func) {
      for (auto& inst : bb) {
        if (inst.opcode() != spv::Op::OpFunctionCall) {
          continue;
        }

        // Get the callee function id
        uint32_t calleeId =
            inst.GetSingleWordInOperand(kFunctionCallCalleeInOperand);

        // Follow the definition of the callee id
        Instruction* calleeDef = get_def_use_mgr()->GetDef(calleeId);
        if (!calleeDef) continue;

        // If it's an OpFunction directly, already a direct call, skip.
        if (calleeDef->opcode() == spv::Op::OpFunction) {
          continue;
        }

        uint32_t resolvedFuncId = 0;

        // Check for OpPhi: all incoming values must be the same OpFunction
        if (calleeDef->opcode() == spv::Op::OpPhi) {
          uint32_t commonFuncId = 0;
          bool allSame = true;

          // OpPhi format: ResultType, Result, (Variable, Parent)*
          // InOperands: (value, parent block)* pairs
          for (uint32_t i = 0; i < calleeDef->NumInOperands(); i += 2) {
            uint32_t incomingVal = calleeDef->GetSingleWordInOperand(i);
            Instruction* incomingDef = get_def_use_mgr()->GetDef(incomingVal);
            if (!incomingDef ||
                incomingDef->opcode() != spv::Op::OpFunction) {
              allSame = false;
              break;
            }
            if (commonFuncId == 0) {
              commonFuncId = incomingVal;
            } else if (incomingVal != commonFuncId) {
              allSame = false;
              break;
            }
          }

          if (allSame && commonFuncId != 0) {
            resolvedFuncId = commonFuncId;
          }
        }

        // Check for OpSelect: both sides must be the same OpFunction
        if (calleeDef->opcode() == spv::Op::OpSelect) {
          // OpSelect format: ResultType, Result, Condition, TrueValue, FalseValue
          // InOperands: Condition(0), TrueValue(1), FalseValue(2)
          uint32_t trueVal = calleeDef->GetSingleWordInOperand(1);
          uint32_t falseVal = calleeDef->GetSingleWordInOperand(2);

          Instruction* trueDef = get_def_use_mgr()->GetDef(trueVal);
          Instruction* falseDef = get_def_use_mgr()->GetDef(falseVal);

          if (trueDef && falseDef &&
              trueDef->opcode() == spv::Op::OpFunction &&
              falseDef->opcode() == spv::Op::OpFunction &&
              trueVal == falseVal) {
            resolvedFuncId = trueVal;
          }
        }

        // If we resolved to a direct function, rewrite the call
        if (resolvedFuncId != 0) {
          // Rewrite the call's function operand
          inst.SetInOperand(kFunctionCallCalleeInOperand, {resolvedFuncId});
          get_def_use_mgr()->AnalyzeInstUse(&inst);
          modified = true;
        }
      }
    }
  }

  return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
}

}  // namespace opt
}  // namespace spvtools
