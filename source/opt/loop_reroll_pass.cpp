// Copyright (c) 2023 Google LLC.
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

#include "source/opt/loop_reroll_pass.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "source/opt/basic_block.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/function.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/loop_descriptor.h"
#include "source/opt/scalar_analysis.h"

namespace spvtools {
namespace opt {

Pass::Status LoopRerollPass::Process() {
  bool modified = false;
  Module* module = get_module();

  for (Function& f : *module) {
    if (ProcessFunction(&f)) {
      modified = true;
    }
  }

  return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
}

bool LoopRerollPass::IsSimpleIV(const Instruction* phi,
                                uint32_t* trip_count_id) const {
  if (phi->opcode() != spv::Op::OpPhi) return false;

  analysis::DefUseManager* def_use_mgr = context()->get_def_use_mgr();

  // Find which loop this phi belongs to.
  const BasicBlock* header_block = context()->get_instr_block(const_cast<Instruction*>(phi));
  if (!header_block) return false;
  const Function* func = header_block->GetParent();
  if (!func) return false;

  LoopDescriptor* ld = context()->GetLoopDescriptor(func);
  if (!ld) return false;

  const Loop* loop = (*ld)[header_block->id()];
  if (!loop || loop->GetHeaderBlock() != header_block) return false;

  // Check init value: must come from outside the loop and be constant 0.
  bool found_init = false;
  bool has_step = false;

  for (uint32_t i = 0; i < phi->NumInOperands(); i += 2) {
    uint32_t value_id = phi->GetSingleWordInOperand(i);
    uint32_t block_id = phi->GetSingleWordInOperand(i + 1);

    if (!loop->IsInsideLoop(block_id)) {
      // Initial value (from preheader).
      found_init = true;
      Instruction* init_inst = def_use_mgr->GetDef(value_id);
      if (!init_inst || init_inst->opcode() != spv::Op::OpConstant)
        return false;

      const analysis::ConstantManager* const_mgr =
          context()->get_constant_mgr();
      const analysis::Constant* constant =
          const_mgr->FindDeclaredConstant(value_id);
      if (!constant) return false;

      const analysis::Integer* int_type = constant->type()->AsInteger();
      if (!int_type) return false;

      int64_t val = int_type->IsSigned() ? constant->GetSignExtendedValue()
                                         : constant->GetZeroExtendedValue();
      if (val != 0) return false;
    }
  }

  if (!found_init) return false;

  // Check step value: must be OpIAdd with the phi and constant 1.
  for (uint32_t i = 0; i < phi->NumInOperands(); i += 2) {
    uint32_t value_id = phi->GetSingleWordInOperand(i);
    uint32_t block_id = phi->GetSingleWordInOperand(i + 1);

    if (loop->IsInsideLoop(block_id)) {
      Instruction* step_inst = def_use_mgr->GetDef(value_id);
      if (!step_inst || step_inst->opcode() != spv::Op::OpIAdd) return false;

      // One operand must be the phi and the other must be constant 1.
      uint32_t op0 = step_inst->GetSingleWordInOperand(0);
      uint32_t op1 = step_inst->GetSingleWordInOperand(1);

      if (op0 != phi->result_id() && op1 != phi->result_id()) return false;

      uint32_t const_id = (op0 == phi->result_id()) ? op1 : op0;
      Instruction* const_inst = def_use_mgr->GetDef(const_id);
      if (!const_inst || const_inst->opcode() != spv::Op::OpConstant)
        return false;

      const analysis::ConstantManager* const_mgr =
          context()->get_constant_mgr();
      const analysis::Constant* constant =
          const_mgr->FindDeclaredConstant(const_id);
      if (!constant) return false;

      const analysis::Integer* int_type = constant->type()->AsInteger();
      if (!int_type) return false;

      int64_t val = int_type->IsSigned() ? constant->GetSignExtendedValue()
                                         : constant->GetZeroExtendedValue();
      if (val != 1) return false;

      has_step = true;
    }
  }

  if (!has_step) return false;

  // Find trip count.
  if (trip_count_id) {
    uint32_t tc_id = GetTripCountFromIV(phi);
    if (tc_id == 0) return false;
    *trip_count_id = tc_id;
  }

  return true;
}

uint32_t LoopRerollPass::GetTripCountFromIV(const Instruction* phi) const {
  const BasicBlock* header = context()->get_instr_block(const_cast<Instruction*>(phi));
  if (!header) return 0;

  const Instruction* terminator = header->terminator();
  if (!terminator || terminator->opcode() != spv::Op::OpBranchConditional)
    return 0;

  uint32_t cond_id = terminator->GetSingleWordInOperand(0);
  Instruction* cond_inst = context()->get_def_use_mgr()->GetDef(cond_id);
  if (!cond_inst) return 0;

  spv::Op cond_op = cond_inst->opcode();
  if (cond_op != spv::Op::OpSLessThan && cond_op != spv::Op::OpULessThan &&
      cond_op != spv::Op::OpSLessThanEqual &&
      cond_op != spv::Op::OpULessThanEqual &&
      cond_op != spv::Op::OpSGreaterThan &&
      cond_op != spv::Op::OpUGreaterThan &&
      cond_op != spv::Op::OpSGreaterThanEqual &&
      cond_op != spv::Op::OpUGreaterThanEqual)
    return 0;

  uint32_t cmp_op0 = cond_inst->GetSingleWordInOperand(0);
  uint32_t cmp_op1 = cond_inst->GetSingleWordInOperand(1);

  if (cmp_op0 == phi->result_id()) {
    return cmp_op1;
  } else if (cmp_op1 == phi->result_id()) {
    return cmp_op0;
  }

  return 0;
}

uint32_t LoopRerollPass::DetectRootIncrements(
    const Instruction* iv,
    std::vector<Instruction*>& root_increments) const {
  // Scan the header block for OpIAdd instructions that add a small constant
  // (1, 2, 3) to the IV. These are potential root increments.
  const BasicBlock* header = context()->get_instr_block(const_cast<Instruction*>(iv));
  if (!header) return 0;

  analysis::DefUseManager* def_use_mgr = context()->get_def_use_mgr();
  std::map<uint32_t, Instruction*> increment_map;

  // Look at all users of the IV in the header block.
  bool has_step_one_increment = false;
  def_use_mgr->ForEachUser(iv, [&](Instruction* user) {
    if (user->opcode() == spv::Op::OpIAdd) {
      uint32_t op0 = user->GetSingleWordInOperand(0);
      uint32_t op1 = user->GetSingleWordInOperand(1);

      if (op0 == iv->result_id() || op1 == iv->result_id()) {
        uint32_t const_id = (op0 == iv->result_id()) ? op1 : op0;
        Instruction* const_inst = def_use_mgr->GetDef(const_id);
        if (const_inst && const_inst->opcode() == spv::Op::OpConstant) {
          const analysis::ConstantManager* const_mgr =
              context()->get_constant_mgr();
          const analysis::Constant* constant =
              const_mgr->FindDeclaredConstant(const_id);
          if (constant) {
            const analysis::Integer* int_type = constant->type()->AsInteger();
            if (int_type) {
              int64_t val = int_type->IsSigned()
                                ? constant->GetSignExtendedValue()
                                : constant->GetZeroExtendedValue();
              // Check if the user feeds back into the phi (loop increment).
              bool is_loop_inc = false;
              def_use_mgr->ForEachUser(user, [&](Instruction* u) {
                if (u == iv) is_loop_inc = true;
              });

              if (is_loop_inc) {
                if (val == 1) has_step_one_increment = true;
                // Otherwise, skip loop increment.
              } else if (val >= 1 && val <= 4) {
                // This is a root increment candidate.
                if (increment_map.find(static_cast<uint32_t>(val)) ==
                    increment_map.end()) {
                  increment_map[static_cast<uint32_t>(val)] = user;
                }
              }
            }
          }
        }
      }
    }
  });

  // We need at least one increment with value 1 for the step.
  if (!has_step_one_increment) return 0;

  // Check if we have a consecutive sequence: 1, 2, ..., scale-1.
  // The scale is determined by the number of consecutive increments + 1.
  // For scale=2: need increments up to 1 (i.e., only iv+1 exists)
  // For scale=3: need increments 1 and 2
  // For scale=4: need increments 1, 2, and 3

  if (increment_map.empty()) return 0;

  // Check for consecutive sequence starting from 1.
  uint32_t max_val = 0;
  for (auto& pair : increment_map) {
    if (pair.first > max_val) max_val = pair.first;
  }

  // Verify we have all values from 1 to max_val consecutively.
  for (uint32_t i = 1; i <= max_val; i++) {
    if (increment_map.find(i) == increment_map.end()) return 0;
  }

  // Scale is max_val + 1.
  uint32_t scale = max_val + 1;
  if (scale < 2 || scale > 4) return 0;

  // Collect root increments in order.
  root_increments.clear();
  for (uint32_t i = 1; i <= max_val; i++) {
    root_increments.push_back(increment_map[i]);
  }

  return scale;
}

bool LoopRerollPass::AreIsomorphic(
    const Instruction* a, const Instruction* b,
    std::unordered_map<uint32_t, uint32_t>& value_map) const {
  // Must have the same opcode.
  if (a->opcode() != b->opcode()) return false;

  // Must have the same type.
  if (a->type_id() != b->type_id()) return false;

  // Must have the same number of input operands.
  if (a->NumInOperands() != b->NumInOperands()) return false;

  analysis::DefUseManager* def_use_mgr = context()->get_def_use_mgr();

  // Check each operand for isomorphism.
  for (uint32_t i = 0; i < a->NumInOperands(); i++) {
    uint32_t op_a = a->GetSingleWordInOperand(i);
    uint32_t op_b = b->GetSingleWordInOperand(i);

    if (op_a == op_b) continue;

    // Check if this operand has a substitution mapping.
    auto it = value_map.find(op_a);
    if (it != value_map.end()) {
      if (it->second == op_b) continue;
      return false;
    }

    // If b's operand is in the map's value side, accept if it maps to a's op.
    // Check if op_b is a mapped value.
    bool found_in_map = false;
    for (auto& pair : value_map) {
      if (pair.second == op_b) {
        found_in_map = true;
        if (pair.first == op_a) continue;
        return false;
      }
    }

    // If not in the map, check if the defining instructions are isomorphic.
    Instruction* def_a = def_use_mgr->GetDef(op_a);
    Instruction* def_b = def_use_mgr->GetDef(op_b);

    if (!def_a || !def_b) return false;

    // Constants: same value.
    if (def_a->opcode() == spv::Op::OpConstant &&
        def_b->opcode() == spv::Op::OpConstant) {
      // Check if they are the same constant.
      const analysis::ConstantManager* const_mgr =
          context()->get_constant_mgr();
      const analysis::Constant* const_a =
          const_mgr->FindDeclaredConstant(op_a);
      const analysis::Constant* const_b =
          const_mgr->FindDeclaredConstant(op_b);
      if (const_a && const_b && op_a == op_b) continue;
      return false;
    }

    // Recurse into the definitions, adding a mapping.
    value_map[op_a] = op_b;
    if (!AreIsomorphic(def_a, def_b, value_map)) return false;
  }

  return true;
}

bool LoopRerollPass::CheckIsomorphism(
    const Instruction* iv, const std::vector<Instruction*>& root_increments,
    uint32_t scale) const {
  // For each root increment (iv+k), we need to check that the set of
  // instructions using that root is isomorphic to the set using the base IV.
  // Simplified: check that the direct users other than the root increments
  // themselves are structurally similar.

  analysis::DefUseManager* def_use_mgr = context()->get_def_use_mgr();

  // Collect users of the base IV (excluding the root increments and the step).
  std::vector<const Instruction*> base_users;
  def_use_mgr->ForEachUser(iv, [&](const Instruction* user) {
    // Skip if this is a root increment or the loop step.
    bool is_root_or_step = false;
    for (Instruction* root : root_increments) {
      if (user == root) {
        is_root_or_step = true;
        break;
      }
    }
    // Check if it's the step (IAdd that feeds the phi).
    if (user->opcode() == spv::Op::OpIAdd) {
      bool feeds_phi = false;
      for (uint32_t j = 0; j < user->NumInOperands(); j++) {
        uint32_t op = user->GetSingleWordInOperand(j);
        // Can't easily check if user feeds phi here
      }
      // Assume it might be the step if it has constant 1 and feeds phi
    }

    if (!is_root_or_step) {
      base_users.push_back(user);
    }
  });

  // For each iteration (root increment), collect users of that root.
  for (uint32_t iter = 0; iter < root_increments.size(); iter++) {
    Instruction* root = root_increments[iter];
    std::vector<const Instruction*> root_users;

    def_use_mgr->ForEachUser(root, [&](const Instruction* user) {
      // Skip the root increments for this iteration (they're higher values).
      bool is_higher_root = false;
      for (uint32_t j = iter + 1; j < root_increments.size(); j++) {
        if (user == root_increments[j]) {
          is_higher_root = true;
          break;
        }
      }
      if (!is_higher_root) {
        root_users.push_back(user);
      }
    });

    // The number of users should match.
    if (root_users.size() != base_users.size()) return false;

    // Check isomorphism for each pair.
    for (size_t i = 0; i < base_users.size(); i++) {
      // Build a value map that maps the base IV to the root increment value.
      std::unordered_map<uint32_t, uint32_t> value_map;
      value_map[iv->result_id()] = root->result_id();

      if (!AreIsomorphic(base_users[i], root_users[i], value_map)) {
        return false;
      }
    }
  }

  return true;
}

bool LoopRerollPass::RerollLoop(Loop* loop, Function* f) {
  BasicBlock* header = loop->GetHeaderBlock();
  if (!header) return false;

  // Find the IV phi.
  Instruction* iv = nullptr;
  uint32_t trip_count_id = 0;

  for (Instruction& inst : *header) {
    if (inst.opcode() != spv::Op::OpPhi) break;
    if (IsSimpleIV(&inst, &trip_count_id)) {
      iv = &inst;
      break;
    }
  }

  if (!iv || trip_count_id == 0) return false;

  // Detect root increments.
  std::vector<Instruction*> root_increments;
  uint32_t scale = DetectRootIncrements(iv, root_increments);
  if (scale < 2) return false;

  // Check isomorphism.
  if (!CheckIsomorphism(iv, root_increments, scale)) return false;

  // ---- TRANSFORM ----
  // 1. Remove all root increment instructions and their dependent chains
  //    except for the base IV's chain.
  // 2. Change the step from `scale` to 1.
  // 3. Multiply the trip count by scale.

  analysis::DefUseManager* def_use_mgr = context()->get_def_use_mgr();

  // Find the loop increment (the OpIAdd that feeds the IV phi).
  Instruction* loop_increment = nullptr;
  for (uint32_t i = 1; i < iv->NumInOperands(); i += 2) {
    uint32_t block_id = iv->GetSingleWordInOperand(i);
    if (loop->IsInsideLoop(block_id)) {
      loop_increment =
          def_use_mgr->GetDef(iv->GetSingleWordInOperand(i - 1));
      break;
    }
  }

  if (!loop_increment) return false;

  // Change the loop increment from `scale` to 1.
  // The increment is OpIAdd %iv, %const_scale. We need to change %const_scale
  // to 1. But we shouldn't modify a constant that might be used elsewhere.
  // Instead, we need to create a new constant 1 of the same type.
  const analysis::ConstantManager* const_mgr = context()->get_constant_mgr();
  const analysis::Type* int_type = const_mgr->FindDeclaredConstant(
      loop_increment->GetSingleWordInOperand(
          (loop_increment->GetSingleWordInOperand(0) == iv->result_id())
              ? 1
              : 0))
      ->type();
  
  // Find the constant operand of the loop increment.
  uint32_t const_operand_idx =
      (loop_increment->GetSingleWordInOperand(0) == iv->result_id()) ? 1 : 0;
  uint32_t old_const_id = loop_increment->GetSingleWordInOperand(const_operand_idx);

  // Create the new step constant 1.
  Instruction* type_inst = def_use_mgr->GetDef(iv->type_id());
  if (!type_inst) return false;

  // Find or create constant 1 of the IV type.
  // Look for existing constant 1 in the module's types/values section.
  uint32_t new_step_id = 0;
  for (auto& inst : get_module()->types_values()) {
    if (inst.opcode() == spv::Op::OpConstant &&
        inst.type_id() == iv->type_id()) {
      const analysis::Constant* c = const_mgr->FindDeclaredConstant(inst.result_id());
      if (c) {
        const analysis::Integer* it = c->type()->AsInteger();
        if (it) {
          int64_t val = it->IsSigned() ? c->GetSignExtendedValue()
                                      : c->GetZeroExtendedValue();
          if (val == 1) {
            new_step_id = inst.result_id();
            break;
          }
        }
      }
    }
  }

  // If not found, we can't easily create new constants in a pass.
  // Instead, we'll skip the transform and bail.
  if (new_step_id == 0) return false;

  // Replace the constant in the loop increment.
  loop_increment->SetInOperand(const_operand_idx, {new_step_id});
  def_use_mgr->AnalyzeInstUse(loop_increment);

  // Multiply the trip count by the scale factor.
  // We need to find the trip count constant and replace it with count * scale.
  Instruction* tc_inst = def_use_mgr->GetDef(trip_count_id);
  if (!tc_inst || tc_inst->opcode() != spv::Op::OpConstant) return false;

  const analysis::Constant* tc_const =
      const_mgr->FindDeclaredConstant(trip_count_id);
  if (!tc_const) return false;

  const analysis::Integer* tc_int_type = tc_const->type()->AsInteger();
  if (!tc_int_type) return false;

  int64_t tc_val = tc_int_type->IsSigned()
                       ? tc_const->GetSignExtendedValue()
                       : tc_const->GetZeroExtendedValue();

  int64_t new_tc_val = tc_val * static_cast<int64_t>(scale);

  // Check for overflow (rough check).
  if (new_tc_val < tc_val && tc_val > 0) return false;  // overflow

  // Create or find a constant for the new trip count.
  uint32_t new_tc_id = 0;
  // Look for existing constant with the same value and type.
  for (auto& inst : get_module()->types_values()) {
    if (inst.opcode() == spv::Op::OpConstant &&
        inst.type_id() == trip_count_id) {
      const analysis::Constant* c = const_mgr->FindDeclaredConstant(inst.result_id());
      if (c) {
        const analysis::Integer* it = c->type()->AsInteger();
        if (it) {
          int64_t val = it->IsSigned() ? c->GetSignExtendedValue()
                                      : c->GetZeroExtendedValue();
          if (val == new_tc_val) {
            new_tc_id = inst.result_id();
            break;
          }
        }
      }
    }
  }

  if (new_tc_id == 0) return false;

  // Find the comparison instruction and replace the trip count.
  const Instruction* terminator = header->terminator();
  if (!terminator || terminator->opcode() != spv::Op::OpBranchConditional)
    return false;

  uint32_t cond_id = terminator->GetSingleWordInOperand(0);
  Instruction* cond_inst = def_use_mgr->GetDef(cond_id);
  if (!cond_inst) return false;

  // Replace the trip count operand in the comparison.
  for (uint32_t i = 0; i < cond_inst->NumInOperands(); i++) {
    if (cond_inst->GetSingleWordInOperand(i) == trip_count_id) {
      cond_inst->SetInOperand(i, {new_tc_id});
      def_use_mgr->AnalyzeInstUse(cond_inst);
      break;
    }
  }

  // Remove the root increment instructions (the extra copies) and all
  // instructions that use them exclusively.
  // Collect all instructions to remove.
  std::unordered_set<Instruction*> to_remove;
  for (Instruction* root : root_increments) {
    // Collect users of this root.
    std::vector<Instruction*> users_to_kill;
    def_use_mgr->ForEachUser(root, [&](Instruction* user) {
      // Don't remove the phi or the condition.
      if (user == iv) return;
      if (user == cond_inst) return;
      // Don't remove higher root increments.
      bool is_higher_root = false;
      for (Instruction* r : root_increments) {
        if (user == r) {
          is_higher_root = true;
          break;
        }
      }
      if (!is_higher_root) {
        users_to_kill.push_back(user);
      }
    });

    for (Instruction* user : users_to_kill) {
      to_remove.insert(user);
    }

    // Remove the root itself (except the last one which is the loop increment).
    if (root != loop_increment) {
      to_remove.insert(root);
    }
  }

  // Kill the instructions.
  for (Instruction* inst : to_remove) {
    // Check that it's safe to remove (no remaining uses outside the loop).
    bool has_external_use = false;
    def_use_mgr->ForEachUser(inst, [&](Instruction* user) {
      if (to_remove.find(user) == to_remove.end() && user != iv) {
        has_external_use = true;
      }
    });

    if (!has_external_use) {
      context()->KillInst(inst);
    }
  }

  return true;
}

bool LoopRerollPass::ProcessFunction(Function* f) {
  bool modified = false;

  LoopDescriptor* ld = context()->GetLoopDescriptor(f);
  if (!ld) return false;

  // Collect loops.
  std::vector<Loop*> loops;
  for (Loop& loop : *ld) {
    loops.push_back(&loop);
  }

  for (Loop* loop : loops) {
    // Only single-block loops (header is the body).
    // The loop should have exactly the header block in its block set,
    // possibly with the latch being the same as the header or a separate
    // continue block.
    if (loop->GetBlocks().size() > 2) continue;

    // Must be structured.
    if (!loop->GetHeaderBlock()->IsLoopHeader()) continue;

    // Must have a preheader and latch.
    if (!loop->GetPreHeaderBlock()) continue;
    if (!loop->GetLatchBlock()) continue;
    if (!loop->GetMergeBlock()) continue;

    // Simple check: the header block is the only body block.
    // The latch should be the header itself (self-loop single block) or
    // a separate continue block. In a single-block loop, the body consists
    // of just the header.
    bool is_single_block = true;
    for (uint32_t bb_id : loop->GetBlocks()) {
      if (bb_id != loop->GetHeaderBlock()->id() &&
          bb_id != loop->GetLatchBlock()->id()) {
        is_single_block = false;
        break;
      }
    }

    if (!is_single_block) continue;

    // Heavy bailout: no function calls, loads/stores, atomics, or nested
    // loops in the body.
    bool has_complex_ops = false;
    for (Instruction& inst : *loop->GetHeaderBlock()) {
      switch (inst.opcode()) {
        case spv::Op::OpFunctionCall:
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
        case spv::Op::OpAtomicFAddEXT:
        case spv::Op::OpImageSampleImplicitLod:
        case spv::Op::OpImageSampleExplicitLod:
        case spv::Op::OpImageFetch:
        case spv::Op::OpImageRead:
        case spv::Op::OpImageWrite:
          has_complex_ops = true;
          break;
        default:
          break;
      }
      if (has_complex_ops) break;
    }

    if (has_complex_ops) continue;

    if (RerollLoop(loop, f)) {
      modified = true;
    }
  }

  return modified;
}

}  // namespace opt
}  // namespace spvtools
