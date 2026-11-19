// Copyright (c) 2026 LuisaCompute Optimization
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

#include "source/opt/boolean_simplify_pass.h"

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "source/opt/constants.h"
#include "source/opt/ir_builder.h"
#include "source/opt/ir_context.h"
#include "source/opt/type_manager.h"

namespace spvtools {
namespace opt {
namespace {

enum class BoolConst { kNone, kTrue, kFalse };

BoolConst GetBoolConst(IRContext* ctx, uint32_t id) {
  Instruction* def = ctx->get_def_use_mgr()->GetDef(id);
  if (!def) return BoolConst::kNone;
  if (def->opcode() == spv::Op::OpConstantTrue) return BoolConst::kTrue;
  if (def->opcode() == spv::Op::OpConstantFalse) return BoolConst::kFalse;
  if (def->opcode() == spv::Op::OpConstantNull) return BoolConst::kFalse;
  return BoolConst::kNone;
}

bool GetInt32Const(IRContext* ctx, uint32_t id, int32_t* out_val) {
  Instruction* def = ctx->get_def_use_mgr()->GetDef(id);
  if (!def) return false;
  const analysis::Constant* cst =
      ctx->get_constant_mgr()->GetConstantFromInst(def);
  if (!cst) return false;
  if (cst->AsNullConstant()) {
    const auto* int_ty = cst->type()->AsInteger();
    if (!int_ty || int_ty->width() != 32) return false;
    *out_val = 0;
    return true;
  }
  const auto* ic = cst->AsIntConstant();
  if (!ic) return false;
  const auto* int_ty = ic->type()->AsInteger();
  if (!int_ty || int_ty->width() != 32) return false;
  *out_val = static_cast<int32_t>(ic->GetU32BitValue());
  return true;
}

bool GetUint32Const(IRContext* ctx, uint32_t id, uint32_t* out_val) {
  Instruction* def = ctx->get_def_use_mgr()->GetDef(id);
  if (!def) return false;
  const analysis::Constant* cst =
      ctx->get_constant_mgr()->GetConstantFromInst(def);
  if (!cst) return false;
  if (cst->AsNullConstant()) {
    const auto* int_ty = cst->type()->AsInteger();
    if (!int_ty || int_ty->width() != 32) return false;
    *out_val = 0;
    return true;
  }
  const auto* ic = cst->AsIntConstant();
  if (!ic) return false;
  const auto* int_ty = ic->type()->AsInteger();
  if (!int_ty || int_ty->width() != 32) return false;
  *out_val = ic->GetU32BitValue();
  return true;
}

uint32_t GetOrCreateUint32Constant(IRContext* ctx, uint32_t uint_type_id,
                                   uint32_t val) {
  auto* const_mgr = ctx->get_constant_mgr();
  const analysis::Type* uint_ty = ctx->get_type_mgr()->GetType(uint_type_id);
  if (!uint_ty) return 0;
  const analysis::Constant* cst =
      const_mgr->GetConstant(uint_ty, {val});
  if (!cst) return 0;
  Instruction* def = const_mgr->GetDefiningInstruction(cst, uint_type_id);
  return def ? def->result_id() : 0;
}

spv::Op InvertIntCmpOp(spv::Op op) {
  switch (op) {
    case spv::Op::OpIEqual:
      return spv::Op::OpINotEqual;
    case spv::Op::OpINotEqual:
      return spv::Op::OpIEqual;
    case spv::Op::OpSLessThan:
      return spv::Op::OpSGreaterThanEqual;
    case spv::Op::OpSGreaterThanEqual:
      return spv::Op::OpSLessThan;
    case spv::Op::OpSLessThanEqual:
      return spv::Op::OpSGreaterThan;
    case spv::Op::OpSGreaterThan:
      return spv::Op::OpSLessThanEqual;
    case spv::Op::OpULessThan:
      return spv::Op::OpUGreaterThanEqual;
    case spv::Op::OpUGreaterThanEqual:
      return spv::Op::OpULessThan;
    case spv::Op::OpULessThanEqual:
      return spv::Op::OpUGreaterThan;
    case spv::Op::OpUGreaterThan:
      return spv::Op::OpULessThanEqual;
    default:
      return spv::Op::OpNop;
  }
}

// If |cond_id| is trivially invertible (OpLogicalNot or single-use integer cmp),
// returns the inverted boolean ID (inserting before |insert_before| if needed).
// Returns 0 otherwise.
uint32_t TryGetFreeInvertedBool(IRContext* ctx, uint32_t cond_id,
                                uint32_t bool_type_id,
                                Instruction* insert_before) {
  auto* def_use = ctx->get_def_use_mgr();
  Instruction* def = def_use->GetDef(cond_id);
  if (!def) return 0;
  if (def->opcode() == spv::Op::OpLogicalNot) {
    return def->GetSingleWordInOperand(0);
  }
  spv::Op inv_op = InvertIntCmpOp(def->opcode());
  if (inv_op != spv::Op::OpNop && def_use->NumUses(cond_id) == 1) {
    InstructionBuilder ir_builder(
        ctx, insert_before,
        IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping);
    Instruction* inv_inst = ir_builder.AddBinaryOp(
        bool_type_id, inv_op, def->GetSingleWordInOperand(0),
        def->GetSingleWordInOperand(1));
    return inv_inst ? inv_inst->result_id() : 0;
  }
  return 0;
}

bool IsPowerOf2U32(uint32_t v) { return v != 0 && (v & (v - 1u)) == 0; }

// Matches signed non-negative lower bound: x >= 0 (or x > -1, 0 <= x, -1 < x).
bool MatchSignedGEZero(IRContext* ctx, Instruction* inst, uint32_t* out_x) {
  if (!inst) return false;
  spv::Op op = inst->opcode();
  if (op != spv::Op::OpSGreaterThanEqual && op != spv::Op::OpSGreaterThan &&
      op != spv::Op::OpSLessThanEqual && op != spv::Op::OpSLessThan) {
    return false;
  }
  uint32_t lhs = inst->GetSingleWordInOperand(0);
  uint32_t rhs = inst->GetSingleWordInOperand(1);
  int32_t c = 0;
  if (op == spv::Op::OpSGreaterThanEqual && GetInt32Const(ctx, rhs, &c) &&
      c == 0) {
    *out_x = lhs;
    return true;
  }
  if (op == spv::Op::OpSGreaterThan && GetInt32Const(ctx, rhs, &c) &&
      c == -1) {
    *out_x = lhs;
    return true;
  }
  if (op == spv::Op::OpSLessThanEqual && GetInt32Const(ctx, lhs, &c) &&
      c == 0) {
    *out_x = rhs;
    return true;
  }
  if (op == spv::Op::OpSLessThan && GetInt32Const(ctx, lhs, &c) && c == -1) {
    *out_x = rhs;
    return true;
  }
  return false;
}

// Matches signed negative check: x < 0 (or x <= -1, 0 > x, -1 >= x).
bool MatchSignedLTZero(IRContext* ctx, Instruction* inst, uint32_t* out_x) {
  if (!inst) return false;
  spv::Op op = inst->opcode();
  if (op != spv::Op::OpSLessThan && op != spv::Op::OpSLessThanEqual &&
      op != spv::Op::OpSGreaterThan && op != spv::Op::OpSGreaterThanEqual) {
    return false;
  }
  uint32_t lhs = inst->GetSingleWordInOperand(0);
  uint32_t rhs = inst->GetSingleWordInOperand(1);
  int32_t c = 0;
  if (op == spv::Op::OpSLessThan && GetInt32Const(ctx, rhs, &c) && c == 0) {
    *out_x = lhs;
    return true;
  }
  if (op == spv::Op::OpSLessThanEqual && GetInt32Const(ctx, rhs, &c) &&
      c == -1) {
    *out_x = lhs;
    return true;
  }
  if (op == spv::Op::OpSGreaterThan && GetInt32Const(ctx, lhs, &c) &&
      c == 0) {
    *out_x = rhs;
    return true;
  }
  if (op == spv::Op::OpSGreaterThanEqual && GetInt32Const(ctx, lhs, &c) &&
      c == -1) {
    *out_x = rhs;
    return true;
  }
  return false;
}

// Matches signed upper bound with constant N >= 0:
// if |inclusive| is false: x < N; if true: x <= N.
bool MatchSignedUpperConst(IRContext* ctx, Instruction* inst, uint32_t* out_x,
                           uint32_t* out_n, bool* out_inclusive) {
  if (!inst) return false;
  spv::Op op = inst->opcode();
  uint32_t lhs = 0, rhs = 0;
  int32_t c = 0;
  if (op == spv::Op::OpSLessThan || op == spv::Op::OpSLessThanEqual) {
    lhs = inst->GetSingleWordInOperand(0);
    rhs = inst->GetSingleWordInOperand(1);
    if (!GetInt32Const(ctx, rhs, &c) || c < 0) return false;
    *out_x = lhs;
    *out_n = static_cast<uint32_t>(c);
    *out_inclusive = (op == spv::Op::OpSLessThanEqual);
    return true;
  }
  if (op == spv::Op::OpSGreaterThan || op == spv::Op::OpSGreaterThanEqual) {
    lhs = inst->GetSingleWordInOperand(0);
    rhs = inst->GetSingleWordInOperand(1);
    if (!GetInt32Const(ctx, lhs, &c) || c < 0) return false;
    *out_x = rhs;
    *out_n = static_cast<uint32_t>(c);
    *out_inclusive = (op == spv::Op::OpSGreaterThanEqual);
    return true;
  }
  return false;
}

// Matches signed out-of-range upper check with constant N >= 0:
// if |strict| is false: x >= N; if true: x > N.
bool MatchSignedOutOfRangeUpperConst(IRContext* ctx, Instruction* inst,
                                     uint32_t* out_x, uint32_t* out_n,
                                     bool* out_strict) {
  if (!inst) return false;
  spv::Op op = inst->opcode();
  uint32_t lhs = 0, rhs = 0;
  int32_t c = 0;
  if (op == spv::Op::OpSGreaterThanEqual || op == spv::Op::OpSGreaterThan) {
    lhs = inst->GetSingleWordInOperand(0);
    rhs = inst->GetSingleWordInOperand(1);
    if (!GetInt32Const(ctx, rhs, &c) || c < 0) return false;
    *out_x = lhs;
    *out_n = static_cast<uint32_t>(c);
    *out_strict = (op == spv::Op::OpSGreaterThan);
    return true;
  }
  if (op == spv::Op::OpSLessThanEqual || op == spv::Op::OpSLessThan) {
    lhs = inst->GetSingleWordInOperand(0);
    rhs = inst->GetSingleWordInOperand(1);
    if (!GetInt32Const(ctx, lhs, &c) || c < 0) return false;
    *out_x = rhs;
    *out_n = static_cast<uint32_t>(c);
    *out_strict = (op == spv::Op::OpSLessThan);
    return true;
  }
  return false;
}

void CollectLogicalLeaves(IRContext* ctx, BasicBlock* bb, spv::Op logic_op,
                          uint32_t id, std::vector<uint32_t>& leaves) {
  Instruction* def = ctx->get_def_use_mgr()->GetDef(id);
  if (def && def->opcode() == logic_op && ctx->get_instr_block(def) == bb) {
    CollectLogicalLeaves(ctx, bb, logic_op, def->GetSingleWordInOperand(0),
                         leaves);
    CollectLogicalLeaves(ctx, bb, logic_op, def->GetSingleWordInOperand(1),
                         leaves);
    return;
  }
  leaves.push_back(id);
}

bool IsRootLogicalOp(IRContext* ctx, BasicBlock* bb, Instruction* inst,
                     spv::Op logic_op) {
  bool has_same_op_user_in_bb = false;
  bool has_other_user = false;
  ctx->get_def_use_mgr()->ForEachUser(
      inst, [&](Instruction* user) {
        if (user->opcode() == logic_op && ctx->get_instr_block(user) == bb) {
          has_same_op_user_in_bb = true;
        } else {
          has_other_user = true;
        }
      });
  return !has_same_op_user_in_bb || has_other_user;
}

}  // namespace

Pass::Status BooleanSimplifyPass::Process() {
  analysis::Bool bool_ty;
  uint32_t bool_type_id = context()->get_type_mgr()->GetId(&bool_ty);
  if (bool_type_id == 0) {
    return Status::SuccessWithoutChange;
  }
  analysis::Integer uint32_ty(32, false);
  uint32_t uint32_type_id =
      context()->get_type_mgr()->GetTypeInstruction(&uint32_ty);
  if (uint32_type_id == 0) {
    return Status::SuccessWithoutChange;
  }

  bool modified = false;
  auto* def_use = context()->get_def_use_mgr();

  // Phase 1: Canonicalize scalar OpSelect %bool -> OpLogicalAnd / OpLogicalOr.
  for (auto& func : *get_module()) {
    if (func.IsDeclaration()) continue;
    bool local_changed = true;
    while (local_changed) {
      local_changed = false;
      for (auto& bb : func) {
        for (auto it = bb.begin(); it != bb.end();) {
          Instruction* inst = &*it;
          auto next_it = std::next(it);
          if (inst->opcode() != spv::Op::OpSelect ||
              inst->type_id() != bool_type_id) {
            it = next_it;
            continue;
          }
          uint32_t cond_id = inst->GetSingleWordInOperand(0);
          uint32_t t_id = inst->GetSingleWordInOperand(1);
          uint32_t f_id = inst->GetSingleWordInOperand(2);
          BoolConst t_c = GetBoolConst(context(), t_id);
          BoolConst f_c = GetBoolConst(context(), f_id);

          spv::Op new_op = spv::Op::OpNop;
          uint32_t op_a = 0, op_b = 0;

          if (t_c == BoolConst::kTrue && f_c == BoolConst::kFalse) {
            context()->ReplaceAllUsesWith(inst->result_id(), cond_id);
            context()->KillInst(inst);
            modified = local_changed = true;
            it = next_it;
            continue;
          }
          if (t_c == BoolConst::kFalse && f_c == BoolConst::kTrue) {
            InstructionBuilder ir_builder(
                context(), inst,
                IRContext::kAnalysisDefUse |
                    IRContext::kAnalysisInstrToBlockMapping);
            Instruction* not_inst =
                ir_builder.AddUnaryOp(bool_type_id, spv::Op::OpLogicalNot, cond_id);
            if (not_inst) {
              context()->ReplaceAllUsesWith(inst->result_id(),
                                            not_inst->result_id());
              context()->KillInst(inst);
              modified = local_changed = true;
            }
            it = next_it;
            continue;
          }
          if (f_c == BoolConst::kFalse) {
            new_op = spv::Op::OpLogicalAnd;
            op_a = cond_id;
            op_b = t_id;
          } else if (t_c == BoolConst::kTrue) {
            new_op = spv::Op::OpLogicalOr;
            op_a = cond_id;
            op_b = f_id;
          } else if (f_c == BoolConst::kTrue) {
            uint32_t inv_c = TryGetFreeInvertedBool(context(), cond_id,
                                                    bool_type_id, inst);
            if (inv_c != 0) {
              new_op = spv::Op::OpLogicalOr;
              op_a = inv_c;
              op_b = t_id;
            }
          } else if (t_c == BoolConst::kFalse) {
            uint32_t inv_c = TryGetFreeInvertedBool(context(), cond_id,
                                                    bool_type_id, inst);
            if (inv_c != 0) {
              new_op = spv::Op::OpLogicalAnd;
              op_a = inv_c;
              op_b = f_id;
            }
          }

          if (new_op != spv::Op::OpNop) {
            InstructionBuilder ir_builder(
                context(), inst,
                IRContext::kAnalysisDefUse |
                    IRContext::kAnalysisInstrToBlockMapping);
            Instruction* logic_inst =
                ir_builder.AddBinaryOp(bool_type_id, new_op, op_a, op_b);
            if (logic_inst) {
              context()->ReplaceAllUsesWith(inst->result_id(),
                                            logic_inst->result_id());
              context()->KillInst(inst);
              modified = local_changed = true;
            }
          }
          it = next_it;
        }
      }
    }
  }

  // Phase 2: Signed range check folding & power-of-2 unsigned range merges.
  for (auto& func : *get_module()) {
    if (func.IsDeclaration()) continue;
    for (auto& bb : func) {
      std::vector<Instruction*> roots;
      for (auto& inst : bb) {
        if ((inst.opcode() == spv::Op::OpLogicalAnd ||
             inst.opcode() == spv::Op::OpLogicalOr) &&
            inst.type_id() == bool_type_id &&
            IsRootLogicalOp(context(), &bb, &inst, inst.opcode())) {
          roots.push_back(&inst);
        }
      }

      for (Instruction* root : roots) {
        spv::Op logic_op = root->opcode();
        std::vector<uint32_t> leaves;
        CollectLogicalLeaves(context(), &bb, logic_op, root->result_id(),
                             leaves);
        if (leaves.size() < 2) continue;

        bool changed_root = false;
        InstructionBuilder ir_builder(
            context(), root,
            IRContext::kAnalysisDefUse |
                IRContext::kAnalysisInstrToBlockMapping);

        // Step 2a: Fold (x >= 0 && x < N) / (x < 0 || x >= N) -> unsigned cmp.
        if (logic_op == spv::Op::OpLogicalAnd) {
          std::vector<bool> used(leaves.size(), false);
          std::vector<uint32_t> new_leaves;
          for (size_t i = 0; i < leaves.size(); ++i) {
            if (used[i]) continue;
            Instruction* li = def_use->GetDef(leaves[i]);
            uint32_t x1 = 0;
            if (MatchSignedGEZero(context(), li, &x1)) {
              size_t match_j = leaves.size();
              uint32_t n_val = 0;
              bool inclusive = false;
              for (size_t j = 0; j < leaves.size(); ++j) {
                if (i == j || used[j]) continue;
                Instruction* lj = def_use->GetDef(leaves[j]);
                uint32_t x2 = 0;
                if (MatchSignedUpperConst(context(), lj, &x2, &n_val,
                                          &inclusive) &&
                    x1 == x2) {
                  match_j = j;
                  break;
                }
              }
              if (match_j < leaves.size()) {
                uint32_t u_x = 0;
                Instruction* x_def = def_use->GetDef(x1);
                if (x_def && x_def->opcode() == spv::Op::OpBitcast &&
                    x_def->GetSingleWordInOperand(0) != 0 &&
                    def_use->GetDef(x_def->GetSingleWordInOperand(0)) &&
                    def_use->GetDef(x_def->GetSingleWordInOperand(0))
                            ->type_id() == uint32_type_id) {
                  u_x = x_def->GetSingleWordInOperand(0);
                } else {
                  Instruction* bc = ir_builder.AddUnaryOp(
                      uint32_type_id, spv::Op::OpBitcast, x1);
                  u_x = bc ? bc->result_id() : 0;
                }
                uint32_t u_n =
                    GetOrCreateUint32Constant(context(), uint32_type_id, n_val);
                if (u_x != 0 && u_n != 0) {
                  Instruction* ucmp = ir_builder.AddBinaryOp(
                      bool_type_id,
                      inclusive ? spv::Op::OpULessThanEqual
                                : spv::Op::OpULessThan,
                      u_x, u_n);
                  if (ucmp) {
                    used[i] = true;
                    used[match_j] = true;
                    new_leaves.push_back(ucmp->result_id());
                    changed_root = true;
                    continue;
                  }
                }
              }
            }
          }
          if (changed_root) {
            for (size_t i = 0; i < leaves.size(); ++i) {
              if (!used[i]) new_leaves.push_back(leaves[i]);
            }
            leaves.swap(new_leaves);
          }
        } else {  // OpLogicalOr
          std::vector<bool> used(leaves.size(), false);
          std::vector<uint32_t> new_leaves;
          for (size_t i = 0; i < leaves.size(); ++i) {
            if (used[i]) continue;
            Instruction* li = def_use->GetDef(leaves[i]);
            uint32_t x1 = 0;
            if (MatchSignedLTZero(context(), li, &x1)) {
              size_t match_j = leaves.size();
              uint32_t n_val = 0;
              bool strict = false;
              for (size_t j = 0; j < leaves.size(); ++j) {
                if (i == j || used[j]) continue;
                Instruction* lj = def_use->GetDef(leaves[j]);
                uint32_t x2 = 0;
                if (MatchSignedOutOfRangeUpperConst(context(), lj, &x2, &n_val,
                                                    &strict) &&
                    x1 == x2) {
                  match_j = j;
                  break;
                }
              }
              if (match_j < leaves.size()) {
                uint32_t u_x = 0;
                Instruction* x_def = def_use->GetDef(x1);
                if (x_def && x_def->opcode() == spv::Op::OpBitcast &&
                    def_use->GetDef(x_def->GetSingleWordInOperand(0)) &&
                    def_use->GetDef(x_def->GetSingleWordInOperand(0))
                            ->type_id() == uint32_type_id) {
                  u_x = x_def->GetSingleWordInOperand(0);
                } else {
                  Instruction* bc = ir_builder.AddUnaryOp(
                      uint32_type_id, spv::Op::OpBitcast, x1);
                  u_x = bc ? bc->result_id() : 0;
                }
                uint32_t u_n =
                    GetOrCreateUint32Constant(context(), uint32_type_id, n_val);
                if (u_x != 0 && u_n != 0) {
                  Instruction* ucmp = ir_builder.AddBinaryOp(
                      bool_type_id,
                      strict ? spv::Op::OpUGreaterThan
                             : spv::Op::OpUGreaterThanEqual,
                      u_x, u_n);
                  if (ucmp) {
                    used[i] = true;
                    used[match_j] = true;
                    new_leaves.push_back(ucmp->result_id());
                    changed_root = true;
                    continue;
                  }
                }
              }
            }
          }
          if (changed_root) {
            for (size_t i = 0; i < leaves.size(); ++i) {
              if (!used[i]) new_leaves.push_back(leaves[i]);
            }
            leaves.swap(new_leaves);
          }
        }

        // Step 2b: Merge unsigned power-of-2 bounds with same bound P:
        // (u0 < P && u1 < P) -> ((u0 | u1) < P)
        // (u0 >= P || u1 >= P) -> ((u0 | u1) >= P)
        const spv::Op target_ucmp =
            (logic_op == spv::Op::OpLogicalAnd)
                ? spv::Op::OpULessThan
                : spv::Op::OpUGreaterThanEqual;
        std::unordered_map<uint32_t, std::vector<size_t>> pow2_groups;
        for (size_t i = 0; i < leaves.size(); ++i) {
          Instruction* li = def_use->GetDef(leaves[i]);
          if (!li || li->opcode() != target_ucmp) continue;
          uint32_t lhs_id = li->GetSingleWordInOperand(0);
          uint32_t rhs_id = li->GetSingleWordInOperand(1);
          Instruction* lhs_def = def_use->GetDef(lhs_id);
          if (!lhs_def || lhs_def->type_id() != uint32_type_id) continue;
          uint32_t p_val = 0;
          if (!GetUint32Const(context(), rhs_id, &p_val) ||
              !IsPowerOf2U32(p_val)) {
            continue;
          }
          pow2_groups[rhs_id].push_back(i);
        }

        bool merged_pow2 = false;
        std::vector<bool> merged_idx(leaves.size(), false);
        std::vector<uint32_t> merged_leaves;
        for (const auto& kv : pow2_groups) {
          if (kv.second.size() < 2) continue;
          uint32_t p_const_id = kv.first;
          uint32_t acc =
              def_use->GetDef(leaves[kv.second[0]])->GetSingleWordInOperand(0);
          for (size_t k = 1; k < kv.second.size(); ++k) {
            uint32_t nxt =
                def_use->GetDef(leaves[kv.second[k]])->GetSingleWordInOperand(0);
            Instruction* or_inst = ir_builder.AddBinaryOp(
                uint32_type_id, spv::Op::OpBitwiseOr, acc, nxt);
            if (!or_inst) {
              acc = 0;
              break;
            }
            acc = or_inst->result_id();
          }
          if (acc != 0) {
            Instruction* cmp_inst = ir_builder.AddBinaryOp(
                bool_type_id, target_ucmp, acc, p_const_id);
            if (cmp_inst) {
              for (size_t idx : kv.second) merged_idx[idx] = true;
              merged_leaves.push_back(cmp_inst->result_id());
              merged_pow2 = true;
            }
          }
        }
        if (merged_pow2) {
          for (size_t i = 0; i < leaves.size(); ++i) {
            if (!merged_idx[i]) merged_leaves.push_back(leaves[i]);
          }
          leaves.swap(merged_leaves);
          changed_root = true;
        }

        if (!changed_root) continue;

        uint32_t rebuilt_id = leaves[0];
        for (size_t i = 1; i < leaves.size(); ++i) {
          Instruction* logic_inst = ir_builder.AddBinaryOp(
              bool_type_id, logic_op, rebuilt_id, leaves[i]);
          if (!logic_inst) {
            rebuilt_id = 0;
            break;
          }
          rebuilt_id = logic_inst->result_id();
        }
        if (rebuilt_id != 0 && rebuilt_id != root->result_id()) {
          context()->ReplaceAllUsesWith(root->result_id(), rebuilt_id);
          context()->KillInst(root);
          modified = true;
        }
      }
    }
  }

  return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
}

}  // namespace opt
}  // namespace spvtools
