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

#ifndef SOURCE_OPT_CONSTRAINT_ELIMINATION_PASS_H_
#define SOURCE_OPT_CONSTRAINT_ELIMINATION_PASS_H_

#include <unordered_map>
#include <utility>

#include "source/opt/dominator_analysis.h"
#include "source/opt/function.h"
#include "source/opt/ir_context.h"
#include "source/opt/pass.h"

namespace spvtools {
namespace opt {

// A pass that eliminates redundant comparison instructions based on
// dominating conditions. It walks through basic blocks in dominator tree
// order, tracking known facts from conditions, and eliminates redundant
// comparisons.
//
// This pass corresponds to the LLVM ConstraintElimination pass.
class ConstraintEliminationPass : public Pass {
 public:
  const char* name() const override { return "constraint-elimination"; }
  Status Process() override;

  IRContext::Analysis GetPreservedAnalyses() override {
    return IRContext::kAnalysisDefUse |
           IRContext::kAnalysisInstrToBlockMapping |
           IRContext::kAnalysisDecorations | IRContext::kAnalysisCombinators |
           IRContext::kAnalysisCFG | IRContext::kAnalysisDominatorAnalysis |
           IRContext::kAnalysisNameMap | IRContext::kAnalysisConstants |
           IRContext::kAnalysisTypes;
  }

 private:
  // Fact map: maps (aId, bId) -> isEqual (true for a==b, false for a!=b).
  using FactMap = std::unordered_map<std::pair<uint32_t, uint32_t>, bool>;

  struct pair_hash {
    size_t operator()(const std::pair<uint32_t, uint32_t>& p) const {
      return (static_cast<size_t>(p.first) << 32) | p.second;
    }
  };

  using FactMapWithHash =
      std::unordered_map<std::pair<uint32_t, uint32_t>, bool, pair_hash>;

  // Adds a fact that A == B (if isEqual is true) or A != B (if isEqual is
  // false). Both (A,B) and (B,A) are stored.
  void AddFact(FactMapWithHash& facts, uint32_t aId, uint32_t bId,
               bool isEqual);

  // Returns true if A == B is known to be true.
  bool IsKnownTrue(FactMapWithHash& facts, uint32_t aId, uint32_t bId);

  // Returns true if A == B is known to be false.
  bool IsKnownFalse(FactMapWithHash& facts, uint32_t aId, uint32_t bId);

  // Attempt to eliminate a comparison instruction based on known facts.
  // Returns true if the instruction was replaced.
  bool EliminateCmp(Instruction* cmpInst, FactMapWithHash& facts);

  // Process a basic block: gather facts from its terminator, eliminate
  // comparisons, and recurse into dominator tree children.
  void ProcessBlock(BasicBlock* bb, FactMapWithHash& facts,
                    DominatorAnalysis* dom);

  // Get the bool type id for true/false constants.
  uint32_t GetBoolTypeId();

  // Cached bool type id.
  uint32_t bool_type_id_;

  // Whether any changes were made.
  bool modified_;
};

}  // namespace opt
}  // namespace spvtools

#endif  // SOURCE_OPT_CONSTRAINT_ELIMINATION_PASS_H_
