#include "vantix/ton.hpp"
#include "vantix/resources.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace vantix {
namespace {

constexpr std::array<std::uint8_t, 32> v4_code_hash = {
    0xfe, 0xb5, 0xff, 0x68, 0x20, 0xe2, 0xff, 0x0d,
    0x94, 0x83, 0xe7, 0xe0, 0xd6, 0x2c, 0x81, 0x7d,
    0x84, 0x67, 0x89, 0xfb, 0x4a, 0xe5, 0x80, 0xc8,
    0x78, 0x86, 0x6d, 0x95, 0x9d, 0xab, 0xd5, 0xc0
};
constexpr std::uint16_t v4_code_depth = 7;
constexpr std::uint32_t v4_wallet_id = 0x29a9a317;
constexpr std::array<std::uint8_t, 32> v5_code_hash = {
    0x20, 0x83, 0x4b, 0x7b, 0x72, 0xb1, 0x12, 0x14,
    0x7e, 0x1b, 0x2f, 0xb4, 0x57, 0xb8, 0x4e, 0x74,
    0xd1, 0xa3, 0x0f, 0x04, 0xf7, 0x37, 0xd4, 0xf6,
    0x2a, 0x66, 0x8e, 0x95, 0x52, 0xd2, 0xb7, 0x2f
};
constexpr std::uint16_t v5_code_depth = 6;
constexpr std::uint32_t v5_mainnet_wallet_id = 0x7fffff11;

Bytes32 sha256(std::span<const std::uint8_t> data) {
    Bytes32 result{};
    unsigned int length = 0;
    if (EVP_Digest(data.data(), data.size(), result.data(), &length, EVP_sha256(), nullptr) != 1 ||
        length != result.size()) {
        throw std::runtime_error("SHA-256 failed");
    }
    return result;
}

std::array<std::uint8_t, 64> entropy_for(const std::string& phrase) {
    std::array<std::uint8_t, 64> entropy{};
    unsigned int length = 0;
    const auto* key = reinterpret_cast<const unsigned char*>(phrase.data());
    const unsigned char empty = 0;
    if (!HMAC(EVP_sha512(), key, static_cast<int>(phrase.size()), &empty, 0,
              entropy.data(), &length) || length != entropy.size()) {
        throw std::runtime_error("HMAC-SHA512 failed");
    }
    return entropy;
}

std::array<std::uint8_t, 64> pbkdf2(
    std::span<const std::uint8_t> password, std::string_view salt, int iterations) {
    std::array<std::uint8_t, 64> output{};
    if (PKCS5_PBKDF2_HMAC(reinterpret_cast<const char*>(password.data()),
                          static_cast<int>(password.size()),
                          reinterpret_cast<const unsigned char*>(salt.data()),
                          static_cast<int>(salt.size()), iterations,
                          EVP_sha512(), static_cast<int>(output.size()), output.data()) != 1) {
        throw std::runtime_error("PBKDF2-HMAC-SHA512 failed");
    }
    return output;
}

std::array<std::uint8_t, 64> hmac512(
    std::span<const std::uint8_t> key, std::span<const std::uint8_t> data) {
    std::array<std::uint8_t, 64> output{};
    unsigned int length = 0;
    if (!HMAC(EVP_sha512(), key.data(), static_cast<int>(key.size()), data.data(),
              data.size(), output.data(), &length) || length != output.size()) {
        throw std::runtime_error("HMAC-SHA512 failed");
    }
    return output;
}

Bytes32 ed25519_public_key(std::span<const std::uint8_t, 32> seed) {
    auto* raw_key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), 32);
    if (!raw_key) throw std::runtime_error("Ed25519 private key creation failed");
    Bytes32 public_key{};
    std::size_t length = public_key.size();
    const auto ok = EVP_PKEY_get_raw_public_key(raw_key, public_key.data(), &length);
    EVP_PKEY_free(raw_key);
    if (ok != 1 || length != public_key.size()) {
        throw std::runtime_error("Ed25519 public key derivation failed");
    }
    return public_key;
}

bool basic_seed(const std::string& phrase) {
    auto entropy = entropy_for(phrase);
    auto test = pbkdf2(entropy, "TON seed version", 390);
    const bool valid = test.front() == 0;
    OPENSSL_cleanse(entropy.data(), entropy.size());
    OPENSSL_cleanse(test.data(), test.size());
    return valid;
}

const std::vector<std::string>& words() {
    static const auto list = [] {
        std::ifstream file(resource_path("data/ton-english.txt"));
        if (!file) {
            throw std::runtime_error("TON wordlist missing");
        }
        std::vector<std::string> result;
        std::string word;
        while (file >> word) {
            result.push_back(word);
        }
        if (result.size() != 2048) {
            throw std::runtime_error("TON wordlist must contain exactly 2048 words");
        }
        return result;
    }();
    return list;
}

std::string join_random_words() {
    const auto& list = words();
    std::array<std::uint8_t, 48> random{};
    if (RAND_priv_bytes(random.data(), static_cast<int>(random.size())) != 1) {
        throw std::runtime_error("OS cryptographic random generator unavailable");
    }
    std::string phrase;
    for (std::size_t i = 0; i < 24; ++i) {
        const auto index = static_cast<std::size_t>(
            (static_cast<unsigned>(random[2 * i]) << 8 | random[2 * i + 1]) & 2047);
        if (i) phrase.push_back(' ');
        phrase += list[index];
    }
    OPENSSL_cleanse(random.data(), random.size());
    return phrase;
}

std::string encode_bip39_12(const std::array<std::uint8_t, 16>& entropy) {
    const auto digest = sha256(entropy);
    const auto& list = words();
    auto bit_at = [&](std::size_t bit) {
        return bit < 128
            ? (entropy[bit / 8] >> (7 - bit % 8)) & 1
            : (digest[0] >> (7 - (bit - 128))) & 1;
    };
    std::string phrase;
    for (std::size_t word = 0; word < 12; ++word) {
        std::size_t index = 0;
        for (std::size_t bit = 0; bit < 11; ++bit)
            index = (index << 1) | bit_at(word * 11 + bit);
        if (word) phrase.push_back(' ');
        phrase += list[index];
    }
    return phrase;
}

std::uint16_t crc16_xmodem(std::span<const std::uint8_t> data) {
    std::uint16_t crc = 0;
    for (auto byte : data) {
        crc ^= static_cast<std::uint16_t>(byte) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<std::uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
        }
    }
    return crc;
}

}  // namespace

std::vector<std::string> load_wordlist() {
    return words();
}

Bytes32 public_key_from_ed25519_seed(const Bytes32& seed) {
    return ed25519_public_key(seed);
}

std::string generate_ton_mnemonic() {
    while (true) {
        auto phrase = join_random_words();
        if (basic_seed(phrase)) return phrase;
        OPENSSL_cleanse(phrase.data(), phrase.size());
    }
}

std::string generate_multichain_mnemonic() {
    std::array<std::uint8_t, 16> entropy{};
    if (RAND_priv_bytes(entropy.data(), static_cast<int>(entropy.size())) != 1) {
        throw std::runtime_error("OS cryptographic random generator unavailable");
    }
    auto phrase = encode_bip39_12(entropy);
    OPENSSL_cleanse(entropy.data(), entropy.size());
    return phrase;
}

bool is_valid_multichain_mnemonic(const std::string& mnemonic) {
    const auto& list = words();
    std::istringstream input(mnemonic);
    std::array<std::size_t, 12> indexes{};
    std::string word;
    for (auto& index : indexes) {
        if (!(input >> word)) return false;
        const auto entry = std::lower_bound(list.begin(), list.end(), word);
        if (entry == list.end() || *entry != word) return false;
        index = static_cast<std::size_t>(entry - list.begin());
    }
    if (input >> word) return false;
    std::array<std::uint8_t, 16> entropy{};
    unsigned checksum = 0;
    for (std::size_t bit = 0; bit < 132; ++bit) {
        const auto value = (indexes[bit / 11] >> (10 - bit % 11)) & 1;
        if (bit < 128) {
            entropy[bit / 8] |= static_cast<std::uint8_t>(value << (7 - bit % 8));
        } else {
            checksum = (checksum << 1) | static_cast<unsigned>(value);
        }
    }
    const auto digest = sha256(entropy);
    OPENSSL_cleanse(entropy.data(), entropy.size());
    return checksum == (digest[0] >> 4);
}

bool is_valid_ton_mnemonic(const std::string& mnemonic) {
    const auto& list = words();
    std::istringstream input(mnemonic);
    std::vector<std::string> entries;
    std::string word;
    while (input >> word) {
        if (!std::binary_search(list.begin(), list.end(), word)) return false;
        entries.push_back(word);
    }
    if (entries.size() != 24) return false;
    std::string normalized;
    for (const auto& entry : entries) {
        if (!normalized.empty()) normalized.push_back(' ');
        normalized += entry;
    }
    return basic_seed(normalized);
}

Bytes32 mnemonic_public_key(const std::string& mnemonic) {
    auto entropy = entropy_for(mnemonic);
    auto seed = pbkdf2(entropy, "TON default seed", 100000);
    OPENSSL_cleanse(entropy.data(), entropy.size());

    const auto public_key = ed25519_public_key(
        std::span<const std::uint8_t, 32>(seed.data(), 32));
    OPENSSL_cleanse(seed.data(), seed.size());
    return public_key;
}

Bytes32 multichain_public_key(const std::string& mnemonic) {
    const auto* phrase = reinterpret_cast<const std::uint8_t*>(mnemonic.data());
    auto root = pbkdf2(std::span<const std::uint8_t>(phrase, mnemonic.size()),
                       "mnemonic", 2048);
    constexpr std::string_view curve = "ed25519 seed";
    const auto* curve_bytes = reinterpret_cast<const std::uint8_t*>(curve.data());
    auto digest = hmac512(std::span<const std::uint8_t>(curve_bytes, curve.size()), root);
    OPENSSL_cleanse(root.data(), root.size());
    for (const std::uint32_t index : {44u, 607u, 0u}) {
        std::array<std::uint8_t, 37> data{};
        std::copy_n(digest.begin(), 32, data.begin() + 1);
        const auto hardened = index | 0x80000000u;
        data[33] = static_cast<std::uint8_t>(hardened >> 24);
        data[34] = static_cast<std::uint8_t>(hardened >> 16);
        data[35] = static_cast<std::uint8_t>(hardened >> 8);
        data[36] = static_cast<std::uint8_t>(hardened);
        auto next = hmac512(std::span<const std::uint8_t>(digest.data() + 32, 32), data);
        OPENSSL_cleanse(data.data(), data.size());
        OPENSSL_cleanse(digest.data(), digest.size());
        digest = next;
        OPENSSL_cleanse(next.data(), next.size());
    }
    const auto public_key = ed25519_public_key(
        std::span<const std::uint8_t, 32>(digest.data(), 32));
    OPENSSL_cleanse(digest.data(), digest.size());
    return public_key;
}

Bytes32 v4r2_address_hash(const Bytes32& public_key) {
    // Data cell: seqno:32, wallet_id:32, pubkey:256, empty plugins dict:1.
    // The final 0x40 is the empty dict bit followed by TON's top-up bit.
    std::array<std::uint8_t, 43> data_representation{};
    data_representation[0] = 0;       // no references
    data_representation[1] = 81;      // floor(321/8) + ceil(321/8)
    data_representation[6] = static_cast<std::uint8_t>(v4_wallet_id >> 24);
    data_representation[7] = static_cast<std::uint8_t>(v4_wallet_id >> 16);
    data_representation[8] = static_cast<std::uint8_t>(v4_wallet_id >> 8);
    data_representation[9] = static_cast<std::uint8_t>(v4_wallet_id);
    std::copy(public_key.begin(), public_key.end(), data_representation.begin() + 10);
    data_representation[42] = 0x40;
    const auto data_hash = sha256(data_representation);

    // StateInit bits: split_depth=0, special=0, code=1, data=1, library=0.
    std::array<std::uint8_t, 71> state_representation{};
    state_representation[0] = 2;      // two references: code and data
    state_representation[1] = 1;      // five bits
    state_representation[2] = 0x34;   // 00110 + top-up bit
    state_representation[3] = static_cast<std::uint8_t>(v4_code_depth >> 8);
    state_representation[4] = static_cast<std::uint8_t>(v4_code_depth);
    // bytes 5 and 6 are the data cell's zero depth
    std::copy(v4_code_hash.begin(), v4_code_hash.end(), state_representation.begin() + 7);
    std::copy(data_hash.begin(), data_hash.end(), state_representation.begin() + 39);
    return sha256(state_representation);
}

Bytes32 v5r1_address_hash(const Bytes32& public_key) {
    // V5R1 data cell: signature_enabled:1, seqno:32, wallet_id:32,
    // public_key:256, empty extensions dict:1. Total: 322 bits.
    std::array<std::uint8_t, 43> data_representation{};
    data_representation[0] = 0;
    data_representation[1] = 81;
    auto set_bit = [&](std::size_t bit, bool value) {
        if (value) data_representation[2 + bit / 8] |=
            static_cast<std::uint8_t>(0x80 >> (bit % 8));
    };
    set_bit(0, true);
    for (std::size_t bit = 0; bit < 32; ++bit) {
        set_bit(33 + bit, (v5_mainnet_wallet_id >> (31 - bit)) & 1);
    }
    for (std::size_t byte = 0; byte < public_key.size(); ++byte) {
        for (std::size_t bit = 0; bit < 8; ++bit) {
            set_bit(65 + byte * 8 + bit, (public_key[byte] >> (7 - bit)) & 1);
        }
    }
    // Bit 321 is the empty dict. Bit 322 is the top-up bit.
    set_bit(322, true);
    const auto data_hash = sha256(data_representation);
    std::array<std::uint8_t, 71> state_representation{};
    state_representation[0] = 2;
    state_representation[1] = 1;
    state_representation[2] = 0x34;
    state_representation[3] = static_cast<std::uint8_t>(v5_code_depth >> 8);
    state_representation[4] = static_cast<std::uint8_t>(v5_code_depth);
    std::copy(v5_code_hash.begin(), v5_code_hash.end(), state_representation.begin() + 7);
    std::copy(data_hash.begin(), data_hash.end(), state_representation.begin() + 39);
    return sha256(state_representation);
}

std::string friendly_address(const Bytes32& hash, bool testnet, bool bounceable) {
    std::array<std::uint8_t, 36> bytes{};
    bytes[0] = static_cast<std::uint8_t>((bounceable ? 0x11 : 0x51) | (testnet ? 0x80 : 0));
    bytes[1] = 0;  // basechain
    std::copy(hash.begin(), hash.end(), bytes.begin() + 2);
    const auto crc = crc16_xmodem(std::span<const std::uint8_t>(bytes.data(), 34));
    bytes[34] = static_cast<std::uint8_t>(crc >> 8);
    bytes[35] = static_cast<std::uint8_t>(crc);
    std::array<unsigned char, 49> encoded{};
    const auto length = EVP_EncodeBlock(encoded.data(), bytes.data(), static_cast<int>(bytes.size()));
    if (length != 48) throw std::runtime_error("TON address encoding failed");
    std::string result(reinterpret_cast<const char*>(encoded.data()), 48);
    std::replace(result.begin(), result.end(), '+', '-');
    std::replace(result.begin(), result.end(), '/', '_');
    return result;
}

WalletCandidate candidate_from_mnemonic(
    const std::string& mnemonic, WalletVersion version, MnemonicScheme scheme) {
    WalletCandidate candidate{};
    candidate.mnemonic = mnemonic;
    candidate.version = version;
    candidate.scheme = scheme;
    candidate.public_key = scheme == MnemonicScheme::multichain12
        ? multichain_public_key(mnemonic) : mnemonic_public_key(mnemonic);
    candidate.address_hash = version == WalletVersion::v5r1
        ? v5r1_address_hash(candidate.public_key)
        : v4r2_address_hash(candidate.public_key);
    candidate.address = friendly_address(candidate.address_hash);
    return candidate;
}

}  // namespace vantix
