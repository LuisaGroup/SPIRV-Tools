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

#include "source/opt/float_div_opt_pass.h"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "source/opt/constants.h"
#include "source/opt/decoration_manager.h"
#include "source/opt/ir_builder.h"
#include "source/opt/ir_context.h"
#include "source/opt/type_manager.h"

namespace spvtools {
namespace opt {
namespace {

bool IsFloat32ScalarOrVector(const analysis::Type* type) {
  if (!type) return false;
  if (const auto* f = type->AsFloat()) return f->width() == 32;
  if (const auto* v = type->AsVector()) {
    if (const auto* ef = v->element_type()->AsFloat()) {
      return ef->width() == 32;
    }
  }
  return false;
}

bool HasNoContraction(IRContext* ctx, uint32_t id) {
  return id != 0 &&
         ctx->get_decoration_mgr()->HasDecoration(
             id, static_cast<uint32_t>(spv::Decoration::NoContraction));
}

bool IsFloat32One(IRContext* ctx, uint32_t id) {
  Instruction* def = ctx->get_def_use_mgr()->GetDef(id);
  if (!def) return false;
  const analysis::Constant* cst =
      ctx->get_constant_mgr()->GetConstantFromInst(def);
  if (!cst) return false;
  if (const auto* fc = cst->AsFloatConstant()) {
    return fc->type()->AsFloat()->width() == 32 && fc->words().size() == 1 &&
           fc->words()[0] == 0x3f800000u;
  }
  if (const auto* vc = cst->AsVectorConstant()) {
    const auto* ef = vc->component_type()->AsFloat();
    if (!ef || ef->width() != 32) return false;
    for (const auto* comp : vc->GetComponents()) {
      const auto* fc = comp->AsFloatConstant();
      if (!fc || fc->words().size() != 1 || fc->words()[0] != 0x3f800000u) {
        return false;
      }
    }
    return true;
  }
  return false;
}

uint32_t GetOrCreateFloat32One(IRContext* ctx, uint32_t type_id) {
  auto* type_mgr = ctx->get_type_mgr();
  auto* const_mgr = ctx->get_constant_mgr();
  const analysis::Type* ty = type_mgr->GetType(type_id);
  if (!ty) return 0;
  if (const auto* f = ty->AsFloat()) {
    if (f->width() != 32) return 0;
    const analysis::Constant* cst = const_mgr->GetConstant(f, {0x3f800000u});
    if (!cst) return 0;
    Instruction* def = const_mgr->GetDefiningInstruction(cst, type_id);
    return def ? def->result_id() : 0;
  }
  if (const auto* v = ty->AsVector()) {
    const auto* ef = v->element_type()->AsFloat();
    if (!ef || ef->width() != 32) return 0;
    uint32_t scalar_type_id = type_mgr->GetId(ef);
    const analysis::Constant* sc = const_mgr->GetConstant(ef, {0x3f800000u});
    if (!sc) return 0;
    Instruction* sc_def = const_mgr->GetDefiningInstruction(sc, scalar_type_id);
    if (!sc_def) return 0;
    std::vector<uint32_t> comp_ids(v->element_count(), sc_def->result_id());
    const analysis::Constant* vc = const_mgr->GetConstant(v, comp_ids);
    if (!vc) return 0;
    Instruction* vc_def = const_mgr->GetDefiningInstruction(vc, type_id);
    return vc_def ? vc_def->result_id() : 0;
  }
  return 0;
}

bool MatchGLSLSqrt(IRContext* ctx, uint32_t glsl_id, uint32_t id,
                   Instruction** out_inst, uint32_t* out_arg) {
  if (glsl_id == 0) return false;
  Instruction* def = ctx->get_def_use_mgr()->GetDef(id);
  if (!def || def->opcode() != spv::Op::OpExtInst) return false;
  if (def->GetSingleWordInOperand(0) != glsl_id) return false;
  if (def->GetSingleWordInOperand(1) != GLSLstd450Sqrt) return false;
  *out_inst = def;
  *out_arg = def->GetSingleWordInOperand(2);
  return true;
}

uint32_t EnsureGLSLstd450(IRContext* ctx) {
  uint32_t id = ctx->get_feature_mgr()->GetExtInstImportId_GLSLstd450();
  if (id == 0) {
    ctx->AddExtInstImport("GLSL.std.450");
    id = ctx->get_feature_mgr()->GetExtInstImportId_GLSLstd450();
  }
  return id;
}

struct PairHash {
  size_t operator()(const std::pair<uint32_t, uint32_t>& p) const {
    return (static_cast<size_t>(p.first) << 32) ^ static_cast<size_t>(p.second);
  }
};

}  // namespace

Pass::Status FloatDivOptPass::Process() {
  bool modified = false;
  auto* def_use = context()->get_def_use_mgr();
  auto* type_mgr = context()->get_type_mgr();

  for (auto& func : *get_module()) {
    if (func.IsDeclaration()) continue;
    for (auto& bb : func) {
      uint32_t glsl_id =
          context()->get_feature_mgr()->GetExtInstImportId_GLSLstd450();

      // Pass A: Algebraic division simplifications (Sqrt -> InverseSqrt,
      // (x / y) / Sqrt(y) -> x * rsqrt(y)^3, (a / b) / c -> a / (b * c)).
      for (auto it = bb.begin(); it != bb.end();) {
        Instruction* inst = &*it;
        auto next_it = std::next(it);

        if (inst->opcode() != spv::Op::OpFDiv) {
          it = next_it;
          continue;
        }
        uint32_t type_id = inst->type_id();
        if (!IsFloat32ScalarOrVector(type_mgr->GetType(type_id)) ||
            HasNoContraction(context(), inst->result_id())) {
          it = next_it;
          continue;
        }

        uint32_t num_id = inst->GetSingleWordInOperand(0);
        uint32_t den_id = inst->GetSingleWordInOperand(1);
        Instruction* num_def = def_use->GetDef(num_id);
        Instruction* sqrt_inst = nullptr;
        uint32_t sqrt_arg = 0;

        // Pattern A1: (x / y) / Sqrt(y) or (x / Sqrt(y)) / y
        if (num_def && num_def->opcode() == spv::Op::OpFDiv &&
            num_def->type_id() == type_id &&
            context()->get_instr_block(num_def) == &bb &&
            def_use->NumUses(num_id) == 1 &&
            !HasNoContraction(context(), num_id)) {
          uint32_t inner_x = num_def->GetSingleWordInOperand(0);
          uint32_t inner_d = num_def->GetSingleWordInOperand(1);
          bool matched_rsqrt3 = false;
          uint32_t rsqrt_arg = 0;
          if (MatchGLSLSqrt(context(), glsl_id, den_id, &sqrt_inst,
                            &sqrt_arg) &&
              sqrt_arg == inner_d &&
              !HasNoContraction(context(), sqrt_inst->result_id())) {
            matched_rsqrt3 = true;
            rsqrt_arg = inner_d;
          } else if (MatchGLSLSqrt(context(), glsl_id, inner_d, &sqrt_inst,
                                   &sqrt_arg) &&
                     sqrt_arg == den_id &&
                     !HasNoContraction(context(), sqrt_inst->result_id())) {
            matched_rsqrt3 = true;
            rsqrt_arg = den_id;
          }
          if (matched_rsqrt3) {
            uint32_t ext_id = EnsureGLSLstd450(context());
            if (ext_id != 0) {
              glsl_id = ext_id;
              InstructionBuilder ir_builder(
                  context(), inst,
                  IRContext::kAnalysisDefUse |
                      IRContext::kAnalysisInstrToBlockMapping);
              Instruction* inv = ir_builder.AddNaryExtendedInstruction(
                  type_id, glsl_id, GLSLstd450InverseSqrt, {rsqrt_arg});
              Instruction* inv2 =
                  inv ? ir_builder.AddBinaryOp(type_id, spv::Op::OpFMul,
                                               inv->result_id(),
                                               inv->result_id())
                      : nullptr;
              Instruction* inv3 =
                  inv2 ? ir_builder.AddBinaryOp(type_id, spv::Op::OpFMul,
                                                inv2->result_id(),
                                                inv->result_id())
                       : nullptr;
              Instruction* res = nullptr;
              if (inv3) {
                res = IsFloat32One(context(), inner_x)
                          ? inv3
                          : ir_builder.AddBinaryOp(type_id, spv::Op::OpFMul,
                                                   inner_x, inv3->result_id());
              }
              if (res) {
                context()->ReplaceAllUsesWith(inst->result_id(),
                                              res->result_id());
                context()->KillInst(inst);
                context()->KillInst(num_def);
                if (sqrt_inst != &*next_it &&
                    def_use->NumUses(sqrt_inst->result_id()) == 0) {
                  context()->KillInst(sqrt_inst);
                }
                modified = true;
                it = next_it;
                continue;
              }
            }
          }
        }

        // Pattern A2: x / Sqrt(y) -> x * InverseSqrt(y) (only if Sqrt has <=1
        // remaining use or x == 1.0, or always when Sqrt is single-use).
        if (MatchGLSLSqrt(context(), glsl_id, den_id, &sqrt_inst, &sqrt_arg) &&
            !HasNoContraction(context(), sqrt_inst->result_id()) &&
            def_use->NumUses(sqrt_inst->result_id()) == 1) {
          uint32_t ext_id = EnsureGLSLstd450(context());
          if (ext_id != 0) {
            glsl_id = ext_id;
            InstructionBuilder ir_builder(
                context(), inst,
                IRContext::kAnalysisDefUse |
                    IRContext::kAnalysisInstrToBlockMapping);
            Instruction* inv = ir_builder.AddNaryExtendedInstruction(
                type_id, glsl_id, GLSLstd450InverseSqrt, {sqrt_arg});
            Instruction* res = nullptr;
            if (inv) {
              res = IsFloat32One(context(), num_id)
                        ? inv
                        : ir_builder.AddBinaryOp(type_id, spv::Op::OpFMul,
                                                 num_id, inv->result_id());
            }
            if (res) {
              context()->ReplaceAllUsesWith(inst->result_id(),
                                            res->result_id());
              context()->KillInst(inst);
              if (sqrt_inst != &*next_it &&
                  def_use->NumUses(sqrt_inst->result_id()) == 0) {
                context()->KillInst(sqrt_inst);
              }
              modified = true;
              it = next_it;
              continue;
            }
          }
        }

        // Pattern A3: (a / b) / c -> a / (b * c)
        if (num_def && num_def->opcode() == spv::Op::OpFDiv &&
            num_def->type_id() == type_id &&
            context()->get_instr_block(num_def) == &bb &&
            def_use->NumUses(num_id) == 1 &&
            !HasNoContraction(context(), num_id)) {
          uint32_t a = num_def->GetSingleWordInOperand(0);
          uint32_t b = num_def->GetSingleWordInOperand(1);
          InstructionBuilder ir_builder(
              context(), inst,
              IRContext::kAnalysisDefUse |
                  IRContext::kAnalysisInstrToBlockMapping);
          Instruction* bc =
              ir_builder.AddBinaryOp(type_id, spv::Op::OpFMul, b, den_id);
          Instruction* res =
              bc ? ir_builder.AddBinaryOp(type_id, spv::Op::OpFDiv, a,
                                          bc->result_id())
                 : nullptr;
          if (res) {
            context()->ReplaceAllUsesWith(inst->result_id(), res->result_id());
            context()->KillInst(inst);
            context()->KillInst(num_def);
            modified = true;
            it = next_it;
            continue;
          }
        }

        it = next_it;
      }

      // Pass B: Common-denominator reciprocal factorization within the block:
      // >= 2 OpFDiv(*, den) -> inv = 1.0 / den; num_i * inv.
      std::unordered_map<std::pair<uint32_t, uint32_t>,
                         std::vector<Instruction*>, PairHash>
          groups;
      std::vector<std::pair<uint32_t, uint32_t>> order;
      for (auto& inst : bb) {
        if (inst.opcode() != spv::Op::OpFDiv) continue;
        uint32_t type_id = inst.type_id();
        if (!IsFloat32ScalarOrVector(type_mgr->GetType(type_id)) ||
            HasNoContraction(context(), inst.result_id())) {
          continue;
        }
        uint32_t den_id = inst.GetSingleWordInOperand(1);
        Instruction* den_def = def_use->GetDef(den_id);
        if (!den_def ||
            context()->get_constant_mgr()->GetConstantFromInst(den_def)) {
          continue;
        }
        auto key = std::make_pair(type_id, den_id);
        auto& vec = groups[key];
        if (vec.empty()) order.push_back(key);
        vec.push_back(&inst);
      }

      for (const auto& key : order) {
        auto& divs = groups[key];
        if (divs.size() < 2) continue;

        uint32_t type_id = key.first;
        uint32_t den_id = key.second;
        Instruction* first = divs.front();

        uint32_t inv_id = 0;
        Instruction* reuse_first = nullptr;
        if (IsFloat32One(context(), first->GetSingleWordInOperand(0))) {
          inv_id = first->result_id();
          reuse_first = first;
        } else {
          uint32_t one_id = GetOrCreateFloat32One(context(), type_id);
          if (one_id == 0) continue;
          InstructionBuilder ir_builder(
              context(), first,
              IRContext::kAnalysisDefUse |
                  IRContext::kAnalysisInstrToBlockMapping);
          Instruction* inv_inst = ir_builder.AddBinaryOp(
              type_id, spv::Op::OpFDiv, one_id, den_id);
          if (!inv_inst) continue;
          inv_id = inv_inst->result_id();
        }

        for (Instruction* div : divs) {
          if (div == reuse_first) continue;
          uint32_t num_id = div->GetSingleWordInOperand(0);
          if (IsFloat32One(context(), num_id)) {
            context()->ReplaceAllUsesWith(div->result_id(), inv_id);
            context()->KillInst(div);
            modified = true;
            continue;
          }
          InstructionBuilder ir_builder(
              context(), div,
              IRContext::kAnalysisDefUse |
                  IRContext::kAnalysisInstrToBlockMapping);
          Instruction* mul = ir_builder.AddBinaryOp(type_id, spv::Op::OpFMul,
                                                    num_id, inv_id);
          if (mul) {
            context()->ReplaceAllUsesWith(div->result_id(), mul->result_id());
            context()->KillInst(div);
            modified = true;
          }
        }
      }
    }
  }

  return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
}

}  // namespace opt
}  // namespace spvtools
