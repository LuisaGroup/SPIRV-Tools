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

#ifndef SOURCE_OPT_INFER_ADDRESS_SPACES_PASS_H_
#define SOURCE_OPT_INFER_ADDRESS_SPACES_PASS_H_

#include <memory>
#include <unordered_map>
#include <vector>

#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/pass.h"

namespace spvtools {
namespace opt {

// See optimizer.hpp for documentation.
//
// This pass attempts to propagate non-Generic storage classes from pointer
// origins through OpBitcast instructions (and OpPtrCastToGeneric /
// OpGenericCastToPtr) to loads, stores, access chains and other bitcasts.
// It is analogous to LLVM's InferAddressSpaces pass.
class InferAddressSpacesPass : public Pass {
 public:
  InferAddressSpacesPass() = default;

  const char* name() const override { return "infer-address-spaces"; }
  Status Process() override;

  IRContext::Analysis GetPreservedAnalyses() override {
    return IRContext::kAnalysisDefUse |
           IRContext::kAnalysisInstrToBlockMapping |
           IRContext::kAnalysisConstants | IRContext::kAnalysisTypes;
  }

 private:
  // Returns true if the module supports the required capabilities for this
  // optimization (VariablePointers or GenericPointer).
  bool HasGenericAddressingCapability() const;

  // Collect all OpBitcast instructions that cast between different pointer
  // storage classes, from a non-Generic source to a more generic destination.
  // Also handles OpPtrCastToGeneric / OpGenericCastToPtr if present.
  void CollectPointerCasts(std::vector<Instruction*>* casts) const;

  // Try to propagate the source storage class of |cast_inst| through its
  // users. Returns true if any change was made.
  bool PropagateStorageClass(Instruction* cast_inst);

  // Returns the storage class of the pointer type given by |type_id|.
  spv::StorageClass GetPointerStorageClass(uint32_t type_id) const;

  // Returns true if |storage_class| is considered non-Generic (i.e., a
  // specific storage class other than Generic).
  bool IsNonGenericStorageClass(spv::StorageClass storage_class) const;
};

}  // namespace opt
}  // namespace spvtools

#endif  // SOURCE_OPT_INFER_ADDRESS_SPACES_PASS_H_
