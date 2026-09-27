#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vantix {

using Bytes32 = std::array<std::uint8_t, 32>;
enum class WalletVersion { v4r2, v5r1 };
enum class MnemonicScheme { multichain12, ton24 };

struct WalletCandidate {
    std::string mnemonic;
    WalletVersion version = WalletVersion::v5r1;
    MnemonicScheme scheme = MnemonicScheme::multichain12;
    Bytes32 public_key{};
    Bytes32 address_hash{};
    std::string address;
};

// Wallet V4R2, workchain 0, wallet_id 0x29a9a317, non-bounceable mainnet.
Bytes32 v4r2_address_hash(const Bytes32& public_key);
Bytes32 v5r1_address_hash(const Bytes32& public_key);
std::string friendly_address(const Bytes32& hash, bool testnet = false, bool bounceable = false);
Bytes32 mnemonic_public_key(const std::string& mnemonic);
Bytes32 multichain_public_key(const std::string& mnemonic);
Bytes32 public_key_from_ed25519_seed(const Bytes32& seed);
bool is_valid_ton_mnemonic(const std::string& mnemonic);
bool is_valid_multichain_mnemonic(const std::string& mnemonic);
WalletCandidate candidate_from_mnemonic(
    const std::string& mnemonic, WalletVersion version = WalletVersion::v5r1,
    MnemonicScheme scheme = MnemonicScheme::multichain12);
std::string generate_ton_mnemonic();
std::string generate_multichain_mnemonic();
std::vector<std::string> load_wordlist();

}  // namespace vantix
