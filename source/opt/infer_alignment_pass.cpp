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

#include "source/opt/infer_alignment_pass.h"

#include <algorithm>
#include <cassert>
#include <vector>

#include "source/opt/decoration_manager.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/type_manager.h"
#include "source/spirv_constant.h"

namespace spvtools {
namespace opt {

namespace {
constexpr uint32_t kOpLoadInOperandPointer = 0u;
constexpr uint32_t kOpLoadInOperandMemoryOperands = 1u;
constexpr uint32_t kOpStoreInOperandPointer = 0u;
constexpr uint32_t kOpStoreInOperandObject = 1u;
constexpr uint32_t kOpStoreInOperandMemoryOperands = 2u;
// Maximum alignment to infer (16 bytes = 128 bits).
constexpr uint32_t kMaxInferredAlignment = 16u;
}  // namespace

uint32_t InferAlignmentPass::NaturalAlignment(uint32_t typeId) {
  analysis::TypeManager* type_mgr = context()->get_type_mgr();
  if (!type_mgr) return 1u;

  const analysis::Type* type = type_mgr->GetType(typeId);
  if (!type) return 1u;

  uint32_t size = 1u;
  if (type->AsInteger()) {
    size = type->AsInteger()->width() / 8u;
  } else if (type->AsFloat()) {
    size = type->AsFloat()->width() / 8u;
  } else if (type->AsVector()) {
    const analysis::Vector* vec = type->AsVector();
    uint32_t elem_size = 1u;
    if (vec->element_type()->AsInteger()) {
      elem_size = vec->element_type()->AsInteger()->width() / 8u;
    } else if (vec->element_type()->AsFloat()) {
      elem_size = vec->element_type()->AsFloat()->width() / 8u;
    }
    size = elem_size * vec->element_count();
  } else if (type->AsMatrix()) {
    const analysis::Matrix* mat = type->AsMatrix();
    uint32_t vec_type_id = type_mgr->GetTypeInstruction(mat->element_type());
    return NaturalAlignment(vec_type_id);
  } else if (type->AsArray()) {
    uint32_t elem_type_id =
        type_mgr->GetTypeInstruction(type->AsArray()->element_type());
    return NaturalAlignment(elem_type_id);
  } else if (type->AsStruct()) {
    uint32_t max_align = 1u;
    for (const auto& member : type->AsStruct()->element_types()) {
      uint32_t member_type_id = type_mgr->GetTypeInstruction(member);
      max_align = std::max(max_align, NaturalAlignment(member_type_id));
    }
    return std::min(max_align, kMaxInferredAlignment);
  } else if (type->AsPointer()) {
    uint32_t pointee_type_id =
        type_mgr->GetTypeInstruction(type->AsPointer()->pointee_type());
    return NaturalAlignment(pointee_type_id);
  }

  // Round up size to power of 2, cap at max alignment
  if (size == 0) return 1u;
  uint32_t align = 1u;
  while (align < size && align < kMaxInferredAlignment) {
    align <<= 1;
  }
  return std::min(align, kMaxInferredAlignment);
}

uint32_t InferAlignmentPass::KnownAlignment(uint32_t ptrId) {
  if (ptrId == 0) return 1u;

  analysis::DefUseManager* def_use_mgr = get_def_use_mgr();
  Instruction* def = def_use_mgr->GetDef(ptrId);
  if (!def) return 1u;

  switch (def->opcode()) {
    case spv::Op::OpVariable: {
      // Check Alignment decoration
      // Check Alignment decoration via GetDecorationsFor
      auto decorations = get_decoration_mgr()->GetDecorationsFor(ptrId, false);
      for (const auto* deco_inst : decorations) {
        if (deco_inst->opcode() == spv::Op::OpDecorate &&
            deco_inst->GetSingleWordInOperand(1) == uint32_t(spv::Decoration::Alignment)) {
          return deco_inst->GetSingleWordInOperand(2);
        }
      }
      // No decoration; compute natural alignment of pointee type
      uint32_t pointee_type_id = GetPointeeTypeId(def);
      if (pointee_type_id != 0) {
        return NaturalAlignment(pointee_type_id);
      }
      return 1u;
    }
    case spv::Op::OpAccessChain:
    case spv::Op::OpInBoundsAccessChain: {
      // Alignment comes from base pointer
      return KnownAlignment(def->GetSingleWordInOperand(0));
    }
    case spv::Op::OpBitcast:
    case spv::Op::OpCopyObject: {
      return KnownAlignment(def->GetSingleWordInOperand(0));
    }
    default:
      return 1u;
  }
}

Pass::Status InferAlignmentPass::Process() {
  // Check addressing model
  const Instruction* mem_model = get_module()->GetMemoryModel();
  if (!mem_model) {
    return Status::SuccessWithoutChange;
  }

  auto addressing_model =
      spv::AddressingModel(mem_model->GetSingleWordInOperand(0));
  if (addressing_model == spv::AddressingModel::Logical) {
    // For Logical addressing model, check if PhysicalStorageBuffer64
    // capability is present. If not, skip.
    bool has_phys_ssbo64 = context()->get_feature_mgr()->HasCapability(
        spv::Capability::PhysicalStorageBufferAddresses);
    if (!has_phys_ssbo64) {
      return Status::SuccessWithoutChange;
    }
  }

  bool modified = false;

  // Process all functions
  for (auto& func : *get_module()) {
    for (auto& bb : func) {
      for (auto& inst : bb) {
        spv::Op opcode = inst.opcode();
        if (opcode != spv::Op::OpLoad && opcode != spv::Op::OpStore) {
          continue;
        }

        // Get the pointer operand and memory operand index
        uint32_t ptr_id = 0;
        uint32_t mem_operand_idx = 0;

        if (opcode == spv::Op::OpLoad) {
          ptr_id = inst.GetSingleWordInOperand(kOpLoadInOperandPointer);
          mem_operand_idx = kOpLoadInOperandMemoryOperands;
        } else {
          ptr_id = inst.GetSingleWordInOperand(kOpStoreInOperandPointer);
          mem_operand_idx = kOpStoreInOperandMemoryOperands;
        }

        uint32_t known = KnownAlignment(ptr_id);
        if (known <= 1u) continue;

        // Check if the instruction has memory operands
        if (inst.NumInOperands() <= mem_operand_idx) {
          // No memory operands; add them.
          inst.AddOperand(
              {SPV_OPERAND_TYPE_MEMORY_ACCESS,
               {static_cast<uint32_t>(spv::MemoryAccessMask::Aligned)}});
          inst.AddOperand({SPV_OPERAND_TYPE_TYPED_LITERAL_NUMBER, {known}});

          // Add Alignment decoration to the variable if applicable
          Instruction* ptr_def = get_def_use_mgr()->GetDef(ptr_id);
          if (ptr_def && ptr_def->opcode() == spv::Op::OpVariable) {
            get_decoration_mgr()->AddDecorationVal(
                ptr_id, uint32_t(spv::Decoration::Alignment), known);
          }

          modified = true;
        } else {
          // Has memory operands; check current alignment
          uint32_t current_mask =
              inst.GetSingleWordInOperand(mem_operand_idx);

          if (current_mask &
              uint32_t(spv::MemoryAccessMask::Aligned)) {
            // Alignment is already set; check if we can improve it
            // The alignment value is the word after the mask.
            uint32_t current_align =
                inst.GetSingleWordInOperand(mem_operand_idx + 1u);
            if (known > current_align) {
              inst.SetInOperand(mem_operand_idx + 1u, {known});

              // Update Alignment decoration on the variable
              Instruction* ptr_def = get_def_use_mgr()->GetDef(ptr_id);
              if (ptr_def && ptr_def->opcode() == spv::Op::OpVariable) {
                get_decoration_mgr()->AddDecorationVal(
                    ptr_id, uint32_t(spv::Decoration::Alignment), known);
              }
              modified = true;
            }
          } else {
            // Has memory operand mask but no Aligned flag; upgrade
            uint32_t new_mask =
                current_mask |
                (uint32_t(spv::MemoryAccessMask::Aligned));
            inst.SetInOperand(mem_operand_idx, {new_mask});

            // Insert alignment value after the mask.
            // We rebuild the operand list to insert the alignment word.
            std::vector<Operand> new_operands;
            for (uint32_t i = 0; i < inst.NumInOperands(); ++i) {
              new_operands.push_back(inst.GetInOperand(i));
              if (i == mem_operand_idx) {
                new_operands.push_back(
                    {SPV_OPERAND_TYPE_TYPED_LITERAL_NUMBER, {known}});
              }
            }
            inst.SetInOperands(std::move(new_operands));

            // Add Alignment decoration to the variable
            Instruction* ptr_def = get_def_use_mgr()->GetDef(ptr_id);
            if (ptr_def && ptr_def->opcode() == spv::Op::OpVariable) {
            get_decoration_mgr()->AddDecorationVal(
                ptr_id, uint32_t(spv::Decoration::Alignment), known);
            }
            modified = true;
          }
        }
      }
    }
  }

  return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
}

}  // namespace opt
}  // namespace spvtools
