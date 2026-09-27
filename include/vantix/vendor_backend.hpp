#pragma once

#include "vantix/compute.hpp"

namespace vantix {

// Vendor modules are loaded only when their runtime is present. Missing modules
// leave Vulkan and CPU usable without a CUDA or HIP installation.
std::vector<std::shared_ptr<IComputeBackend>> load_vendor_backends(
    ComputeApi api, WalletVersion version, MnemonicScheme scheme);

}  // namespace vantix
