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

#ifndef SOURCE_OPT_FMA_CONTRACTION_PASS_H_
#define SOURCE_OPT_FMA_CONTRACTION_PASS_H_

#include "source/opt/pass.h"

namespace spvtools {
namespace opt {

// Contracts single-use OpFMul + OpFAdd (and free-negation OpFSub) pairs
// lacking NoContraction decoration into OpFmaKHR (when |use_khr_fma_| is true
// for 32-bit floats) or GLSLstd450Fma.
class FmaContractionPass : public Pass {
 public:
  explicit FmaContractionPass(bool use_khr_fma = false)
      : use_khr_fma_(use_khr_fma) {}

  const char* name() const override { return "fma-contraction"; }
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

 private:
  bool use_khr_fma_;
};

}  // namespace opt
}  // namespace spvtools

#endif  // SOURCE_OPT_FMA_CONTRACTION_PASS_H_
