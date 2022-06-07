// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once
#include <memory>
#include <vector>
#include <list>
#include <unordered_map>

#include "core/common/status.h"
#include "core/framework/kernel_type_str_resolver.h"
#include "core/graph/graph_viewer.h"
#include "core/platform/ort_mutex.h"

namespace onnxruntime {
struct KernelCreateInfo;
class ExecutionProviders;
class IExecutionProvider;
class KernelRegistry;
class OpKernel;
class SessionState;

// Kernel registries' manager.
// There're 2 kinds of kernel registries with priority from high to low as below,
// 1. Custom execution provider type specific kernel registries.
// 2. common execution provider type specific kernel registries.
// The 1st and 2nd ones are shared across sessions.

// This class is not thread safe.
class KernelRegistryManager {
 public:
  KernelRegistryManager() = default;

  // Register kernels from providers
  Status RegisterKernels(const ExecutionProviders& execution_providers);

#if !defined(ORT_MINIMAL_BUILD) || defined(ORT_EXTENDED_MINIMAL_BUILD) || defined(ORT_MINIMAL_BUILD_CUSTOM_OPS)
  // The registry passed in this function has highest priority than anything already in this KernelRegistryManager,
  // and anything registered from RegisterKernels
  // For example, if you do:
  // RegisterKernels(providers)
  // RegisterKernelRegistry(A);
  // RegisterKernelRegistry(B);
  // Then B > A > providers
  void RegisterKernelRegistry(std::shared_ptr<KernelRegistry> kernel_registry);

  /**
   * Search kernel registry by provider type.
   * @param type provider type string
   * @return It returns all the possible results. The returned value may contain garbage that doesn't belong to
   *         this provider. Caller should do the filtering. The returned value won't have any nullptrs.
   */
  std::vector<const KernelRegistry*> GetKernelRegistriesByProviderType(const std::string& type) const {
    std::vector<const KernelRegistry*> result;
    for (auto& registry : custom_kernel_registries_) {
      result.push_back(registry.get());
    }
    auto iter = provider_type_to_registry_.find(type);
    if (iter != provider_type_to_registry_.end()) result.push_back(iter->second.get());
    return result;
  }
#endif

  // This function assumes the node is already assigned to an execution provider
  // Don't call this function before graph partition is done
  Status SearchKernelRegistry(const Node& node,
                              /*out*/ const KernelCreateInfo** kernel_create_info) const;

  /**
   * Whether this node can be run on this provider
   */
  static bool HasImplementationOf(const KernelRegistryManager& r, const Node& node, const std::string& provider_type);

  /**
   * Search the kernel registries given a kernel def hash.
   */
  bool SearchKernelRegistriesByHash(HashValue kernel_def_hash,
                                    const KernelCreateInfo** kernel_create_info) const;

  Status CreateKernel(const Node& node,
                      const IExecutionProvider& execution_provider,
                      SessionState& session_state,
                      const KernelCreateInfo& kernel_create_info, std::unique_ptr<OpKernel>& out) const;

  const KernelTypeStrResolver& GetKernelTypeStrResolver() const {
    return kernel_type_str_resolver_;
  }

  void SetKernelTypeStrResolver(KernelTypeStrResolver kernel_type_str_resolver) {
    kernel_type_str_resolver_ = std::move(kernel_type_str_resolver);
  }

  ORT_DISALLOW_COPY_ASSIGNMENT_AND_MOVE(KernelRegistryManager);

 private:
#if !defined(ORT_MINIMAL_BUILD)
  Status EnsureKernelTypeStrResolvesForNodeOpSchema(const Node& node) const;
#endif  // !defined(ORT_MINIMAL_BUILD)

  // key is provider type. Each kernel registry in this collection only belongs to one specific provider
  std::unordered_map<std::string, std::shared_ptr<KernelRegistry>> provider_type_to_registry_;
  // Each kernel registry may contain kernels from many different providers.
  // in order to search kernels from a specific provider, we have to iterate all its elements
  std::list<std::shared_ptr<KernelRegistry>> custom_kernel_registries_;

  // kernel type str resolver used by kernel registries for kernel matching
#if !defined(ORT_MINIMAL_BUILD)
  // in a full build, this serves as a cache that is populated incrementally, so we make it `mutable`
  // in a minimal build, it should be fully populated externally
  mutable
#endif
      KernelTypeStrResolver kernel_type_str_resolver_;
};
}  // namespace onnxruntime