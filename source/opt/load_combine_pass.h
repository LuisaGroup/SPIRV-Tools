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

#ifndef SOURCE_OPT_LOAD_COMBINE_PASS_H_
#define SOURCE_OPT_LOAD_COMBINE_PASS_H_

#include <unordered_map>
#include <vector>

#include "source/opt/basic_block.h"
#include "source/opt/ir_context.h"
#include "source/opt/mem_dependence_util.h"
#include "source/opt/pass.h"

namespace spvtools {
namespace opt {

// See optimizer.hpp for documentation.
//
// LoadCombinePass: combines adjacent loads from the same base pointer into
// wider loads, reducing the total number of memory accesses.
class LoadCombinePass : public Pass {
 public:
  const char* name() const override { return "load-combine"; }
  Status Process() override;

  IRContext::Analysis GetPreservedAnalyses() override {
    return IRContext::kAnalysisDefUse | IRContext::kAnalysisInstrToBlockMapping |
           IRContext::kAnalysisCFG | IRContext::kAnalysisNameMap |
           IRContext::kAnalysisConstants | IRContext::kAnalysisTypes;
  }

 private:
  struct PointerOffsetPair {
    uint32_t base_id = 0;
    uint64_t byte_offset = 0;
    bool valid = false;
  };

  struct LoadInfo {
    Instruction* load_inst = nullptr;
    uint32_t base_id = 0;
    uint64_t byte_offset = 0;
    uint32_t insert_index = 0;
  };

  // Walks the pointer operand of |load| through bitcasts and access chains to
  // compute {base_id, byte_offset}. Returns valid=false if any index is
  // non-constant or the pointer chain is too complex.
  PointerOffsetPair GetPointerOffsetPair(Instruction* load);

  // Combines loads in |load_map|. For each base with contiguous loads,
  // attempts to replace them with a single wider load.
  bool CombineLoads(
      std::unordered_map<uint32_t, std::vector<LoadInfo>>* load_map);

  // Tries to combine a sorted list of contiguous loads for a single base.
  bool CombineContiguousLoads(std::vector<LoadInfo>* loads);

  // Returns the bit width of the type with the given |type_id|.
  uint32_t GetTypeBitWidth(uint32_t type_id);

  // Returns true if the load is a candidate for combining.
  bool IsCandidateLoad(Instruction* load);

  // Checks if |inst| is a memory-modifying instruction that should flush the
  // load map.
  bool IsMemoryBarrier(Instruction* inst);

  // Max loads to track per base before forcing a flush.
  static const size_t kMaxLoadsPerBase = 32;
};

}  // namespace opt
}  // namespace spvtools

#endif  // SOURCE_OPT_LOAD_COMBINE_PASS_H_
