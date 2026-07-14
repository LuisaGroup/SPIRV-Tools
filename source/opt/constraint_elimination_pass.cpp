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

#include "source/opt/constraint_elimination_pass.h"

#include <unordered_map>
#include <utility>
#include <vector>

#include "source/opt/constants.h"
#include "source/opt/dominator_analysis.h"
#include "source/opt/dominator_tree.h"
#include "source/opt/ir_context.h"
#include "source/opt/type_manager.h"
#include "source/opt/types.h"

namespace spvtools {
namespace opt {

Pass::Status ConstraintEliminationPass::Process() {
  modified_ = false;
  bool_type_id_ = 0;

  // Find the bool type id from the module.
  for (auto& inst : get_module()->types_values()) {
    if (inst.opcode() == spv::Op::OpTypeBool) {
      bool_type_id_ = inst.result_id();
      break;
    }
  }

  if (bool_type_id_ == 0) {
    // No bool type in module, nothing to do.
    return Status::SuccessWithoutChange;
  }

  for (auto& func : *get_module()) {
    if (func.IsDeclaration()) {
      continue;
    }

    DominatorAnalysis* dom = context()->GetDominatorAnalysis(&func);
    if (!dom) continue;

    FactMapWithHash facts;
    ProcessBlock(func.entry().get(), facts, dom);
    // Count modified by checking if any comparisons were eliminated.
    // We tracked via eliminated count in the block processing.
  }

  return (modified_ ? Status::SuccessWithChange : Status::SuccessWithoutChange);
}

void ConstraintEliminationPass::AddFact(FactMapWithHash& facts, uint32_t aId,
                                        uint32_t bId, bool isEqual) {
  facts[{aId, bId}] = isEqual;
  facts[{bId, aId}] = isEqual;
}

bool ConstraintEliminationPass::IsKnownTrue(FactMapWithHash& facts,
                                            uint32_t aId, uint32_t bId) {
  if (aId == bId) return true;
  auto it = facts.find({aId, bId});
  if (it != facts.end() && it->second) return true;
  return false;
}

bool ConstraintEliminationPass::IsKnownFalse(FactMapWithHash& facts,
                                             uint32_t aId, uint32_t bId) {
  auto it = facts.find({aId, bId});
  if (it != facts.end() && !it->second) return true;
  return false;
}

bool ConstraintEliminationPass::EliminateCmp(Instruction* cmpInst,
                                              FactMapWithHash& facts) {
  uint32_t op0 = cmpInst->GetSingleWordInOperand(0);
  uint32_t op1 = cmpInst->GetSingleWordInOperand(1);

  bool result = false;
  bool known = false;

  switch (cmpInst->opcode()) {
    case spv::Op::OpIEqual:
    case spv::Op::OpLogicalEqual:
      if (IsKnownTrue(facts, op0, op1)) {
        result = true;
        known = true;
      } else if (IsKnownFalse(facts, op0, op1)) {
        result = false;
        known = true;
      }
      break;
    case spv::Op::OpINotEqual:
    case spv::Op::OpLogicalNotEqual:
      if (IsKnownFalse(facts, op0, op1)) {
        result = true;
        known = true;
      } else if (IsKnownTrue(facts, op0, op1)) {
        result = false;
        known = true;
      }
      break;
    case spv::Op::OpULessThanEqual:
    case spv::Op::OpSLessThanEqual:
    case spv::Op::OpUGreaterThanEqual:
    case spv::Op::OpSGreaterThanEqual:
      if (op0 == op1) {
        result = true;
        known = true;
      }
      break;
    case spv::Op::OpULessThan:
    case spv::Op::OpSLessThan:
    case spv::Op::OpUGreaterThan:
    case spv::Op::OpSGreaterThan:
      if (op0 == op1) {
        result = false;
        known = true;
      }
      break;
    default:
      break;
  }

  if (known) {
    // Get or create the bool constant.
    analysis::ConstantManager* const_mgr = context()->get_constant_mgr();
    analysis::TypeManager* type_mgr = context()->get_type_mgr();

    analysis::Bool bool_type;
    analysis::Type* registered_type = type_mgr->GetRegisteredType(&bool_type);
    if (!registered_type) return false;

    const analysis::Constant* constant =
        const_mgr->GetConstant(registered_type, {result ? 1u : 0u});
    if (!constant) return false;

    Instruction* const_inst =
        const_mgr->GetDefiningInstruction(constant, bool_type_id_);
    if (!const_inst) return false;

    // Replace uses and kill the comparison.
    context()->ReplaceAllUsesWith(cmpInst->result_id(), const_inst->result_id());
    context()->KillInst(cmpInst);
    modified_ = true;
    return true;
  }

  return false;
}

void ConstraintEliminationPass::ProcessBlock(BasicBlock* bb,
                                              FactMapWithHash& facts,
                                              DominatorAnalysis* dom) {
  // Eliminate comparisons in this block using only the facts inherited from
  // dominating blocks.
  for (auto it = bb->begin(); it != bb->end(); ) {
    Instruction& inst = *it;
    ++it;  // Advance before potential kill.

    switch (inst.opcode()) {
      case spv::Op::OpIEqual:
      case spv::Op::OpINotEqual:
      case spv::Op::OpLogicalEqual:
      case spv::Op::OpLogicalNotEqual:
      case spv::Op::OpULessThan:
      case spv::Op::OpSLessThan:
      case spv::Op::OpULessThanEqual:
      case spv::Op::OpSLessThanEqual:
      case spv::Op::OpUGreaterThan:
      case spv::Op::OpSGreaterThan:
      case spv::Op::OpUGreaterThanEqual:
      case spv::Op::OpSGreaterThanEqual:
        EliminateCmp(&inst, facts);
        break;
      default:
        break;
    }
  }

  // Gather edge facts from the terminator. A branch condition only holds on
  // the corresponding outgoing edge: it is true on the true edge and false on
  // the false edge. It must never be applied to this block itself.
  FactMapWithHash true_facts = facts;
  FactMapWithHash false_facts = facts;
  bool has_edge_facts = false;
  BasicBlock* true_succ = nullptr;
  BasicBlock* false_succ = nullptr;
  Instruction* terminator = bb->terminator();
  if (terminator->opcode() == spv::Op::OpBranchConditional) {
    // The condition is operand 0, the true label operand 1, the false label
    // operand 2.
    uint32_t condId = terminator->GetSingleWordInOperand(0);
    Instruction* condInst = get_def_use_mgr()->GetDef(condId);
    if (condInst) {
      bool is_equal = false;
      bool is_not_equal = false;
      if (condInst->opcode() == spv::Op::OpIEqual ||
          condInst->opcode() == spv::Op::OpLogicalEqual) {
        is_equal = true;
      } else if (condInst->opcode() == spv::Op::OpINotEqual ||
                 condInst->opcode() == spv::Op::OpLogicalNotEqual) {
        is_not_equal = true;
      }
      if (is_equal || is_not_equal) {
        uint32_t aId = condInst->GetSingleWordInOperand(0);
        uint32_t bId = condInst->GetSingleWordInOperand(1);
        // On the true edge the condition holds; on the false edge its
        // negation holds.
        AddFact(true_facts, aId, bId, is_equal);
        AddFact(false_facts, aId, bId, is_not_equal);
        true_succ = context()->get_instr_block(
            get_def_use_mgr()->GetDef(terminator->GetSingleWordInOperand(1)));
        false_succ = context()->get_instr_block(
            get_def_use_mgr()->GetDef(terminator->GetSingleWordInOperand(2)));
        // If both edges lead to the same block the facts contradict; drop
        // them.
        has_edge_facts = true_succ != nullptr && false_succ != nullptr &&
                         true_succ != false_succ;
      }
    }
  }

  // Recurse into dominator tree children, giving each child the facts of the
  // edge through which it is reached.
  DominatorTreeNode* node = dom->GetDomTree().GetTreeNode(bb);
  if (!node) return;

  for (DominatorTreeNode* child : node->children_) {
    FactMapWithHash childFacts = facts;
    if (has_edge_facts) {
      if (dom->Dominates(true_succ, child->bb_)) {
        childFacts = true_facts;
      } else if (dom->Dominates(false_succ, child->bb_)) {
        childFacts = false_facts;
      }
    }
    ProcessBlock(child->bb_, childFacts, dom);
  }
}

}  // namespace opt
}  // namespace spvtools
