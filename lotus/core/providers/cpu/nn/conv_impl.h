/**
* Copyright (c) 2016-present, Facebook, Inc.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
/* Modifications Copyright (c) Microsoft. */

#pragma once

#include "core/providers/cpu/nn/conv.h"
#include "core/util/math.h"
#include "core/util/math_cpuonly.h"

#if defined(USE_MLAS)
#include <windows.h>
#include <mlas.h>
#endif

namespace Lotus {

template <typename T>
Status Conv<T>::Compute(OpKernelContext* context) const {
  size_t num_inputs = OpKernel::Node().InputDefs().size();
  bool Is2DKernel = kernel_shape_.size() == 2;
  const Tensor* X = context->Input<Tensor>(0);
  const Tensor* W = context->Input<Tensor>(1);
  const Tensor* B = num_inputs == 3 ? context->Input<Tensor>(2) : nullptr;
  const int64_t N = X->Shape()[0];
  const int64_t C = X->Shape()[1];
  const int64_t M = W->Shape()[0];

  vector<int64_t> pads(pads_);  // copy pads since it can be modified by InferOutputShape
  vector<int64_t> Y_dims;
  Y_dims.insert(Y_dims.begin(), {N, M});
  TensorShape input_shape = X->Shape().Slice(2);
  InferOutputShape(input_shape, &pads, &Y_dims);
  Tensor* Y = context->Output(0, TensorShape(Y_dims));
  TensorShape output_shape = Y->Shape().Slice(2);

  const int64_t input_image_size = input_shape.Size();
  const int64_t output_image_size = output_shape.Size();
  const int64_t kernel_size = TensorShape(kernel_shape_).Size();
  const int64_t X_offset = C / group_ * input_image_size;
  const int64_t Y_offset = Y->Shape().Size() / Y->Shape()[0] / group_;
  const int64_t W_offset = W->Shape().Size() / group_;
  const int64_t kernel_dim = C / group_ * kernel_size;
  const int64_t col_buffer_size = kernel_dim * output_image_size;

  AllocatorPtr alloc;
  LOTUS_RETURN_IF_ERROR(context->GetTempSpaceAllocator(&alloc));

  auto col_data = alloc->Alloc(sizeof(T) * col_buffer_size);
  BufferUniquePtr col_buffer(col_data, BufferDeleter(alloc));
  T* col_buffer_data = static_cast<T*>(col_buffer.get());

  const T* Xdata = X->template Data<T>();
  T* Ydata = Y->template MutableData<T>();

  TensorShape image_shape = X->Shape().Slice(1);
  vector<int64_t> col_buffer_shape{kernel_dim};
  col_buffer_shape.insert(col_buffer_shape.end(), output_shape.GetDims().begin(),
                          output_shape.GetDims().end());

  for (int image_id = 0; image_id < N; ++image_id) {
    for (int group_id = 0; group_id < group_; ++group_id) {
      if (Is2DKernel) {
        Math::Im2col<T, CPUMathUtil, StorageOrder::NCHW>(
            Xdata + group_id * X_offset,
            C / group_,
            input_shape[0],
            input_shape[1],
            kernel_shape_[0],
            kernel_shape_[1],
            dilations_[0],
            dilations_[1],
            pads[0],
            pads[1],
            pads[2],
            pads[3],
            strides_[0],
            strides_[1],
            col_buffer_data,
            &CPUMathUtil::Instance());
      } else {
        Math::Im2colNd<T, CPUMathUtil, StorageOrder::NCHW>(
            Xdata + group_id * X_offset,
            image_shape.GetDims().data(),
            col_buffer_shape.data(),
            C * input_image_size,
            col_buffer_size,
            kernel_shape_.data(),
            strides_.data(),
            dilations_.data(),
            pads.data(),
            static_cast<int>(kernel_shape_.size()),
            col_buffer_data,
            &CPUMathUtil::Instance());
      }
      Math::Gemm<T, CPUMathUtil>(
          CblasNoTrans,
          CblasNoTrans,
          M / group_,
          output_image_size,
          kernel_dim,
          1,
          W->template Data<T>() + group_id * W_offset,
          col_buffer_data,
          0,
          Ydata + group_id * Y_offset,
          &CPUMathUtil::Instance());
    }

    if (B != nullptr) {
      auto Ymatrix = EigenMatrixMap<T>(Ydata, output_image_size, M);
      auto Bvec = ConstEigenVectorMap<T>(B->template Data<T>(), M);
      Ymatrix.rowwise() += Bvec.transpose();
    }

    Xdata += X_offset * group_;
    Ydata += Y_offset * group_;
  }

  return Status::OK();
}

/* Use the high performance CPU convolution implementation. */
#if defined(USE_MLAS)
template <>
Status Conv<float>::Compute(OpKernelContext* context) const {
  size_t num_inputs = OpKernel::Node().InputDefs().size();
  bool Is2DKernel = kernel_shape_.size() == 2;
  const Tensor* X = context->Input<Tensor>(0);
  const Tensor* W = context->Input<Tensor>(1);
  const Tensor* B = num_inputs == 3 ? context->Input<Tensor>(2) : nullptr;
  const int64_t N = X->Shape()[0];
  const int64_t C = X->Shape()[1];
  const int64_t M = W->Shape()[0];

  vector<int64_t> pads(pads_);  // copy pads since it can be modified by InferOutputShape
  vector<int64_t> Y_dims;
  Y_dims.insert(Y_dims.begin(), {N, M});
  TensorShape input_shape = X->Shape().Slice(2);
  InferOutputShape(input_shape, &pads, &Y_dims);
  Tensor* Y = context->Output(0, TensorShape(Y_dims));
  TensorShape output_shape = Y->Shape().Slice(2);

  const int64_t input_image_size = input_shape.Size();
  const int64_t output_image_size = output_shape.Size();
  const int64_t kernel_size = TensorShape(kernel_shape_).Size();
  const int64_t X_offset = C / group_ * input_image_size;
  const int64_t Y_offset = Y->Shape().Size() / Y->Shape()[0] / group_;
  const int64_t W_offset = W->Shape().Size() / group_;
  const int64_t kernel_dim = C / group_ * kernel_size;
  const int64_t col_buffer_size = kernel_dim * output_image_size;

  AllocatorPtr alloc;
  LOTUS_RETURN_IF_ERROR(context->GetTempSpaceAllocator(&alloc));

  const float* Xdata = X->template Data<float>();
  float* Ydata = Y->template MutableData<float>();

  TensorShape image_shape = X->Shape().Slice(1);
  vector<int64_t> col_buffer_shape{kernel_dim};
  col_buffer_shape.insert(col_buffer_shape.end(), output_shape.GetDims().begin(),
                          output_shape.GetDims().end());

  MLAS_CONV2D_PARAMETERS Parameters;
  SIZE_T WorkingBufferSize;

  if (Is2DKernel) {
    MlasConv2DPrepare(
        &Parameters,
        C / group_,
        input_shape[0],
        input_shape[1],
        kernel_shape_[0],
        kernel_shape_[1],
        dilations_[0],
        dilations_[1],
        pads[0],
        pads[1],
        pads[2],
        pads[3],
        strides_[0],
        strides_[1],
        M / group_,
        &WorkingBufferSize);
  } else {
    WorkingBufferSize = col_buffer_size;
  }

  auto col_data = alloc->Alloc(sizeof(float) * WorkingBufferSize);
  BufferUniquePtr col_buffer(col_data, BufferDeleter(alloc));
  float* col_buffer_data = static_cast<float*>(col_buffer.get());

  for (int image_id = 0; image_id < N; ++image_id) {
    for (int group_id = 0; group_id < group_; ++group_id) {
      if (Is2DKernel) {
        MlasConv2D(
            &Parameters,
            Xdata + group_id * X_offset,
            W->template Data<float>() + group_id * W_offset,
            col_buffer_data,
            Ydata + group_id * Y_offset);
      } else {
        Math::Im2colNd<float, CPUMathUtil, StorageOrder::NCHW>(
            Xdata + group_id * X_offset,
            image_shape.GetDims().data(),
            col_buffer_shape.data(),
            C * input_image_size,
            col_buffer_size,
            kernel_shape_.data(),
            strides_.data(),
            dilations_.data(),
            pads.data(),
            static_cast<int>(kernel_shape_.size()),
            col_buffer_data,
            &CPUMathUtil::Instance());
        Math::Gemm<float, CPUMathUtil>(
            CblasNoTrans,
            CblasNoTrans,
            M / group_,
            output_image_size,
            kernel_dim,
            1,
            W->template Data<float>() + group_id * W_offset,
            col_buffer_data,
            0,
            Ydata + group_id * Y_offset,
            &CPUMathUtil::Instance());
      }
    }

    if (B != nullptr) {
      auto Ymatrix = EigenMatrixMap<float>(Ydata, output_image_size, M);
      auto Bvec = ConstEigenVectorMap<float>(B->template Data<float>(), M);
      Ymatrix.rowwise() += Bvec.transpose();
    }

    Xdata += X_offset * group_;
    Ydata += Y_offset * group_;
  }

  return Status::OK();
}
#endif

}  // namespace Lotus