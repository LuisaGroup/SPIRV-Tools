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
  // Gather facts from the terminator.
  Instruction* terminator = bb->terminator();
  if (terminator->opcode() == spv::Op::OpBranchConditional) {
    // The condition is operand 0.
    uint32_t condId = terminator->GetSingleWordInOperand(0);
    Instruction* condInst = get_def_use_mgr()->GetDef(condId);
    if (condInst) {
      if (condInst->opcode() == spv::Op::OpIEqual ||
          condInst->opcode() == spv::Op::OpLogicalEqual) {
        AddFact(facts, condInst->GetSingleWordInOperand(0),
                condInst->GetSingleWordInOperand(1), true);
      } else if (condInst->opcode() == spv::Op::OpINotEqual ||
                 condInst->opcode() == spv::Op::OpLogicalNotEqual) {
        AddFact(facts, condInst->GetSingleWordInOperand(0),
                condInst->GetSingleWordInOperand(1), false);
      }
    }
  }

  // Eliminate comparisons in this block.
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

  // Recurse into dominator tree children with a copy of the facts.
  DominatorTreeNode* node = dom->GetDomTree().GetTreeNode(bb);
  if (!node) return;

  for (DominatorTreeNode* child : node->children_) {
    FactMapWithHash childFacts = facts;
    ProcessBlock(child->bb_, childFacts, dom);
  }
}

}  // namespace opt
}  // namespace spvtools
