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

#ifndef SOURCE_OPT_LOAD_STORE_MOTION_PASS_H_
#define SOURCE_OPT_LOAD_STORE_MOTION_PASS_H_

#include "source/opt/basic_block.h"
#include "source/opt/ir_context.h"
#include "source/opt/mem_dependence_util.h"
#include "source/opt/pass.h"

namespace spvtools {
namespace opt {

// See optimizer.hpp for documentation.
//
// MergedLoadStoreMotion pass: hoists loads out of diamonds and sinks stores
// into the merge block when they access the same address on both sides.
class LoadStoreMotionPass : public Pass {
 public:
  const char* name() const override { return "load-store-motion"; }
  Status Process() override;

  IRContext::Analysis GetPreservedAnalyses() override {
    return IRContext::kAnalysisDefUse | IRContext::kAnalysisDominatorAnalysis |
           IRContext::kAnalysisInstrToBlockMapping | IRContext::kAnalysisCFG |
           IRContext::kAnalysisNameMap | IRContext::kAnalysisConstants |
           IRContext::kAnalysisTypes;
  }

 private:
  // Compile-time control limit matching LLVM's MagicCompileTimeControl.
  static const size_t kMagicCompileTimeControl = 250;

  // Returns true if |bb| is the head of a structured diamond:
  //   header -> {left, right} -> merge (OpSelectionMerge)
  bool IsDiamondHead(BasicBlock* bb);

  // Returns the merge block of the diamond headed by |bb|.
  // Precondition: IsDiamondHead(bb).
  BasicBlock* GetDiamondMergeBlock(BasicBlock* bb);

  // Returns the "left" and "right" arms of the diamond. The arm with the
  // smaller block id is stored in |arm_a| to provide a deterministic ordering.
  void GetDiamondArms(BasicBlock* header, BasicBlock** arm_a,
                      BasicBlock** arm_b);

  // Hoists equivalent loads from both sides of the diamond into |header|.
  // Returns true if any loads were hoisted.
  bool MergeLoads(BasicBlock* header);

  // Sinks equivalent stores from both sides of the diamond into |merge|.
  // Returns true if any stores were sunk.
  bool MergeStores(BasicBlock* merge);

  // Finds a load in |arm| that matches |load| (same type, must-alias pointer,
  // no barrier). Returns the matching load or nullptr.
  Instruction* FindMatchingLoad(BasicBlock* arm, Instruction* load);

  // Finds a store in |arm| that matches |store| (same type, must-alias pointer,
  // no barrier). Returns the matching store or nullptr.
  Instruction* FindMatchingStore(BasicBlock* arm, Instruction* store);

  // Counts the number of instructions in a basic block.
  size_t CountInstructions(BasicBlock* bb);
};

}  // namespace opt
}  // namespace spvtools

#endif  // SOURCE_OPT_LOAD_STORE_MOTION_PASS_H_
