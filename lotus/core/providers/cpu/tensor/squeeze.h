#pragma once

#include "core/common/common.h"
#include "core/framework/op_kernel.h"

namespace Lotus {
template <typename T>
class Squeeze final : public OpKernel {
 public:
  Squeeze(const OpKernelInfo& info) : OpKernel(info) {
    std::vector<int64_t> axes;
    Status status = info.GetAttrs<int64_t>("axes", axes);
    LOTUS_ENFORCE(status.IsOK(), "Attribute axes is not set.");

    // Handle out of order and repeating dims.
    std::sort(axes.begin(), axes.end());
    axes.erase(std::unique(axes.begin(), axes.end()), axes.end());
    axes_ = axes;
  }

  static std::vector<int64_t> ComputeOutputShape(
      std::vector<int64_t> input_shape,
      std::vector<int64_t> axes) {
    int j = 0;
    std::vector<int64_t> output_shape;
    for (int i = 0; i < input_shape.size(); ++i) {
      if (j < axes.size() && axes[j] == i) {
        LOTUS_ENFORCE(input_shape[i] == 1, "Dimension of input ", i,
                      " must be 1 instead of ", input_shape[i]);
        ++j;
        continue;
      }
      output_shape.push_back(input_shape[i]);
    }
    return output_shape;
  }

  Status Compute(OpKernelContext* context) const override {
    const Tensor* X = context->Input<Tensor>(0);
    const TensorShape& X_shape = X->Shape();
    std::vector<int64_t> output_shape = ComputeOutputShape(X_shape.GetDims(), axes_);

    Tensor* Y = context->Output(0, TensorShape(output_shape));
    const std::vector<std::pair<int, int>>& alias = KernelDef().Alias();
    //If input X and output Y are not aliases, it means the kernel is not doing inplace operation.
    if (std::find(alias.begin(), alias.end(), std::pair<int, int>(0, 0)) == alias.end()) {
      //copying
      memcpy(Y->MutableData<T>(), X->Data<T>(), X_shape.Size() * sizeof(T));
    } else {  //non-copying
      *(Y->MutableData<T>()) = *(X->Data<T>());
    }
    return Status::OK();
  }

 private:
  std::vector<int64_t> axes_;
};
}  // namespace Lotus