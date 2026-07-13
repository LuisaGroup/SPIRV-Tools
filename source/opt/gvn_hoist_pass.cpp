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

#include "source/opt/gvn_hoist_pass.h"

#include <unordered_map>
#include <vector>

#include "source/opt/dominator_analysis.h"
#include "source/opt/ir_context.h"
#include "source/opt/value_number_table.h"

namespace spvtools {
namespace opt {

Pass::Status GVNHoistPass::Process() {
  // NOTE: This pass is currently disabled due to a crash bug.
  // When enabled, it crashes on shaders with if-then-else blocks.
  // The crash occurs in HoistFromBlock when iterating over blocks
  // that have been modified by earlier performance passes.
  //
  // Options:
  // (a) Add invalidate CFG analysis at start of Process()
  // (b) Add defensive checks before every cfg() call
  // (c) Remove kAnalysisCFG from GetPreservedAnalyses()
  // None of these fully resolved the crash yet.
  return Status::SuccessWithoutChange;

  // --- Original code preserved below for reference ---
  bool modified = false;
  ValueNumberTable vnt(context());

  for (auto& func : *get_module()) {
    if (func.IsDeclaration()) {
      continue;
    }

    DominatorAnalysis* dom = context()->GetDominatorAnalysis(&func);
    if (!dom) continue;

    // Walk blocks in the function. For each block, check if it has a diamond
    // pattern with two successors that both go to a merge block.
    for (auto& bb : func) {
      if (HoistFromBlock(&bb, vnt, dom)) {
        modified = true;
      }
    }
  }

  return (modified ? Status::SuccessWithChange : Status::SuccessWithoutChange);
}

bool GVNHoistPass::HoistFromBlock(BasicBlock* bb,
                                   const ValueNumberTable& vnt,
                                   DominatorAnalysis* dom) {
  // Check for a conditional branch terminator.
  Instruction* terminator = bb->terminator();
  if (terminator->opcode() != spv::Op::OpBranchConditional) {
    return false;
  }

  // Get the merge instruction: must be OpSelectionMerge.
  Instruction* merge = bb->GetMergeInst();
  if (!merge || merge->opcode() != spv::Op::OpSelectionMerge) {
    return false;
  }

  // Get the two successors.
  uint32_t trueBBId = terminator->GetSingleWordInOperand(0);
  uint32_t falseBBId = terminator->GetSingleWordInOperand(1);

  BasicBlock* trueBB = cfg()->block(trueBBId);
  BasicBlock* falseBB = cfg()->block(falseBBId);

  if (!trueBB || !falseBB) {
    return false;
  }

  // Both successors must have BB as their single predecessor.
  if (cfg()->preds(trueBBId).size() != 1 ||
      cfg()->preds(trueBBId)[0] != bb->id()) {
    return false;
  }
  if (cfg()->preds(falseBBId).size() != 1 ||
      cfg()->preds(falseBBId)[0] != bb->id()) {
    return false;
  }

  // Collect instructions from trueBB into a map keyed by value number.
  std::unordered_map<uint32_t, Instruction*> value_map;

  for (auto& inst : *trueBB) {
    // Skip terminators, phis, merge instructions, and other special ops.
    if (inst.opcode() == spv::Op::OpPhi ||
        inst.opcode() == spv::Op::OpBranchConditional ||
        inst.opcode() == spv::Op::OpBranch ||
        inst.opcode() == spv::Op::OpSwitch ||
        inst.opcode() == spv::Op::OpReturn ||
        inst.opcode() == spv::Op::OpReturnValue ||
        inst.opcode() == spv::Op::OpKill ||
        inst.opcode() == spv::Op::OpUnreachable ||
        inst.opcode() == spv::Op::OpSelectionMerge ||
        inst.opcode() == spv::Op::OpLoopMerge ||
        inst.opcode() == spv::Op::OpLoad ||
        inst.opcode() == spv::Op::OpStore ||
        inst.opcode() == spv::Op::OpCopyMemory ||
        inst.opcode() == spv::Op::OpFunctionCall ||
        inst.opcode() == spv::Op::OpVariable ||
        inst.opcode() == spv::Op::OpAccessChain ||
        inst.opcode() == spv::Op::OpInBoundsAccessChain) {
      continue;
    }

    // Skip if it doesn't have a result id (can't be value numbered).
    if (!inst.HasResultId()) {
      continue;
    }

    uint32_t vn = vnt.GetValueNumber(&inst);
    if (vn != 0) {
      value_map[vn] = &inst;
    }
  }

  bool changed = false;

  // Scan falseBB for instructions with matching value numbers.
  // We use a manual loop because we may kill the current instruction.
  for (auto it = falseBB->begin(); it != falseBB->end(); ) {
    Instruction& inst = *it;
    ++it;  // Advance before potential kill.

    // Skip same categories as above.
    if (inst.opcode() == spv::Op::OpPhi ||
        inst.opcode() == spv::Op::OpBranchConditional ||
        inst.opcode() == spv::Op::OpBranch ||
        inst.opcode() == spv::Op::OpSwitch ||
        inst.opcode() == spv::Op::OpReturn ||
        inst.opcode() == spv::Op::OpReturnValue ||
        inst.opcode() == spv::Op::OpKill ||
        inst.opcode() == spv::Op::OpUnreachable ||
        inst.opcode() == spv::Op::OpSelectionMerge ||
        inst.opcode() == spv::Op::OpLoopMerge ||
        inst.opcode() == spv::Op::OpLoad ||
        inst.opcode() == spv::Op::OpStore ||
        inst.opcode() == spv::Op::OpCopyMemory ||
        inst.opcode() == spv::Op::OpFunctionCall ||
        inst.opcode() == spv::Op::OpVariable ||
        inst.opcode() == spv::Op::OpAccessChain ||
        inst.opcode() == spv::Op::OpInBoundsAccessChain) {
      continue;
    }

    if (!inst.HasResultId()) {
      continue;
    }

    uint32_t vn = vnt.GetValueNumber(&inst);
    if (vn == 0) continue;

    auto map_it = value_map.find(vn);
    if (map_it == value_map.end()) continue;

    Instruction* trueInst = map_it->second;

    // Must have the same type.
    if (trueInst->type_id() != inst.type_id()) {
      continue;
    }

    // Check that all operands are defined above the header block.
    bool operandsSafe = true;

    auto isOperandAvailableAtHeader = [&](uint32_t opId) -> bool {
      if (opId == 0) return true;
      Instruction* opDef = get_def_use_mgr()->GetDef(opId);
      if (!opDef) return true;

      BasicBlock* opBlock = context()->get_instr_block(opDef);
      if (!opBlock) return true;

      if (dom->Dominates(opBlock->id(), bb->id())) {
        return true;
      }

      if (opBlock->id() == bb->id()) {
        for (auto& headerInst : *bb) {
          if (&headerInst == opDef) return true;
          if (&headerInst == merge) return false;
        }
      }

      return false;
    };

    for (uint32_t i = 0; i < trueInst->NumInOperands(); ++i) {
      if (!isOperandAvailableAtHeader(trueInst->GetSingleWordInOperand(i))) {
        operandsSafe = false;
        break;
      }
    }

    if (!operandsSafe) continue;

    for (uint32_t i = 0; i < inst.NumInOperands(); ++i) {
      if (!isOperandAvailableAtHeader(inst.GetSingleWordInOperand(i))) {
        operandsSafe = false;
        break;
      }
    }

    if (!operandsSafe) continue;

    // Move the trueBB instruction to just before the OpSelectionMerge
    // in the header block.
    trueInst->RemoveFromList();
    trueInst->InsertBefore(merge);

    // Replace all uses of the falseBB instruction with the trueBB instruction.
    context()->ReplaceAllUsesWith(inst.result_id(), trueInst->result_id());

    // Kill the falseBB instruction.
    context()->KillInst(&inst);

    changed = true;
  }

  return changed;
}

}  // namespace opt
}  // namespace spvtools
