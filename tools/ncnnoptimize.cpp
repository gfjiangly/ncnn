// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2019 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include <set>
#include <vector>

// ncnn public header
#include "net.h"
#include "layer.h"

// ncnn private header
#include "layer/batchnorm.h"
#include "layer/bias.h"
#include "layer/binaryop.h"
#include "layer/clip.h"
#include "layer/concat.h"
#include "layer/convolution.h"
#include "layer/convolutiondepthwise.h"
#include "layer/crop.h"
#include "layer/deconvolution.h"
#include "layer/deconvolutiondepthwise.h"
#include "layer/detectionoutput.h"
#include "layer/dropout.h"
#include "layer/eltwise.h"
#include "layer/elu.h"
#include "layer/exp.h"
#include "layer/innerproduct.h"
#include "layer/input.h"
#include "layer/instancenorm.h"
#include "layer/interp.h"
#include "layer/log.h"
#include "layer/lrn.h"
#include "layer/mvn.h"
#include "layer/normalize.h"
#include "layer/padding.h"
#include "layer/permute.h"
#include "layer/pooling.h"
#include "layer/power.h"
#include "layer/prelu.h"
#include "layer/priorbox.h"
#include "layer/proposal.h"
#include "layer/psroipooling.h"
#include "layer/quantize.h"
#include "layer/reduction.h"
#include "layer/relu.h"
#include "layer/reorg.h"
#include "layer/requantize.h"
#include "layer/reshape.h"
#include "layer/roialign.h"
#include "layer/roipooling.h"
#include "layer/scale.h"
#include "layer/slice.h"
#include "layer/shufflechannel.h"
#include "layer/softmax.h"
#include "layer/threshold.h"
#include "layer/unaryop.h"
#include "layer/yolodetectionoutput.h"
#include "layer/yolov3detectionoutput.h"

class NetOptimize : public ncnn::Net
{
public:
    int fuse_batchnorm_scale();
    int fuse_convolution_batchnorm();
    int fuse_convolutiondepthwise_batchnorm();
    int fuse_deconvolution_batchnorm();
    int fuse_deconvolutiondepthwise_batchnorm();
    int fuse_innerproduct_batchnorm();
    int fuse_convolution_activation();
    int fuse_convolutiondepthwise_activation();
    int fuse_deconvolution_activation();
    int fuse_deconvolutiondepthwise_activation();
    int fuse_innerproduct_activation();

    int eliminate_dropout();

public:
    int fprintf_param_int_array(int id, const ncnn::Mat& m, FILE* pp);
    int fprintf_param_float_array(int id, const ncnn::Mat& m, FILE* pp);

    int fwrite_weight_tag(int tag, FILE* bp);
    int fwrite_weight_data(const ncnn::Mat& data, FILE* bp);

    int save(const char* parampath, const char* binpath);
};

int NetOptimize::fuse_batchnorm_scale()
{
    const int layer_count = layers.size();
    for (int i=0; i<layer_count; i++)
    {
        if (layers[i]->type != "BatchNorm")
            continue;

        // BatchNorm - Scale
        int top_blob_index = layers[i]->tops[0];

        int j = i + 1;
        for (; j<layer_count; j++)
        {
            if (layers[j]->type != "Scale")
                continue;

            if (layers[j]->bottoms.size() != 1)
                continue;

            if (layers[j]->bottoms[0] == top_blob_index)
                break;
        }

        if (j == layer_count)
            continue;

        // fuse BatchNorm - Scale to BatchNorm
        ncnn::BatchNorm* batchnorm = (ncnn::BatchNorm*)layers[i];
        ncnn::Scale* scale = (ncnn::Scale*)layers[j];

        fprintf(stderr, "fuse_batchnorm_scale %s %s\n", batchnorm->name.c_str(), scale->name.c_str());

        {
//             v = ((v - mean) / sqrt(var + eps) * slope + bias) * s + b
//               =  (v - mean) / sqrt(var + eps) * (slope * s) + (bias * s + b)

            int channels = batchnorm->channels;

            float* slope = batchnorm->slope_data;
            float* bias = batchnorm->bias_data;

            for (int q=0; q<channels; q++)
            {
                slope[q] = slope[q] * scale->scale_data[q];
                if (scale->bias_term)
                    bias[q] = bias[q] * scale->scale_data[q] + scale->bias_data[q];
                else
                    bias[q] = bias[q] * scale->scale_data[q];
            }
        }

        int top_blob_index_final = scale->tops[0];
        batchnorm->tops[0] = top_blob_index_final;
        blobs[top_blob_index_final].producer = i;
        scale->type = "ncnnfused";
    }

    return 0;
}

int NetOptimize::fuse_convolution_batchnorm()
{
    const int layer_count = layers.size();
    for (int i=0; i<layer_count; i++)
    {
        if (layers[i]->type != "Convolution")
            continue;

        // Convolution - BatchNorm
        int top_blob_index = layers[i]->tops[0];

        int j = i + 1;
        for (; j<layer_count; j++)
        {
            if (layers[j]->type != "BatchNorm")
                continue;

            if (layers[j]->bottoms.size() != 1)
                continue;

            if (layers[j]->bottoms[0] == top_blob_index)
                break;
        }

        if (j == layer_count)
            continue;

        // fuse Convolution - BatchNorm to Convolution
        ncnn::Convolution* convolution = (ncnn::Convolution*)layers[i];
        ncnn::BatchNorm* batchnorm = (ncnn::BatchNorm*)layers[j];

        fprintf(stderr, "fuse_convolution_batchnorm %s %s\n", convolution->name.c_str(), batchnorm->name.c_str());

        {
            int channels = batchnorm->channels;
            float eps = batchnorm->eps;

            // a = bias - slope * mean / sqrt(var + eps)
            // b = slope / sqrt(var + eps)
            // value = value * b + a

            std::vector<float> a(channels);
            std::vector<float> b(channels);
            for (int i=0; i<channels; i++)
            {
                float sqrt_var = sqrt(batchnorm->var_data[i] + eps);
                a[i] = batchnorm->bias_data[i] - batchnorm->slope_data[i] * batchnorm->mean_data[i] / sqrt_var;
                b[i] = batchnorm->slope_data[i] / sqrt_var;
            }

            if (convolution->bias_term == 0)
            {
                // init bias as zero
                convolution->bias_term = 1;
                convolution->bias_data = ncnn::Mat(channels);
                convolution->bias_data.fill(0.f);
            }

            const int weight_per_outch = convolution->weight_data_size / channels;

            float* weight = convolution->weight_data;
            float* bias = convolution->bias_data;
            for (int i=0; i<channels; i++)
            {
                float* conv_weight_outch = weight + weight_per_outch * i;
                for (int j=0; j<weight_per_outch; j++)
                {
                    conv_weight_outch[j] *= b[i];
                }

                bias[i] += a[i];
            }
        }

        int top_blob_index_final = batchnorm->tops[0];
        convolution->tops[0] = top_blob_index_final;
        blobs[top_blob_index_final].producer = i;
        batchnorm->type = "ncnnfused";
    }

    return 0;
}

int NetOptimize::fuse_convolutiondepthwise_batchnorm()
{
    const int layer_count = layers.size();
    for (int i=0; i<layer_count; i++)
    {
        if (layers[i]->type != "ConvolutionDepthWise")
            continue;

        // ConvolutionDepthWise - BatchNorm
        int top_blob_index = layers[i]->tops[0];

        int j = i + 1;
        for (; j<layer_count; j++)
        {
            if (layers[j]->type != "BatchNorm")
                continue;

            if (layers[j]->bottoms.size() != 1)
                continue;

            if (layers[j]->bottoms[0] == top_blob_index)
                break;
        }

        if (j == layer_count)
            continue;

        // fuse ConvolutionDepthWise - BatchNorm to ConvolutionDepthWise
        ncnn::ConvolutionDepthWise* convolutiondepthwise = (ncnn::ConvolutionDepthWise*)layers[i];
        ncnn::BatchNorm* batchnorm = (ncnn::BatchNorm*)layers[j];

        fprintf(stderr, "fuse_convolutiondepthwise_batchnorm %s %s\n", convolutiondepthwise->name.c_str(), batchnorm->name.c_str());

        {
            int channels = batchnorm->channels;
            float eps = batchnorm->eps;

            // a = bias - slope * mean / sqrt(var + eps)
            // b = slope / sqrt(var + eps)
            // value = value * b + a

            std::vector<float> a(channels);
            std::vector<float> b(channels);
            for (int i=0; i<channels; i++)
            {
                float sqrt_var = sqrt(batchnorm->var_data[i] + eps);
                a[i] = batchnorm->bias_data[i] - batchnorm->slope_data[i] * batchnorm->mean_data[i] / sqrt_var;
                b[i] = batchnorm->slope_data[i] / sqrt_var;
            }

            if (convolutiondepthwise->bias_term == 0)
            {
                // init bias as zero
                convolutiondepthwise->bias_term = 1;
                convolutiondepthwise->bias_data = ncnn::Mat(channels);
                convolutiondepthwise->bias_data.fill(0.f);
            }

            const int weight_per_outch = convolutiondepthwise->weight_data_size / channels;

            float* weight = convolutiondepthwise->weight_data;
            float* bias = convolutiondepthwise->bias_data;
            for (int i=0; i<channels; i++)
            {
                float* conv_weight_outch = weight + weight_per_outch * i;
                for (int j=0; j<weight_per_outch; j++)
                {
                    conv_weight_outch[j] *= b[i];
                }

                bias[i] += a[i];
            }
        }

        int top_blob_index_final = batchnorm->tops[0];
        convolutiondepthwise->tops[0] = top_blob_index_final;
        blobs[top_blob_index_final].producer = i;
        batchnorm->type = "ncnnfused";
    }

    return 0;
}

int NetOptimize::fuse_deconvolution_batchnorm()
{
    const int layer_count = layers.size();
    for (int i=0; i<layer_count; i++)
    {
        if (layers[i]->type != "Deconvolution")
            continue;

        // Deconvolution - BatchNorm
        int top_blob_index = layers[i]->tops[0];

        int j = i + 1;
        for (; j<layer_count; j++)
        {
            if (layers[j]->type != "BatchNorm")
                continue;

            if (layers[j]->bottoms.size() != 1)
                continue;

            if (layers[j]->bottoms[0] == top_blob_index)
                break;
        }

        if (j == layer_count)
            continue;

        // fuse Deconvolution - BatchNorm to Deconvolution
        ncnn::Deconvolution* deconvolution = (ncnn::Deconvolution*)layers[i];
        ncnn::BatchNorm* batchnorm = (ncnn::BatchNorm*)layers[j];

        fprintf(stderr, "fuse_deconvolution_batchnorm %s %s\n", deconvolution->name.c_str(), batchnorm->name.c_str());

        {
            int channels = batchnorm->channels;
            float eps = batchnorm->eps;

            // a = bias - slope * mean / sqrt(var + eps)
            // b = slope / sqrt(var + eps)
            // value = value * b + a

            std::vector<float> a(channels);
            std::vector<float> b(channels);
            for (int i=0; i<channels; i++)
            {
                float sqrt_var = sqrt(batchnorm->var_data[i] + eps);
                a[i] = batchnorm->bias_data[i] - batchnorm->slope_data[i] * batchnorm->mean_data[i] / sqrt_var;
                b[i] = batchnorm->slope_data[i] / sqrt_var;
            }

            if (deconvolution->bias_term == 0)
            {
                // init bias as zero
                deconvolution->bias_term = 1;
                deconvolution->bias_data = ncnn::Mat(channels);
                deconvolution->bias_data.fill(0.f);
            }

            const int weight_per_outch = deconvolution->weight_data_size / channels;

            float* weight = deconvolution->weight_data;
            float* bias = deconvolution->bias_data;
            for (int i=0; i<channels; i++)
            {
                float* conv_weight_outch = weight + weight_per_outch * i;
                for (int j=0; j<weight_per_outch; j++)
                {
                    conv_weight_outch[j] *= b[i];
                }

                bias[i] += a[i];
            }
        }

        int top_blob_index_final = batchnorm->tops[0];
        deconvolution->tops[0] = top_blob_index_final;
        blobs[top_blob_index_final].producer = i;
        batchnorm->type = "ncnnfused";
    }

    return 0;
}

int NetOptimize::fuse_deconvolutiondepthwise_batchnorm()
{
    const int layer_count = layers.size();
    for (int i=0; i<layer_count; i++)
    {
        if (layers[i]->type != "DeconvolutionDepthWise")
            continue;

        // DeconvolutionDepthWise - BatchNorm
        int top_blob_index = layers[i]->tops[0];

        int j = i + 1;
        for (; j<layer_count; j++)
        {
            if (layers[j]->type != "BatchNorm")
                continue;

            if (layers[j]->bottoms.size() != 1)
                continue;

            if (layers[j]->bottoms[0] == top_blob_index)
                break;
        }

        if (j == layer_count)
            continue;

        // fuse DeconvolutionDepthWise - BatchNorm to DeconvolutionDepthWise
        ncnn::DeconvolutionDepthWise* deconvolutiondepthwise = (ncnn::DeconvolutionDepthWise*)layers[i];
        ncnn::BatchNorm* batchnorm = (ncnn::BatchNorm*)layers[j];

        fprintf(stderr, "fuse_deconvolutiondepthwise_batchnorm %s %s\n", deconvolutiondepthwise->name.c_str(), batchnorm->name.c_str());

        {
            int channels = batchnorm->channels;
            float eps = batchnorm->eps;

            // a = bias - slope * mean / sqrt(var + eps)
            // b = slope / sqrt(var + eps)
            // value = value * b + a

            std::vector<float> a(channels);
            std::vector<float> b(channels);
            for (int i=0; i<channels; i++)
            {
                float sqrt_var = sqrt(batchnorm->var_data[i] + eps);
                a[i] = batchnorm->bias_data[i] - batchnorm->slope_data[i] * batchnorm->mean_data[i] / sqrt_var;
                b[i] = batchnorm->slope_data[i] / sqrt_var;
            }

            if (deconvolutiondepthwise->bias_term == 0)
            {
                // init bias as zero
                deconvolutiondepthwise->bias_term = 1;
                deconvolutiondepthwise->bias_data = ncnn::Mat(channels);
                deconvolutiondepthwise->bias_data.fill(0.f);
            }

            const int weight_per_outch = deconvolutiondepthwise->weight_data_size / channels;

            float* weight = deconvolutiondepthwise->weight_data;
            float* bias = deconvolutiondepthwise->bias_data;
            for (int i=0; i<channels; i++)
            {
                float* conv_weight_outch = weight + weight_per_outch * i;
                for (int j=0; j<weight_per_outch; j++)
                {
                    conv_weight_outch[j] *= b[i];
                }

                bias[i] += a[i];
            }
        }

        int top_blob_index_final = batchnorm->tops[0];
        deconvolutiondepthwise->tops[0] = top_blob_index_final;
        blobs[top_blob_index_final].producer = i;
        batchnorm->type = "ncnnfused";
    }

    return 0;
}

int NetOptimize::fuse_innerproduct_batchnorm()
{
    const int layer_count = layers.size();
    for (int i=0; i<layer_count; i++)
    {
        if (layers[i]->type != "InnerProduct")
            continue;

        // InnerProduct - BatchNorm
        int top_blob_index = layers[i]->tops[0];

        int j = i + 1;
        for (; j<layer_count; j++)
        {
            if (layers[j]->type != "BatchNorm")
                continue;

            if (layers[j]->bottoms.size() != 1)
                continue;

            if (layers[j]->bottoms[0] == top_blob_index)
                break;
        }

        if (j == layer_count)
            continue;

        // fuse InnerProduct - BatchNorm to InnerProduct
        ncnn::InnerProduct* innerproduct = (ncnn::InnerProduct*)layers[i];
        ncnn::BatchNorm* batchnorm = (ncnn::BatchNorm*)layers[j];

        fprintf(stderr, "fuse_innerproduct_batchnorm %s %s\n", innerproduct->name.c_str(), batchnorm->name.c_str());

        {
            int channels = batchnorm->channels;
            float eps = batchnorm->eps;

            // a = bias - slope * mean / sqrt(var + eps)
            // b = slope / sqrt(var + eps)
            // value = value * b + a

            std::vector<float> a(channels);
            std::vector<float> b(channels);
            for (int i=0; i<channels; i++)
            {
                float sqrt_var = sqrt(batchnorm->var_data[i] + eps);
                a[i] = batchnorm->bias_data[i] - batchnorm->slope_data[i] * batchnorm->mean_data[i] / sqrt_var;
                b[i] = batchnorm->slope_data[i] / sqrt_var;
            }

            if (innerproduct->bias_term == 0)
            {
                // init bias as zero
                innerproduct->bias_term = 1;
                innerproduct->bias_data = ncnn::Mat(channels);
                innerproduct->bias_data.fill(0.f);
            }

            const int weight_per_outch = innerproduct->weight_data_size / channels;

            float* weight = innerproduct->weight_data;
            float* bias = innerproduct->bias_data;
            for (int i=0; i<channels; i++)
            {
                float* conv_weight_outch = weight + weight_per_outch * i;
                for (int j=0; j<weight_per_outch; j++)
                {
                    conv_weight_outch[j] *= b[i];
                }

                bias[i] += a[i];
            }
        }

        int top_blob_index_final = batchnorm->tops[0];
        innerproduct->tops[0] = top_blob_index_final;
        blobs[top_blob_index_final].producer = i;
        batchnorm->type = "ncnnfused";
    }

    return 0;
}

int NetOptimize::fuse_convolution_activation()
{
    const int layer_count = layers.size();
    for (int i=0; i<layer_count; i++)
    {
        if (layers[i]->type != "Convolution")
            continue;

        // Convolution - Activation
        int top_blob_index = layers[i]->tops[0];

        int j = i + 1;
        for (; j<layer_count; j++)
        {
            if (layers[j]->type != "ReLU" && layers[j]->type != "Clip")
                continue;

            if (layers[j]->bottoms.size() != 1)
                continue;

            if (layers[j]->bottoms[0] == top_blob_index)
                break;
        }

        if (j == layer_count)
            continue;

        // fuse Convolution - Activation to Convolution
        ncnn::Convolution* convolution = (ncnn::Convolution*)layers[i];
        ncnn::Layer* activation = layers[j];

        fprintf(stderr, "fuse_convolution_activation %s %s\n", convolution->name.c_str(), activation->name.c_str());

        if (activation->type == "ReLU")
        {
            ncnn::ReLU* relu = (ncnn::ReLU*)activation;

            if (relu->slope == 0.f)
            {
                convolution->activation_type = 1;
            }
            else
            {
                convolution->activation_type = 2;
                convolution->activation_params = ncnn::Mat(1);
                convolution->activation_params[0] = relu->slope;
            }
        }
        else if (activation->type == "Clip")
        {
            ncnn::Clip* clip = (ncnn::Clip*)activation;

            convolution->activation_type = 3;
            convolution->activation_params = ncnn::Mat(2);
            convolution->activation_params[0] = clip->min;
            convolution->activation_params[1] = clip->max;
        }

        int top_blob_index_final = activation->tops[0];
        convolution->tops[0] = top_blob_index_final;
        blobs[top_blob_index_final].producer = i;
        activation->type = "ncnnfused";
    }

    return 0;
}

int NetOptimize::fuse_convolutiondepthwise_activation()
{
    const int layer_count = layers.size();
    for (int i=0; i<layer_count; i++)
    {
        if (layers[i]->type != "ConvolutionDepthWise")
            continue;

        // ConvolutionDepthWise - Activation
        int top_blob_index = layers[i]->tops[0];

        int j = i + 1;
        for (; j<layer_count; j++)
        {
            if (layers[j]->type != "ReLU" && layers[j]->type != "Clip")
                continue;

            if (layers[j]->bottoms.size() != 1)
                continue;

            if (layers[j]->bottoms[0] == top_blob_index)
                break;
        }

        if (j == layer_count)
            continue;

        // fuse ConvolutionDepthWise - Activation to ConvolutionDepthWise
        ncnn::ConvolutionDepthWise* convolutiondepthwise = (ncnn::ConvolutionDepthWise*)layers[i];
        ncnn::Layer* activation = layers[j];

        fprintf(stderr, "fuse_convolutiondepthwise_activation %s %s\n", convolutiondepthwise->name.c_str(), activation->name.c_str());

        if (activation->type == "ReLU")
        {
            ncnn::ReLU* relu = (ncnn::ReLU*)activation;

            if (relu->slope == 0.f)
            {
                convolutiondepthwise->activation_type = 1;
            }
            else
            {
                convolutiondepthwise->activation_type = 2;
                convolutiondepthwise->activation_params = ncnn::Mat(1);
                convolutiondepthwise->activation_params[0] = relu->slope;
            }
        }
        else if (activation->type == "Clip")
        {
            ncnn::Clip* clip = (ncnn::Clip*)activation;

            convolutiondepthwise->activation_type = 3;
            convolutiondepthwise->activation_params = ncnn::Mat(2);
            convolutiondepthwise->activation_params[0] = clip->min;
            convolutiondepthwise->activation_params[1] = clip->max;
        }

        int top_blob_index_final = activation->tops[0];
        convolutiondepthwise->tops[0] = top_blob_index_final;
        blobs[top_blob_index_final].producer = i;
        activation->type = "ncnnfused";
    }

    return 0;
}

int NetOptimize::fuse_deconvolution_activation()
{
    const int layer_count = layers.size();
    for (int i=0; i<layer_count; i++)
    {
        if (layers[i]->type != "Deconvolution")
            continue;

        // Deconvolution - Activation
        int top_blob_index = layers[i]->tops[0];

        int j = i + 1;
        for (; j<layer_count; j++)
        {
            if (layers[j]->type != "ReLU" && layers[j]->type != "Clip")
                continue;

            if (layers[j]->bottoms.size() != 1)
                continue;

            if (layers[j]->bottoms[0] == top_blob_index)
                break;
        }

        if (j == layer_count)
            continue;

        // fuse Deconvolution - Activation to Deconvolution
        ncnn::Deconvolution* deconvolution = (ncnn::Deconvolution*)layers[i];
        ncnn::Layer* activation = layers[j];

        fprintf(stderr, "fuse_deconvolution_activation %s %s\n", deconvolution->name.c_str(), activation->name.c_str());

        if (activation->type == "ReLU")
        {
            ncnn::ReLU* relu = (ncnn::ReLU*)activation;

            if (relu->slope == 0.f)
            {
                deconvolution->activation_type = 1;
            }
            else
            {
                deconvolution->activation_type = 2;
                deconvolution->activation_params = ncnn::Mat(1);
                deconvolution->activation_params[0] = relu->slope;
            }
        }
        else if (activation->type == "Clip")
        {
            ncnn::Clip* clip = (ncnn::Clip*)activation;

            deconvolution->activation_type = 3;
            deconvolution->activation_params = ncnn::Mat(2);
            deconvolution->activation_params[0] = clip->min;
            deconvolution->activation_params[1] = clip->max;
        }

        int top_blob_index_final = activation->tops[0];
        deconvolution->tops[0] = top_blob_index_final;
        blobs[top_blob_index_final].producer = i;
        activation->type = "ncnnfused";
    }

    return 0;
}

int NetOptimize::fuse_deconvolutiondepthwise_activation()
{
    const int layer_count = layers.size();
    for (int i=0; i<layer_count; i++)
    {
        if (layers[i]->type != "DeconvolutionDepthWise")
            continue;

        // DeconvolutionDepthWise - Activation
        int top_blob_index = layers[i]->tops[0];

        int j = i + 1;
        for (; j<layer_count; j++)
        {
            if (layers[j]->type != "ReLU" && layers[j]->type != "Clip")
                continue;

            if (layers[j]->bottoms.size() != 1)
                continue;

            if (layers[j]->bottoms[0] == top_blob_index)
                break;
        }

        if (j == layer_count)
            continue;

        // fuse DeconvolutionDepthWise - Activation to DeconvolutionDepthWise
        ncnn::DeconvolutionDepthWise* deconvolutiondepthwise = (ncnn::DeconvolutionDepthWise*)layers[i];
        ncnn::Layer* activation = layers[j];

        fprintf(stderr, "fuse_deconvolutiondepthwise_activation %s %s\n", deconvolutiondepthwise->name.c_str(), activation->name.c_str());

        if (activation->type == "ReLU")
        {
            ncnn::ReLU* relu = (ncnn::ReLU*)activation;

            if (relu->slope == 0.f)
            {
                deconvolutiondepthwise->activation_type = 1;
            }
            else
            {
                deconvolutiondepthwise->activation_type = 2;
                deconvolutiondepthwise->activation_params = ncnn::Mat(1);
                deconvolutiondepthwise->activation_params[0] = relu->slope;
            }
        }
        else if (activation->type == "Clip")
        {
            ncnn::Clip* clip = (ncnn::Clip*)activation;

            deconvolutiondepthwise->activation_type = 3;
            deconvolutiondepthwise->activation_params = ncnn::Mat(2);
            deconvolutiondepthwise->activation_params[0] = clip->min;
            deconvolutiondepthwise->activation_params[1] = clip->max;
        }

        int top_blob_index_final = activation->tops[0];
        deconvolutiondepthwise->tops[0] = top_blob_index_final;
        blobs[top_blob_index_final].producer = i;
        activation->type = "ncnnfused";
    }

    return 0;
}

int NetOptimize::fuse_innerproduct_activation()
{
    const int layer_count = layers.size();
    for (int i=0; i<layer_count; i++)
    {
        if (layers[i]->type != "InnerProduct")
            continue;

        // InnerProduct - Activation
        int top_blob_index = layers[i]->tops[0];

        int j = i + 1;
        for (; j<layer_count; j++)
        {
            if (layers[j]->type != "ReLU" && layers[j]->type != "Clip")
                continue;

            if (layers[j]->bottoms.size() != 1)
                continue;

            if (layers[j]->bottoms[0] == top_blob_index)
                break;
        }

        if (j == layer_count)
            continue;

        // fuse InnerProduct - Activation to InnerProduct
        ncnn::InnerProduct* innerproduct = (ncnn::InnerProduct*)layers[i];
        ncnn::Layer* activation = layers[j];

        fprintf(stderr, "fuse_innerproduct_activation %s %s\n", innerproduct->name.c_str(), activation->name.c_str());

        if (activation->type == "ReLU")
        {
            ncnn::ReLU* relu = (ncnn::ReLU*)activation;

            if (relu->slope == 0.f)
            {
                innerproduct->activation_type = 1;
            }
            else
            {
                innerproduct->activation_type = 2;
                innerproduct->activation_params = ncnn::Mat(1);
                innerproduct->activation_params[0] = relu->slope;
            }
        }
        else if (activation->type == "Clip")
        {
            ncnn::Clip* clip = (ncnn::Clip*)activation;

            innerproduct->activation_type = 3;
            innerproduct->activation_params = ncnn::Mat(2);
            innerproduct->activation_params[0] = clip->min;
            innerproduct->activation_params[1] = clip->max;
        }

        int top_blob_index_final = activation->tops[0];
        innerproduct->tops[0] = top_blob_index_final;
        blobs[top_blob_index_final].producer = i;
        activation->type = "ncnnfused";
    }

    return 0;
}

int NetOptimize::eliminate_dropout()
{
    const int layer_count = layers.size();
    for (int i=0; i<layer_count; i++)
    {
        if (layers[i]->type != "Dropout")
            continue;

        ncnn::Dropout* dropout = (ncnn::Dropout*)layers[i];
        if (dropout->scale != 1.f)
            continue;

        // Any - Dropout
        int bottom_blob_index = layers[i]->bottoms[0];

        int j = i - 1;
        for (; j>=0; j--)
        {
            if (layers[j]->type == "ncnnfused")
                continue;

            if (layers[j]->tops.size() != 1)
                continue;

            if (layers[j]->tops[0] == bottom_blob_index)
                break;
        }

        if (j == -1)
            continue;

        ncnn::Layer* any = layers[j];

        fprintf(stderr, "eliminate_dropout %s %s\n", any->name.c_str(), dropout->name.c_str());

        int top_blob_index_final = dropout->tops[0];
        any->tops[0] = top_blob_index_final;
        blobs[top_blob_index_final].producer = j;
        dropout->type = "ncnnfused";
    }

    return 0;
}

int NetOptimize::fprintf_param_int_array(int id, const ncnn::Mat& m, FILE* pp)
{
    const int count = m.w;
    const int* ptr = m;

    fprintf(pp, " -%d=%d", 23300 + id, count);
    for (int i=0; i<count; i++)
    {
        fprintf(pp, ",%d", ptr[i]);
    }

    return 0;
}

int NetOptimize::fprintf_param_float_array(int id, const ncnn::Mat& m, FILE* pp)
{
    const int count = m.w;
    const float* ptr = m;

    fprintf(pp, " -%d=%d", 23300 + id, count);
    for (int i=0; i<count; i++)
    {
        fprintf(pp, ",%f", ptr[i]);
    }

    return 0;
}

int NetOptimize::fwrite_weight_tag(int tag, FILE* bp)
{
    fwrite(&tag, sizeof(int), 1, bp);
    return 0;
}

int NetOptimize::fwrite_weight_data(const ncnn::Mat& data, FILE* bp)
{
    ncnn::Mat data_flattened = data.reshape(data.w * data.h * data.c);
    fwrite(data_flattened.data, data_flattened.elemsize, data_flattened.w, bp);
    return 0;
}

int NetOptimize::save(const char* parampath, const char* binpath)
{
    FILE* pp = fopen(parampath, "wb");
    FILE* bp = fopen(binpath, "wb");

    fprintf(pp, "7767517\n");

    const int layer_count = layers.size();

    int layer_count_fused = 0;
    std::set<std::string> blob_names;
    for (int i=0; i<layer_count; i++)
    {
        const ncnn::Layer* layer = layers[i];
        if (layer->type == "ncnnfused")
            continue;

        layer_count_fused++;

        int bottom_count = layer->bottoms.size();
        for (int j=0; j<bottom_count; j++)
        {
            int bottom_blob_index = layer->bottoms[j];
            blob_names.insert(blobs[bottom_blob_index].name);
        }

        int top_count = layer->tops.size();
        for (int j=0; j<top_count; j++)
        {
            int top_blob_index = layer->tops[j];
            blob_names.insert(blobs[top_blob_index].name);
        }
    }

    int blob_count_fused = blob_names.size();

    fprintf(pp, "%d %d\n", layer_count_fused, blob_count_fused);

    for (int i=0; i<layer_count; i++)
    {
        const ncnn::Layer* layer = layers[i];
        if (layer->type == "ncnnfused")
            continue;

        int bottom_count = layer->bottoms.size();
        int top_count = layer->tops.size();

        fprintf(pp, "%-24s %-24s %d %d", layer->type.c_str(), layer->name.c_str(), bottom_count, top_count);

        for (int j=0; j<bottom_count; j++)
        {
            int bottom_blob_index = layer->bottoms[j];
            fprintf(pp, " %s", blobs[bottom_blob_index].name.c_str());
        }
        for (int j=0; j<top_count; j++)
        {
            int top_blob_index = layer->tops[j];
            fprintf(pp, " %s", blobs[top_blob_index].name.c_str());
        }

        ncnn::Layer* layer_default = ncnn::create_layer(layer->typeindex);

        ncnn::ParamDict pd;
        layer_default->load_param(pd);

#define fprintf_param_value(format, phase) \
        { if (op->phase != op_default->phase) fprintf(pp, format, op->phase); }

        if (layer->type == "BatchNorm")
        {
            ncnn::BatchNorm* op = (ncnn::BatchNorm*)layer;
            ncnn::BatchNorm* op_default = (ncnn::BatchNorm*)layer_default;

            fprintf_param_value(" 0=%d", channels)
            fprintf_param_value(" 1=%f", eps)

            fwrite_weight_data(op->slope_data, bp);
            fwrite_weight_data(op->mean_data, bp);
            fwrite_weight_data(op->var_data, bp);
            fwrite_weight_data(op->bias_data, bp);
        }
        else if (layer->type == "Bias")
        {
            ncnn::Bias* op = (ncnn::Bias*)layer;
            ncnn::Bias* op_default = (ncnn::Bias*)layer_default;

            fprintf_param_value(" 0=%d", bias_data_size)

            fwrite_weight_data(op->bias_data, bp);
        }
        else if (layer->type == "BinaryOp")
        {
            ncnn::BinaryOp* op = (ncnn::BinaryOp*)layer;
            ncnn::BinaryOp* op_default = (ncnn::BinaryOp*)layer_default;

            fprintf_param_value(" 0=%d", op_type)
            fprintf_param_value(" 1=%d", with_scalar)
            fprintf_param_value(" 2=%f", b)
        }
        else if (layer->type == "Clip")
        {
            ncnn::Clip* op = (ncnn::Clip*)layer;
            ncnn::Clip* op_default = (ncnn::Clip*)layer_default;

            fprintf_param_value(" 0=%f", min)
            fprintf_param_value(" 1=%f", max)
        }
        else if (layer->type == "Concat")
        {
            ncnn::Concat* op = (ncnn::Concat*)layer;
            ncnn::Concat* op_default = (ncnn::Concat*)layer_default;

            fprintf_param_value(" 0=%d", axis)
        }
        else if (layer->type == "Convolution")
        {
            ncnn::Convolution* op = (ncnn::Convolution*)layer;
            ncnn::Convolution* op_default = (ncnn::Convolution*)layer_default;

            fprintf_param_value(" 0=%d", num_output)
            fprintf_param_value(" 1=%d", kernel_w)
            { if (op->kernel_h != op->kernel_w) fprintf(pp, " 11=%d", op->kernel_h); }
            fprintf_param_value(" 2=%d", dilation_w)
            { if (op->dilation_h != op->dilation_w) fprintf(pp, " 12=%d", op->dilation_h); }
            fprintf_param_value(" 3=%d", stride_w)
            { if (op->stride_h != op->stride_w) fprintf(pp, " 13=%d", op->stride_h); }
            fprintf_param_value(" 4=%d", pad_w)
            { if (op->pad_h != op->pad_w) fprintf(pp, " 14=%d", op->pad_h); }
            fprintf_param_value(" 5=%d", bias_term)
            fprintf_param_value(" 6=%d", weight_data_size)
            fprintf_param_value(" 8=%d", int8_scale_term)
            fprintf_param_value(" 9=%d", activation_type)
            { if (!op->activation_params.empty()) fprintf_param_int_array(10, op->activation_params, pp); }

            fwrite_weight_tag(0, bp);
            fwrite_weight_data(op->weight_data, bp);
            fwrite_weight_data(op->bias_data, bp);
        }
        else if (layer->type == "ConvolutionDepthWise")
        {
            ncnn::ConvolutionDepthWise* op = (ncnn::ConvolutionDepthWise*)layer;
            ncnn::ConvolutionDepthWise* op_default = (ncnn::ConvolutionDepthWise*)layer_default;

            fprintf_param_value(" 0=%d", num_output)
            fprintf_param_value(" 1=%d", kernel_w)
            { if (op->kernel_h != op->kernel_w) fprintf(pp, " 11=%d", op->kernel_h); }
            fprintf_param_value(" 2=%d", dilation_w)
            { if (op->dilation_h != op->dilation_w) fprintf(pp, " 12=%d", op->dilation_h); }
            fprintf_param_value(" 3=%d", stride_w)
            { if (op->stride_h != op->stride_w) fprintf(pp, " 13=%d", op->stride_h); }
            fprintf_param_value(" 4=%d", pad_w)
            { if (op->pad_h != op->pad_w) fprintf(pp, " 14=%d", op->pad_h); }
            fprintf_param_value(" 5=%d", bias_term)
            fprintf_param_value(" 6=%d", weight_data_size)
            fprintf_param_value(" 7=%d", group)
            fprintf_param_value(" 8=%d", int8_scale_term)
            fprintf_param_value(" 9=%d", activation_type)
            { if (!op->activation_params.empty()) fprintf_param_int_array(10, op->activation_params, pp); }

            fwrite_weight_tag(0, bp);
            fwrite_weight_data(op->weight_data, bp);
            fwrite_weight_data(op->bias_data, bp);
        }
        else if (layer->type == "Crop")
        {
            ncnn::Crop* op = (ncnn::Crop*)layer;
            ncnn::Crop* op_default = (ncnn::Crop*)layer_default;

            fprintf_param_value(" 0=%d", woffset)
            fprintf_param_value(" 1=%d", hoffset)
            fprintf_param_value(" 2=%d", coffset)
            fprintf_param_value(" 3=%d", outw)
            fprintf_param_value(" 4=%d", outh)
            fprintf_param_value(" 5=%d", outc)
        }
        else if (layer->type == "Deconvolution")
        {
            ncnn::Deconvolution* op = (ncnn::Deconvolution*)layer;
            ncnn::Deconvolution* op_default = (ncnn::Deconvolution*)layer_default;

            fprintf_param_value(" 0=%d", num_output)
            fprintf_param_value(" 1=%d", kernel_w)
            { if (op->kernel_h != op->kernel_w) fprintf(pp, " 11=%d", op->kernel_h); }
            fprintf_param_value(" 2=%d", dilation_w)
            { if (op->dilation_h != op->dilation_w) fprintf(pp, " 12=%d", op->dilation_h); }
            fprintf_param_value(" 3=%d", stride_w)
            { if (op->stride_h != op->stride_w) fprintf(pp, " 13=%d", op->stride_h); }
            fprintf_param_value(" 4=%d", pad_w)
            { if (op->pad_h != op->pad_w) fprintf(pp, " 14=%d", op->pad_h); }
            fprintf_param_value(" 5=%d", bias_term)
            fprintf_param_value(" 6=%d", weight_data_size)
            fprintf_param_value(" 9=%d", activation_type)
            { if (!op->activation_params.empty()) fprintf_param_int_array(10, op->activation_params, pp); }

            fwrite_weight_tag(0, bp);
            fwrite_weight_data(op->weight_data, bp);
            fwrite_weight_data(op->bias_data, bp);
        }
        else if (layer->type == "DeconvolutionDepthWise")
        {
            ncnn::DeconvolutionDepthWise* op = (ncnn::DeconvolutionDepthWise*)layer;
            ncnn::DeconvolutionDepthWise* op_default = (ncnn::DeconvolutionDepthWise*)layer_default;

            fprintf_param_value(" 0=%d", num_output)
            fprintf_param_value(" 1=%d", kernel_w)
            { if (op->kernel_h != op->kernel_w) fprintf(pp, " 11=%d", op->kernel_h); }
            fprintf_param_value(" 2=%d", dilation_w)
            { if (op->dilation_h != op->dilation_w) fprintf(pp, " 12=%d", op->dilation_h); }
            fprintf_param_value(" 3=%d", stride_w)
            { if (op->stride_h != op->stride_w) fprintf(pp, " 13=%d", op->stride_h); }
            fprintf_param_value(" 4=%d", pad_w)
            { if (op->pad_h != op->pad_w) fprintf(pp, " 14=%d", op->pad_h); }
            fprintf_param_value(" 5=%d", bias_term)
            fprintf_param_value(" 6=%d", weight_data_size)
            fprintf_param_value(" 7=%d", group)
            fprintf_param_value(" 9=%d", activation_type)
            { if (!op->activation_params.empty()) fprintf_param_int_array(10, op->activation_params, pp); }

            fwrite_weight_tag(0, bp);
            fwrite_weight_data(op->weight_data, bp);
            fwrite_weight_data(op->bias_data, bp);
        }
        else if (layer->type == "DetectionOutput")
        {
            ncnn::DetectionOutput* op = (ncnn::DetectionOutput*)layer;
            ncnn::DetectionOutput* op_default = (ncnn::DetectionOutput*)layer_default;

            fprintf_param_value(" 0=%d", num_class)
            fprintf_param_value(" 1=%f", nms_threshold)
            fprintf_param_value(" 2=%d", nms_top_k)
            fprintf_param_value(" 3=%d", keep_top_k)
            fprintf_param_value(" 4=%f", confidence_threshold)
            fprintf_param_value(" 5=%f", variances[0])
            fprintf_param_value(" 6=%f", variances[1])
            fprintf_param_value(" 7=%f", variances[2])
            fprintf_param_value(" 8=%f", variances[3])
        }
        else if (layer->type == "Dropout")
        {
            ncnn::Dropout* op = (ncnn::Dropout*)layer;
            ncnn::Dropout* op_default = (ncnn::Dropout*)layer_default;

            fprintf_param_value(" 0=%f", scale)
        }
        else if (layer->type == "Eltwise")
        {
            ncnn::Eltwise* op = (ncnn::Eltwise*)layer;
            ncnn::Eltwise* op_default = (ncnn::Eltwise*)layer_default;

            fprintf_param_value(" 0=%d", op_type)
            { if (!op->coeffs.empty()) fprintf_param_int_array(1, op->coeffs, pp); }
        }
        else if (layer->type == "ELU")
        {
            ncnn::ELU* op = (ncnn::ELU*)layer;
            ncnn::ELU* op_default = (ncnn::ELU*)layer_default;

            fprintf_param_value(" 0=%f", alpha)
        }
        else if (layer->type == "Exp")
        {
            ncnn::Exp* op = (ncnn::Exp*)layer;
            ncnn::Exp* op_default = (ncnn::Exp*)layer_default;

            fprintf_param_value(" 0=%f", base)
            fprintf_param_value(" 1=%f", scale)
            fprintf_param_value(" 2=%f", shift)
        }
        else if (layer->type == "InnerProduct")
        {
            ncnn::InnerProduct* op = (ncnn::InnerProduct*)layer;
            ncnn::InnerProduct* op_default = (ncnn::InnerProduct*)layer_default;

            fprintf_param_value(" 0=%d", num_output)
            fprintf_param_value(" 1=%d", bias_term)
            fprintf_param_value(" 2=%d", weight_data_size)
            fprintf_param_value(" 8=%d", int8_scale_term)
            fprintf_param_value(" 9=%d", activation_type)
            { if (!op->activation_params.empty()) fprintf_param_int_array(10, op->activation_params, pp); }

            fwrite_weight_tag(0, bp);
            fwrite_weight_data(op->weight_data, bp);
            fwrite_weight_data(op->bias_data, bp);
        }
        else if (layer->type == "Input")
        {
            ncnn::Input* op = (ncnn::Input*)layer;
            ncnn::Input* op_default = (ncnn::Input*)layer_default;

            fprintf_param_value(" 0=%d", w)
            fprintf_param_value(" 1=%d", h)
            fprintf_param_value(" 2=%d", c)
        }
        else if (layer->type == "InstanceNorm")
        {
            ncnn::InstanceNorm* op = (ncnn::InstanceNorm*)layer;
            ncnn::InstanceNorm* op_default = (ncnn::InstanceNorm*)layer_default;

            fprintf_param_value(" 0=%d", channels)
            fprintf_param_value(" 1=%f", eps)
        }
        else if (layer->type == "Interp")
        {
            ncnn::Interp* op = (ncnn::Interp*)layer;
            ncnn::Interp* op_default = (ncnn::Interp*)layer_default;

            fprintf_param_value(" 0=%d", resize_type)
            fprintf_param_value(" 1=%f", height_scale)
            fprintf_param_value(" 2=%f", width_scale)
            fprintf_param_value(" 3=%d", output_height)
            fprintf_param_value(" 4=%d", output_width)
        }
        else if (layer->type == "Log")
        {
            ncnn::Log* op = (ncnn::Log*)layer;
            ncnn::Log* op_default = (ncnn::Log*)layer_default;

            fprintf_param_value(" 0=%f", base)
            fprintf_param_value(" 1=%f", scale)
            fprintf_param_value(" 2=%f", shift)
        }
        else if (layer->type == "LRN")
        {
            ncnn::LRN* op = (ncnn::LRN*)layer;
            ncnn::LRN* op_default = (ncnn::LRN*)layer_default;

            fprintf_param_value(" 0=%d", region_type)
            fprintf_param_value(" 1=%d", local_size)
            fprintf_param_value(" 2=%f", alpha)
            fprintf_param_value(" 3=%f", beta)
            fprintf_param_value(" 4=%f", bias)
        }
        else if (layer->type == "MVN")
        {
            ncnn::MVN* op = (ncnn::MVN*)layer;
            ncnn::MVN* op_default = (ncnn::MVN*)layer_default;

            fprintf_param_value(" 0=%d", normalize_variance)
            fprintf_param_value(" 1=%d", across_channels)
            fprintf_param_value(" 2=%f", eps)
        }
        else if (layer->type == "Normalize")
        {
            ncnn::Normalize* op = (ncnn::Normalize*)layer;
            ncnn::Normalize* op_default = (ncnn::Normalize*)layer_default;

            fprintf_param_value(" 0=%d", across_spatial)
            fprintf_param_value(" 1=%d", channel_shared)
            fprintf_param_value(" 2=%f", eps)
            fprintf_param_value(" 3=%d", scale_data_size)
            fprintf_param_value(" 4=%d", across_channel)

            fwrite_weight_data(op->scale_data, bp);
        }
        else if (layer->type == "Padding")
        {
            ncnn::Padding* op = (ncnn::Padding*)layer;
            ncnn::Padding* op_default = (ncnn::Padding*)layer_default;

            fprintf_param_value(" 0=%d", top)
            fprintf_param_value(" 1=%d", bottom)
            fprintf_param_value(" 2=%d", left)
            fprintf_param_value(" 3=%d", right)
            fprintf_param_value(" 4=%d", type)
            fprintf_param_value(" 5=%f", value)
        }
        else if (layer->type == "Permute")
        {
            ncnn::Permute* op = (ncnn::Permute*)layer;
            ncnn::Permute* op_default = (ncnn::Permute*)layer_default;

            fprintf_param_value(" 0=%d", order_type)
        }
        else if (layer->type == "Pooling")
        {
            ncnn::Pooling* op = (ncnn::Pooling*)layer;
            ncnn::Pooling* op_default = (ncnn::Pooling*)layer_default;

            fprintf_param_value(" 0=%d", pooling_type)
            fprintf_param_value(" 1=%d", kernel_w)
            { if (op->kernel_h != op->kernel_w) fprintf(pp, " 11=%d", op->kernel_h); }
            fprintf_param_value(" 2=%d", stride_w)
            { if (op->stride_h != op->stride_w) fprintf(pp, " 12=%d", op->stride_h); }
            fprintf_param_value(" 3=%d", pad_left)
            { if (op->pad_top != op->pad_left) fprintf(pp, " 13=%d", op->pad_top); }
            { if (op->pad_right != op->pad_left) fprintf(pp, " 14=%d", op->pad_right); }
            { if (op->pad_bottom != op->pad_top) fprintf(pp, " 15=%d", op->pad_bottom); }
            fprintf_param_value(" 4=%d", global_pooling)
            fprintf_param_value(" 5=%d", pad_mode)
        }
        else if (layer->type == "Power")
        {
            ncnn::Power* op = (ncnn::Power*)layer;
            ncnn::Power* op_default = (ncnn::Power*)layer_default;

            fprintf_param_value(" 0=%f", power)
            fprintf_param_value(" 1=%f", scale)
            fprintf_param_value(" 2=%f", shift)
        }
        else if (layer->type == "PReLU")
        {
            ncnn::PReLU* op = (ncnn::PReLU*)layer;
            ncnn::PReLU* op_default = (ncnn::PReLU*)layer_default;

            fprintf_param_value(" 0=%d", num_slope)

            fwrite_weight_data(op->slope_data, bp);
        }
        else if (layer->type == "PriorBox")
        {
            ncnn::PriorBox* op = (ncnn::PriorBox*)layer;
            ncnn::PriorBox* op_default = (ncnn::PriorBox*)layer_default;

            { if (!op->min_sizes.empty()) fprintf_param_int_array(0, op->min_sizes, pp); }
            { if (!op->max_sizes.empty()) fprintf_param_int_array(1, op->max_sizes, pp); }
            { if (!op->aspect_ratios.empty()) fprintf_param_int_array(2, op->aspect_ratios, pp); }
            fprintf_param_value(" 3=%f", variances[0])
            fprintf_param_value(" 4=%f", variances[1])
            fprintf_param_value(" 5=%f", variances[2])
            fprintf_param_value(" 6=%f", variances[3])
            fprintf_param_value(" 7=%d", flip)
            fprintf_param_value(" 8=%d", clip)
            fprintf_param_value(" 9=%d", image_width)
            fprintf_param_value(" 10=%d", image_height)
            fprintf_param_value(" 11=%f", step_width)
            fprintf_param_value(" 12=%f", step_height)
            fprintf_param_value(" 13=%f", offset)
        }
        else if (layer->type == "Proposal")
        {
            ncnn::Proposal* op = (ncnn::Proposal*)layer;
            ncnn::Proposal* op_default = (ncnn::Proposal*)layer_default;

            fprintf_param_value(" 0=%d", feat_stride)
            fprintf_param_value(" 1=%d", base_size)
            fprintf_param_value(" 2=%d", pre_nms_topN)
            fprintf_param_value(" 3=%d", after_nms_topN)
            fprintf_param_value(" 4=%f", nms_thresh)
            fprintf_param_value(" 5=%d", min_size)
        }
        else if (layer->type == "PSROIPooling")
        {
            ncnn::PSROIPooling* op = (ncnn::PSROIPooling*)layer;
            ncnn::PSROIPooling* op_default = (ncnn::PSROIPooling*)layer_default;

            fprintf_param_value(" 0=%d", pooled_width)
            fprintf_param_value(" 1=%d", pooled_height)
            fprintf_param_value(" 2=%f", spatial_scale)
            fprintf_param_value(" 3=%d", output_dim)
        }
        else if (layer->type == "Quantize")
        {
            ncnn::Quantize* op = (ncnn::Quantize*)layer;
            ncnn::Quantize* op_default = (ncnn::Quantize*)layer_default;

            fprintf_param_value(" 0=%f", scale)
        }
        else if (layer->type == "Reduction")
        {
            ncnn::Reduction* op = (ncnn::Reduction*)layer;
            ncnn::Reduction* op_default = (ncnn::Reduction*)layer_default;

            fprintf_param_value(" 0=%d", operation)
            fprintf_param_value(" 1=%d", dim)
            fprintf_param_value(" 2=%f", coeff)
        }
        else if (layer->type == "ReLU")
        {
            ncnn::ReLU* op = (ncnn::ReLU*)layer;
            ncnn::ReLU* op_default = (ncnn::ReLU*)layer_default;

            fprintf_param_value(" 0=%f", slope)
        }
        else if (layer->type == "Reorg")
        {
            ncnn::Reorg* op = (ncnn::Reorg*)layer;
            ncnn::Reorg* op_default = (ncnn::Reorg*)layer_default;

            fprintf_param_value(" 0=%d", stride)
        }
        else if (layer->type == "Requantize")
        {
            ncnn::Requantize* op = (ncnn::Requantize*)layer;
            ncnn::Requantize* op_default = (ncnn::Requantize*)layer_default;

            fprintf_param_value(" 0=%f", scale_in)
            fprintf_param_value(" 1=%f", scale_out)
            fprintf_param_value(" 2=%d", bias_term)
            fprintf_param_value(" 3=%d", bias_data_size)
            fprintf_param_value(" 4=%d", fusion_relu)
        }
        else if (layer->type == "Reshape")
        {
            ncnn::Reshape* op = (ncnn::Reshape*)layer;
            ncnn::Reshape* op_default = (ncnn::Reshape*)layer_default;

            fprintf_param_value(" 0=%d", w)
            fprintf_param_value(" 1=%d", h)
            fprintf_param_value(" 2=%d", c)
            fprintf_param_value(" 3=%d", permute)
        }
        else if (layer->type == "ROIAlign")
        {
            ncnn::ROIAlign* op = (ncnn::ROIAlign*)layer;
            ncnn::ROIAlign* op_default = (ncnn::ROIAlign*)layer_default;

            fprintf_param_value(" 0=%d", pooled_width)
            fprintf_param_value(" 1=%d", pooled_height)
            fprintf_param_value(" 2=%f", spatial_scale)
        }
        else if (layer->type == "ROIPooling")
        {
            ncnn::ROIPooling* op = (ncnn::ROIPooling*)layer;
            ncnn::ROIPooling* op_default = (ncnn::ROIPooling*)layer_default;

            fprintf_param_value(" 0=%d", pooled_width)
            fprintf_param_value(" 1=%d", pooled_height)
            fprintf_param_value(" 2=%f", spatial_scale)
        }
        else if (layer->type == "Scale")
        {
            ncnn::Scale* op = (ncnn::Scale*)layer;
            ncnn::Scale* op_default = (ncnn::Scale*)layer_default;

            fprintf_param_value(" 0=%d", scale_data_size)
            fprintf_param_value(" 1=%d", bias_term)

            fwrite_weight_data(op->scale_data, bp);
            fwrite_weight_data(op->bias_data, bp);
        }
        else if (layer->type == "ShuffleChannel")
        {
            ncnn::ShuffleChannel* op = (ncnn::ShuffleChannel*)layer;
            ncnn::ShuffleChannel* op_default = (ncnn::ShuffleChannel*)layer_default;

            fprintf_param_value(" 0=%d", group)
        }
        else if (layer->type == "Slice")
        {
            ncnn::Slice* op = (ncnn::Slice*)layer;
            ncnn::Slice* op_default = (ncnn::Slice*)layer_default;

            { if (!op->slices.empty()) fprintf_param_int_array(0, op->slices, pp); }
            fprintf_param_value(" 1=%d", axis)
        }
        else if (layer->type == "Softmax")
        {
            ncnn::Softmax* op = (ncnn::Softmax*)layer;
            ncnn::Softmax* op_default = (ncnn::Softmax*)layer_default;

            fprintf_param_value(" 0=%d", axis)

            // HACK
            if (op->axis != 0)
            {
                int fixbug0 = 1;
                fprintf(pp, " 1=%d", fixbug0);
            }
        }
        else if (layer->type == "Threshold")
        {
            ncnn::Threshold* op = (ncnn::Threshold*)layer;
            ncnn::Threshold* op_default = (ncnn::Threshold*)layer_default;

            fprintf_param_value(" 0=%f", threshold)
        }
        else if (layer->type == "UnaryOp")
        {
            ncnn::UnaryOp* op = (ncnn::UnaryOp*)layer;
            ncnn::UnaryOp* op_default = (ncnn::UnaryOp*)layer_default;

            fprintf_param_value(" 0=%d", op_type)
        }
        else if (layer->type == "YoloDetectionOutput")
        {
            ncnn::YoloDetectionOutput* op = (ncnn::YoloDetectionOutput*)layer;
            ncnn::YoloDetectionOutput* op_default = (ncnn::YoloDetectionOutput*)layer_default;

            fprintf_param_value(" 0=%d", num_class)
            fprintf_param_value(" 1=%d", num_box)
            fprintf_param_value(" 2=%f", confidence_threshold)
            fprintf_param_value(" 3=%f", nms_threshold)
            { if (!op->biases.empty()) fprintf_param_int_array(4, op->biases, pp); }
        }
        else if (layer->type == "Yolov3DetectionOutput")
        {
            ncnn::Yolov3DetectionOutput* op = (ncnn::Yolov3DetectionOutput*)layer;
            ncnn::Yolov3DetectionOutput* op_default = (ncnn::Yolov3DetectionOutput*)layer_default;

            fprintf_param_value(" 0=%d", num_class)
            fprintf_param_value(" 1=%d", num_box)
            fprintf_param_value(" 2=%f", confidence_threshold)
            fprintf_param_value(" 3=%f", nms_threshold)
            { if (!op->biases.empty()) fprintf_param_int_array(4, op->biases, pp); }
            { if (!op->mask.empty()) fprintf_param_int_array(5, op->mask, pp); }
            { if (!op->anchors_scale.empty()) fprintf_param_int_array(6, op->anchors_scale, pp); }
        }

#undef fprintf_param_value

        fprintf(pp, "\n");

        delete layer_default;
    }

    fclose(pp);
    fclose(bp);

    return 0;
}

int main(int argc, char** argv)
{
    // in in out out 65535

    const char* inparam = argv[1];
    const char* inbin = argv[2];
    const char* outparam = argv[3];
    const char* outbin = argv[4];
    int flag = atoi(argv[5]);

    NetOptimize optimizer;
    optimizer.load_param(inparam);
    optimizer.load_model(inbin);

    optimizer.fuse_batchnorm_scale();
    optimizer.fuse_convolution_batchnorm();
    optimizer.fuse_convolutiondepthwise_batchnorm();
    optimizer.fuse_deconvolution_batchnorm();
    optimizer.fuse_deconvolutiondepthwise_batchnorm();
    optimizer.fuse_innerproduct_batchnorm();
    optimizer.fuse_convolution_activation();
    optimizer.fuse_convolutiondepthwise_activation();
    optimizer.fuse_deconvolution_activation();
    optimizer.fuse_deconvolutiondepthwise_activation();
    optimizer.fuse_innerproduct_activation();

    optimizer.eliminate_dropout();

    optimizer.save(outparam, outbin);

    return 0;
}
