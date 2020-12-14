//
//  CPUConvInt8.cpp
//  MNN
//
//  Created by MNN on 2019/5/17.
//  Copyright © 2018, Alibaba Group Holding Limited
//

#include "backend/cpu/CPUConvInt8.hpp"
#include <math.h>
#include "backend/cpu/CPUBackend.hpp"
#include "backend/cpu/compute/CommonOptFunction.h"
#include "backend/cpu/compute/ConvInt8_1xN.hpp"
#include "core/Concurrency.h"
#include "core/Macro.h"
#include "core/TensorUtils.hpp"
#include <math.h>
#include "compute/ConvInt83x3.hpp"
#include "compute/ConvolutionWinograd.hpp"

namespace MNN {

static void _fastIm2Col(int8_t* colAddr, const int8_t* inputOrigin,
                        const ConvolutionCommon::Im2ColParameter* im2colParameter, size_t xIndexStart,
                        size_t realDstCount) {
    const int col_buffer_size = im2colParameter->kernelCountUnit * GEMM_INT8_DST_XUNIT * GEMM_INT8_SRC_UNIT * sizeof(int8_t);
    ::memset(colAddr, 0, col_buffer_size);
    const int icDiv8   = im2colParameter->icDiv4 / 2;
    const int srcZStep = im2colParameter->iw * im2colParameter->ih * 4;
    inputOrigin += xIndexStart * GEMM_INT8_UNIT;
    for (int i = 0; i < realDstCount; ++i) {
        auto colAddrI = colAddr + GEMM_INT8_SRC_UNIT * i;
        auto inputK   = inputOrigin + GEMM_INT8_UNIT * i;
        for (int sz = 0; sz < icDiv8; ++sz) {
            auto inputZ0           = inputK + srcZStep * (2 * sz + 0);
            auto inputZ1           = inputK + srcZStep * (2 * sz + 1);
            const int indexOutside = sz / 2;
            const int indexInsize  = sz % 2;

            auto dstK0         = colAddrI + (indexOutside * GEMM_INT8_DST_XUNIT * 2 + indexInsize) * (2 * GEMM_INT8_UNIT);
            auto dstK1         = dstK0 + GEMM_INT8_UNIT;
            *((int32_t*)dstK0) = *((int32_t*)inputZ0);
            *((int32_t*)dstK1) = *((int32_t*)inputZ1);
        }
    }
}

static void _im2colCommonZ1(int8_t* colAddr, const int8_t* inputOrigin,
                            const ConvolutionCommon::Im2ColParameter* im2colParameter, size_t xIndexStart,
                            size_t realDstCount) {
    int col_buffer_size = im2colParameter->kernelCountUnit * GEMM_INT8_DST_XUNIT * GEMM_INT8_SRC_UNIT * sizeof(int8_t);
    ::memset(colAddr, 0, col_buffer_size);
    auto ih                     = im2colParameter->ih;
    auto iw                     = im2colParameter->iw;
    auto kh                     = im2colParameter->kernelY;
    auto kw                     = im2colParameter->kernelX;
    auto dilateX                = im2colParameter->dilateX;
    auto dilateY                = im2colParameter->dilateY;
    constexpr int dstXStepInt32 = GEMM_INT8_SRC_UNIT * GEMM_INT8_DST_XUNIT / sizeof(int32_t);
    for (int i = 0; i < realDstCount; ++i) {
        int xIndex = (int)xIndexStart + i;
        int ox     = xIndex % im2colParameter->ow;
        int oy     = xIndex / im2colParameter->ow;

        int sx = ox * im2colParameter->strideX - im2colParameter->padX;
        int sy = oy * im2colParameter->strideY - im2colParameter->padY;

        int sfy = ALIMAX(0, (UP_DIV(-sy, im2colParameter->dilateY)));
        int efy = ALIMIN(kh, UP_DIV(ih - sy, im2colParameter->dilateY));
        int sfx = ALIMAX(0, (UP_DIV(-sx, im2colParameter->dilateX)));
        int efx = ALIMIN(kw, UP_DIV(iw - sx, im2colParameter->dilateX));
        int fyC = efy - sfy;
        int fxC = efx - sfx;

        auto colAddrI    = colAddr + GEMM_INT8_SRC_UNIT * i;
        auto inputOffset = inputOrigin + (sx + sfx * dilateX + (sy + sfy * dilateY) * iw) * GEMM_INT8_UNIT;
        auto indexOffset = sfy * kw + sfx;
        for (int fy = 0; fy < fyC; ++fy) {
            for (int fx = 0; fx < fxC; ++fx) {
                auto inputK       = inputOffset + (fx * dilateX + fy * dilateY * iw) * GEMM_INT8_UNIT;
                auto indexStart   = indexOffset + fy * kw + fx;
                auto indexInside  = indexStart % 4;
                auto indexOutside = indexStart / 4;
                auto dstK0        = (int32_t*)colAddrI + indexOutside * dstXStepInt32 + indexInside;
                dstK0[0]          = *((int32_t*)inputK);
            }
        }
    }
}

static void _im2colCommon(int8_t* colAddr, const int8_t* inputOrigin,
                          const ConvolutionCommon::Im2ColParameter* im2colParameter, size_t xIndexStart,
                          size_t realDstCount) {
    const int col_buffer_size = im2colParameter->kernelCountUnit * GEMM_INT8_DST_XUNIT * GEMM_INT8_SRC_UNIT * sizeof(int8_t);
    ::memset(colAddr, 0, col_buffer_size);
    auto ih                     = im2colParameter->ih;
    auto iw                     = im2colParameter->iw;
    auto kh                     = im2colParameter->kernelY;
    auto kw                     = im2colParameter->kernelX;
    auto dilateX                = im2colParameter->dilateX;
    auto dilateY                = im2colParameter->dilateY;
    auto icDiv4                 = im2colParameter->icDiv4;
    auto srcZStep               = iw * ih * GEMM_INT8_UNIT;
    constexpr int dstXStepInt32 = GEMM_INT8_SRC_UNIT * GEMM_INT8_DST_XUNIT / sizeof(int32_t);
    for (int i = 0; i < realDstCount; ++i) {
        int xIndex = (int)xIndexStart + i;
        int ox     = xIndex % im2colParameter->ow;
        int oy     = xIndex / im2colParameter->ow;

        int sx = ox * im2colParameter->strideX - im2colParameter->padX;
        int sy = oy * im2colParameter->strideY - im2colParameter->padY;

        int sfy = ALIMAX(0, (UP_DIV(-sy, im2colParameter->dilateY)));
        int efy = ALIMIN(kh, UP_DIV(ih - sy, im2colParameter->dilateY));
        int sfx = ALIMAX(0, (UP_DIV(-sx, im2colParameter->dilateX)));
        int efx = ALIMIN(kw, UP_DIV(iw - sx, im2colParameter->dilateX));
        int fyC = efy - sfy;
        int fxC = efx - sfx;

        auto colAddrI    = colAddr + GEMM_INT8_SRC_UNIT * i;
        auto inputOffset = inputOrigin + (sx + sfx * dilateX + (sy + sfy * dilateY) * iw) * GEMM_INT8_UNIT;
        auto indexOffset = (sfy * kw + sfx) * icDiv4;
        for (int fy = 0; fy < fyC; ++fy) {
            for (int fx = 0; fx < fxC; ++fx) {
                auto inputK     = inputOffset + (fx * dilateX + fy * dilateY * iw) * GEMM_INT8_UNIT;
                auto indexStart = indexOffset + (fy * kw + fx) * icDiv4;
                for (int sz = 0; sz < icDiv4; ++sz) {
                    const int yIndex      = indexStart + sz;
                    const int ySubOutside = yIndex / GEMM_INT8_UNIT;
                    const int ySubInside  = yIndex % GEMM_INT8_UNIT;
                    auto dstK0            = (int32_t*)colAddrI + ySubOutside * dstXStepInt32 + ySubInside;
                    dstK0[0]              = *((int32_t*)inputK);
                    inputK += srcZStep;
                }
            }
        }
    }
}
CPUConvInt8::~CPUConvInt8() {
    if(mWeightInt8 != nullptr){
        backend()->onReleaseBuffer(mWeightInt8.get(), Backend::STATIC);
    }
    if(mBiasInt32 != nullptr){
        backend()->onReleaseBuffer(mBiasInt32.get(), Backend::STATIC);
    }
    if(mScaleFloat != nullptr){
        backend()->onReleaseBuffer(mScaleFloat.get(), Backend::STATIC);
    }
}
CPUConvInt8::CPUConvInt8(Backend* backend, const MNN::Convolution2D* convParam, const std::vector<Tensor*>& inputs)
    : CPUConvolution(convParam->common(), backend) {
    const auto convCommon             = convParam->common();
    const auto kx                     = convCommon->kernelX();
    const auto ky                     = convCommon->kernelY();
    const auto kernelCount            = kx * ky;
    const auto srcCount               = inputs[0]->channel();
    const auto outputCount            = convCommon->outputCount();
    const auto outputCountUnit        = UP_DIV(outputCount, GEMM_INT8_UNIT);
    const auto srcCountUnit           = UP_DIV(srcCount, GEMM_INT8_UNIT);
    const auto totalKernelCountD8     = UP_DIV(srcCountUnit * kernelCount, 2);
    const auto totalKernelCountD8Div2 = UP_DIV(totalKernelCountD8, 2);
        
    // choose int8 gemm kernel
    mGemmKernel = MNNGemmInt8AddBiasScale_16x4_Unit;
    if(convParam->symmetricQuan()->method() == QuantizeAlgo_OVERFLOW_AWARE){
    // if(true) { // debug, always be chosen
        mGemmKernel = MNNGemmInt8AddBiasScale_16x4_Unit_FAST;
    }
    mActBits = convParam->symmetricQuan()->nbits();
    
    mWeightInt8.reset(Tensor::createDevice<int8_t>({outputCountUnit, totalKernelCountD8Div2, GEMM_INT8_UNIT, GEMM_INT8_SRC_UNIT}));
    auto allocRes = backend->onAcquireBuffer(mWeightInt8.get(), Backend::STATIC);
    if (!allocRes) {
        mValid = false;
        return;
    }
    const int oneTileLen         = mWeightInt8->stride(1);
    const int outputChnnelStride = mWeightInt8->stride(0);
    const int8_t *weightSrc = nullptr;
    std::shared_ptr<ConvolutionCommon::Int8Common> quanCommon;
    if (convParam->quanParameter() != nullptr) {
        quanCommon = ConvolutionCommon::load(convParam->quanParameter(), false);
        weightSrc = quanCommon->weight.get();
    } else {
        weightSrc = convParam->symmetricQuan()->weight()->data();
    }
    auto weightDst               = mWeightInt8->host<int8_t>();
    memset(weightDst, 0, mWeightInt8->size());
    // reorder weight
    for (int k = 0; k < kernelCount; ++k) {
        const auto srcK = weightSrc + k;
        for (int y = 0; y < srcCount; ++y) {
            const int yOutSide    = y / GEMM_INT8_UNIT;
            const int yInSide     = y % GEMM_INT8_UNIT;
            const int yIndex      = yOutSide + k * srcCountUnit;
            const int ySubOutSide = yIndex / GEMM_INT8_UNIT;
            const int ySubInSide  = yIndex % GEMM_INT8_UNIT;

            auto dstY       = weightDst + ySubOutSide * oneTileLen + ySubInSide * GEMM_INT8_UNIT + yInSide;
            const auto srcY = srcK + y * kernelCount;
            for (int x = 0; x < outputCount; ++x) {
                const int xOutSide = x / GEMM_INT8_UNIT;
                const int xInSide  = x % GEMM_INT8_UNIT;
                const int dstIndex = xOutSide * outputChnnelStride + xInSide * GEMM_INT8_SRC_UNIT;
                const int srcIndex = x * kernelCount * srcCount;
                dstY[dstIndex]     = srcY[srcIndex];
            }
        }
    }
    const int outputChannleUp4 = ALIGN_UP4(outputCount);
    mBiasInt32.reset(Tensor::createDevice<int32_t>({outputChannleUp4}));
    allocRes = backend->onAcquireBuffer(mBiasInt32.get(), Backend::STATIC);
    if (!allocRes) {
        mValid = false;
        return;
    }
    auto biasPtr = mBiasInt32->host<int32_t>();
    memset(biasPtr, 0, outputChannleUp4 * sizeof(int32_t));
    memcpy(biasPtr, convParam->symmetricQuan()->bias()->data(), outputCount * sizeof(int32_t));

    mScaleFloat.reset(Tensor::createDevice<float>({outputChannleUp4}));
    allocRes = backend->onAcquireBuffer(mScaleFloat.get(), Backend::STATIC);
    if (!allocRes) {
        mValid = false;
        return;
    }

    auto scalePtr = mScaleFloat->host<float>();
    memset(scalePtr, 0, outputChannleUp4 * sizeof(float));
    memcpy(scalePtr, convParam->symmetricQuan()->scale()->data(), outputCount * sizeof(float));

    mIm2ColParamter.dilateX         = convCommon->dilateX();
    mIm2ColParamter.dilateY         = convCommon->dilateY();
    mIm2ColParamter.strideX         = convCommon->strideX();
    mIm2ColParamter.strideY         = convCommon->strideY();
    mIm2ColParamter.padX            = convCommon->padX();
    mIm2ColParamter.padY            = convCommon->padY();
    mIm2ColParamter.icDiv4          = srcCountUnit;
    mIm2ColParamter.kernelX         = convCommon->kernelX();
    mIm2ColParamter.kernelY         = convCommon->kernelY();
    mIm2ColParamter.kernelCountUnit = totalKernelCountD8Div2;

    mRelu = convCommon->relu() || convCommon->relu6();
}

ErrorCode CPUConvInt8::onResize(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs) {
    CPUConvolution::onResize(inputs, outputs);
    auto input  = inputs[0];
    auto output = outputs[0];

    mIm2ColParamter.padX = mPadX;
    mIm2ColParamter.padY = mPadY;

    mIm2ColParamter.ih = input->height();
    mIm2ColParamter.iw = input->width();
    mIm2ColParamter.oh = output->height();
    mIm2ColParamter.ow = output->width();

    mTileCount        = UP_DIV(output->height() * output->width(), GEMM_INT8_DST_XUNIT);
    const int threads = std::max(static_cast<CPUBackend*>(backend())->threadNumber(), 1);
    mThreadNums       = std::min(threads, mTileCount);

    // set im2col tensor info
    mTempIm2ColBuffer.setType(DataType_DT_INT8);
    mTempIm2ColBuffer.buffer().dimensions = 3;
    mTempIm2ColBuffer.setLength(0, mThreadNums);
    mTempIm2ColBuffer.setLength(1, GEMM_INT8_DST_XUNIT);
    mTempIm2ColBuffer.setLength(2, mWeightInt8->length(1) * GEMM_INT8_SRC_UNIT);
    TensorUtils::setLinearLayout(&mTempIm2ColBuffer);

    // set reamin tensor info
    mTempRemainBuffer.setType(DataType_DT_INT8);
    mTempRemainBuffer.buffer().dimensions = 3;
    mTempRemainBuffer.setLength(0, mThreadNums);
    mTempRemainBuffer.setLength(1, GEMM_INT8_DST_XUNIT);
    mTempRemainBuffer.setLength(2, ALIGN_UP4(output->channel()));
    TensorUtils::setLinearLayout(&mTempRemainBuffer);

    bool success = backend()->onAcquireBuffer(&mTempIm2ColBuffer, Backend::DYNAMIC);
    success      = success && backend()->onAcquireBuffer(&mTempRemainBuffer, Backend::DYNAMIC);
    if (!success) {
        return OUT_OF_MEMORY;
    }

    backend()->onReleaseBuffer(&mTempIm2ColBuffer, Backend::DYNAMIC);
    backend()->onReleaseBuffer(&mTempRemainBuffer, Backend::DYNAMIC);

    return NO_ERROR;
}

ErrorCode CPUConvInt8::onExecute(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs) {
    const auto input = inputs[0];
    auto output      = outputs[0];

    const int outputPlaneLen = output->height() * output->width();
    const int dstZStep       = outputPlaneLen * 4;

    const int batch                  = input->batch();
    const int ocDiv4                 = UP_DIV(output->channel(), 4);
    const auto kernelCountUnitDouble = mIm2ColParamter.kernelCountUnit;

    bool fastIm2Col = mIm2ColParamter.kernelX == 1 && mIm2ColParamter.kernelY == 1 && mIm2ColParamter.icDiv4 % 2 == 0 &&
                      mIm2ColParamter.strideX == 1 && mIm2ColParamter.strideY == 1 && mIm2ColParamter.padX == 0 &&
                      mIm2ColParamter.padY == 0;
    auto im2ColProcess = _im2colCommon;
    if (fastIm2Col) {
        im2ColProcess = _fastIm2Col;
    } else if (input->channel() <= 4) {
        im2ColProcess = _im2colCommonZ1;
    }
    //auto remain = outputPlaneLen % GEMM_INT8_DST_XUNIT;
    //FUNC_PRINT(remain);

    const auto inputDataPtr = input->host<int8_t>();

    const auto weightDataPtr = mWeightInt8->host<int8_t>();
    const auto biasDataPtr   = mBiasInt32->host<int32_t>();
    const auto scaleDataPtr  = mScaleFloat->host<float>();
    auto im2colPtr           = mTempIm2ColBuffer.host<int8_t>();
    auto outputDataPtr       = output->host<int8_t>();
    auto tempRemainPtr       = mTempRemainBuffer.host<int8_t>();
    QuanPostTreatParameters quanParameters;
    quanParameters.scale = scaleDataPtr;
    quanParameters.bias = biasDataPtr;
    quanParameters.maxValue = 127;
    if (mRelu) {
        quanParameters.minValue = 0;
    } else {
        quanParameters.minValue = -128;
    }

    for (int bIndex = 0; bIndex < batch; ++bIndex) {
        const auto srcPtr = inputDataPtr + bIndex * input->stride(0);
        auto dstPtr       = outputDataPtr + bIndex * output->stride(0);

        auto threadFunction = [&](int tId) {
            auto colAddr        = im2colPtr + tId * mTempIm2ColBuffer.stride(0);
            auto gemmOutputAddr = tempRemainPtr + tId * mTempRemainBuffer.stride(0);

            for (int tIndex = tId; tIndex < mTileCount; tIndex += mThreadNums) {
                const int xIndexStart  = tIndex * GEMM_INT8_DST_XUNIT;
                const int realDstCount = ALIMIN(outputPlaneLen - xIndexStart, GEMM_INT8_DST_XUNIT);
                // im2col
                im2ColProcess(colAddr, srcPtr, &mIm2ColParamter, xIndexStart, realDstCount);
                auto outputInTilePtr = dstPtr + xIndexStart * GEMM_INT8_UNIT;
                mGemmKernel(outputInTilePtr, colAddr, weightDataPtr, kernelCountUnitDouble, dstZStep * sizeof(int8_t), ocDiv4, &quanParameters, realDstCount);
            }
        };

        MNN_CONCURRENCY_BEGIN(tId, mThreadNums) {
            threadFunction((int)tId);
        }
        MNN_CONCURRENCY_END();
    }

    return NO_ERROR;
}

#if defined(__aarch64__) && defined(ENABLE_ARMV82)
CPUConvArm82Int8::CPUConvArm82Int8(Backend* backend, const MNN::Convolution2D* convParam)
    : CPUConvolution(convParam->common(), backend) {
    const auto convCommon      = convParam->common();
    const auto kx              = convCommon->kernelX();
    const auto ky              = convCommon->kernelY();
    const auto kernelCount     = kx * ky;
    const auto srcCount        = convCommon->inputCount();
    const auto outputCount     = convCommon->outputCount();
    const auto outputCountUnit = UP_DIV(outputCount, GEMM_INT8_UNIT);
    const auto srcCountUnit    = UP_DIV(srcCount, GEMM_INT8_UNIT);

    const auto totalKernelCountUnit = srcCountUnit * kernelCount;
    mWeightInt8.reset(Tensor::createDevice<int8_t>({outputCountUnit, totalKernelCountUnit, GEMM_INT8_UNIT, GEMM_INT8_UNIT}));
    auto allocRes = backend->onAcquireBuffer(mWeightInt8.get(), Backend::STATIC);
    if (!allocRes) {
        mValid = false;
        return;
    }

    const int weightOutputChannelStride = mWeightInt8->stride(0);
    const int8_t *weightSrc = nullptr;
    std::shared_ptr<ConvolutionCommon::Int8Common> quanCommon;
    if (convParam->quanParameter() != nullptr) {
        quanCommon = ConvolutionCommon::load(convParam->quanParameter(), false);
        weightSrc = quanCommon->weight.get();
    } else {
        weightSrc = convParam->symmetricQuan()->weight()->data();
    }
    auto weightDst                      = mWeightInt8->host<int8_t>();
    memset(weightDst, 0, mWeightInt8->size());
    // reorder weight
    for (int k = 0; k < kernelCount; ++k) {
        const auto weightSrcK = weightSrc + k;
        auto weightDstK       = weightDst + k * srcCountUnit * GEMM_INT8_UNIT * GEMM_INT8_UNIT;
        for (int y = 0; y < srcCount; ++y) {
            const int yOutSide = y / GEMM_INT8_UNIT;
            const int yInSide  = y % GEMM_INT8_UNIT;

            auto dstY       = weightDstK + yOutSide * GEMM_INT8_UNIT * GEMM_INT8_UNIT + yInSide;
            const auto srcY = weightSrcK + y * kernelCount;

            for (int x = 0; x < outputCount; ++x) {
                const int xOutSide = x / GEMM_INT8_UNIT;
                const int xInSide  = x % GEMM_INT8_UNIT;
                const int dstIndex = xOutSide * weightOutputChannelStride + xInSide * GEMM_INT8_UNIT;
                const int srcIndex = x * kernelCount * srcCount;
                dstY[dstIndex]     = srcY[srcIndex];
            }
        }
    }

    mBiasInt32.reset(Tensor::createDevice<int32_t>({outputCountUnit * GEMM_INT8_UNIT}));
    allocRes = backend->onAcquireBuffer(mBiasInt32.get(), Backend::STATIC);
    if (!allocRes) {
        mValid = false;
        return;
    }
    auto biasPtr = mBiasInt32->host<int32_t>();
    memset(biasPtr, 0, outputCountUnit * GEMM_INT8_UNIT * sizeof(int32_t));
    memcpy(biasPtr, convParam->symmetricQuan()->bias()->data(), outputCount * sizeof(int32_t));

    mScaleFloat.reset(Tensor::createDevice<float>({outputCountUnit * GEMM_INT8_UNIT}));
    allocRes = backend->onAcquireBuffer(mScaleFloat.get(), Backend::STATIC);
    if (!allocRes) {
        mValid = false;
        return;
    }

    auto scalePtr = mScaleFloat->host<float>();
    memset(scalePtr, 0, outputCountUnit * GEMM_INT8_UNIT * sizeof(float));
    memcpy(scalePtr, convParam->symmetricQuan()->scale()->data(), outputCount * sizeof(float));

    mIm2ColParamter.dilateX         = convCommon->dilateX();
    mIm2ColParamter.dilateY         = convCommon->dilateY();
    mIm2ColParamter.strideX         = convCommon->strideX();
    mIm2ColParamter.strideY         = convCommon->strideY();
    mIm2ColParamter.padX            = convCommon->padX();
    mIm2ColParamter.padY            = convCommon->padY();
    mIm2ColParamter.icDiv4          = srcCountUnit;
    mIm2ColParamter.kernelX         = convCommon->kernelX();
    mIm2ColParamter.kernelY         = convCommon->kernelY();
    mIm2ColParamter.kernelCountUnit = totalKernelCountUnit;

    mRelu = convCommon->relu() || convCommon->relu6();
}

CPUConvArm82Int8::~CPUConvArm82Int8() {
    if(mWeightInt8 != nullptr){
        backend()->onReleaseBuffer(mWeightInt8.get(), Backend::STATIC);
    }
    if(mBiasInt32 != nullptr){
        backend()->onReleaseBuffer(mBiasInt32.get(), Backend::STATIC);
    }
    if(mScaleFloat != nullptr){
        backend()->onReleaseBuffer(mScaleFloat.get(), Backend::STATIC);
    }
}

ErrorCode CPUConvArm82Int8::onResize(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs) {
    CPUConvolution::onResize(inputs, outputs);
    auto input           = inputs[0];
    auto output          = outputs[0];
    mIm2ColParamter.padX = mPadX;
    mIm2ColParamter.padY = mPadY;
    mIm2ColParamter.ih   = input->height();
    mIm2ColParamter.iw   = input->width();
    mIm2ColParamter.oh   = output->height();
    mIm2ColParamter.ow   = output->width();

    mTileCount        = UP_DIV(output->height() * output->width(), DST_XUNIT_ARMV82);
    const int threads = std::max(static_cast<CPUBackend*>(backend())->threadNumber(), 1);
    mThreadNums       = std::min(threads, mTileCount);

    mTempIm2ColBuffer.setType(DataType_DT_INT8);
    mTempIm2ColBuffer.buffer().dimensions = 3;
    mTempIm2ColBuffer.setLength(0, mThreadNums);
    mTempIm2ColBuffer.setLength(1, DST_XUNIT_ARMV82);
    mTempIm2ColBuffer.setLength(2, mWeightInt8->length(1) * GEMM_INT8_UNIT);
    TensorUtils::setLinearLayout(&mTempIm2ColBuffer);

    mTempRemainBuffer.setType(DataType_DT_INT8);
    mTempRemainBuffer.buffer().dimensions = 3;
    mTempRemainBuffer.setLength(0, mThreadNums);
    mTempRemainBuffer.setLength(1, DST_XUNIT_ARMV82);
    mTempRemainBuffer.setLength(2, ALIGN_UP4(output->channel()));
    TensorUtils::setLinearLayout(&mTempRemainBuffer);

    bool success = backend()->onAcquireBuffer(&mTempIm2ColBuffer, Backend::DYNAMIC);
    success      = success && backend()->onAcquireBuffer(&mTempRemainBuffer, Backend::DYNAMIC);
    if (!success) {
        return OUT_OF_MEMORY;
    }

    backend()->onReleaseBuffer(&mTempIm2ColBuffer, Backend::DYNAMIC);
    backend()->onReleaseBuffer(&mTempRemainBuffer, Backend::DYNAMIC);

    return NO_ERROR;
}

static void _im2colCommonArmv82(int8_t* colAddr, const int8_t* src,
                                const ConvolutionCommon::Im2ColParameter* im2colParameter, size_t xIndexStart,
                                size_t realDstCount) {
    const int colBufferSize = im2colParameter->kernelCountUnit * DST_XUNIT_ARMV82 * GEMM_INT8_UNIT * sizeof(int8_t);
    memset(colAddr, 0, colBufferSize);
    auto ih = im2colParameter->ih;
    auto iw = im2colParameter->iw;
    // auto oh = im2colParameter->oh;
    auto ow                     = im2colParameter->ow;
    auto kh                     = im2colParameter->kernelY;
    auto kw                     = im2colParameter->kernelX;
    auto dilateX                = im2colParameter->dilateX;
    auto dilateY                = im2colParameter->dilateY;
    auto icDiv4                 = im2colParameter->icDiv4;
    auto srcChannleStride       = iw * ih * GEMM_INT8_UNIT;
    constexpr int dstXStepInt32 = GEMM_INT8_UNIT * DST_XUNIT_ARMV82 / sizeof(int32_t);

    for (int i = 0; i < realDstCount; ++i) {
        int xIndex = (int)xIndexStart + i;
        int ox     = xIndex % ow;
        int oy     = xIndex / ow;
        int sx     = ox * im2colParameter->strideX - im2colParameter->padX;
        int sy     = oy * im2colParameter->strideY - im2colParameter->padY;
        int sfy    = ALIMAX(0, (UP_DIV(-sy, im2colParameter->dilateY)));
        int efy    = ALIMIN(kh, UP_DIV(ih - sy, im2colParameter->dilateY));
        int sfx    = ALIMAX(0, (UP_DIV(-sx, im2colParameter->dilateX)));
        int efx    = ALIMIN(kw, UP_DIV(iw - sx, im2colParameter->dilateX));
        int fyC    = efy - sfy;
        int fxC    = efx - sfx;

        auto colAddrI    = colAddr + GEMM_INT8_UNIT * i;
        auto inputOffset = src + (sx + sfx * dilateX + (sy + sfy * dilateY) * iw) * GEMM_INT8_UNIT;
        auto indexOffset = (sfy * kw + sfx) * icDiv4;

        for (int fy = 0; fy < fyC; ++fy) {
            for (int fx = 0; fx < fxC; ++fx) {
                auto inputK     = inputOffset + (fx * dilateX + fy * dilateY * iw) * GEMM_INT8_UNIT;
                auto indexStart = (indexOffset + (fy * kw + fx) * icDiv4) * dstXStepInt32;
                for (int sz = 0; sz < icDiv4; ++sz) {
                    auto dstK0 = (int32_t*)colAddrI + indexStart + sz * dstXStepInt32;
                    dstK0[0]   = *((int32_t*)inputK);
                    inputK += srcChannleStride;
                }
            }
        }
    }
}

static void _fastIm2ColArmv82(int8_t* colAddr, const int8_t* inputOrigin,
                              const ConvolutionCommon::Im2ColParameter* im2colParameter, size_t xIndexStart,
                              size_t realDstCount) {
    const int col_buffer_size = im2colParameter->kernelCountUnit * DST_XUNIT_ARMV82 * GEMM_INT8_UNIT * sizeof(int8_t);
    ::memset(colAddr, 0, col_buffer_size);
    const int icDiv4    = im2colParameter->icDiv4;
    const int srcZStep  = im2colParameter->iw * im2colParameter->ih * GEMM_INT8_UNIT;
    auto inputOffsetPtr = inputOrigin + xIndexStart * GEMM_INT8_UNIT;
    for (int i = 0; i < realDstCount; ++i) {
        auto colAddrI = colAddr + GEMM_INT8_UNIT * i;
        auto inputK   = inputOffsetPtr + GEMM_INT8_UNIT * i;
        for (int sz = 0; sz < icDiv4; ++sz) {
            auto inputZ0       = inputK + srcZStep * sz;
            auto dstK0         = colAddrI + sz * GEMM_INT8_UNIT * DST_XUNIT_ARMV82;
            *((int32_t*)dstK0) = *((int32_t*)inputZ0);
        }
    }
}

ErrorCode CPUConvArm82Int8::onExecute(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs) {
    auto input                 = inputs[0];
    auto output                = outputs[0];
    const int outputPlaneLen   = output->height() * output->width();
    const int dstZStep         = outputPlaneLen * 4;
    const int batch            = input->batch();
    const int ocDiv4           = UP_DIV(output->channel(), 4);
    const auto kernelCountUnit = mIm2ColParamter.kernelCountUnit;

    const auto inputDataPtr = input->host<int8_t>();

    const auto weightDataPtr = mWeightInt8->host<int8_t>();
    const auto biasDataPtr   = mBiasInt32->host<int32_t>();
    const auto scaleDataPtr  = mScaleFloat->host<float>();
    auto im2colPtr           = mTempIm2ColBuffer.host<int8_t>();
    auto outputDataPtr       = output->host<int8_t>();
    auto tempRemainPtr       = mTempRemainBuffer.host<int8_t>();

    auto im2ColProcess = _im2colCommonArmv82;
    bool useFastIm2Col = mIm2ColParamter.kernelX == 1 && mIm2ColParamter.kernelY == 1 && mIm2ColParamter.strideX == 1 &&
                         mIm2ColParamter.strideY == 1 && mIm2ColParamter.padX == 0 && mIm2ColParamter.padY == 0;

    if (useFastIm2Col) {
        im2ColProcess = _fastIm2ColArmv82;
    }

    QuanPostTreatParameters quanParameters;
    quanParameters.scale = scaleDataPtr;
    quanParameters.bias = biasDataPtr;
    quanParameters.maxValue = 127;
    if (mRelu) {
        quanParameters.minValue = 0;
    } else {
        quanParameters.minValue = -128;
    }

    for (int bIndex = 0; bIndex < batch; ++bIndex) {
        const auto srcPtr = inputDataPtr + bIndex * input->stride(0);
        auto dstPtr       = outputDataPtr + bIndex * output->stride(0);

        auto threadFunction = [&](int tId) {
            auto colAddr        = im2colPtr + tId * mTempIm2ColBuffer.stride(0);
            auto gemmOutputAddr = tempRemainPtr + tId * mTempRemainBuffer.stride(0);

            for (int tIndex = tId; tIndex < mTileCount; tIndex += mThreadNums) {
                const int xIndexStart  = tIndex * DST_XUNIT_ARMV82;
                const int realDstCount = ALIMIN(outputPlaneLen - xIndexStart, DST_XUNIT_ARMV82);
                // im2col
                im2ColProcess(colAddr, srcPtr, &mIm2ColParamter, xIndexStart, realDstCount);
                auto outputInTilePtr = dstPtr + xIndexStart * GEMM_INT8_UNIT;
                if (realDstCount == DST_XUNIT_ARMV82) {
                    MNNGemmInt8AddBiasScale_ARMV82_Unit(outputInTilePtr, colAddr, weightDataPtr, kernelCountUnit, dstZStep * sizeof(int8_t),
                                                        ocDiv4, realDstCount, &quanParameters);
                } else {
                    MNNGemmInt8AddBiasScale_ARMV82_Unit(gemmOutputAddr, colAddr, weightDataPtr, kernelCountUnit, GEMM_INT8_UNIT * DST_XUNIT_ARMV82 * sizeof(int8_t),
                                                        ocDiv4, realDstCount, &quanParameters);
                    for (int z = 0; z < ocDiv4; ++z) {
                        auto outputZ = outputInTilePtr + z * dstZStep;
                        auto srcZ    = gemmOutputAddr + z * GEMM_INT8_UNIT * DST_XUNIT_ARMV82;
                        memcpy(outputZ, srcZ, realDstCount * GEMM_INT8_UNIT * sizeof(int8_t));
                    }
                }
            }
        };

        MNN_CONCURRENCY_BEGIN(tId, mThreadNums) {
            threadFunction((int)tId);
        }
        MNN_CONCURRENCY_END();
    }

    return NO_ERROR;
}
#endif

#include "compute/Int8FunctionsOpt.h"
static int _int8bestWinogradUnit(const Convolution2DCommon *common, const Tensor *inputTensor,
                                          const Tensor *outputTensor, int threadNumber) {
    int ow      = outputTensor->width();
    int oh      = outputTensor->height();
    int oc      = outputTensor->channel();
    int unit2   = UP_DIV(ow * oh, DST_XUNIT * threadNumber);
    int maxUnit = (int)::sqrtf((float)unit2);
    maxUnit     = std::min(maxUnit, 6);
    maxUnit     = std::max(maxUnit, 2);

    int ic           = inputTensor->channel();
    auto kernelSize  = common->kernelY();
    int unit         = 2;
    float maxRate    = 0.0f;
    float originCost = (float)ow * oh * (float)ic * oc * kernelSize * kernelSize;
    static std::set<int> supportSu{4, 8};
    for (int u = 2; u <= maxUnit; ++u) {
        float su = (float)(u + kernelSize - 1);
        if (supportSu.find(su) == supportSu.end()) {
            continue;
        }
        if (nullptr == WinogradFunction::chooseDestTransform((int)su, u)) {
            continue;
        }
        /*Let F(6,3) be choosed when it can speed up from F(2,3) than 0.6*/
        float penalty = (su * su) / (float)(kernelSize * kernelSize) * 0.12f;
        float winogradCost =
            (2 * su * su * ic + su * su * ic * oc + 2 * su * u * oc) * (UP_DIV(ow, u) * UP_DIV(oh, u));
        float reduceRate = originCost / winogradCost - penalty;
        //MNN_PRINT("ic=%d, oc=%d,ow=%d, oh=%d, %f-%f, %f, winograd unit:%d\n", ic, oc, ow, oh, winogradCost, originCost, reduceRate, u);
        if (reduceRate > maxRate) {
            maxRate = reduceRate;
            unit    = u;
        }
    }
    if (maxRate < 1.0f) {
        return 0;
    }
    return unit;
}
class CPUConvInt8Creator : public CPUBackend::Creator {
public:
    virtual Execution* onCreate(const std::vector<Tensor*>& inputs, const std::vector<Tensor*>& outputs,
                                const MNN::Op* op, Backend* backend) const override {
#if defined(__aarch64__) && defined(ENABLE_ARMV82)
        if(static_cast<CPUBackend*>(backend)->supportDot()){
            return new CPUConvArm82Int8(backend, op->main_as_Convolution2D());
        }
#endif
        auto threadNumber = ((CPUBackend*)backend)->threadNumber();
        auto conv2D = op->main_as_Convolution2D()->common();
        if (1 == conv2D->strideX() && 1 == conv2D->strideY() && 1 == conv2D->dilateX() && 1 == conv2D->dilateY()) {
            int actBits = op->main_as_Convolution2D()->symmetricQuan()->nbits();
            int weightBits = actBits;

            auto kx = conv2D->kernelX(), ky = conv2D->kernelY();
            if (kx == 3 && ky == 3 && weightBits <= 6 && actBits <= 6) {
                auto unit = _int8bestWinogradUnit(conv2D, inputs[0], outputs[0], threadNumber);
                if (unit >= 2) {
                    return new ConvInt83x3(backend, op->main_as_Convolution2D(), inputs, outputs);
                }
            } else if (((kx == 1 && ky != 1) || (kx != 1 && ky == 1)) && weightBits <= 7 && actBits <= 7) {
                return new ConvInt8_1xN(backend, op->main_as_Convolution2D());
            }
        }
        return new CPUConvInt8(backend, op->main_as_Convolution2D(), inputs);
    }
};

REGISTER_CPU_OP_CREATOR(CPUConvInt8Creator, OpType_ConvInt8);

} // namespace MNN
