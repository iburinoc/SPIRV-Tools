// Copyright (c) 2017 The Khronos Group Inc.
// Copyright (c) 2017 Valve Corporation
// Copyright (c) 2017 LunarG Inc.
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

#include "local_access_chain_convert_pass.h"

#include "ir_context.h"
#include "iterator.h"

namespace spvtools {
namespace opt {

namespace {

const uint32_t kStoreValIdInIdx = 1;
const uint32_t kAccessChainPtrIdInIdx = 0;
const uint32_t kConstantValueInIdx = 0;
const uint32_t kTypeIntWidthInIdx = 0;

}  // anonymous namespace

void LocalAccessChainConvertPass::BuildAndAppendInst(
    SpvOp opcode, uint32_t typeId, uint32_t resultId,
    const std::vector<opt::Operand>& in_opnds,
    std::vector<std::unique_ptr<opt::Instruction>>* newInsts) {
  std::unique_ptr<opt::Instruction> newInst(
      new opt::Instruction(context(), opcode, typeId, resultId, in_opnds));
  get_def_use_mgr()->AnalyzeInstDefUse(&*newInst);
  newInsts->emplace_back(std::move(newInst));
}

uint32_t LocalAccessChainConvertPass::BuildAndAppendVarLoad(
    const opt::Instruction* ptrInst, uint32_t* varId, uint32_t* varPteTypeId,
    std::vector<std::unique_ptr<opt::Instruction>>* newInsts) {
  const uint32_t ldResultId = TakeNextId();
  *varId = ptrInst->GetSingleWordInOperand(kAccessChainPtrIdInIdx);
  const opt::Instruction* varInst = get_def_use_mgr()->GetDef(*varId);
  assert(varInst->opcode() == SpvOpVariable);
  *varPteTypeId = GetPointeeTypeId(varInst);
  BuildAndAppendInst(SpvOpLoad, *varPteTypeId, ldResultId,
                     {{spv_operand_type_t::SPV_OPERAND_TYPE_ID, {*varId}}},
                     newInsts);
  return ldResultId;
}

void LocalAccessChainConvertPass::AppendConstantOperands(
    const opt::Instruction* ptrInst, std::vector<opt::Operand>* in_opnds) {
  uint32_t iidIdx = 0;
  ptrInst->ForEachInId([&iidIdx, &in_opnds, this](const uint32_t* iid) {
    if (iidIdx > 0) {
      const opt::Instruction* cInst = get_def_use_mgr()->GetDef(*iid);
      uint32_t val = cInst->GetSingleWordInOperand(kConstantValueInIdx);
      in_opnds->push_back(
          {spv_operand_type_t::SPV_OPERAND_TYPE_LITERAL_INTEGER, {val}});
    }
    ++iidIdx;
  });
}

uint32_t LocalAccessChainConvertPass::GenAccessChainLoadReplacement(
    const opt::Instruction* ptrInst,
    std::vector<std::unique_ptr<opt::Instruction>>* newInsts) {
  // Build and append load of variable in ptrInst
  uint32_t varId;
  uint32_t varPteTypeId;
  const uint32_t ldResultId =
      BuildAndAppendVarLoad(ptrInst, &varId, &varPteTypeId, newInsts);

  // Build and append Extract
  const uint32_t extResultId = TakeNextId();
  const uint32_t ptrPteTypeId = GetPointeeTypeId(ptrInst);
  std::vector<opt::Operand> ext_in_opnds = {
      {spv_operand_type_t::SPV_OPERAND_TYPE_ID, {ldResultId}}};
  AppendConstantOperands(ptrInst, &ext_in_opnds);
  BuildAndAppendInst(SpvOpCompositeExtract, ptrPteTypeId, extResultId,
                     ext_in_opnds, newInsts);
  return extResultId;
}

void LocalAccessChainConvertPass::GenAccessChainStoreReplacement(
    const opt::Instruction* ptrInst, uint32_t valId,
    std::vector<std::unique_ptr<opt::Instruction>>* newInsts) {
  // Build and append load of variable in ptrInst
  uint32_t varId;
  uint32_t varPteTypeId;
  const uint32_t ldResultId =
      BuildAndAppendVarLoad(ptrInst, &varId, &varPteTypeId, newInsts);

  // Build and append Insert
  const uint32_t insResultId = TakeNextId();
  std::vector<opt::Operand> ins_in_opnds = {
      {spv_operand_type_t::SPV_OPERAND_TYPE_ID, {valId}},
      {spv_operand_type_t::SPV_OPERAND_TYPE_ID, {ldResultId}}};
  AppendConstantOperands(ptrInst, &ins_in_opnds);
  BuildAndAppendInst(SpvOpCompositeInsert, varPteTypeId, insResultId,
                     ins_in_opnds, newInsts);

  // Build and append Store
  BuildAndAppendInst(SpvOpStore, 0, 0,
                     {{spv_operand_type_t::SPV_OPERAND_TYPE_ID, {varId}},
                      {spv_operand_type_t::SPV_OPERAND_TYPE_ID, {insResultId}}},
                     newInsts);
}

bool LocalAccessChainConvertPass::IsConstantIndexAccessChain(
    const opt::Instruction* acp) const {
  uint32_t inIdx = 0;
  return acp->WhileEachInId([&inIdx, this](const uint32_t* tid) {
    if (inIdx > 0) {
      opt::Instruction* opInst = get_def_use_mgr()->GetDef(*tid);
      if (opInst->opcode() != SpvOpConstant) return false;
    }
    ++inIdx;
    return true;
  });
}

bool LocalAccessChainConvertPass::HasOnlySupportedRefs(uint32_t ptrId) {
  if (supported_ref_ptrs_.find(ptrId) != supported_ref_ptrs_.end()) return true;
  if (get_def_use_mgr()->WhileEachUser(ptrId, [this](opt::Instruction* user) {
        SpvOp op = user->opcode();
        if (IsNonPtrAccessChain(op) || op == SpvOpCopyObject) {
          if (!HasOnlySupportedRefs(user->result_id())) {
            return false;
          }
        } else if (op != SpvOpStore && op != SpvOpLoad && op != SpvOpName &&
                   !IsNonTypeDecorate(op)) {
          return false;
        }
        return true;
      })) {
    supported_ref_ptrs_.insert(ptrId);
    return true;
  }
  return false;
}

void LocalAccessChainConvertPass::FindTargetVars(opt::Function* func) {
  for (auto bi = func->begin(); bi != func->end(); ++bi) {
    for (auto ii = bi->begin(); ii != bi->end(); ++ii) {
      switch (ii->opcode()) {
        case SpvOpStore:
        case SpvOpLoad: {
          uint32_t varId;
          opt::Instruction* ptrInst = GetPtr(&*ii, &varId);
          if (!IsTargetVar(varId)) break;
          const SpvOp op = ptrInst->opcode();
          // Rule out variables with non-supported refs eg function calls
          if (!HasOnlySupportedRefs(varId)) {
            seen_non_target_vars_.insert(varId);
            seen_target_vars_.erase(varId);
            break;
          }
          // Rule out variables with nested access chains
          // TODO(): Convert nested access chains
          if (IsNonPtrAccessChain(op) && ptrInst->GetSingleWordInOperand(
                                             kAccessChainPtrIdInIdx) != varId) {
            seen_non_target_vars_.insert(varId);
            seen_target_vars_.erase(varId);
            break;
          }
          // Rule out variables accessed with non-constant indices
          if (!IsConstantIndexAccessChain(ptrInst)) {
            seen_non_target_vars_.insert(varId);
            seen_target_vars_.erase(varId);
            break;
          }
        } break;
        default:
          break;
      }
    }
  }
}

bool LocalAccessChainConvertPass::ConvertLocalAccessChains(
    opt::Function* func) {
  FindTargetVars(func);
  // Replace access chains of all targeted variables with equivalent
  // extract and insert sequences
  bool modified = false;
  for (auto bi = func->begin(); bi != func->end(); ++bi) {
    std::vector<opt::Instruction*> dead_instructions;
    for (auto ii = bi->begin(); ii != bi->end(); ++ii) {
      switch (ii->opcode()) {
        case SpvOpLoad: {
          uint32_t varId;
          opt::Instruction* ptrInst = GetPtr(&*ii, &varId);
          if (!IsNonPtrAccessChain(ptrInst->opcode())) break;
          if (!IsTargetVar(varId)) break;
          std::vector<std::unique_ptr<opt::Instruction>> newInsts;
          uint32_t replId = GenAccessChainLoadReplacement(ptrInst, &newInsts);
          context()->KillNamesAndDecorates(&*ii);
          context()->ReplaceAllUsesWith(ii->result_id(), replId);
          dead_instructions.push_back(&*ii);
          ++ii;
          ii = ii.InsertBefore(std::move(newInsts));
          ++ii;
          modified = true;
        } break;
        case SpvOpStore: {
          uint32_t varId;
          opt::Instruction* ptrInst = GetPtr(&*ii, &varId);
          if (!IsNonPtrAccessChain(ptrInst->opcode())) break;
          if (!IsTargetVar(varId)) break;
          std::vector<std::unique_ptr<opt::Instruction>> newInsts;
          uint32_t valId = ii->GetSingleWordInOperand(kStoreValIdInIdx);
          GenAccessChainStoreReplacement(ptrInst, valId, &newInsts);
          dead_instructions.push_back(&*ii);
          ++ii;
          ii = ii.InsertBefore(std::move(newInsts));
          ++ii;
          ++ii;
          modified = true;
        } break;
        default:
          break;
      }
    }

    while (!dead_instructions.empty()) {
      opt::Instruction* inst = dead_instructions.back();
      dead_instructions.pop_back();
      DCEInst(inst, [&dead_instructions](opt::Instruction* other_inst) {
        auto i = std::find(dead_instructions.begin(), dead_instructions.end(),
                           other_inst);
        if (i != dead_instructions.end()) {
          dead_instructions.erase(i);
        }
      });
    }
  }
  return modified;
}

void LocalAccessChainConvertPass::Initialize(opt::IRContext* c) {
  InitializeProcessing(c);

  // Initialize Target Variable Caches
  seen_target_vars_.clear();
  seen_non_target_vars_.clear();

  // Initialize collections
  supported_ref_ptrs_.clear();

  // Initialize extension whitelist
  InitExtensions();
}

bool LocalAccessChainConvertPass::AllExtensionsSupported() const {
  // If any extension not in whitelist, return false
  for (auto& ei : get_module()->extensions()) {
    const char* extName =
        reinterpret_cast<const char*>(&ei.GetInOperand(0).words[0]);
    if (extensions_whitelist_.find(extName) == extensions_whitelist_.end())
      return false;
  }
  return true;
}

Pass::Status LocalAccessChainConvertPass::ProcessImpl() {
  // If non-32-bit integer type in module, terminate processing
  // TODO(): Handle non-32-bit integer constants in access chains
  for (const opt::Instruction& inst : get_module()->types_values())
    if (inst.opcode() == SpvOpTypeInt &&
        inst.GetSingleWordInOperand(kTypeIntWidthInIdx) != 32)
      return Status::SuccessWithoutChange;
  // Do not process if module contains OpGroupDecorate. Additional
  // support required in KillNamesAndDecorates().
  // TODO(greg-lunarg): Add support for OpGroupDecorate
  for (auto& ai : get_module()->annotations())
    if (ai.opcode() == SpvOpGroupDecorate) return Status::SuccessWithoutChange;
  // Do not process if any disallowed extensions are enabled
  if (!AllExtensionsSupported()) return Status::SuccessWithoutChange;
  // Process all entry point functions.
  ProcessFunction pfn = [this](opt::Function* fp) {
    return ConvertLocalAccessChains(fp);
  };
  bool modified = ProcessEntryPointCallTree(pfn, get_module());
  return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
}

LocalAccessChainConvertPass::LocalAccessChainConvertPass() {}

Pass::Status LocalAccessChainConvertPass::Process(opt::IRContext* c) {
  Initialize(c);
  return ProcessImpl();
}

void LocalAccessChainConvertPass::InitExtensions() {
  extensions_whitelist_.clear();
  extensions_whitelist_.insert({
      "SPV_AMD_shader_explicit_vertex_parameter",
      "SPV_AMD_shader_trinary_minmax",
      "SPV_AMD_gcn_shader",
      "SPV_KHR_shader_ballot",
      "SPV_AMD_shader_ballot",
      "SPV_AMD_gpu_shader_half_float",
      "SPV_KHR_shader_draw_parameters",
      "SPV_KHR_subgroup_vote",
      "SPV_KHR_16bit_storage",
      "SPV_KHR_device_group",
      "SPV_KHR_multiview",
      "SPV_NVX_multiview_per_view_attributes",
      "SPV_NV_viewport_array2",
      "SPV_NV_stereo_view_rendering",
      "SPV_NV_sample_mask_override_coverage",
      "SPV_NV_geometry_shader_passthrough",
      "SPV_AMD_texture_gather_bias_lod",
      "SPV_KHR_storage_buffer_storage_class",
      // SPV_KHR_variable_pointers
      //   Currently do not support extended pointer expressions
      "SPV_AMD_gpu_shader_int16",
      "SPV_KHR_post_depth_coverage",
      "SPV_KHR_shader_atomic_counter_ops",
      "SPV_EXT_shader_stencil_export",
      "SPV_EXT_shader_viewport_index_layer",
      "SPV_AMD_shader_image_load_store_lod",
      "SPV_AMD_shader_fragment_mask",
      "SPV_EXT_fragment_fully_covered",
      "SPV_AMD_gpu_shader_half_float_fetch",
      "SPV_GOOGLE_decorate_string",
      "SPV_GOOGLE_hlsl_functionality1",
      "SPV_NV_shader_subgroup_partitioned",
      "SPV_EXT_descriptor_indexing",
  });
}

}  // namespace opt
}  // namespace spvtools
