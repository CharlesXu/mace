// Copyright 2018 The MACE Authors. All Rights Reserved.
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

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "mace/core/ops/operator.h"
#include "mace/core/registry/ops_registry.h"
#include "mace/ops/common/coordinate_transformation_mode.h"
#include "mace/ops/common/utils.h"
#ifdef MACE_ENABLE_OPENCL
#include "mace/ops/opencl/image/resize_bicubic.h"
#endif  // MACE_ENABLE_OPENCL
#include "mace/utils/memory.h"

namespace mace {
namespace ops {

inline const std::shared_ptr<float> InitCoeffsTable(const float A) {
  // Allocate and initialize coefficients table using Bicubic
  // convolution algorithm.
  // https://en.wikipedia.org/wiki/Bicubic_interpolation
  auto coeffs_tab = std::shared_ptr<float>(
      new float[(common::utils::kTableSize + 1) * 2],
      std::default_delete<float[]>());
  float *coeffs_tab_ptr = coeffs_tab.get();
  for (int i = 0; i <= common::utils::kTableSize; ++i) {
    float x = i * 1.0f / common::utils::kTableSize;
    coeffs_tab_ptr[i * 2] = ((A + 2) * x - (A + 3)) * x * x + 1;
    x += 1.0;
    coeffs_tab_ptr[i * 2 + 1] = ((A * x - 5 * A) * x + 8 * A) * x - 4 * A;
  }
  return coeffs_tab;
}

inline const float *GetCoeffsTable(bool is_tensorflow_half_pixel = false) {
  // Static so that we initialize it on first use
  if (is_tensorflow_half_pixel) {
    static const std::shared_ptr<float> coeffs_tab = InitCoeffsTable(-0.5f);
    return coeffs_tab.get();
  } else {
    static const std::shared_ptr<float> coeffs_tab = InitCoeffsTable(-0.75f);
    return coeffs_tab.get();
  }
}

inline index_t Bound(index_t val, index_t limit) {
  return std::min<index_t>(limit - 1ll, std::max<index_t>(0ll, val));
}

inline void GetWeightsAndIndices(
    float scale,
    CoordinateTransformationMode coordinate_transformation_mode,
    index_t out_loc,
    index_t out_size,
    index_t limit,
    std::vector<float> *weights,
    std::vector<index_t> *indices) {
  float in = out_loc * scale;
  if (coordinate_transformation_mode == HALF_PIXEL) {
    in = (static_cast<float>(out_loc) + 0.5f) * scale - 0.5f;
  } else if (coordinate_transformation_mode == PYTORCH_HALF_PIXEL) {
    in = out_size > 1 ? (static_cast<float>(out_loc) + 0.5f) * scale - 0.5f : 0;
  }
  const index_t in_loc = std::floor(in);
  const float delta = in - in_loc;
  const index_t offset = lrintf(delta * common::utils::kTableSize);

  *indices = {Bound(in_loc - 1, limit), Bound(in_loc, limit),
              Bound(in_loc + 1, limit), Bound(in_loc + 2, limit)};
  if (coordinate_transformation_mode == HALF_PIXEL) {  // for TensorFlow >= 1.14
    static const float *coeffs_tab = GetCoeffsTable(true);
    weights->resize(4);
    (*weights)[0] =
        ((*indices)[0] == in_loc - 1 ? coeffs_tab[offset * 2 + 1] : 0.0f);
    (*weights)[1] =
        ((*indices)[1] == in_loc ? coeffs_tab[offset * 2] : 0.0f);
    (*weights)[2] =
        ((*indices)[2] == in_loc + 1
             ? coeffs_tab[(common::utils::kTableSize - offset) * 2]
             : 0.0f);
    (*weights)[3] =
        ((*indices)[3] == in_loc + 2
             ? coeffs_tab[(common::utils::kTableSize - offset) * 2 + 1]
             : 0.0f);
    const float weight_sum =
        (*weights)[0] + (*weights)[1] + (*weights)[2] + (*weights)[3];
    if (std::abs(weight_sum) >= 1000.0f * std::numeric_limits<float>::min()) {
      const float one_over_weight_sum = 1.0f / weight_sum;
      (*weights)[0] *= one_over_weight_sum;
      (*weights)[1] *= one_over_weight_sum;
      (*weights)[2] *= one_over_weight_sum;
      (*weights)[3] *= one_over_weight_sum;
    }
  } else {
    static const float *coeffs_tab = GetCoeffsTable(false);
    *weights = {coeffs_tab[offset * 2 + 1],
                coeffs_tab[offset * 2],
                coeffs_tab[(common::utils::kTableSize - offset) * 2],
                coeffs_tab[(common::utils::kTableSize - offset) * 2 + 1]};
  }
}

inline float Interpolate1D(const std::vector<float> &weights,
                           const std::vector<float> &values) {
  return values[0] * weights[0] + values[1] * weights[1] +
      values[2] * weights[2] + values[3] * weights[3];
}

inline void ResizeImage(
    const OpContext *context,
    const float *images,
    const index_t batch_size,
    const index_t in_height,
    const index_t in_width,
    const index_t out_height,
    const index_t out_width,
    const index_t channels,
    const float height_scale,
    const float width_scale,
    const CoordinateTransformationMode coordinate_transformation_mode,
    float *output) {
  utils::ThreadPool
      &thread_pool = context->device()->cpu_runtime()->thread_pool();

  thread_pool.Compute2D([=](index_t start0, index_t end0, index_t step0,
                            index_t start1, index_t end1, index_t step1) {
    for (index_t b = start0; b < end0; b += step0) {
      for (index_t y = start1; y < end1; y += step1) {
        std::vector<float> y_weights;
        std::vector<index_t> y_indices;
        GetWeightsAndIndices(height_scale, coordinate_transformation_mode, y,
                             out_height, in_height, &y_weights, &y_indices);
        for (index_t x = 0; x < out_width; ++x) {
          std::vector<float> x_weights;
          std::vector<index_t> x_indices;
          GetWeightsAndIndices(width_scale, coordinate_transformation_mode, x,
                               out_width, in_width, &x_weights, &x_indices);

          for (index_t c = 0; c < channels; ++c) {
            // Use a 4x4 patch to compute the interpolated output value at
            // (b, y, x, c).
            const float *channel_input_ptr =
                images + (b * channels + c) * in_height * in_width;
            float *channel_output_ptr =
                output + (b * channels + c) * out_height * out_width;
            std::vector<float> coeff(4, 0.0);
            for (index_t i = 0; i < 4; ++i) {
              const std::vector<float> values = {
                  channel_input_ptr[y_indices[i] * in_width + x_indices[0]],
                  channel_input_ptr[y_indices[i] * in_width + x_indices[1]],
                  channel_input_ptr[y_indices[i] * in_width + x_indices[2]],
                  channel_input_ptr[y_indices[i] * in_width + x_indices[3]]};
              coeff[i] = Interpolate1D(x_weights, values);
            }
            channel_output_ptr[y * out_width + x] =
                Interpolate1D(y_weights, coeff);
          }
        }
      }
    }
  }, 0, batch_size, 1, 0, out_height, 1);
}

template<DeviceType D, class T>
class ResizeBicubicOp;

template<>
class ResizeBicubicOp<DeviceType::CPU, float> : public Operation {
 public:
  explicit ResizeBicubicOp(OpConstructContext *context)
      : Operation(context),
        align_corners_(Operation::GetOptionalArg<bool>("align_corners", false)),
        coordinate_transformation_mode_(
            static_cast<CoordinateTransformationMode>(
                Operation::GetOptionalArg<int>("coordinate_transformation_mode",
                                               0))),
        size_(Operation::GetRepeatedArgs<index_t>("size", {-1, -1})) {}

  MaceStatus Run(OpContext *context) override {
    MACE_UNUSED(context);
    const Tensor *input = this->Input(0);
    Tensor *output = this->Output(0);

    MACE_CHECK(input->dim_size() == 4, "input must be 4-dimensional.",
               input->dim_size());
    const index_t batch = input->dim(0);
    const index_t channels = input->dim(1);
    const index_t in_height = input->dim(2);
    const index_t in_width = input->dim(3);

    index_t out_height = 0;
    index_t out_width = 0;
    if (size_.size() == 2 && size_[0] > 0 && size_[1] > 0) {
      out_height = size_[0];
      out_width = size_[1];
    } else {
      // For tensorflow's dynamic size tensor
      MACE_CHECK(InputSize() >= 2);
      common::utils::GetSizeParamFromTensor(Input(1), &out_height, &out_width);
    }
    std::vector<index_t> out_shape{batch, channels, out_height, out_width};
    MACE_RETURN_IF_ERROR(output->Resize(out_shape));

    Tensor::MappingGuard input_mapper(input);
    Tensor::MappingGuard output_mapper(output);
    const float *input_data = input->data<float>();
    float *output_data = output->mutable_data<float>();

    if (out_height == in_height && out_width == in_width) {
      std::copy(input_data,
                input_data + batch * channels * in_height * in_width,
                output_data);
      return MaceStatus::MACE_SUCCESS;
    }

    float height_scale =
        common::utils::CalculateResizeScale(in_height,
                                            out_height,
                                            align_corners_);
    float width_scale =
        common::utils::CalculateResizeScale(in_width,
                                            out_width,
                                            align_corners_);

    ResizeImage(context,
                input_data,
                batch,
                in_height,
                in_width,
                out_height,
                out_width,
                channels,
                height_scale,
                width_scale,
                coordinate_transformation_mode_,
                output_data);

    return MaceStatus::MACE_SUCCESS;
  }

 private:
  bool align_corners_;
  CoordinateTransformationMode coordinate_transformation_mode_;
  std::vector<index_t> size_;
};

#ifdef MACE_ENABLE_OPENCL
template<>
class ResizeBicubicOp<DeviceType::GPU, float> : public Operation {
 public:
  explicit ResizeBicubicOp(OpConstructContext *context)
      : Operation(context) {
    bool align_corners = Operation::GetOptionalArg<bool>(
        "align_corners", false);
    CoordinateTransformationMode coordinate_transformation_mode =
        static_cast<CoordinateTransformationMode>(
            Operation::GetOptionalArg<int>("coordinate_transformation_mode",
                                           0));
    size_ = Operation::GetRepeatedArgs<index_t>("size", {-1, -1});
    if (context->GetOpMemoryType() == MemoryType::GPU_IMAGE) {
      kernel_ = make_unique<opencl::image::ResizeBicubicKernel>(
          align_corners, coordinate_transformation_mode);
    } else {
      MACE_NOT_IMPLEMENTED;
    }
  }
  MaceStatus Run(OpContext *context) override {
    const Tensor *input = this->Input(0);
    Tensor *output = this->Output(0);
    MACE_CHECK(input->dim_size() == 4, "input must be 4-dimensional.",
               input->dim_size());

    index_t out_height = 0;
    index_t out_width = 0;
    if (size_.size() == 2 && size_[0] > 0 && size_[1] > 0) {
      out_height = size_[0];
      out_width = size_[1];
    } else {
      // For tensorflow's dynamic size tensor
      MACE_CHECK(InputSize() >= 2);
      common::utils::GetSizeParamFromTensor(Input(1), &out_height, &out_width);
    }

    return kernel_->Compute(context, input, out_height, out_width, output);
  }

 private:
  std::unique_ptr<OpenCLResizeBicubicKernel> kernel_;
  std::vector<index_t> size_;
};
#endif  // MACE_ENABLE_OPENCL

void RegisterResizeBicubic(OpRegistry *op_registry) {
  MACE_REGISTER_OP(op_registry, "ResizeBicubic", ResizeBicubicOp,
                   DeviceType::CPU, float);

  MACE_REGISTER_GPU_OP(op_registry, "ResizeBicubic", ResizeBicubicOp);

#ifdef MACE_ENABLE_OPENCL
  MACE_REGISTER_OP_CONDITION(
      op_registry,
      OpConditionBuilder("ResizeBicubic").SetInputMemoryTypeSetter(
          [](OpConditionContext *context) -> void {
            OpenCLUtil::SetOpenclInputToCpuBuffer(context, 1, DT_INT32);
          }));
#endif  // MACE_ENABLE_OPENCL
}

}  // namespace ops
}  // namespace mace
