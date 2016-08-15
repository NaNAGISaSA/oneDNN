/*******************************************************************************
* Copyright 2016 Intel Corporation
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

#include "mkl_dnn_types.h"

#include "c_types_map.hpp"
#include "reference_inner_product.hpp"
#include "type_helpers.hpp"

#define CHECK(f)               \
    do {                       \
        status_t status = f;   \
        if (status != success) \
            return status;     \
    } while (0)

namespace mkl_dnn {
namespace impl {
namespace cpu {

using namespace mkl_dnn::impl::status;
using namespace mkl_dnn::impl::prop_kind;
using namespace mkl_dnn::impl::precision;
using namespace mkl_dnn::impl::memory_format;
using namespace mkl_dnn::impl::primitive_kind;

template <impl::precision_t prec>
status_t reference_inner_product<prec>::execute_forward()
{
    auto obtain_ptr = [this](uint32_t idx) {
        const size_t oi = this->input()[idx].output_index;
        return reinterpret_cast<const data_t *>(
                this->input()[idx].primitive->output()[oi]->memory_const());
    };
    const data_t *src = obtain_ptr(0);
    const data_t *weights = obtain_ptr(1);
    const data_t *bias = _with_bias ? obtain_ptr(2) : nullptr;
    data_t *dst = reinterpret_cast<data_t *>(this->output()[0]->memory());

    const memory_desc_wrapper src_d(this->_ippd.src_primitive_desc.memory_desc),
            weights_d(this->_ippd.weights_primitive_desc.memory_desc),
            bias_d(this->_ippd.bias_primitive_desc.memory_desc),
            dst_d(this->_ippd.dst_primitive_desc.memory_desc);

    const uint32_t MB = src_d.dims()[0];
    const uint32_t OC = weights_d.dims()[0];
    const uint32_t IC = weights_d.dims()[1];

    const bool src_has_spatial = src_d.ndims() == 4;
    auto ker_has_spatial = [=](data_t *d, uint32_t mb, uint32_t oc) {
        const uint32_t KH = weights_d.dims()[2];
        const uint32_t KW = weights_d.dims()[3];
        for (uint32_t ic = 0; ic < IC; ++ic) {
            for (uint32_t kh = 0; kh < KH; ++kh) {
                for (uint32_t kw = 0; kw < KW; ++kw) {
                    *d += src[src_d.off(mb, ic, kh, kw)]
                        * weights[weights_d.off(oc, ic, kh, kw)];
                }
            }
        }
    };

    auto ker_no_spatial = [=](data_t *d, uint32_t mb, uint32_t oc) {
        for (uint32_t ic = 0; ic < IC; ++ic) {
            *d += src[src_d.off(mb, ic)] * weights[weights_d.off(oc, ic)];
        }
    };

#   pragma omp parallel for collapse(2)
    for (uint32_t mb = 0; mb < MB; ++mb) {
        for (uint32_t oc = 0; oc < OC; ++oc) {
            data_t *d = &dst[dst_d.off(mb, oc)];
            *d = bias ? bias[bias_d.off(oc)] : data_t(0);
            if (src_has_spatial) {
                ker_has_spatial(d, mb, oc);
            } else {
                ker_no_spatial(d, mb, oc);
            }
        }
    }

    return success;
}

template <impl::precision_t prec>
status_t reference_inner_product<prec>::execute_backward_data()
{
    return unimplemented;
}

template <impl::precision_t prec>
status_t reference_inner_product<prec>::execute_backward_weights()
{
    return unimplemented;
}

template <impl::precision_t prec>
status_t reference_inner_product<prec>::execute_backward_bias()
{
    return unimplemented;
}

template <impl::precision_t prec>
status_t reference_inner_product<prec>::primitive_desc_init(
        primitive_desc_t *primitive_desc, const op_desc_t &op_desc,
        const mkl_dnn::impl::engine &engine)
{
    if (op_desc._kind != primitive_kind::inner_product)
        return invalid_arguments;
    auto ip_d = op_desc.inner_product;

    if (ip_d.prop_kind != forward)
        return unimplemented;

    if (ip_d.weights_desc.tensor_desc.ndims !=
            ip_d.weights_desc.tensor_desc.ndims)
        return invalid_arguments;

    /* memory descriptors check and fill-in */
    if (ip_d.src_desc.format == any) {
        if (ip_d.src_desc.tensor_desc.ndims == 4) {
            CHECK(mkl_dnn_memory_desc_init(
                    &ip_d.src_desc, &ip_d.src_desc.tensor_desc, f32, nchw));
        } else if (ip_d.src_desc.tensor_desc.ndims == 2) {
            CHECK(mkl_dnn_memory_desc_init(
                    &ip_d.src_desc, &ip_d.src_desc.tensor_desc, f32, nc));
        } else {
            return unimplemented;
        }
    }
    if (ip_d.weights_desc.format == any) {
        if (ip_d.weights_desc.tensor_desc.ndims == 4) {
            CHECK(mkl_dnn_memory_desc_init(&ip_d.weights_desc,
                    &ip_d.weights_desc.tensor_desc, f32, oihw));
        } else if (ip_d.src_desc.tensor_desc.ndims == 2) {
            CHECK(mkl_dnn_memory_desc_init(&ip_d.weights_desc,
                    &ip_d.weights_desc.tensor_desc, f32, oi));
        } else {
            return unimplemented;
        }
    }
    const bool with_bias = !memory_desc_wrapper(ip_d.bias_desc).is_zero();
    if (with_bias && ip_d.bias_desc.format == any)
        CHECK(mkl_dnn_memory_desc_init(&ip_d.bias_desc,
                    &ip_d.bias_desc.tensor_desc, f32, x));
    if (ip_d.dst_desc.format == any)
        CHECK(mkl_dnn_memory_desc_init(
                &ip_d.dst_desc, &ip_d.dst_desc.tensor_desc, f32, nc));

    /* memory primitive descriptors check */
    memory_primitive_desc_t src_pd, weights_pd, bias_pd, dst_pd;
    CHECK(mkl_dnn_memory_primitive_desc_init(&src_pd, &ip_d.src_desc, &engine));
    CHECK(mkl_dnn_memory_primitive_desc_init(
            &weights_pd, &ip_d.weights_desc, &engine));
    CHECK(mkl_dnn_memory_primitive_desc_init(
            &bias_pd, &ip_d.bias_desc, &engine));
    CHECK(mkl_dnn_memory_primitive_desc_init(&dst_pd, &ip_d.dst_desc, &engine));

    /* final stage */
    inner_product_primitive_desc_t ippd;
    ippd.base.primitive_kind = inner_product;
    ippd.base.engine = &engine;
    ippd.base.implementation = reinterpret_cast<const void*>(&implementation);
    ippd.inner_product_desc = ip_d;
    ippd.src_primitive_desc = src_pd;
    ippd.weights_primitive_desc = weights_pd;
    ippd.bias_primitive_desc = bias_pd;
    ippd.dst_primitive_desc = dst_pd;

    // if (!inner_product_primitive_desc_is_ok(ippd)) return invalid_arguments; // ???

    primitive_desc->inner_product = ippd;

    return success;
}

namespace {
template <impl::precision_t prec>
status_t create(primitive **aprimitive,
        const primitive_desc_t *primitive_desc, const primitive_at_t inputs[],
        const primitive *outputs[])
{
    assert(primitive_desc->base.primitive_kind == inner_product);

    auto &ippd = primitive_desc->inner_product;
    // TODO: some checks here.

    *aprimitive = new reference_inner_product<prec>(ippd, inputs, outputs);
    return aprimitive ? success : out_of_memory;
}
}

template <impl::precision_t prec>
const primitive_impl reference_inner_product<prec>::implementation = {
    create<prec>
};

template class reference_inner_product<f32>;
}
}
}

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
