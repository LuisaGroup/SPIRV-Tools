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

#ifndef SOURCE_OPT_MEM_DEPENDENCE_UTIL_H_
#define SOURCE_OPT_MEM_DEPENDENCE_UTIL_H_

#include "source/opt/def_use_manager.h"
#include "source/opt/instruction.h"

namespace spvtools {
namespace opt {

// Returns true if |inst| may modify memory.
//
// Conservatively treats the following as memory modifiers:
//  - OpStore
//  - OpCopyMemory / OpCopyMemorySized
//  - All atomic instructions that store (OpAtomicStore, OpAtomicExchange,
//    OpAtomicCompareExchange, OpAtomicCompareExchangeWeak, OpAtomicIIncrement,
//    OpAtomicIDecrement, OpAtomicIAdd, OpAtomicISub, OpAtomicSMin, OpAtomicUMin,
//    OpAtomicSMax, OpAtomicUMax, OpAtomicAnd, OpAtomicOr, OpAtomicXor,
//    OpAtomicFAddEXT, OpAtomicFMinEXT, OpAtomicFMaxEXT,
//    OpAtomicFlagTestAndSet, OpAtomicFlagClear)
//  - OpFunctionCall (conservative — we can't see inside)
//  - OpControlBarrier / OpMemoryBarrier
//  - OpImageWrite
//
// OpAtomicLoad is NOT a modifier (it only reads).
inline bool IsMemoryModifier(const Instruction* inst) {
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
    case spv::Op::OpAtomicFAddEXT:
    case spv::Op::OpAtomicISub:
    case spv::Op::OpAtomicSMin:
    case spv::Op::OpAtomicUMin:
    case spv::Op::OpAtomicFMinEXT:
    case spv::Op::OpAtomicSMax:
    case spv::Op::OpAtomicUMax:
    case spv::Op::OpAtomicFMaxEXT:
    case spv::Op::OpAtomicAnd:
    case spv::Op::OpAtomicOr:
    case spv::Op::OpAtomicXor:
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

// Returns true if |ptrA| and |ptrB| are known to always refer to the same
// memory location (i.e., they must-alias).
//
// Two pointers must-alias if:
//   (1) They have the same result id (same instruction).
//   (2) They are both OpAccessChain or OpInBoundsAccessChain instructions with
//       identical base result id AND identical index result ids (all operands
//       match).
//
// Returns false otherwise (conservative: do not assume aliasing).
inline bool IsMustAlias(const Instruction* ptrA, const Instruction* ptrB,
                        analysis::DefUseManager* defUseMgr) {
  (void)defUseMgr;  // Reserved for future use (e.g., analyzing index constants).

  // Same instruction pointer or same result id => must alias.
  if (ptrA == ptrB) return true;
  if (ptrA->result_id() == ptrB->result_id()) return true;

  // Both must be access chain instructions.
  if (ptrA->opcode() != spv::Op::OpAccessChain &&
      ptrA->opcode() != spv::Op::OpInBoundsAccessChain) {
    return false;
  }
  if (ptrB->opcode() != spv::Op::OpAccessChain &&
      ptrB->opcode() != spv::Op::OpInBoundsAccessChain) {
    return false;
  }

  // Access chains: in-operand 0 = Base, in-operands 1..N = Indexes.
  // They must have the same number of in-operands.
  if (ptrA->NumInOperands() != ptrB->NumInOperands()) return false;

  // Base must match.
  if (ptrA->GetSingleWordInOperand(0) != ptrB->GetSingleWordInOperand(0))
    return false;

  // All index result ids must match.
  for (uint32_t i = 1; i < ptrA->NumInOperands(); ++i) {
    if (ptrA->GetSingleWordInOperand(i) != ptrB->GetSingleWordInOperand(i))
      return false;
  }

  return true;
}

// Returns true if |ptrInst| points to read-only memory in the Vulkan model.
//
// Delegates to Instruction::IsReadOnlyPointer().
inline bool IsReadOnlyPointer(const Instruction* ptrInst) {
  return ptrInst->IsReadOnlyPointer();
}

// Scans instructions in the same basic block from |from| (inclusive) to |to|
// (exclusive), and returns true if any instruction in that range modifies
// memory that |ptrInst| may alias.
//
// For each instruction |I| in the range:
//  - If IsMemoryModifier(I) is true and the pointer it modifies must-alias
//    with |ptrInst| (via IsMustAlias), returns true.
//  - If the modifying instruction's pointer cannot be determined (e.g.,
//    OpFunctionCall, barriers), returns true conservatively.
//
// |from| and |to| are assumed to be in the same basic block, with |from|
// appearing at or before |to| in the instruction list.
inline bool HasMemoryBarrierBetween(const Instruction* from,
                                    const Instruction* to,
                                    const Instruction* ptrInst,
                                    analysis::DefUseManager* defUseMgr) {
  for (const Instruction* curr = from; curr && curr != to;
       curr = curr->NextNode()) {
    if (!IsMemoryModifier(curr)) continue;

    // For function calls and barriers, we cannot determine the modified
    // pointer, so conservatively assume the worst.
    switch (curr->opcode()) {
      case spv::Op::OpFunctionCall:
      case spv::Op::OpControlBarrier:
      case spv::Op::OpMemoryBarrier:
        return true;
      default:
        break;
    }

    // All other modifiers have their target pointer at in-operand 0.
    // (This includes OpStore, OpCopyMemory, OpCopyMemorySized, all
    // storing atomics, OpImageWrite, etc.)
    const Instruction* modifiedPtr =
        defUseMgr->GetDef(curr->GetSingleWordInOperand(0));
    if (modifiedPtr && IsMustAlias(modifiedPtr, ptrInst, defUseMgr)) {
      return true;
    }
  }
  return false;
}

}  // namespace opt
}  // namespace spvtools

#endif  // SOURCE_OPT_MEM_DEPENDENCE_UTIL_H_
