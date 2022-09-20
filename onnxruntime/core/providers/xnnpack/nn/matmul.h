// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/framework/op_kernel.h"
#include "core/framework/allocator.h"
#include "core/providers/cpu/math/gemm_base.h"
#include "core/providers/xnnpack/detail/utils.h"
#include "xnnpack.h"
#include "core/common/common.h"
#include "core/util/math.h"

namespace onnxruntime {
class GraphViewer;
class Node;
namespace xnnpack {

class MatMul : public OpKernel {
 public:
  MatMul(const OpKernelInfo& info);

  Status Compute(OpKernelContext* /*context*/) const override;

  // Required for checking XNNpack restrictions on ORT side
  static bool IsOnnxNodeSupported(const onnxruntime::Node& nchw_node, const GraphViewer& graph);

  Status PrePack(const Tensor& tensor, int input_idx, AllocatorPtr alloc,
                 /*out*/ bool& is_packed,
                 /*out*/ PrePackedWeights* prepacked_weights) override;

  Status CreateXnnpackOpp(
      size_t input_channels,
      size_t output_channels,
      size_t input_stride,
      size_t output_stride,
      const float* kernel,
      const float* bias,
      float output_min,
      float output_max,
      uint32_t flags);

private:
  TensorShape b_shape_;
  BufferUniquePtr packed_b_;
  AllocatorPtr myAlloc;
  float alpha_attr_;
  int64_t trans_a_attr_;
  int64_t trans_b_attr_;
  bool trans_batch_a_;
  bool trans_batch_b_;
  std::optional<std::pair<float, float>> clip_min_max_;
  XnnpackOperator op0_ = nullptr;

};

}  // namespace xnnpack
}  // namespace onnxruntime
