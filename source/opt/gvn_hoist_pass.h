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

#ifndef SOURCE_OPT_GVN_HOIST_PASS_H_
#define SOURCE_OPT_GVN_HOIST_PASS_H_

#include <unordered_map>

#include "source/opt/dominator_analysis.h"
#include "source/opt/function.h"
#include "source/opt/ir_context.h"
#include "source/opt/pass.h"
#include "source/opt/value_number_table.h"

namespace spvtools {
namespace opt {

// A pass that hoists identical instructions from then/else blocks to their
// common predecessor if they dominate their uses. Uses GVN-style analysis
// to detect identical expressions.
//
// This pass corresponds to the LLVM GVNHoist pass.
class GVNHoistPass : public Pass {
 public:
  const char* name() const override { return "gvn-hoist"; }
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
  // Attempts to hoist identical instructions from the two successors of |bb|.
  // Returns true if any instructions were hoisted.
  bool HoistFromBlock(BasicBlock* bb, const ValueNumberTable& vnt,
                      DominatorAnalysis* dom);
};

}  // namespace opt
}  // namespace spvtools

#endif  // SOURCE_OPT_GVN_HOIST_PASS_H_
