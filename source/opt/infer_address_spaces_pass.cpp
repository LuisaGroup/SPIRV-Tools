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

#include "source/opt/infer_address_spaces_pass.h"

#include <set>
#include <vector>

#include "source/opt/def_use_manager.h"
#include "source/opt/ir_context.h"
#include "source/opt/type_manager.h"

namespace spvtools {
namespace opt {

Pass::Status InferAddressSpacesPass::Process() {
  // Capability gate: require GenericPointer or VariablePointers.
  if (!HasGenericAddressingCapability()) {
    return Status::SuccessWithoutChange;
  }

  // Collect all pointer-to-pointer casts (OpBitcast between different storage
  // classes, and OpPtrCastToGeneric / OpGenericCastToPtr).
  std::vector<Instruction*> casts;
  CollectPointerCasts(&casts);

  bool modified = false;
  for (Instruction* cast_inst : casts) {
    modified |= PropagateStorageClass(cast_inst);
  }

  return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
}

bool InferAddressSpacesPass::HasGenericAddressingCapability() const {
  return get_feature_mgr()->HasCapability(
             spv::Capability::VariablePointers) ||
         get_feature_mgr()->HasCapability(
             spv::Capability::VariablePointersStorageBuffer) ||
         get_feature_mgr()->HasCapability(spv::Capability::GenericPointer);
}

void InferAddressSpacesPass::CollectPointerCasts(
    std::vector<Instruction*>* casts) const {
  for (auto& func : *get_module()) {
    func.ForEachInst([this, casts](Instruction* inst) {
      // Check for OpBitcast between pointer types with different storage
      // classes.
      if (inst->opcode() == spv::Op::OpBitcast) {
        // Both source and result must be pointer types.
        if (inst->type_id() == 0 || inst->NumInOperands() == 0) return;

        uint32_t src_id = inst->GetSingleWordInOperand(0);
        Instruction* src_def = get_def_use_mgr()->GetDef(src_id);
        if (src_def == nullptr || src_def->type_id() == 0) return;

        // Check if both types are pointers.
        Instruction* src_type_inst =
            get_def_use_mgr()->GetDef(src_def->type_id());
        Instruction* dst_type_inst =
            get_def_use_mgr()->GetDef(inst->type_id());
        if (src_type_inst == nullptr || dst_type_inst == nullptr) return;
        if (src_type_inst->opcode() != spv::Op::OpTypePointer) return;
        if (dst_type_inst->opcode() != spv::Op::OpTypePointer) return;

        // Get storage classes.
        spv::StorageClass src_sc =
            static_cast<spv::StorageClass>(src_type_inst->GetSingleWordInOperand(0));
        spv::StorageClass dst_sc =
            static_cast<spv::StorageClass>(dst_type_inst->GetSingleWordInOperand(0));

        // We want source to be non-Generic and destination to be different
        // (more generic).  Propagate from the specific storage class to the
        // users that use the cast result.
        if (IsNonGenericStorageClass(src_sc) && src_sc != dst_sc) {
          casts->push_back(inst);
        }
        return;
      }

      // Also check for OpPtrCastToGeneric / OpGenericCastToPtr.
      if (inst->opcode() == spv::Op::OpPtrCastToGeneric ||
          inst->opcode() == spv::Op::OpGenericCastToPtr) {
        if (inst->type_id() == 0 || inst->NumInOperands() == 0) return;
        uint32_t src_id = inst->GetSingleWordInOperand(0);
        Instruction* src_def = get_def_use_mgr()->GetDef(src_id);
        if (src_def == nullptr || src_def->type_id() == 0) return;

        Instruction* src_type_inst =
            get_def_use_mgr()->GetDef(src_def->type_id());
        if (src_type_inst == nullptr) return;
        if (src_type_inst->opcode() != spv::Op::OpTypePointer) return;

        spv::StorageClass src_sc =
            static_cast<spv::StorageClass>(src_type_inst->GetSingleWordInOperand(0));
        if (IsNonGenericStorageClass(src_sc)) {
          casts->push_back(inst);
        }
      }
    });
  }
}

bool InferAddressSpacesPass::PropagateStorageClass(Instruction* cast_inst) {
  // Get the source pointer and its storage class.
  uint32_t src_id = cast_inst->GetSingleWordInOperand(0);
  Instruction* src_def = get_def_use_mgr()->GetDef(src_id);
  Instruction* src_type_inst =
      get_def_use_mgr()->GetDef(src_def->type_id());
  assert(src_type_inst->opcode() == spv::Op::OpTypePointer);
  spv::StorageClass src_sc =
      static_cast<spv::StorageClass>(src_type_inst->GetSingleWordInOperand(0));

  uint32_t cast_result_id = cast_inst->result_id();
  bool modified = false;

  // Collect users up front since we may modify the use list during iteration.
  std::vector<Instruction*> users;
  get_def_use_mgr()->ForEachUser(
      cast_result_id, [&users](Instruction* user) { users.push_back(user); });

  // We process each user unless we hit an unhandled type, in which case we
  // abort optimization for this cast entirely (to be safe).
  for (Instruction* user : users) {
    // Skip the cast instruction itself (e.g., self-referencing).
    if (user == cast_inst) continue;

    switch (user->opcode()) {
      case spv::Op::OpLoad: {
        // OpLoad: replace the pointer operand with the source pointer.
        // The loaded value type is independent of the pointer storage class,
        // so we just change the pointer operand.
        uint32_t ptr_operand_idx = 0;  // First in-operand is the pointer.
        if (user->GetSingleWordInOperand(ptr_operand_idx) == cast_result_id) {
          user->SetInOperand(ptr_operand_idx, {src_id});
          context()->UpdateDefUse(user);
          modified = true;
        }
        break;
      }
      case spv::Op::OpStore: {
        // OpStore: replace the pointer operand with the source pointer.
        uint32_t ptr_operand_idx = 0;  // First in-operand is the pointer.
        if (user->GetSingleWordInOperand(ptr_operand_idx) == cast_result_id) {
          user->SetInOperand(ptr_operand_idx, {src_id});
          context()->UpdateDefUse(user);
          modified = true;
        }
        break;
      }
      case spv::Op::OpAccessChain:
      case spv::Op::OpInBoundsAccessChain:
      case spv::Op::OpPtrAccessChain:
      case spv::Op::OpInBoundsPtrAccessChain: {
        // The base pointer is in-operand 0 for OpAccessChain /
        // OpInBoundsAccessChain, or in-operand 1 for OpPtrAccessChain /
        // OpInBoundsPtrAccessChain (operand 0 is the "element" pointer).
        uint32_t base_operand_idx = 0;
        if (user->opcode() == spv::Op::OpPtrAccessChain ||
            user->opcode() == spv::Op::OpInBoundsPtrAccessChain) {
          base_operand_idx = 1;
        }

        if (user->GetSingleWordInOperand(base_operand_idx) != cast_result_id) {
          // This access chain uses the cast indirectly; skip.
          return modified;
        }

        // Change the base pointer to the source pointer.
        user->SetInOperand(base_operand_idx, {src_id});

        // Fix the result type: the result pointer type should have the source
        // storage class.  Walk the access chain to compute the result pointee
        // type, then rebuild the result pointer type with the source storage
        // class.
        Instruction* result_type_inst =
            get_def_use_mgr()->GetDef(user->type_id());
        assert(result_type_inst->opcode() == spv::Op::OpTypePointer);

        // The original type started from the cast result type, which was
        // already a pointer.  We walk from the source pointer's pointee type
        // through the access chain indices to compute the new pointee type.
        uint32_t pointee_type_id = src_type_inst->GetSingleWordInOperand(1);

        // Walk through access chain indices starting from the first index
        // operand.
        uint32_t start_idx = 1;
        if (user->opcode() == spv::Op::OpPtrAccessChain ||
            user->opcode() == spv::Op::OpInBoundsPtrAccessChain) {
          start_idx = 2;
        }

        uint32_t current_type_id = pointee_type_id;
        for (uint32_t i = start_idx; i < user->NumInOperands(); ++i) {
          Instruction* type_inst =
              get_def_use_mgr()->GetDef(current_type_id);
          if (type_inst == nullptr) break;

          switch (type_inst->opcode()) {
            case spv::Op::OpTypeArray:
            case spv::Op::OpTypeRuntimeArray:
            case spv::Op::OpTypeMatrix:
            case spv::Op::OpTypeVector:
              current_type_id = type_inst->GetSingleWordInOperand(0);
              break;
            case spv::Op::OpTypeStruct: {
              // We need the constant index to walk into a struct.
              // For simplicity, we look up the index operand value.
              uint32_t index_id = user->GetSingleWordInOperand(i);
              const analysis::Constant* index_const =
                  context()->get_constant_mgr()->FindDeclaredConstant(index_id);
              if (index_const == nullptr) {
                // Non-constant index into struct: can't walk, bail out.
                // Reset the change we made and abort.
                user->SetInOperand(base_operand_idx, {cast_result_id});
                context()->UpdateDefUse(user);
                return modified;
              }
              uint32_t index =
                  static_cast<uint32_t>(index_const->GetSignExtendedValue());
              current_type_id = type_inst->GetSingleWordInOperand(index);
              break;
            }
            default:
              // Can't walk further.
              break;
          }
        }

        // Create the new result pointer type with the source storage class.
        uint32_t new_result_type_id =
            context()->get_type_mgr()->FindPointerToType(
                current_type_id, src_sc);
        if (new_result_type_id != 0 &&
            new_result_type_id != user->type_id()) {
          user->SetResultType(new_result_type_id);
        }

        context()->UpdateDefUse(user);
        modified = true;
        break;
      }
      case spv::Op::OpBitcast:
      case spv::Op::OpPtrCastToGeneric:
      case spv::Op::OpGenericCastToPtr: {
        // Chain of casts: we skip this user.  It will be handled in a
        // subsequent iteration when we process that cast.
        break;
      }
      default: {
        // Unhandled instruction type.  Abort all changes for this cast to
        // avoid invalid IR.
        return modified;
      }
    }
  }

  return modified;
}

spv::StorageClass InferAddressSpacesPass::GetPointerStorageClass(
    uint32_t type_id) const {
  Instruction* type_inst = get_def_use_mgr()->GetDef(type_id);
  if (type_inst == nullptr ||
      type_inst->opcode() != spv::Op::OpTypePointer) {
    return spv::StorageClass::Max;
  }
  return static_cast<spv::StorageClass>(
      type_inst->GetSingleWordInOperand(0));
}

bool InferAddressSpacesPass::IsNonGenericStorageClass(
    spv::StorageClass storage_class) const {
  return storage_class != spv::StorageClass::Generic &&
         storage_class != spv::StorageClass::Max;
}

}  // namespace opt
}  // namespace spvtools
