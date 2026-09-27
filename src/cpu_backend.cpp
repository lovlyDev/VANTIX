#include "vantix/compute.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace vantix {

BackendInfo CpuBackend::info() const {
    return {std::string("OpenSSL CPU ") +
                (scheme_ == MnemonicScheme::multichain12 ? "BIP39-12 " : "TON-24 ") +
                (version_ == WalletVersion::v5r1 ? "V5R1" : "V4R2"),
            ComputeApi::cpu, "cpu:host", __DATE__ " " __TIME__};
}

bool CpuBackend::self_test() {
    // Public fixtures derived with the TON SDK; never fund them.
    constexpr std::string_view ton_phrase =
        "tag couch guilt honey garden illegal injury harsh battle elder donor hidden "
        "forget imitate suffer apology check hedgehog ability window clip romance correct jacket";
    constexpr std::string_view multichain_phrase =
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon about";
    constexpr std::string_view ton_v4 =
        "UQB8bN7tL9Zyet9jvvzkllhUnQAuGe3T6-PiRZsF5mq8__D8";
    constexpr std::string_view ton_v5 =
        "UQBXB_8fB0iAE1pf3seahie7sM6HZSoWh9xq5_oqYK44x3Br";
    constexpr std::string_view multichain_v4 =
        "UQAzWZa6nM5mJev91wGc7VCSfBoIsYRqKJpV78N8Add9-RKY";
    constexpr std::string_view multichain_v5 =
        "UQBHyu-oZVDHRYQ1-rKlGqpHy5yAqanPBirEQNMNOmfHLtaT";
    const auto phrase = scheme_ == MnemonicScheme::multichain12
        ? multichain_phrase : ton_phrase;
    if (scheme_ == MnemonicScheme::multichain12
            ? !is_valid_multichain_mnemonic(std::string(phrase))
            : !is_valid_ton_mnemonic(std::string(phrase))) return false;
    auto candidate = candidate_from_mnemonic(std::string(phrase), version_, scheme_);
    const bool valid = candidate.address ==
        (scheme_ == MnemonicScheme::multichain12
             ? (version_ == WalletVersion::v5r1 ? multichain_v5 : multichain_v4)
             : (version_ == WalletVersion::v5r1 ? ton_v5 : ton_v4));
    OPENSSL_cleanse(candidate.mnemonic.data(), candidate.mnemonic.size());
    return valid;
}

std::uint64_t CpuBackend::run_batch(
    std::uint32_t size,
    const SearchPattern& pattern,
    const std::function<void(WalletCandidate&&)>& on_match) {
    std::uint64_t generated = 0;
    for (std::uint32_t i = 0; i < size; ++i) {
        auto mnemonic = scheme_ == MnemonicScheme::multichain12
            ? generate_multichain_mnemonic() : generate_ton_mnemonic();
        WalletCandidate candidate{};
        try {
            candidate.version = version_;
            candidate.scheme = scheme_;
            candidate.public_key = scheme_ == MnemonicScheme::multichain12
                ? multichain_public_key(mnemonic) : mnemonic_public_key(mnemonic);
            candidate.address_hash = version_ == WalletVersion::v5r1
                ? v5r1_address_hash(candidate.public_key)
                : v4r2_address_hash(candidate.public_key);
            candidate.address = friendly_address(candidate.address_hash);
            if (pattern.matches(candidate.address)) {
                candidate.mnemonic = mnemonic;
                on_match(std::move(candidate));
            }
        } catch (...) {
            OPENSSL_cleanse(candidate.mnemonic.data(), candidate.mnemonic.size());
            OPENSSL_cleanse(mnemonic.data(), mnemonic.size());
            throw;
        }
        OPENSSL_cleanse(candidate.mnemonic.data(), candidate.mnemonic.size());
        OPENSSL_cleanse(mnemonic.data(), mnemonic.size());
        ++generated;
    }
    return generated;
}

void BackendRegistry::add(std::shared_ptr<IComputeBackend> backend) {
    if (!backend) throw std::invalid_argument("backend is null");
    const auto incoming = backend->info();
    for (const auto& existing : backends_) {
        const auto registered = existing->info();
        if (registered.api == incoming.api &&
            registered.hardware_id == incoming.hardware_id) {
            throw std::invalid_argument("duplicate backend for device and API");
        }
    }
    backends_.push_back(std::move(backend));
}

const std::vector<std::shared_ptr<IComputeBackend>>& BackendRegistry::backends() const {
    return backends_;
}

std::vector<TuningResult> AutoTuner::benchmark(
    const BackendRegistry& registry, std::chrono::milliseconds duration) {
    std::vector<TuningResult> results;
    for (const auto& backend : registry.backends()) {
        if (!backend->self_test()) continue;
        const auto workers = backend->info().api == ComputeApi::cpu
            ? std::max(1u, std::thread::hardware_concurrency()) : 1u;
        const auto batch = backend->info().api == ComputeApi::cpu ? 16u
            : backend->info().api == ComputeApi::vulkan ? 64u : 128u;
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + duration;
        std::atomic<std::uint64_t> count = 0;
        std::mutex failure_mutex;
        std::exception_ptr failure;
        {
            std::vector<std::jthread> threads;
            threads.reserve(workers);
            for (unsigned worker = 0; worker < workers; ++worker) {
                threads.emplace_back([&] {
                    try {
                        do {
                            count.fetch_add(backend->run_batch(
                                batch, SearchPattern{"Z", "", "", false},
                                [](WalletCandidate&&) {}), std::memory_order_relaxed);
                        } while (std::chrono::steady_clock::now() < deadline);
                    } catch (...) {
                        std::lock_guard lock(failure_mutex);
                        if (!failure) failure = std::current_exception();
                    }
                });
            }
        }
        if (failure) continue;
        const auto seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        results.push_back({backend, count.load(std::memory_order_relaxed) / seconds});
    }
    std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) {
        return a.verified_addresses_per_second > b.verified_addresses_per_second;
    });
    return results;
}

std::shared_ptr<IComputeBackend> FallbackManager::choose(
    const std::vector<TuningResult>& tested) {
    if (tested.empty()) throw std::runtime_error("No validated compute backend available");
    return tested.front().backend;
}

std::string api_name(ComputeApi api) {
    switch (api) {
        case ComputeApi::cpu: return "CPU";
        case ComputeApi::vulkan: return "Vulkan";
        case ComputeApi::cuda: return "CUDA";
        case ComputeApi::hip: return "HIP";
    }
    return "Unknown";
}

}  // namespace vantix
