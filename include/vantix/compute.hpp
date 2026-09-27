#pragma once

#include "vantix/ton.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vantix {

enum class ComputeApi { cpu, vulkan, cuda, hip };

struct DeviceInfo {
    std::string hardware_id;
    std::string name;
    std::string vendor;
    std::string driver;
    std::uint64_t memory_bytes = 0;
    std::vector<ComputeApi> available_apis;
    bool usable_for_search = false;
};

struct BackendInfo {
    std::string name;
    ComputeApi api;
    std::string hardware_id;
    std::string build_id;
};

struct SearchPattern {
    std::string prefix;
    std::string suffix;
    std::string contains;
    bool ignore_case = false;
    bool matches(const std::string& address) const {
        if (!ignore_case) {
            return (prefix.empty() || address.starts_with(prefix)) &&
                   (suffix.empty() || address.ends_with(suffix)) &&
                   (contains.empty() || address.find(contains) != std::string::npos);
        }
        auto equal_ascii = [](std::string_view left, std::string_view right) {
            if (left.size() != right.size()) return false;
            for (std::size_t i = 0; i < left.size(); ++i) {
                auto fold = [](char ch) {
                    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
                };
                if (fold(left[i]) != fold(right[i])) return false;
            }
            return true;
        };
        const std::string_view haystack(address);
        auto starts = [&](std::string_view needle) {
            return needle.empty() ||
                   (needle.size() <= haystack.size() &&
                    equal_ascii(haystack.substr(0, needle.size()), needle));
        };
        auto ends = [&](std::string_view needle) {
            return needle.empty() ||
                   (needle.size() <= haystack.size() &&
                    equal_ascii(haystack.substr(haystack.size() - needle.size()), needle));
        };
        auto has = [&](std::string_view needle) {
            if (needle.empty()) return true;
            if (needle.size() > haystack.size()) return false;
            for (std::size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
                if (equal_ascii(haystack.substr(i, needle.size()), needle)) return true;
            }
            return false;
        };
        return starts(prefix) && ends(suffix) && has(contains);
    }
    void validate() const;
};

class IComputeBackend {
public:
    virtual ~IComputeBackend() = default;
    virtual BackendInfo info() const = 0;
    virtual bool self_test() = 0;
    virtual std::uint64_t run_batch(
        std::uint32_t size,
        const SearchPattern& pattern,
        const std::function<void(WalletCandidate&&)>& on_match) = 0;
};

class BackendRegistry {
public:
    void add(std::shared_ptr<IComputeBackend> backend);
    const std::vector<std::shared_ptr<IComputeBackend>>& backends() const;
private:
    std::vector<std::shared_ptr<IComputeBackend>> backends_;
};

struct TuningResult {
    std::shared_ptr<IComputeBackend> backend;
    double verified_addresses_per_second = 0;
};

class AutoTuner {
public:
    static std::vector<TuningResult> benchmark(
        const BackendRegistry& registry,
        std::chrono::milliseconds duration = std::chrono::milliseconds(350));
};

class CpuBackend final : public IComputeBackend {
public:
    explicit CpuBackend(
        WalletVersion version = WalletVersion::v5r1,
        MnemonicScheme scheme = MnemonicScheme::multichain12)
        : version_(version), scheme_(scheme) {}
    BackendInfo info() const override;
    bool self_test() override;
    std::uint64_t run_batch(
        std::uint32_t size,
        const SearchPattern& pattern,
        const std::function<void(WalletCandidate&&)>& on_match) override;
private:
    WalletVersion version_;
    MnemonicScheme scheme_;
};

class DeviceManager {
public:
    static std::vector<DeviceInfo> discover();
};

class PerformanceMonitor {
public:
    void record(std::uint64_t count);
    std::uint64_t total() const;
    double rate() const;
private:
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::atomic<std::uint64_t> total_ = 0;
};

class FallbackManager {
public:
    static std::shared_ptr<IComputeBackend> choose(
        const std::vector<TuningResult>& tested);
};

class WalletPersistenceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct SearchOptions {
    SearchPattern pattern;
    std::uint64_t max_candidates = 0;
    std::uint32_t batch_size = 16;
    std::uint32_t cpu_threads = 0;  // 0 = all logical threads
    std::string output_directory = "matches";
    bool progress = false;
    std::uint32_t gpu_duty_percent = 100;
    std::shared_ptr<std::atomic<bool>> paused;
    std::shared_ptr<std::atomic<bool>> stop_requested;
};

class ComputeScheduler {
public:
    static std::uint64_t search(
        IComputeBackend& backend,
        const SearchOptions& options,
        PerformanceMonitor& monitor);
    static std::uint64_t search(
        const std::vector<IComputeBackend*>& backends,
        const SearchOptions& options,
        PerformanceMonitor& monitor);
};

std::string api_name(ComputeApi api);
std::string reveal_saved_mnemonic(const std::string& file_path);

}  // namespace vantix
