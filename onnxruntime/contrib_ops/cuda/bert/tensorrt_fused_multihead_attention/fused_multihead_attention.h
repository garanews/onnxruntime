/*
 * SPDX-FileCopyrightText: Copyright (c) 1993-2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include <memory>
#include <mutex>
#include <set>
#include <cstdint>
#include <utility>
#include <unordered_map>
#include <vector>
#include <cuda_runtime_api.h>
#include "contrib_ops/cuda/bert/tensorrt_fused_multihead_attention/cudaDriverWrapper.h"
#include "contrib_ops/cuda/bert/tensorrt_fused_multihead_attention/fused_multihead_attention_common.h"

namespace onnxruntime {
namespace contrib {
namespace cuda {

template <typename TKernelMeta, typename TKernelParam>
class TFusedMultiHeadAttentionXMMAKernel {
 public:
  using KernelMeta = TKernelMeta;
  using KernelParam = TKernelParam;
  inline uint64_t hashID(uint32_t s, uint32_t d) const {
    return (uint64_t)s << 32 | d;
  }
  virtual uint64_t hashID(const KernelMeta& kernelMeta) const {
    return hashID(kernelMeta.mS, kernelMeta.mD);
  }

  TFusedMultiHeadAttentionXMMAKernel(const TKernelMeta* pMetaStart, uint32_t nMetaCount, Data_type type, uint32_t sm)
      : mDataType(type), mKernelMeta(pMetaStart), mKernelMetaCount(nMetaCount), mSM(sm) {
  }

  void loadXMMAKernels(uint32_t smVersion) {
    for (uint32_t i = 0; i < mKernelMetaCount; ++i) {
      const auto& kernelMeta = mKernelMeta[i];
      const auto kernelKey = hashID(kernelMeta);
      if (kernelMeta.mSM == smVersion &&
          kernelMeta.mDataType == mDataType &&
          mFunctions.find(kernelKey) == mFunctions.end()) {
        const uint32_t DEFAULT_SMEM_SIZE{48 * 1024};
        if (kernelMeta.mSharedMemBytes >= DEFAULT_SMEM_SIZE) {
          int32_t deviceID{0};
          cudaGetDevice(&deviceID);
          int32_t sharedMemPerMultiprocessor{0};
          if (cudaDeviceGetAttribute(
                  &sharedMemPerMultiprocessor, cudaDevAttrMaxSharedMemoryPerBlockOptin, deviceID) != cudaSuccess ||
              sharedMemPerMultiprocessor < static_cast<int32_t>(kernelMeta.mSharedMemBytes)) {
            // skip load function because not enough shared memory to launch the kernel
            printf("skip loading trt fused attention kernel %s because not enough shared memory",
                   kernelMeta.mFuncName);
            continue;
          }
        }

        CUmodule hmod{0};
        auto findModuleIter = mModules.find(kernelMeta.mCubin);
        if (findModuleIter != mModules.end()) {
          hmod = findModuleIter->second;
        } else {
          cuErrCheck(mDriver.cuModuleLoadData(&hmod, kernelMeta.mCubin), mDriver);
          mModules.insert(std::make_pair(kernelMeta.mCubin, hmod));
        }

        FusedMultiHeadAttentionKernelInfo funcInfo;
        funcInfo.mMetaInfoIndex = i;
        cuErrCheck(mDriver.cuModuleGetFunction(&funcInfo.mDeviceFunction, hmod, kernelMeta.mFuncName), mDriver);
        if (kernelMeta.mSharedMemBytes >= DEFAULT_SMEM_SIZE) {
          if (CUDA_SUCCESS != mDriver.cuFuncSetAttribute(funcInfo.mDeviceFunction,
                                                         CU_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES,
                                                         kernelMeta.mSharedMemBytes)) {
            // some chip may not have enough shared memory to launch the kernel
            printf("skip loading trt fused attention kernel %s because no enough shared memory",
                   kernelMeta.mFuncName);
            continue;
          }
        }
        mFunctions.insert({kernelKey, funcInfo});
        const int s = static_cast<int>(kernelMeta.mS);
#ifndef NDEBUG
        printf("loaded trt fused attention kernel (%s)\n", kernelMeta.mFuncName);
#endif
        if (mValidSequences.find(s) == mValidSequences.end()) {
          mValidSequences.insert(s);
        }
      }
    }
  }

  void loadXMMAKernels() {
    if (!mFunctions.empty()) {
      return;
    }

    loadXMMAKernels(mSM);

    // sm_86 chips prefer sm_86 sass, but can also use sm_80 sass if sm_86 not exist.
    // sm_87 cannot run sm_80 sass
    if (mSM == kSM_86) {
      loadXMMAKernels(kSM_80);
    }
  }

  bool isValid(int s) const {
    return (mValidSequences.find(s) != mValidSequences.end());
  }

  virtual void run(TKernelParam& params, cudaStream_t ss) const {
    const auto findIter = mFunctions.find(hashID(params.s, params.d));
    ORT_ENFORCE(findIter != mFunctions.end());

    const auto& kernelMeta = mKernelMeta[findIter->second.mMetaInfoIndex];
    const CUfunction func = findIter->second.mDeviceFunction;

    void* kernelParams[] = {&params, nullptr};
    cuErrCheck(mDriver.cuLaunchKernel(func, params.h, params.b, 1, kernelMeta.mThreadsPerCTA, 1, 1,
                                      kernelMeta.mSharedMemBytes, ss, kernelParams, nullptr),
               mDriver);
  }

  virtual ~TFusedMultiHeadAttentionXMMAKernel() = default;

 protected:
  CUDADriverWrapper mDriver;

  Data_type mDataType;
  const TKernelMeta* mKernelMeta;
  uint32_t mKernelMetaCount;
  uint32_t mSM;
  std::unordered_map<const unsigned char*, CUmodule> mModules;
  struct FusedMultiHeadAttentionKernelInfo {
    uint32_t mMetaInfoIndex;
    CUfunction mDeviceFunction;
  };
  std::unordered_map<uint64_t, FusedMultiHeadAttentionKernelInfo> mFunctions;
  std::set<int> mValidSequences;
};

template <typename TFusedMHAKernelList>
class TFusedMHAKernelFactory {
 public:
  const TFusedMHAKernelList* getXMMAKernels(
      const typename TFusedMHAKernelList::KernelMeta* pKernelList, uint32_t nbKernels, Data_type type, uint32_t sm) {
    static std::mutex s_mutex;
    std::lock_guard<std::mutex> lg(s_mutex);

    const auto id = hashID(type, sm);
    const auto findIter = mKernels.find(id);
    if (findIter == mKernels.end()) {
      TFusedMHAKernelList* newKernel = new TFusedMHAKernelList{pKernelList, nbKernels, type, sm};
      newKernel->loadXMMAKernels();
      mKernels.insert(std::make_pair(id, std::unique_ptr<TFusedMHAKernelList>(newKernel)));
      return newKernel;
    }
    return findIter->second.get();
  }

  static TFusedMHAKernelFactory<TFusedMHAKernelList>& Get() {
    static TFusedMHAKernelFactory<TFusedMHAKernelList> s_factory;
    return s_factory;
  }

 private:
  TFusedMHAKernelFactory() = default;

  inline uint64_t hashID(Data_type type, uint32_t sm) const {
    // use deviceID in hasID for multi GPU support before driver support context-less loading of cubin
    int32_t deviceID{0};
    CUDA_CALL_THROW(cudaGetDevice(&deviceID));

    ORT_ENFORCE((deviceID & 0xFFFF) == deviceID);
    ORT_ENFORCE((type & 0xFFFF) == type);
    ORT_ENFORCE((sm & 0xFFFFFFFF) == sm);
    return (uint64_t)type << 48 | (uint64_t)deviceID << 32 | sm;
  }

  std::unordered_map<uint64_t, const std::unique_ptr<TFusedMHAKernelList>> mKernels;
};

}  // namespace cuda
}  // namespace contrib
}  // namespace onnxruntime
