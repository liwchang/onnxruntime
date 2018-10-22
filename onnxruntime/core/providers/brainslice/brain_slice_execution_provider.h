// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once
#include "core/providers/brainslice/brainslice_mem_planner.h"
#include "core/framework/allocatormgr.h"
#include "core/framework/execution_provider.h"
#include "core/providers/brainslice/fpga_handle.h"

namespace onnxruntime {
namespace brainslice {
class BrainSliceExecutionProvider : public IExecutionProvider {
 public:
  friend class BrainSliceOpKernel;
  explicit BrainSliceExecutionProvider(const fpga::FPGAInfo& info);

  virtual ~BrainSliceExecutionProvider() {}

  std::string Type() const override {
    return kBrainSliceExecutionProvider;
  }

  Status CopyTensor(const Tensor& src, Tensor& dst) const override;

  virtual const void* GetExecutionHandle() const noexcept override {
    // The brainslice xp interface does not return anything interesting.
    return nullptr;
  }

  std::vector<std::unique_ptr<ComputationCapacity>> GetCapability(const onnxruntime::Graph& graph,
                                                                  const std::vector<const KernelRegistry*>& kernel_registries) const override;

  BrainSliceMemoryPlanner* GetBrainSliceMemoryPlanner(ISA_Mem mem_type);

  size_t GetBrainSliceNativeDim() {
    return handle_.GetCapacities().m_bsParameters.HWVEC_ELEMS;
  }

  virtual std::shared_ptr<KernelRegistry> GetKernelRegistry() const override;

  const fpga::FPGAHandle& GetFPGAHandle() const {
    return handle_;
  }

 private:
  // Check does FPGA has enough capacity to run this node
  bool CheckNodeWithCapacity(const onnxruntime::Graph& graph, const onnxruntime::Node& node) const;

  fpga::FPGAHandle handle_;

  //memory planner for matrix register file on BrainSlice
  std::unique_ptr<BrainSliceMemoryPlanner> matrix_rf_planner_;
  //memory planner for vector multiplication register file on BrainSlice
  std::unique_ptr<BrainSliceMemoryPlanner> multiply_vrf_planner_;
  //memory planner for vector add/sub register file on BrainSlice.
  std::unique_ptr<BrainSliceMemoryPlanner> add_sub_vrf_planner_;
  //TODO: support dram
};
}  // namespace brainslice
}  // namespace onnxruntime
