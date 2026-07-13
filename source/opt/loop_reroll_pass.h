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

#ifndef SOURCE_OPT_LOOP_REROLL_PASS_H_
#define SOURCE_OPT_LOOP_REROLL_PASS_H_

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "source/opt/loop_descriptor.h"
#include "source/opt/pass.h"

namespace spvtools {
namespace opt {

// Implements a simplified loop rerolling optimization.
// This pass detects single-block loops that have been unrolled by a factor
// (2, 3, or 4) and re-rolls them back to a single iteration body with an
// adjusted trip count.
//
// Example:
//   for (int i = 0; i < N; i += 3) {
//     foo(i);
//     foo(i+1);
//     foo(i+2);
//   }
// -> rerolls to:
//   for (int i = 0; i < N; ++i) {
//     foo(i);
//   }
//
// This is a simplified implementation that bails on any complexity.
class LoopRerollPass : public Pass {
 public:
  LoopRerollPass() = default;

  const char* name() const override { return "loop-reroll"; }

  Pass::Status Process() override;

  IRContext::Analysis GetPreservedAnalyses() override {
    return IRContext::kAnalysisDefUse |
           IRContext::kAnalysisInstrToBlockMapping |
           IRContext::kAnalysisDecorations | IRContext::kAnalysisCombinators |
           IRContext::kAnalysisNameMap | IRContext::kAnalysisConstants |
           IRContext::kAnalysisTypes;
  }

 private:
  // Process the given function; returns true if changes were made.
  bool ProcessFunction(Function* f);

  // Check if a phi instruction is a simple induction variable:
  // init = 0 (from preheader), step = +1 (from latch).
  // Returns true and fills |trip_count_id| with the upper bound value id.
  bool IsSimpleIV(const Instruction* phi, uint32_t* trip_count_id) const;

  // Get the trip count value from an IV phi's exit condition.
  uint32_t GetTripCountFromIV(const Instruction* phi) const;

  // Try to re-roll a single-block loop. Returns true if successful.
  bool RerollLoop(Loop* loop, Function* f);

  // Detect root increments: look for OpIAdd instructions that add small
  // constants (1, 2, ..., scale-1) to the IV. Returns the detected scale
  // (2, 3, or 4) or 0 if no valid unrolled pattern found.
  uint32_t DetectRootIncrements(
      const Instruction* iv,
      std::vector<Instruction*>& root_increments) const;

  // Check if the instruction DAGs rooted at each increment are isomorphic
  // to the DAG rooted at the base IV. Only the first scale-1 increments
  // need to match the base.
  bool CheckIsomorphism(const Instruction* iv,
                        const std::vector<Instruction*>& root_increments,
                        uint32_t scale) const;

  // Compare two instructions structurally (same opcode, type, and isomorphic
  // operands) allowing for a fixed substitution set.
  bool AreIsomorphic(const Instruction* a, const Instruction* b,
                     std::unordered_map<uint32_t, uint32_t>& value_map) const;
};

}  // namespace opt
}  // namespace spvtools

#endif  // SOURCE_OPT_LOOP_REROLL_PASS_H_
