// Copyright (c) 2024 Google LLC
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

#include "source/opt/load_combine_pass.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "source/opt/constants.h"
#include "source/opt/ir_builder.h"
#include "source/opt/ir_context.h"
#include "source/opt/type_manager.h"
#include "source/opt/types.h"

namespace spvtools {
namespace opt {

Pass::Status LoadCombinePass::Process() {
  bool modified = false;

  for (auto& func : *get_module()) {
    if (func.IsDeclaration()) continue;

    for (auto& bb : func) {
      std::unordered_map<uint32_t, std::vector<LoadInfo>> load_map;
      uint32_t index = 0;

      for (auto it = bb.begin(); it != bb.end(); ++it) {
        Instruction& inst = *it;

        if (IsMemoryBarrier(&inst)) {
          if (CombineLoads(&load_map)) modified = true;
          load_map.clear();
          continue;
        }

        if (!IsCandidateLoad(&inst)) continue;

        PointerOffsetPair pop = GetPointerOffsetPair(&inst);
        if (!pop.valid) continue;

        LoadInfo info;
        info.load_inst = &inst;
        info.base_id = pop.base_id;
        info.byte_offset = pop.byte_offset;
        info.insert_index = index++;

        load_map[pop.base_id].push_back(info);

        if (load_map[pop.base_id].size() >= kMaxLoadsPerBase) {
          if (CombineLoads(&load_map)) modified = true;
          load_map.clear();
        }
      }

      if (CombineLoads(&load_map)) modified = true;
    }
  }

  return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
}

bool LoadCombinePass::IsCandidateLoad(Instruction* load) {
  if (load->opcode() != spv::Op::OpLoad) return false;

  uint32_t ptr_op = load->GetSingleWordInOperand(0);
  Instruction* ptr = get_def_use_mgr()->GetDef(ptr_op);
  if (!ptr || !ptr->IsReadOnlyPointer()) return false;

  uint32_t type_id = load->type_id();
  Instruction* type_inst = get_def_use_mgr()->GetDef(type_id);
  if (!type_inst) return false;

  spv::Op type_op = type_inst->opcode();
  if (type_op != spv::Op::OpTypeInt && type_op != spv::Op::OpTypeFloat) {
    return false;
  }

  uint32_t bit_width = type_inst->GetSingleWordInOperand(0);
  if (bit_width != 8 && bit_width != 16 && bit_width != 32 && bit_width != 64) {
    return false;
  }

  return true;
}

bool LoadCombinePass::IsMemoryBarrier(Instruction* inst) {
  switch (inst->opcode()) {
    case spv::Op::OpStore:
    case spv::Op::OpCopyMemory:
    case spv::Op::OpCopyMemorySized:
    case spv::Op::OpAtomicStore:
    case spv::Op::OpAtomicExchange:
    case spv::Op::OpAtomicCompareExchange:
    case spv::Op::OpAtomicCompareExchangeWeak:
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
    case spv::Op::OpAtomicFAddEXT:
    case spv::Op::OpAtomicFMinEXT:
    case spv::Op::OpAtomicFMaxEXT:
    case spv::Op::OpAtomicFlagTestAndSet:
    case spv::Op::OpAtomicFlagClear:
    case spv::Op::OpFunctionCall:
    case spv::Op::OpControlBarrier:
    case spv::Op::OpMemoryBarrier:
    case spv::Op::OpImageWrite:
      return true;
    default:
      return false;
  }
}

LoadCombinePass::PointerOffsetPair LoadCombinePass::GetPointerOffsetPair(
    Instruction* load) {
  PointerOffsetPair result;
  result.valid = true;
  result.byte_offset = 0;

  uint32_t ptr_id = load->GetSingleWordInOperand(0);
  Instruction* ptr_inst = get_def_use_mgr()->GetDef(ptr_id);

  if (!ptr_inst) {
    result.valid = false;
    return result;
  }

  Instruction* current = ptr_inst;
  while (true) {
    switch (current->opcode()) {
      case spv::Op::OpBitcast:
        current = get_def_use_mgr()->GetDef(current->GetSingleWordInOperand(0));
        if (!current) {
          result.valid = false;
          return result;
        }
        break;

      case spv::Op::OpCopyObject:
        current = get_def_use_mgr()->GetDef(current->GetSingleWordInOperand(0));
        if (!current) {
          result.valid = false;
          return result;
        }
        break;

      case spv::Op::OpAccessChain:
      case spv::Op::OpInBoundsAccessChain: {
        uint32_t base_id = current->GetSingleWordInOperand(0);
        Instruction* base_inst = get_def_use_mgr()->GetDef(base_id);
        if (!base_inst) {
          result.valid = false;
          return result;
        }

        uint32_t base_type_id = base_inst->type_id();
        Instruction* base_type_inst = get_def_use_mgr()->GetDef(base_type_id);
        if (!base_type_inst) {
          result.valid = false;
          return result;
        }

        if (base_type_inst->opcode() != spv::Op::OpTypePointer) {
          result.valid = false;
          return result;
        }
        uint32_t pointed_to_type_id = base_type_inst->GetSingleWordInOperand(1);

        Instruction* type_to_index =
            get_def_use_mgr()->GetDef(pointed_to_type_id);
        for (uint32_t i = 1; i < current->NumInOperands(); ++i) {
          uint32_t index_id = current->GetSingleWordInOperand(i);
          Instruction* index_inst = get_def_use_mgr()->GetDef(index_id);

          if (!index_inst || !IsConstantInst(index_inst->opcode())) {
            result.valid = false;
            return result;
          }

          uint64_t index_value = 0;
          if (index_inst->opcode() == spv::Op::OpConstant) {
            if (index_inst->NumInOperands() >= 1) {
              index_value = index_inst->GetSingleWordInOperand(0);
            }
          } else {
            result.valid = false;
            return result;
          }

          if (!type_to_index) {
            result.valid = false;
            return result;
          }

          switch (type_to_index->opcode()) {
            case spv::Op::OpTypeArray: {
              uint32_t elem_type_id =
                  type_to_index->GetSingleWordInOperand(0);
              uint32_t elem_size = GetTypeBitWidth(elem_type_id) / 8;
              if (elem_size == 0) {
                result.valid = false;
                return result;
              }
              result.byte_offset += index_value * elem_size;
              type_to_index = get_def_use_mgr()->GetDef(elem_type_id);
              break;
            }
            case spv::Op::OpTypeStruct: {
              if (index_value >= type_to_index->NumInOperands()) {
                result.valid = false;
                return result;
              }
              uint32_t member_type_id =
                  type_to_index->GetSingleWordInOperand(
                      static_cast<uint32_t>(index_value));
              for (uint32_t j = 0; j < index_value; ++j) {
                uint32_t prev_member_type_id =
                    type_to_index->GetSingleWordInOperand(j);
                result.byte_offset +=
                    GetTypeBitWidth(prev_member_type_id) / 8;
              }
              type_to_index = get_def_use_mgr()->GetDef(member_type_id);
              break;
            }
            case spv::Op::OpTypeVector: {
              uint32_t elem_type_id =
                  type_to_index->GetSingleWordInOperand(0);
              uint32_t elem_size = GetTypeBitWidth(elem_type_id) / 8;
              result.byte_offset += index_value * elem_size;
              type_to_index = get_def_use_mgr()->GetDef(elem_type_id);
              break;
            }
            case spv::Op::OpTypeMatrix: {
              uint32_t col_type_id =
                  type_to_index->GetSingleWordInOperand(0);
              uint32_t col_size = GetTypeBitWidth(col_type_id) / 8;
              result.byte_offset += index_value * col_size;
              type_to_index = get_def_use_mgr()->GetDef(col_type_id);
              break;
            }
            case spv::Op::OpTypeRuntimeArray: {
              uint32_t elem_type_id =
                  type_to_index->GetSingleWordInOperand(0);
              uint32_t elem_size = GetTypeBitWidth(elem_type_id) / 8;
              result.byte_offset += index_value * elem_size;
              type_to_index = get_def_use_mgr()->GetDef(elem_type_id);
              break;
            }
            default:
              result.valid = false;
              return result;
          }
        }

        current = base_inst;
        break;
      }

      default:
        result.base_id = current->result_id();
        return result;
    }
  }

  return result;
}

uint32_t LoadCombinePass::GetTypeBitWidth(uint32_t type_id) {
  Instruction* type_inst = get_def_use_mgr()->GetDef(type_id);
  if (!type_inst) return 0;

  switch (type_inst->opcode()) {
    case spv::Op::OpTypeInt:
    case spv::Op::OpTypeFloat:
      return type_inst->GetSingleWordInOperand(0);
    case spv::Op::OpTypeVector: {
      uint32_t elem_type_id = type_inst->GetSingleWordInOperand(0);
      uint32_t elem_count = type_inst->GetSingleWordInOperand(1);
      return GetTypeBitWidth(elem_type_id) * elem_count;
    }
    case spv::Op::OpTypeArray: {
      uint32_t elem_type_id = type_inst->GetSingleWordInOperand(0);
      uint32_t elem_count_id = type_inst->GetSingleWordInOperand(1);
      Instruction* count_inst = get_def_use_mgr()->GetDef(elem_count_id);
      if (count_inst && count_inst->opcode() == spv::Op::OpConstant) {
        uint32_t count = count_inst->GetSingleWordInOperand(0);
        return GetTypeBitWidth(elem_type_id) * count;
      }
      return 0;
    }
    default:
      return 0;
  }
}

bool LoadCombinePass::CombineLoads(
    std::unordered_map<uint32_t, std::vector<LoadInfo>>* load_map) {
  bool combined = false;

  for (auto& entry : *load_map) {
    if (entry.second.size() < 2) continue;

    std::sort(entry.second.begin(), entry.second.end(),
              [](const LoadInfo& a, const LoadInfo& b) {
                return a.byte_offset < b.byte_offset;
              });

    std::vector<LoadInfo> current_run;
    uint64_t prev_offset = 0;
    uint64_t prev_size = 0;
    bool first = true;

    for (auto& info : entry.second) {
      uint32_t bw = GetTypeBitWidth(info.load_inst->type_id());
      uint64_t size_bytes = bw / 8;
      if (size_bytes == 0) {
        if (current_run.size() >= 2) {
          if (CombineContiguousLoads(&current_run)) combined = true;
        }
        current_run.clear();
        first = true;
        continue;
      }

      if (first) {
        prev_offset = info.byte_offset;
        prev_size = size_bytes;
        current_run.push_back(info);
        first = false;
      } else if (info.byte_offset == prev_offset + prev_size) {
        prev_offset = info.byte_offset;
        prev_size = size_bytes;
        current_run.push_back(info);
      } else if (info.byte_offset > prev_offset + prev_size) {
        if (current_run.size() >= 2) {
          if (CombineContiguousLoads(&current_run)) combined = true;
        }
        current_run.clear();
        prev_offset = info.byte_offset;
        prev_size = size_bytes;
        current_run.push_back(info);
      }
    }

    if (current_run.size() >= 2) {
      if (CombineContiguousLoads(&current_run)) combined = true;
    }
  }

  return combined;
}

bool LoadCombinePass::CombineContiguousLoads(std::vector<LoadInfo>* loads) {
  if (loads->size() < 2) return false;

  // Compute total size in bits.
  uint32_t total_size = 0;
  for (const auto& info : *loads) {
    total_size += GetTypeBitWidth(info.load_inst->type_id());
  }

  // Trim from the end until total_size is a power of two.
  while (total_size != 0 && (total_size & (total_size - 1)) != 0) {
    total_size -= GetTypeBitWidth(loads->back().load_inst->type_id());
    loads->pop_back();
  }

  if (loads->size() < 2) return false;

  // Find earliest load by insert order.
  LoadInfo* earliest = &(*loads)[0];
  for (auto& info : *loads) {
    if (info.insert_index < earliest->insert_index) {
      earliest = &info;
    }
  }

  Instruction* first_load = (*loads)[0].load_inst;
  uint32_t first_ptr_id = first_load->GetSingleWordInOperand(0);
  uint32_t first_type_id = first_load->type_id();

  // Create combined type.
  uint32_t combined_type_id = 0;
  analysis::TypeManager* type_mgr = context()->get_type_mgr();

  if (total_size == 64) {
    analysis::Integer int_type(64, false);
    combined_type_id = type_mgr->GetTypeInstruction(&int_type);
  } else if (total_size == 32) {
    analysis::Integer int_type(32, false);
    combined_type_id = type_mgr->GetTypeInstruction(&int_type);
  } else if (total_size == 16) {
    analysis::Integer int_type(16, false);
    combined_type_id = type_mgr->GetTypeInstruction(&int_type);
  } else if (total_size == 128) {
    analysis::Integer int_type(128, false);
    combined_type_id = type_mgr->GetTypeInstruction(&int_type);
  } else {
    return false;
  }

  // Get pointer type for the combined type.
  Instruction* first_ptr = get_def_use_mgr()->GetDef(first_ptr_id);
  spv::StorageClass storage_class = spv::StorageClass::Function;
  if (first_ptr) {
    Instruction* first_ptr_type =
        get_def_use_mgr()->GetDef(first_ptr->type_id());
    if (first_ptr_type &&
        first_ptr_type->opcode() == spv::Op::OpTypePointer) {
      storage_class =
          static_cast<spv::StorageClass>(
              first_ptr_type->GetSingleWordInOperand(0));
    }
  }

  uint32_t wide_ptr_type_id =
      type_mgr->FindPointerToType(combined_type_id, storage_class);
  if (wide_ptr_type_id == 0) return false;

  // Build the wider load before the earliest load's position.
  InstructionBuilder builder(
      context(), earliest->load_inst,
      IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);

  // Bitcast pointer to wider pointer type.
  Instruction* bitcast =
      builder.AddUnaryOp(wide_ptr_type_id, spv::Op::OpBitcast, first_ptr_id);

  // Create wider load.
  Instruction* wider_load = builder.AddUnaryOp(combined_type_id, spv::Op::OpLoad,
                                                bitcast->result_id());

  // For each original load, extract the appropriate bits.
  uint64_t base_offset = (*loads)[0].byte_offset;
  InstructionBuilder::InsertionPointTy insert_before(earliest->load_inst);
  IRContext* ctx = context();

  for (auto& info : *loads) {
    uint32_t orig_type_id = info.load_inst->type_id();
    uint32_t orig_bw = GetTypeBitWidth(orig_type_id);
    uint64_t offset_bits = (info.byte_offset - base_offset) * 8;

    InstructionBuilder extract_builder(
        ctx, info.load_inst,
        IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);

    Instruction* extracted = nullptr;

    if (total_size == 64 && orig_bw == 32) {
      if (offset_bits == 0) {
        extracted = extract_builder.AddUnaryOp(orig_type_id, spv::Op::OpBitcast,
                                                wider_load->result_id());
      } else if (offset_bits == 32) {
        // Create constant 32 for the shift amount.
        analysis::Integer int32_type(32, false);
        const analysis::Constant* shift_const_32 =
            ctx->get_constant_mgr()->GetConstant(&int32_type, {32});
        Instruction* shift_32_inst =
            ctx->get_constant_mgr()->GetDefiningInstruction(shift_const_32);

        Instruction* shifted = extract_builder.AddBinaryOp(
            combined_type_id, spv::Op::OpShiftRightLogical,
            wider_load->result_id(), shift_32_inst->result_id());
        extracted = extract_builder.AddUnaryOp(orig_type_id, spv::Op::OpBitcast,
                                                shifted->result_id());
      }
    } else if (total_size == 128 && orig_bw == 32) {
      if (offset_bits % 32 == 0) {
        uint32_t shift_amount = static_cast<uint32_t>(offset_bits);
        analysis::Integer int32_type(32, false);
        const analysis::Constant* shift_const =
            ctx->get_constant_mgr()->GetConstant(&int32_type, {shift_amount});
        Instruction* shift_inst =
            ctx->get_constant_mgr()->GetDefiningInstruction(shift_const);

        Instruction* shifted = extract_builder.AddBinaryOp(
            combined_type_id, spv::Op::OpShiftRightLogical,
            wider_load->result_id(), shift_inst->result_id());
        extracted = extract_builder.AddUnaryOp(orig_type_id, spv::Op::OpBitcast,
                                                shifted->result_id());
      }
    } else if (total_size == 64 && orig_bw == 16) {
      uint32_t shift_amount = static_cast<uint32_t>(offset_bits);
      analysis::Integer int32_type(32, false);
      const analysis::Constant* shift_const =
          ctx->get_constant_mgr()->GetConstant(&int32_type, {shift_amount});
      Instruction* shift_inst =
          ctx->get_constant_mgr()->GetDefiningInstruction(shift_const);

      Instruction* shifted = extract_builder.AddBinaryOp(
          combined_type_id, spv::Op::OpShiftRightLogical,
          wider_load->result_id(), shift_inst->result_id());
      extracted = extract_builder.AddUnaryOp(orig_type_id, spv::Op::OpBitcast,
                                              shifted->result_id());
    } else if (total_size == 32 && orig_bw == 16) {
      uint32_t shift_amount = static_cast<uint32_t>(offset_bits);
      analysis::Integer int32_type(32, false);
      const analysis::Constant* shift_const =
          ctx->get_constant_mgr()->GetConstant(&int32_type, {shift_amount});
      Instruction* shift_inst =
          ctx->get_constant_mgr()->GetDefiningInstruction(shift_const);

      Instruction* shifted = extract_builder.AddBinaryOp(
          combined_type_id, spv::Op::OpShiftRightLogical,
          wider_load->result_id(), shift_inst->result_id());
      extracted = extract_builder.AddUnaryOp(orig_type_id, spv::Op::OpBitcast,
                                              shifted->result_id());
    }

    if (extracted) {
      ctx->ReplaceAllUsesWith(info.load_inst->result_id(),
                               extracted->result_id());
      ctx->KillInst(info.load_inst);
    }
  }

  return true;
}

}  // namespace opt
}  // namespace spvtools
