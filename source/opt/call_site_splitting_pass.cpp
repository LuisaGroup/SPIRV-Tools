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

#include "source/opt/call_site_splitting_pass.h"

#include <vector>

#include "source/opt/dominator_analysis.h"
#include "source/opt/ir_context.h"

namespace spvtools {
namespace opt {

Pass::Status CallSiteSplittingPass::Process() {
  bool modified = false;

  for (auto& func : *get_module()) {
    if (func.IsDeclaration()) {
      continue;
    }

    DominatorAnalysis* dom = context()->GetDominatorAnalysis(&func);
    if (!dom) continue;

    // Scan all instructions in the function for function calls.
    for (auto& bb : func) {
      for (auto& inst : bb) {
        if (inst.opcode() == spv::Op::OpFunctionCall) {
          if (ProcessCallSite(&inst, dom)) {
            modified = true;
          }
        }
      }
    }
  }

  return (modified ? Status::SuccessWithChange : Status::SuccessWithoutChange);
}

bool CallSiteSplittingPass::ProcessCallSite(Instruction* callInst,
                                             DominatorAnalysis* dom) {
  // Get the block containing the call.
  BasicBlock* callBlock = context()->get_instr_block(callInst);
  if (!callBlock) return false;

  bool modified = false;

  // Build a map: for each argument id in the call, what constant id should
  // replace it (if any) based on dominating conditions.
  // Key: argument id, Value: constant id to replace with.
  std::vector<std::pair<uint32_t, uint32_t>> replacements;

  // Get the predecessors of this block.
  const std::vector<uint32_t>& preds = cfg()->preds(callBlock->id());

  for (uint32_t predId : preds) {
    BasicBlock* predBB = cfg()->block(predId);
    if (!predBB) continue;

    // The predecessor's terminator must be OpBranchConditional.
    Instruction* term = predBB->terminator();
    if (term->opcode() != spv::Op::OpBranchConditional) continue;

    // Get the condition.
    uint32_t condId = term->GetSingleWordInOperand(0);
    Instruction* condDef = get_def_use_mgr()->GetDef(condId);
    if (!condDef) continue;

    // The condition must be a comparison.
    spv::Op condOp = condDef->opcode();
    bool isIEqual = false;

    switch (condOp) {
      case spv::Op::OpIEqual:
        isIEqual = true;
        break;
      case spv::Op::OpINotEqual:
        isIEqual = false;
        break;
      case spv::Op::OpLogicalEqual:
        isIEqual = true;
        break;
      case spv::Op::OpLogicalNotEqual:
        isIEqual = false;
        break;
      default:
        continue;
    }

    uint32_t cmpOp0 = condDef->GetSingleWordInOperand(0);
    uint32_t cmpOp1 = condDef->GetSingleWordInOperand(1);

    // Determine which edge goes to the call block.
    uint32_t trueEdgeBB = term->GetSingleWordInOperand(1);
    uint32_t falseEdgeBB = term->GetSingleWordInOperand(2);

    // Check if the edge from predBB to callBlock corresponds to the "true" or
    // "false" outcome.
    bool isTrueEdge;
    if (trueEdgeBB == callBlock->id()) {
      isTrueEdge = true;
    } else if (falseEdgeBB == callBlock->id()) {
      isTrueEdge = false;
    } else {
      continue;  // Call is not in either successor.
    }

    // Check if any of the call's arguments match the comparison operands.
    // callInst operands: [result_type, result_id, function_id, arg0, arg1, ...]
    // In operands for OpFunctionCall: [function_id, arg0, arg1, ...]
    // Skip operand 0 (function_id), scan from operand 1 onwards.
    for (uint32_t i = 1; i < callInst->NumInOperands(); ++i) {
      uint32_t argId = callInst->GetSingleWordInOperand(i);

      uint32_t otherId;
      if (cmpOp0 == argId) {
        otherId = cmpOp1;
      } else if (cmpOp1 == argId) {
        otherId = cmpOp0;
      } else {
        continue;  // This argument is not involved in the comparison.
      }

      // Determine the relationship between arg and other on this edge.
      // On true edge with OpIEqual: arg == other
      // On true edge with OpINotEqual: arg != other
      // On false edge with OpIEqual: arg != other
      // On false edge with OpINotEqual: arg == other
      bool argEqualsOther = (isTrueEdge == isIEqual);

      if (!argEqualsOther) continue;

      // Check that the other operand is a constant.
      Instruction* otherDef = get_def_use_mgr()->GetDef(otherId);
      if (!otherDef) continue;

      // Check if other is a constant.
      bool isConstant = false;
      switch (otherDef->opcode()) {
        case spv::Op::OpConstant:
        case spv::Op::OpConstantTrue:
        case spv::Op::OpConstantFalse:
        case spv::Op::OpConstantNull:
        case spv::Op::OpConstantComposite:
        case spv::Op::OpConstantSampler:
          isConstant = true;
          break;
        default:
          break;
      }

      if (!isConstant) continue;

      // The constant must dominate the call instruction (always true for
      // module-level constants).
      if (!dom->Dominates(otherDef, callInst)) {
        continue;
      }

      // Record this replacement: argId -> otherId.
      replacements.push_back({argId, otherId});
    }
  }

  // Apply the replacements within the call block.
  if (!replacements.empty()) {
    for (auto& bb_inst : *callBlock) {
      for (auto& rep : replacements) {
        uint32_t argId = rep.first;
        uint32_t constId = rep.second;

        // Check each in-operand of this instruction.
        for (uint32_t opIdx = 0; opIdx < bb_inst.NumInOperands(); ++opIdx) {
          if (bb_inst.GetSingleWordInOperand(opIdx) == argId) {
            bb_inst.SetInOperand(opIdx, {constId});
            modified = true;
          }
        }
      }
    }
  }

  return modified;
}

}  // namespace opt
}  // namespace spvtools
