#pragma once

#include "vantix/compute.hpp"

namespace vantix {

class VulkanBackend final : public IComputeBackend {
public:
    static std::vector<std::shared_ptr<VulkanBackend>> create_all(
        WalletVersion version, MnemonicScheme scheme);
    ~VulkanBackend() override;
    BackendInfo info() const override;
    bool self_test() override;
    std::uint64_t run_batch(std::uint32_t size, const SearchPattern& pattern,
        const std::function<void(WalletCandidate&&)>& on_match) override;
private:
    struct Impl;
    explicit VulkanBackend(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace vantix
