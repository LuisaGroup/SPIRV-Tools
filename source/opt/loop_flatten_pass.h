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

#ifndef SOURCE_OPT_LOOP_FLATTEN_PASS_H_
#define SOURCE_OPT_LOOP_FLATTEN_PASS_H_

#include "source/opt/loop_descriptor.h"
#include "source/opt/pass.h"

namespace spvtools {
namespace opt {

// Implements the loop flatten optimization.
// This pass flattens perfect loop nests into a single loop when the inner loop
// has a simple induction variable and both loops are in simplified form.
//
// For a perfect nest like:
//   for (i = 0; i < N; i++)
//     for (j = 0; j < M; j++)
//       body(i, j)
//
// This pass will convert it to:
//   for (k = 0; k < N*M; k++)
//     body(k / M, k % M)  (or equivalent with adjusted induction)
//
// This is a simplification pass: if any precondition fails, returns
// SuccessWithoutChange.
class LoopFlattenPass : public Pass {
 public:
  LoopFlattenPass() = default;

  const char* name() const override { return "loop-flatten"; }

  // Processes the given module. Returns Status::Failure if errors occur when
  // processing. Returns the corresponding Status::Success if processing is
  // successful to indicate whether changes have been made to the module.
  Pass::Status Process() override;

  IRContext::Analysis GetPreservedAnalyses() override {
    return IRContext::kAnalysisDefUse |
           IRContext::kAnalysisInstrToBlockMapping |
           IRContext::kAnalysisDecorations | IRContext::kAnalysisCombinators |
           IRContext::kAnalysisNameMap | IRContext::kAnalysisConstants |
           IRContext::kAnalysisTypes;
  }

 private:
  // Process the given function and return true if changes were made.
  bool ProcessFunction(Function* f);

  // Check if a loop has simplified form:
  // - Single preheader (unique entry)
  // - Single latch (unique back-edge)
  // - Single exit (merge block)
  bool IsLoopSimplifiedForm(const Loop* loop) const;

  // Check if an OpPhi induction variable is simple:
  //   init = 0, step = +1
  // Returns the trip count instruction id if valid, 0 otherwise.
  // Also fills in |trip_count_id| with the id of the trip count value.
  bool IsSimpleIV(const Instruction* phi, uint32_t* trip_count_id) const;

  // Get the trip count value id from an IV phi.
  // The trip count is the other operand in the exit comparison.
  uint32_t GetTripCountFromIV(const Instruction* phi) const;

  // Check if the inner loop body uses the outer induction variable.
  // The only allowed use is in the inner loop's exit condition comparison.
  bool InnerBodyUsesOuterIV(const Loop* inner, const Instruction* outer_iv,
                            const Instruction* inner_iv) const;
};

}  // namespace opt
}  // namespace spvtools

#endif  // SOURCE_OPT_LOOP_FLATTEN_PASS_H_
