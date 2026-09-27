#pragma once

#include "vantix/compute.hpp"

namespace vantix {
std::vector<IComputeBackend*> load_tuning_profile(
    const BackendRegistry& registry, const std::vector<DeviceInfo>& devices,
    WalletVersion wallet, MnemonicScheme scheme);
void save_tuning_profile(
    const BackendRegistry& registry,
    const std::vector<IComputeBackend*>& backends,
    const std::vector<DeviceInfo>& devices,
    WalletVersion wallet, MnemonicScheme scheme);
}
