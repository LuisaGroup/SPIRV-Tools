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

#include "source/opt/load_store_motion_pass.h"

#include <iterator>

#include "source/opt/ir_builder.h"
#include "source/opt/ir_context.h"

namespace spvtools {
namespace opt {

Pass::Status LoadStoreMotionPass::Process() {
  bool modified = false;

  for (auto& func : *get_module()) {
    if (func.IsDeclaration()) {
      continue;
    }

    for (auto& bb : func) {
      if (!IsDiamondHead(&bb)) continue;

      if (MergeLoads(&bb)) {
        modified = true;
      }

      BasicBlock* merge = GetDiamondMergeBlock(&bb);
      if (merge && MergeStores(merge)) {
        modified = true;
      }
    }
  }

  return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
}

bool LoadStoreMotionPass::IsDiamondHead(BasicBlock* bb) {
  if (!bb) return false;

  Instruction* terminator = bb->terminator();
  if (!terminator || terminator->opcode() != spv::Op::OpBranchConditional) {
    return false;
  }

  Instruction* merge_inst = bb->GetMergeInst();
  if (!merge_inst || merge_inst->opcode() != spv::Op::OpSelectionMerge) {
    return false;
  }

  uint32_t merge_bb_id = merge_inst->GetSingleWordInOperand(0);
  uint32_t left_id = terminator->GetSingleWordInOperand(1);
  uint32_t right_id = terminator->GetSingleWordInOperand(2);

  const auto& left_preds = cfg()->preds(left_id);
  const auto& right_preds = cfg()->preds(right_id);
  if (left_preds.size() != 1 || left_preds[0] != bb->id()) return false;
  if (right_preds.size() != 1 || right_preds[0] != bb->id()) return false;

  BasicBlock* left_bb = context()->get_instr_block(left_id);
  BasicBlock* right_bb = context()->get_instr_block(right_id);
  if (!left_bb || !right_bb) return false;

  Instruction* left_term = left_bb->terminator();
  Instruction* right_term = right_bb->terminator();
  if (!left_term || !right_term) return false;

  if (left_term->opcode() != spv::Op::OpBranch) return false;
  if (right_term->opcode() != spv::Op::OpBranch) return false;

  if (left_term->GetSingleWordInOperand(0) != merge_bb_id) return false;
  if (right_term->GetSingleWordInOperand(0) != merge_bb_id) return false;

  return true;
}

BasicBlock* LoadStoreMotionPass::GetDiamondMergeBlock(BasicBlock* bb) {
  Instruction* merge_inst = bb->GetMergeInst();
  if (!merge_inst) return nullptr;
  return context()->get_instr_block(merge_inst->GetSingleWordInOperand(0));
}

void LoadStoreMotionPass::GetDiamondArms(BasicBlock* header,
                                         BasicBlock** arm_a,
                                         BasicBlock** arm_b) {
  Instruction* terminator = header->terminator();
  uint32_t left_id = terminator->GetSingleWordInOperand(1);
  uint32_t right_id = terminator->GetSingleWordInOperand(2);

  BasicBlock* left_bb = context()->get_instr_block(left_id);
  BasicBlock* right_bb = context()->get_instr_block(right_id);

  if (left_id < right_id) {
    *arm_a = left_bb;
    *arm_b = right_bb;
  } else {
    *arm_a = right_bb;
    *arm_b = left_bb;
  }
}

size_t LoadStoreMotionPass::CountInstructions(BasicBlock* bb) {
  return std::distance(bb->begin(), bb->end());
}

Instruction* LoadStoreMotionPass::FindMatchingLoad(BasicBlock* arm,
                                                    Instruction* load) {
  uint32_t ptr_op = load->GetSingleWordInOperand(0);
  Instruction* ptr_inst = get_def_use_mgr()->GetDef(ptr_op);
  IRContext* ctx = context();

  for (auto& inst : *arm) {
    if (inst.opcode() != spv::Op::OpLoad) continue;
    if (inst.type_id() != load->type_id()) continue;

    // Check if the load is used outside the arm block.
    bool used_outside = false;
    get_def_use_mgr()->ForEachUser(
        inst.result_id(), [&used_outside, arm, ctx](Instruction* user) {
          BasicBlock* user_bb = ctx->get_instr_block(user);
          if (user_bb != arm) {
            used_outside = true;
          }
        });
    if (used_outside) continue;

    uint32_t other_ptr_op = inst.GetSingleWordInOperand(0);
    Instruction* other_ptr = get_def_use_mgr()->GetDef(other_ptr_op);

    if (!IsMustAlias(ptr_inst, other_ptr, get_def_use_mgr())) continue;

    if (HasMemoryBarrierBetween(&*arm->begin(), &inst, ptr_inst,
                                 get_def_use_mgr())) {
      continue;
    }

    return &inst;
  }
  return nullptr;
}

Instruction* LoadStoreMotionPass::FindMatchingStore(BasicBlock* arm,
                                                     Instruction* store) {
  uint32_t ptr_op = store->GetSingleWordInOperand(0);
  Instruction* ptr_inst = get_def_use_mgr()->GetDef(ptr_op);

  for (auto it = arm->rbegin(); it != arm->rend(); ++it) {
    Instruction& inst = *it;
    if (inst.opcode() != spv::Op::OpStore) continue;

    uint32_t other_ptr_op = inst.GetSingleWordInOperand(0);
    Instruction* other_ptr = get_def_use_mgr()->GetDef(other_ptr_op);

    if (!IsMustAlias(ptr_inst, other_ptr, get_def_use_mgr())) continue;

    if (HasMemoryBarrierBetween(&inst, &*arm->end(), ptr_inst,
                                 get_def_use_mgr())) {
      continue;
    }

    return &inst;
  }
  return nullptr;
}

bool LoadStoreMotionPass::MergeLoads(BasicBlock* header) {
  bool merged = false;

  BasicBlock* arm_a = nullptr;
  BasicBlock* arm_b = nullptr;
  GetDiamondArms(header, &arm_a, &arm_b);

  size_t size_b = CountInstructions(arm_b);
  size_t num_loads = 0;
  Instruction* merge_inst = header->GetMergeInst();
  IRContext* ctx = context();

  for (auto it = arm_a->begin(); it != arm_a->end();) {
    Instruction& inst = *it;
    ++it;

    if (inst.opcode() != spv::Op::OpLoad) continue;

    Instruction* ptr = get_def_use_mgr()->GetDef(inst.GetSingleWordInOperand(0));
    if (!ptr || !ptr->IsReadOnlyPointer()) continue;

    // Check if the load is used outside arm_a.
    bool used_outside = false;
    get_def_use_mgr()->ForEachUser(
        inst.result_id(), [&used_outside, arm_a, ctx](Instruction* user) {
          BasicBlock* user_bb = ctx->get_instr_block(user);
          if (user_bb != arm_a) {
            used_outside = true;
          }
        });
    if (used_outside) continue;

    if (HasMemoryBarrierBetween(&*arm_a->begin(), &inst, ptr,
                                 get_def_use_mgr())) {
      continue;
    }

    ++num_loads;
    if (num_loads * size_b >= kMagicCompileTimeControl) break;

    Instruction* match = FindMatchingLoad(arm_b, &inst);
    if (!match) continue;

    // Clone the load and insert into header before merge instruction.
    std::unique_ptr<Instruction> clone_inst(inst.Clone(ctx));
    uint32_t new_id = TakeNextId();
    clone_inst->SetResultId(new_id);

    Instruction* new_inst = clone_inst.release();
    new_inst->InsertBefore(merge_inst);
    ctx->set_instr_block(new_inst, header);

    ctx->ReplaceAllUsesWith(inst.result_id(), new_id);
    ctx->ReplaceAllUsesWith(match->result_id(), new_id);

    ctx->KillInst(&inst);
    ctx->KillInst(match);

    merged = true;
  }

  return merged;
}

bool LoadStoreMotionPass::MergeStores(BasicBlock* merge) {
  const auto& merge_preds = cfg()->preds(merge->id());
  if (merge_preds.size() != 2) return false;

  BasicBlock* arm_a = context()->get_instr_block(merge_preds[0]);
  BasicBlock* arm_b = context()->get_instr_block(merge_preds[1]);
  IRContext* ctx = context();

  if (!arm_a || !arm_b) return false;

  if (arm_a->id() > arm_b->id()) {
    std::swap(arm_a, arm_b);
  }

  bool merged = false;
  size_t size_b = CountInstructions(arm_b);
  size_t num_stores = 0;

  // Find insertion point after all phis.
  BasicBlock::iterator insert_point = merge->begin();
  while (insert_point != merge->end() &&
         insert_point->opcode() == spv::Op::OpPhi) {
    ++insert_point;
  }

  for (auto it = arm_a->rbegin(); it != arm_a->rend();) {
    Instruction& inst = *it;
    ++it;

    if (inst.opcode() != spv::Op::OpStore) continue;

    Instruction* ptr = get_def_use_mgr()->GetDef(inst.GetSingleWordInOperand(0));
    if (!ptr || ptr->IsReadOnlyPointer()) continue;

    if (HasMemoryBarrierBetween(&inst, &*arm_a->end(), ptr,
                                 get_def_use_mgr())) {
      continue;
    }

    ++num_stores;
    if (num_stores * size_b >= kMagicCompileTimeControl) break;

    Instruction* match = FindMatchingStore(arm_b, &inst);
    if (!match) continue;

    if (HasMemoryBarrierBetween(match, &*arm_b->end(), ptr,
                                 get_def_use_mgr())) {
      continue;
    }

    uint32_t ptr_op = inst.GetSingleWordInOperand(0);
    uint32_t value_a = inst.GetSingleWordInOperand(1);
    uint32_t value_b = match->GetSingleWordInOperand(1);

    if (value_a == value_b) {
      // Same value: create the store directly.
      std::unique_ptr<Instruction> new_store_inst(
          new Instruction(ctx, spv::Op::OpStore, 0, 0,
                          {{SPV_OPERAND_TYPE_ID, {ptr_op}},
                           {SPV_OPERAND_TYPE_ID, {value_a}}}));
      Instruction* new_store = new_store_inst.release();
      new_store->InsertBefore(&*insert_point);
      ctx->set_instr_block(new_store, merge);
    } else {
      // Different values: create OpPhi in the merge block.
      Instruction* val_def_a = get_def_use_mgr()->GetDef(value_a);
      uint32_t val_type_id = val_def_a ? val_def_a->type_id() : 0;

      std::unique_ptr<Instruction> phi_inst(
          new Instruction(ctx, spv::Op::OpPhi, val_type_id, TakeNextId(),
                          {{SPV_OPERAND_TYPE_ID, {value_a}},
                           {SPV_OPERAND_TYPE_ID, {arm_a->id()}},
                           {SPV_OPERAND_TYPE_ID, {value_b}},
                           {SPV_OPERAND_TYPE_ID, {arm_b->id()}}}));

      uint32_t phi_id = phi_inst->result_id();
      Instruction* phi = phi_inst.release();
      phi->InsertBefore(&*insert_point);
      ctx->set_instr_block(phi, merge);

      // Store using the phi.
      std::unique_ptr<Instruction> new_store_inst(
          new Instruction(ctx, spv::Op::OpStore, 0, 0,
                          {{SPV_OPERAND_TYPE_ID, {ptr_op}},
                           {SPV_OPERAND_TYPE_ID, {phi_id}}}));
      Instruction* new_store = new_store_inst.release();
      new_store->InsertAfter(phi);
      ctx->set_instr_block(new_store, merge);
    }

    ctx->KillInst(&inst);
    ctx->KillInst(match);

    merged = true;
    it = arm_a->rbegin();
  }

  return merged;
}

}  // namespace opt
}  // namespace spvtools
