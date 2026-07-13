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

#include "source/opt/attributor_pass.h"

#include <unordered_set>
#include <vector>

#include "source/opt/decoration_manager.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"

namespace spvtools {
namespace opt {

namespace {
constexpr uint32_t kTypePointerStorageClassInIdx = 0u;

// Returns the storage class of an OpVariable by looking at its pointer type.
spv::StorageClass GetVariableStorageClass(Instruction* varInst,
                                          analysis::DefUseManager* def_use_mgr) {
  assert(varInst->opcode() == spv::Op::OpVariable);
  const uint32_t varTypeId = varInst->type_id();
  const Instruction* varTypeInst = def_use_mgr->GetDef(varTypeId);
  if (varTypeInst->opcode() != spv::Op::OpTypePointer) {
    return spv::StorageClass::Max;
  }
  return spv::StorageClass(
      varTypeInst->GetSingleWordInOperand(kTypePointerStorageClassInIdx));
}

// Traces through OpAccessChain and OpCopyObject to find the root variable.
// Returns true if the user is a memory access (load/store/atomic).
bool UserAccessesMemory(Instruction* user) {
  switch (user->opcode()) {
    case spv::Op::OpLoad:
    case spv::Op::OpStore:
    case spv::Op::OpAtomicLoad:
    case spv::Op::OpAtomicStore:
    case spv::Op::OpAtomicExchange:
    case spv::Op::OpAtomicCompareExchange:
    case spv::Op::OpAtomicIIncrement:
    case spv::Op::OpAtomicIDecrement:
    case spv::Op::OpAtomicIAdd:
    case spv::Op::OpAtomicISub:
    case spv::Op::OpAtomicSMin:
    case spv::Op::OpAtomicUMin:
    case spv::Op::OpAtomicSMax:
    case spv::Op::OpAtomicUMax:
    case spv::Op::OpAtomicAnd:
    case spv::Op::OpAtomicOr:
    case spv::Op::OpAtomicXor:
    case spv::Op::OpAtomicFlagTestAndSet:
    case spv::Op::OpAtomicFlagClear:
      return true;
    default:
      return false;
  }
}

// Classifies uses of a variable: check if there are any loads or stores.
// Returns (hasLoad, hasStore).
std::pair<bool, bool> ClassifyVariableUses(
    uint32_t varId, analysis::DefUseManager* def_use_mgr) {
  bool hasLoad = false;
  bool hasStore = false;

  def_use_mgr->ForEachUser(varId, [&](Instruction* user) {
    switch (user->opcode()) {
      case spv::Op::OpLoad:
        hasLoad = true;
        break;
      case spv::Op::OpStore: {
        // For OpStore, the pointer is operand 0. If varId is operand 1
        // (the value being stored), it's not a store TO the variable.
        uint32_t ptr_id = user->GetSingleWordInOperand(0);
        if (ptr_id == varId) {
          hasStore = true;
        }
        break;
      }
      case spv::Op::OpAtomicStore:
      case spv::Op::OpAtomicExchange:
      case spv::Op::OpAtomicCompareExchange:
      case spv::Op::OpAtomicIIncrement:
      case spv::Op::OpAtomicIDecrement:
      case spv::Op::OpAtomicIAdd:
      case spv::Op::OpAtomicISub:
      case spv::Op::OpAtomicSMin:
      case spv::Op::OpAtomicUMin:
      case spv::Op::OpAtomicSMax:
      case spv::Op::OpAtomicUMax:
      case spv::Op::OpAtomicAnd:
      case spv::Op::OpAtomicOr:
      case spv::Op::OpAtomicXor:
      case spv::Op::OpAtomicFlagTestAndSet:
      case spv::Op::OpAtomicFlagClear:
        hasStore = true;
        break;
      case spv::Op::OpAccessChain:
      case spv::Op::OpInBoundsAccessChain:
      case spv::Op::OpCopyObject: {
        // Trace through to classify uses of the access chain / copy
        auto sub_result = ClassifyVariableUses(user->result_id(), def_use_mgr);
        if (sub_result.first) hasLoad = true;
        if (sub_result.second) hasStore = true;
        break;
      }
      case spv::Op::OpFunctionCall:
        // Can't analyze through function calls; conservatively assume both
        hasLoad = true;
        hasStore = true;
        break;
      default:
        break;
    }
  });

  return {hasLoad, hasStore};
}

}  // namespace

Pass::Status AttributorPass::Process() {
  bool modified = false;

  analysis::DefUseManager* def_use_mgr = get_def_use_mgr();
  analysis::DecorationManager* dec_mgr = get_decoration_mgr();

  // Collect all OpVariable instructions from the module
  std::vector<Instruction*> target_vars;

  // Module-level variables
  for (auto& inst : get_module()->types_values()) {
    if (inst.opcode() == spv::Op::OpVariable) {
      target_vars.push_back(&inst);
    }
  }

  // Function-level variables
  for (auto& func : *get_module()) {
    for (auto& bb : func) {
      for (auto& inst : bb) {
        if (inst.opcode() == spv::Op::OpVariable) {
          target_vars.push_back(&inst);
        }
      }
    }
  }

  for (Instruction* varInst : target_vars) {
    uint32_t varId = varInst->result_id();
    spv::StorageClass sc = GetVariableStorageClass(varInst, def_use_mgr);

    // Only process storage classes where NonWritable/NonReadable are valid.
    // Per SPIR-V spec, NonWritable must target a storage image, tensor in
    // UniformConstant, uniform block, or storage buffer.
    // NonReadable must not be applied to objects in Input/Output storage class.
    // PushConstant, Input, Output, and others are excluded.
    if (sc != spv::StorageClass::Uniform &&
        sc != spv::StorageClass::UniformConstant &&
        sc != spv::StorageClass::StorageBuffer) {
      continue;
    }

		// Check if already has the target decorations
		bool hasNonWritable = dec_mgr->FindDecoration(
		    varId, uint32_t(spv::Decoration::NonWritable),
		    [](const Instruction&) { return true; });
		bool hasNonReadable = dec_mgr->FindDecoration(
		    varId, uint32_t(spv::Decoration::NonReadable),
		    [](const Instruction&) { return true; });

		// For UniformConstant storage class, NonWritable is only valid on
		// storage images (OpTypeImage with Sampled=2). Regular sampled images
		// (Sampled=1), samplers, and other UniformConstant types cannot have
		// NonWritable per the SPIR-V spec.
		if (sc == spv::StorageClass::UniformConstant && !hasNonWritable) {
		  const uint32_t varTypeId = varInst->type_id();
		  const Instruction* varTypeInst = def_use_mgr->GetDef(varTypeId);
		  uint32_t pointeeTypeId = 0;
		  if (varTypeInst && varTypeInst->opcode() == spv::Op::OpTypePointer) {
		    pointeeTypeId = varTypeInst->GetSingleWordInOperand(1);
		  }
		  Instruction* pointeeTypeInst = def_use_mgr->GetDef(pointeeTypeId);
	    if (pointeeTypeInst &&
	        pointeeTypeInst->opcode() == spv::Op::OpTypeImage) {
	      // OpTypeImage operand 3 is the Sampled parameter.
	      // 1 = sampled image, 2 = storage image.
	      uint32_t sampled = pointeeTypeInst->GetSingleWordInOperand(3);
	      if (sampled != 2u) {
	        // Not a storage image; skip NonWritable for this variable.
	        // We can still add NonReadable if there are only stores.
	        hasNonWritable = true;  // Pretend already has it to skip adding
	      }
	    } else if (pointeeTypeInst) {
	      // Non-image type in UniformConstant (e.g., sampler) - cannot have
	      // NonWritable.
	      hasNonWritable = true;
	    }
	  }

    // Skip if both decorations already present
    if (hasNonWritable && hasNonReadable) continue;

    // Classify uses
    auto [hasLoad, hasStore] = ClassifyVariableUses(varId, def_use_mgr);

    // Add decorations based on access patterns
    if (hasLoad && !hasStore && !hasNonWritable) {
      dec_mgr->AddDecoration(varId, uint32_t(spv::Decoration::NonWritable));
      modified = true;
    }

    if (hasStore && !hasLoad && !hasNonReadable) {
      dec_mgr->AddDecoration(varId, uint32_t(spv::Decoration::NonReadable));
      modified = true;
    }
  }

  return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
}

}  // namespace opt
}  // namespace spvtools
