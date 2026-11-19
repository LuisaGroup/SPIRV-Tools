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

#ifndef SOURCE_OPT_BOOLEAN_SIMPLIFY_PASS_H_
#define SOURCE_OPT_BOOLEAN_SIMPLIFY_PASS_H_

#include "source/opt/pass.h"

namespace spvtools {
namespace opt {

// Canonicalizes scalar OpSelect %bool into OpLogicalAnd / OpLogicalOr,
// eliminates redundant OpLogicalNot / invertible comparisons on boolean selects,
// folds signed range checks ((x >= 0) && (x < N)) and ((x < 0) || (x >= N))
// into unsigned bounds checks, and merges power-of-2 unsigned range checks
// via bitwise OR.
class BooleanSimplifyPass : public Pass {
 public:
  const char* name() const override { return "boolean-simplify"; }
  Status Process() override;

  IRContext::Analysis GetPreservedAnalyses() override {
    return IRContext::kAnalysisDefUse |
           IRContext::kAnalysisInstrToBlockMapping |
           IRContext::kAnalysisCFG |
           IRContext::kAnalysisDominatorAnalysis |
           IRContext::kAnalysisLoopAnalysis |
           IRContext::kAnalysisNameMap |
           IRContext::kAnalysisConstants |
           IRContext::kAnalysisTypes;
  }
};

}  // namespace opt
}  // namespace spvtools

#endif  // SOURCE_OPT_BOOLEAN_SIMPLIFY_PASS_H_
