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

#ifndef SOURCE_OPT_FUNCTION_SPECIALIZATION_PASS_H_
#define SOURCE_OPT_FUNCTION_SPECIALIZATION_PASS_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/pass.h"

namespace spvtools {
namespace opt {

// See optimizer.hpp for documentation.
//
// This pass specializes functions for constant arguments by cloning the
// function with the constant propagated. When a function is called with
// constant arguments, it creates a specialized version of the function
// with those constants baked in.  This is analogous to LLVM's
// FunctionSpecialization pass.
//
// Simplification: only functions where ALL arguments are constants are
// fully specialized (all parameters become constants, clone becomes a thunk
// with no parameters).
class FunctionSpecializationPass : public Pass {
 public:
  FunctionSpecializationPass() = default;

  const char* name() const override { return "function-specialization"; }
  Status Process() override;

  IRContext::Analysis GetPreservedAnalyses() override {
    return IRContext::kAnalysisDefUse |
           IRContext::kAnalysisInstrToBlockMapping |
           IRContext::kAnalysisConstants | IRContext::kAnalysisTypes;
  }

 private:
  // Returns a key that uniquely identifies a specialization: {function id,
  // argument index -> constant id pairs}.
  using SpecializationKey = std::string;

  // Build a deduplication key for a function call specialization.
  SpecializationKey BuildSpecializationKey(
      uint32_t func_id,
      const std::vector<std::pair<uint32_t, uint32_t>>& const_args) const;

  // Generate a deterministic name for a specialized function.
  std::string GetSpecializedName(
      uint32_t func_id, const std::string& func_name,
      const std::vector<std::pair<uint32_t, uint32_t>>& const_args) const;

  // Check if a call instruction has all constant arguments (OpConstant,
  // OpConstantComposite, or OpSpecConstant).  Fills |const_args| with
  // (argument_index, constant_id) pairs.
  bool GetAllConstantArgs(
      Instruction* call_inst,
      std::vector<std::pair<uint32_t, uint32_t>>* const_args) const;

  // Rewrite a call instruction to call a different function with no arguments.
  void RewriteCall(Instruction* call_inst, uint32_t new_func_id);

  // Set of specialization keys already created, to avoid duplicates.
  std::unordered_set<SpecializationKey> specialized_keys_;
};

}  // namespace opt
}  // namespace spvtools

#endif  // SOURCE_OPT_FUNCTION_SPECIALIZATION_PASS_H_
