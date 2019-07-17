/*******************************************************************************
* Copyright 2019 Intel Corporation
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
*******************************************************************************/

#include "common/c_types_map.hpp"
#include "common/math_utils.hpp"
#include "common/mkldnn_thread.hpp"
#include "common/mkldnn_traits.hpp"
#include "common/type_helpers.hpp"

#include "ocl/jit_gen9_common_convolution.hpp"

namespace mkldnn {
namespace impl {
namespace ocl {

using math::saturate;

template <data_type_t src_type, data_type_t wei_type, data_type_t dst_type,
        data_type_t acc_type>
status_t jit_gen9_common_convolution_fwd_t<src_type, wei_type, dst_type,
        acc_type>::execute_forward(const exec_ctx_t &ctx) const {
    auto *compute_stream
            = utils::downcast<compute::compute_stream_t *>(ctx.stream());

    auto &src = CTX_IN_STORAGE(MKLDNN_ARG_SRC);
    auto &weights = CTX_IN_STORAGE(MKLDNN_ARG_WEIGHTS);
    auto &bias = CTX_IN_STORAGE(MKLDNN_ARG_BIAS);
    auto &dst = CTX_OUT_STORAGE(MKLDNN_ARG_DST);

    const auto &jcp = ker_->jcp;

    compute::kernel_arg_list_t arg_list;
    arg_list.set(0, src);
    arg_list.set(1, weights);
    arg_list.set(2, bias);
    arg_list.set(3, dst);
    arg_list.set(4, jcp.relu_negative_slope);
    arg_list.set(5, jcp.sum_scale);

    if (src_type == data_type::u8) {
        float scales = pd()->attr()->output_scales_.scales_[0];
        arg_list.set(6, scales);
        arg_list.set(7, jcp.wht_slm_size, nullptr);
        arg_list.set(8, jcp.src_slm_size, nullptr);
    }

    auto nd_range = compute::nd_range_t(jcp.gws_d, jcp.lws_d);
    status_t status = compute_stream->parallel_for(nd_range, kernel_, arg_list);

    return status;
}

template <data_type_t diff_src_type, data_type_t wei_type,
        data_type_t diff_dst_type, data_type_t acc_type>
status_t
jit_gen9_common_convolution_bwd_data_t<diff_src_type, wei_type, diff_dst_type,
        acc_type>::execute_backward_data(const exec_ctx_t &ctx) const {
    auto *compute_stream
            = utils::downcast<compute::compute_stream_t *>(ctx.stream());

    auto &diff_dst = CTX_IN_STORAGE(MKLDNN_ARG_DIFF_DST);
    auto &weights = CTX_IN_STORAGE(MKLDNN_ARG_WEIGHTS);
    auto &diff_src = CTX_OUT_STORAGE(MKLDNN_ARG_DIFF_SRC);
    auto &bias = CTX_IN_STORAGE(MKLDNN_ARG_BIAS);

    const auto &jcp = ker_->jcp;

    compute::kernel_arg_list_t arg_list;
    arg_list.set(0, diff_src);
    arg_list.set(1, weights);
    arg_list.set(2, diff_dst);
    arg_list.set(3, bias);

    auto nd_range = compute::nd_range_t(jcp.gws_d, jcp.lws_d);
    status_t status = compute_stream->parallel_for(nd_range, kernel_, arg_list);

    return status;
}

template <data_type_t src_type, data_type_t diff_wei_type,
        data_type_t diff_dst_type, data_type_t acc_type>
status_t jit_gen9_common_convolution_bwd_weights_t<src_type, diff_wei_type,
        diff_dst_type, acc_type>::execute_backward_weights(const exec_ctx_t
                &ctx) const {
    auto *compute_stream
            = utils::downcast<compute::compute_stream_t *>(ctx.stream());

    auto &src = CTX_IN_STORAGE(MKLDNN_ARG_SRC);
    auto &diff_dst = CTX_IN_STORAGE(MKLDNN_ARG_DIFF_DST);
    auto &diff_weights = CTX_OUT_STORAGE(MKLDNN_ARG_DIFF_WEIGHTS);
    auto &diff_bias = CTX_OUT_STORAGE(MKLDNN_ARG_DIFF_BIAS);

    const auto &jcp = ker_->jcp;

    if (jcp.ver == ver_8ow16c) {
        compute::kernel_arg_list_t arg_list;
        arg_list.set(0, src);
        arg_list.set(1, *tails);
        status_t status = compute_stream->parallel_for(
                compute::nd_range_t({ 1 }), load_tails_, arg_list);
        if (status != status::success)
            return status;
    }

    compute::kernel_arg_list_t arg_list;
    arg_list.set(0, src);
    if (jcp.ver == ver_16mb16c || jcp.ver == ver_8ow16c
            || pd()->jcp_.ver == ver_1stconv) {
        arg_list.set(1, *wht_work);
        arg_list.set(2, *bias_work);
    } else {
        arg_list.set(1, diff_weights);
        arg_list.set(2, diff_bias);
    }
    arg_list.set(3, diff_dst);
    if (jcp.ver == ver_8ow16c) {
        arg_list.set(4, *tails);
    }

    status_t status = compute_stream->parallel_for(
            compute::nd_range_t(jcp.gws_d, jcp.lws_d), kernel_, arg_list);
    if (status != status::success)
        return status;

    if (jcp.ver == ver_16mb16c || jcp.ver == ver_8ow16c
            || pd()->jcp_.ver == ver_1stconv) {
        compute::kernel_arg_list_t arg_list;
        arg_list.set(0, diff_weights);
        arg_list.set(1, *wht_work);
        arg_list.set(2, diff_bias);
        arg_list.set(3, *bias_work);
        status_t status = compute_stream->parallel_for(
                compute::nd_range_t(2, jcp.gws_d, jcp.lws_d), reduce_kernel_,
                arg_list);
        if (status != status::success)
            return status;
    }

    return status::success;
}

using namespace data_type;

template struct jit_gen9_common_convolution_fwd_t<u8, s8, u8, s32>;
template struct jit_gen9_common_convolution_fwd_t<f16>;
template struct jit_gen9_common_convolution_fwd_t<f32>;
template struct jit_gen9_common_convolution_bwd_data_t<f16, f16, f16, f16>;
template struct jit_gen9_common_convolution_bwd_data_t<f32, f32, f32, f32>;
template struct jit_gen9_common_convolution_bwd_weights_t<f32, f32, f32, f32>;

} // namespace ocl
} // namespace impl
} // namespace mkldnn

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s