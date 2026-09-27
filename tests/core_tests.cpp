#include "vantix/compute.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#include <cstdlib>
#endif

namespace {

std::string hex(const vantix::Bytes32& bytes) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (auto byte : bytes) stream << std::setw(2) << static_cast<unsigned>(byte);
    return stream.str();
}

}  // namespace

int main() {
    using namespace vantix;
    constexpr auto phrase =
        "tag couch guilt honey garden illegal injury harsh battle elder donor hidden "
        "forget imitate suffer apology check hedgehog ability window clip romance correct jacket";
    assert(load_wordlist().size() == 2048);
    assert(is_valid_ton_mnemonic(phrase));
    assert(!is_valid_ton_mnemonic("this is not a TON mnemonic"));
    const auto candidate = candidate_from_mnemonic(
        phrase, WalletVersion::v4r2, MnemonicScheme::ton24);
    assert(hex(candidate.public_key) ==
           "9e315b6568d2b9e7e353b73b70a64b5b028d0dd36a4457854d1dd9f69959feb8");
    assert(hex(candidate.address_hash) ==
           "7c6cdeed2fd6727adf63befce49658549d002e19edd3ebe3e2459b05e66abcff");
    assert(candidate.address == "UQB8bN7tL9Zyet9jvvzkllhUnQAuGe3T6-PiRZsF5mq8__D8");

    const Bytes32 zero{};
    assert(hex(v4r2_address_hash(zero)) ==
           "f0c4c1c69a8227f77ea088dadd8adf14610c8de361779411c5c43c45aa2737e6");
    assert(friendly_address(v4r2_address_hash(zero)) ==
           "UQDwxMHGmoIn936giNrdit8UYQyN42F3lBHFxDxFqic35mIP");
    assert(hex(v5r1_address_hash(zero)) ==
           "e0e92fabe2b74d53e3eebe0486bf54887f212d3f79b884593347b834b776b20a");
    assert(friendly_address(v5r1_address_hash(zero)) ==
           "UQDg6S-r4rdNU-PuvgSGv1SIfyEtP3m4hFkzR7g0t3ayCnIt");
    const auto v5_candidate = candidate_from_mnemonic(
        phrase, WalletVersion::v5r1, MnemonicScheme::ton24);
    assert(v5_candidate.address ==
           "UQBXB_8fB0iAE1pf3seahie7sM6HZSoWh9xq5_oqYK44x3Br");
    constexpr auto bip39_phrase =
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon about";
    assert(is_valid_multichain_mnemonic(bip39_phrase));
    const auto multichain = candidate_from_mnemonic(bip39_phrase);
    assert(hex(multichain.public_key) ==
           "7952e94118f34607c75e23258dd9220d66ccac5a3ee074125c25068e8107bfbf");
    assert(multichain.address == "UQBHyu-oZVDHRYQ1-rKlGqpHy5yAqanPBirEQNMNOmfHLtaT");

    CpuBackend cpu;
    assert(cpu.self_test());
    assert(CpuBackend(WalletVersion::v4r2, MnemonicScheme::ton24).self_test());
    std::uint64_t observed = 0;
    assert(cpu.run_batch(1, SearchPattern{"UQ", "", "", false}, [&](WalletCandidate&& item) {
        assert(is_valid_multichain_mnemonic(item.mnemonic));
        assert(item.address == friendly_address(v5r1_address_hash(item.public_key)));
        ++observed;
    }) == 1);
    assert(observed == 1);

    BackendRegistry registry;
    registry.add(std::make_shared<CpuBackend>());
    bool duplicate_rejected = false;
    try { registry.add(std::make_shared<CpuBackend>()); }
    catch (const std::invalid_argument&) { duplicate_rejected = true; }
    assert(duplicate_rejected);
    const auto devices = DeviceManager::discover();
    assert(!devices.empty() && devices.front().hardware_id == "cpu:host");
    SearchOptions options;
    options.pattern.prefix = "Z";
    options.max_candidates = 2;
    options.batch_size = 1;
    options.cpu_threads = 1;
    PerformanceMonitor monitor;
    assert(ComputeScheduler::search(cpu, options, monitor) == 2);

#ifndef _WIN32
    // A Linux result must remain encrypted at rest and reject a wrong password.
    assert(setenv("VANTIX_WALLET_PASSWORD", "testing-wallet-password-123", 1) == 0);
    auto secure_dir = std::filesystem::temp_directory_path() /
        ("vantix-wallet-test-" + std::to_string(getpid()));
    SearchOptions secure_options;
    secure_options.pattern.prefix = "UQ";
    secure_options.max_candidates = 1;
    secure_options.batch_size = 1;
    secure_options.cpu_threads = 1;
    secure_options.output_directory = secure_dir.string();
    PerformanceMonitor secure_monitor;
    assert(ComputeScheduler::search(cpu, secure_options, secure_monitor) == 1);
    auto result = std::filesystem::directory_iterator(secure_dir)->path();
    std::ifstream saved(result);
    const std::string contents((std::istreambuf_iterator<char>(saved)),
                               std::istreambuf_iterator<char>());
    assert(contents.find("mnemonic_aes256gcm=") != std::string::npos);
    assert(contents.find("\nmnemonic=") == std::string::npos);
    const auto recovered = reveal_saved_mnemonic(result.string());
    assert(is_valid_multichain_mnemonic(recovered));
    assert(candidate_from_mnemonic(recovered).address == result.stem().string());
    assert(setenv("VANTIX_WALLET_PASSWORD", "wrong-wallet-password-123", 1) == 0);
    bool wrong_password_rejected = false;
    try { (void)reveal_saved_mnemonic(result.string()); }
    catch (const std::runtime_error&) { wrong_password_rejected = true; }
    assert(wrong_password_rejected);
    std::filesystem::remove(result);
    std::filesystem::remove(secure_dir);
#endif

    std::cout << "VANTIX core tests passed\n";
}
