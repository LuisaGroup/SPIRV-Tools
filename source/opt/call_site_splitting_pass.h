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

#ifndef SOURCE_OPT_CALL_SITE_SPLITTING_PASS_H_
#define SOURCE_OPT_CALL_SITE_SPLITTING_PASS_H_

#include "source/opt/dominator_analysis.h"
#include "source/opt/function.h"
#include "source/opt/ir_context.h"
#include "source/opt/pass.h"

namespace spvtools {
namespace opt {

// A pass that splits call sites based on known constraints from dominating
// conditions. When a call is inside a branch on a comparison with an argument,
// the call site is split with the constraint info so that later passes can
// optimize the callee better.
//
// This pass corresponds to the LLVM CallSiteSplitting pass.
class CallSiteSplittingPass : public Pass {
 public:
  const char* name() const override { return "call-site-splitting"; }
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
  // Process a function call instruction, checking its predecessor blocks for
  // dominating conditions that constrain its arguments. Returns true if any
  // substitution was made.
  bool ProcessCallSite(Instruction* callInst, DominatorAnalysis* dom);
};

}  // namespace opt
}  // namespace spvtools

#endif  // SOURCE_OPT_CALL_SITE_SPLITTING_PASS_H_
