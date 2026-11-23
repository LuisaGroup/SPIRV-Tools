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

#include "source/opt/fma_contraction_pass.h"

#include <vector>

#include "source/opt/constants.h"
#include "source/opt/decoration_manager.h"
#include "source/opt/ir_builder.h"
#include "source/opt/ir_context.h"
#include "source/opt/type_manager.h"

namespace spvtools {
namespace opt {
namespace {

uint32_t GetFloatWidth(const analysis::Type* type) {
  if (!type) return 0;
  if (const auto* f = type->AsFloat()) return f->width();
  if (const auto* v = type->AsVector()) {
    if (const auto* ef = v->element_type()->AsFloat()) return ef->width();
  }
  return 0;
}

bool HasNoContraction(IRContext* ctx, uint32_t id) {
  return id != 0 &&
         ctx->get_decoration_mgr()->HasDecoration(
             id, static_cast<uint32_t>(spv::Decoration::NoContraction));
}

bool HasRelaxedPrecision(IRContext* ctx, uint32_t id) {
  return id != 0 &&
         ctx->get_decoration_mgr()->HasDecoration(
             id, static_cast<uint32_t>(spv::Decoration::RelaxedPrecision));
}

// Returns a free negated value ID if |val_id| is an OpFNegate or a float
// constant/null, otherwise returns 0. If |out_dead_negate| is non-null and
// |val_id| was a single-use OpFNegate, sets it so caller may kill it.
uint32_t TryGetFreeNegatedId(IRContext* ctx, uint32_t val_id,
                             Instruction** out_dead_negate) {
  if (out_dead_negate) *out_dead_negate = nullptr;
  auto* def_use = ctx->get_def_use_mgr();
  Instruction* def = def_use->GetDef(val_id);
  if (!def) return 0;
  if (def->opcode() == spv::Op::OpFNegate) {
    if (def_use->NumUses(val_id) == 1 && out_dead_negate) {
      *out_dead_negate = def;
    }
    return def->GetSingleWordInOperand(0);
  }
  auto* const_mgr = ctx->get_constant_mgr();
  const analysis::Constant* cst = const_mgr->GetConstantFromInst(def);
  if (!cst) return 0;
  if (cst->AsNullConstant()) {
    // OpConstantNull is positive zero. Reusing it as a negated FMA operand
    // changes signed-zero results (for example, -0 - (+0 * b) must stay -0).
    auto make_negative_zero = [&](const analysis::Float* type) -> uint32_t {
      if (!type) return 0;
      std::vector<uint32_t> words(type->width() == 64 ? 2u : 1u, 0u);
      if (type->width() == 16) {
        words[0] = 0x8000u;
      } else if (type->width() == 32) {
        words[0] = 0x80000000u;
      } else if (type->width() == 64) {
        words[1] = 0x80000000u;
      } else {
        return 0;
      }
      const analysis::Constant* negative_zero =
          const_mgr->GetConstant(type, std::move(words));
      Instruction* negative_zero_inst =
          const_mgr->GetDefiningInstruction(negative_zero);
      return negative_zero_inst ? negative_zero_inst->result_id() : 0;
    };
    if (const auto* scalar_type = cst->type()->AsFloat()) {
      return make_negative_zero(scalar_type);
    }
    const auto* vector_type = cst->type()->AsVector();
    if (!vector_type) return 0;
    uint32_t component =
        make_negative_zero(vector_type->element_type()->AsFloat());
    if (component == 0) return 0;
    std::vector<uint32_t> components(vector_type->element_count(), component);
    const analysis::Constant* negative_zero =
        const_mgr->GetConstant(cst->type(), components);
    Instruction* negative_zero_inst =
        const_mgr->GetDefiningInstruction(negative_zero);
    return negative_zero_inst ? negative_zero_inst->result_id() : 0;
  }
  if (const auto* fc = cst->AsFloatConstant()) {
    std::vector<uint32_t> words = fc->words();
    uint32_t width = fc->type()->AsFloat()->width();
    if (width == 32 && words.size() == 1) {
      words[0] ^= 0x80000000u;
    } else if (width == 64 && words.size() == 2) {
      words[1] ^= 0x80000000u;
    } else if (width == 16 && words.size() == 1) {
      words[0] ^= 0x8000u;
    } else {
      return 0;
    }
    const analysis::Constant* neg_cst =
        const_mgr->GetConstant(fc->type(), std::move(words));
    if (!neg_cst) return 0;
    Instruction* neg_inst = const_mgr->GetDefiningInstruction(neg_cst);
    return neg_inst ? neg_inst->result_id() : 0;
  }
  return 0;
}

}  // namespace

Pass::Status FmaContractionPass::Process() {
  bool modified = false;
  bool used_khr_fma = false;
  auto* def_use = context()->get_def_use_mgr();
  auto* type_mgr = context()->get_type_mgr();

  auto try_contract_mul = [&](BasicBlock* bb, uint32_t type_id, uint32_t cand_id,
                              Instruction*& out_mul) -> bool {
    Instruction* cand = def_use->GetDef(cand_id);
    if (!cand || cand->opcode() != spv::Op::OpFMul) return false;
    if (cand->type_id() != type_id) return false;
    if (context()->get_instr_block(cand) != bb) return false;
    if (def_use->NumUses(cand->result_id()) != 1) return false;
    if (HasNoContraction(context(), cand->result_id())) return false;
    out_mul = cand;
    return true;
  };

  for (auto& func : *get_module()) {
    if (func.IsDeclaration()) continue;
    for (auto& bb : func) {
      for (auto it = bb.begin(); it != bb.end();) {
        Instruction* inst = &*it;
        auto next_it = std::next(it);

        const spv::Op opc = inst->opcode();
        if (opc != spv::Op::OpFAdd && opc != spv::Op::OpFSub) {
          it = next_it;
          continue;
        }
        const uint32_t type_id = inst->type_id();
        const analysis::Type* type = type_mgr->GetType(type_id);
        const uint32_t float_width = GetFloatWidth(type);
        if (float_width == 0) {
          it = next_it;
          continue;
        }
        if (HasNoContraction(context(), inst->result_id())) {
          it = next_it;
          continue;
        }

        uint32_t op0 = inst->GetSingleWordInOperand(0);
        uint32_t op1 = inst->GetSingleWordInOperand(1);
        Instruction* mul_inst = nullptr;
        Instruction* dead_neg_inst = nullptr;
        uint32_t a_id = 0, b_id = 0, c_id = 0;

        if (opc == spv::Op::OpFAdd) {
          if (try_contract_mul(&bb, type_id, op0, mul_inst)) {
            a_id = mul_inst->GetSingleWordInOperand(0);
            b_id = mul_inst->GetSingleWordInOperand(1);
            c_id = op1;
          } else if (try_contract_mul(&bb, type_id, op1, mul_inst)) {
            a_id = mul_inst->GetSingleWordInOperand(0);
            b_id = mul_inst->GetSingleWordInOperand(1);
            c_id = op0;
          }
        } else {  // OpFSub
          if (try_contract_mul(&bb, type_id, op0, mul_inst)) {
            uint32_t neg_c = TryGetFreeNegatedId(context(), op1, &dead_neg_inst);
            if (neg_c != 0) {
              a_id = mul_inst->GetSingleWordInOperand(0);
              b_id = mul_inst->GetSingleWordInOperand(1);
              c_id = neg_c;
            } else {
              mul_inst = nullptr;
            }
          } else if (try_contract_mul(&bb, type_id, op1, mul_inst)) {
            uint32_t ma = TryGetFreeNegatedId(
                context(), mul_inst->GetSingleWordInOperand(0), &dead_neg_inst);
            if (ma != 0) {
              a_id = ma;
              b_id = mul_inst->GetSingleWordInOperand(1);
              c_id = op0;
            } else {
              uint32_t mb = TryGetFreeNegatedId(
                  context(), mul_inst->GetSingleWordInOperand(1),
                  &dead_neg_inst);
              if (mb != 0) {
                a_id = mul_inst->GetSingleWordInOperand(0);
                b_id = mb;
                c_id = op0;
              } else {
                mul_inst = nullptr;
              }
            }
          }
        }

        if (!mul_inst) {
          it = next_it;
          continue;
        }

        InstructionBuilder ir_builder(
            context(), inst,
            IRContext::kAnalysisDefUse |
                IRContext::kAnalysisInstrToBlockMapping);

        Instruction* fma_inst = nullptr;
        if (use_khr_fma_ && float_width == 32) {
          fma_inst = ir_builder.AddTernaryOp(type_id, spv::Op::OpFmaKHR, a_id,
                                             b_id, c_id);
          if (fma_inst) used_khr_fma = true;
        } else {
          uint32_t glsl_id =
              context()->get_feature_mgr()->GetExtInstImportId_GLSLstd450();
          if (glsl_id == 0) {
            context()->AddExtInstImport("GLSL.std.450");
            glsl_id =
                context()->get_feature_mgr()->GetExtInstImportId_GLSLstd450();
          }
          if (glsl_id != 0) {
            fma_inst = ir_builder.AddNaryExtendedInstruction(
                type_id, glsl_id, GLSLstd450Fma, {a_id, b_id, c_id});
          }
        }

        if (!fma_inst) {
          it = next_it;
          continue;
        }

        if (HasRelaxedPrecision(context(), inst->result_id())) {
          context()->get_decoration_mgr()->AddDecoration(
              fma_inst->result_id(),
              static_cast<uint32_t>(spv::Decoration::RelaxedPrecision));
        }

        context()->ReplaceAllUsesWith(inst->result_id(), fma_inst->result_id());
        context()->KillInst(inst);
        context()->KillInst(mul_inst);
        if (dead_neg_inst && dead_neg_inst != &*next_it &&
            def_use->NumUses(dead_neg_inst->result_id()) == 0) {
          context()->KillInst(dead_neg_inst);
        }

        modified = true;
        it = next_it;
      }
    }
  }

  if (used_khr_fma) {
    if (!context()->get_feature_mgr()->HasCapability(spv::Capability::FMAKHR)) {
      context()->AddCapability(spv::Capability::FMAKHR);
    }
    bool has_ext = false;
    for (const auto& ext : get_module()->extensions()) {
      if (ext.opcode() == spv::Op::OpExtension &&
          ext.GetInOperand(0).AsString() == "SPV_KHR_fma") {
        has_ext = true;
        break;
      }
    }
    if (!has_ext) {
      context()->AddExtension("SPV_KHR_fma");
    }
  }

  return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
}

}  // namespace opt
}  // namespace spvtools
