#include "vantix/config.hpp"

#include <toml++/toml.hpp>

#include <cstdint>
#include <stdexcept>

namespace vantix {

AppConfig load_config(const std::string& path) {
    const auto file = toml::parse_file(path);
    AppConfig config;
    if (const auto version = file["wallet"]["version"].value<std::string>()) {
        if (*version == "v5") config.wallet = WalletVersion::v5r1;
        else if (*version == "v4") config.wallet = WalletVersion::v4r2;
        else throw std::invalid_argument("wallet.version must be v5 or v4");
    }
    if (const auto scheme = file["wallet"]["mnemonic_scheme"].value<std::string>()) {
        if (*scheme == "12") config.mnemonic_scheme = MnemonicScheme::multichain12;
        else if (*scheme == "24") config.mnemonic_scheme = MnemonicScheme::ton24;
        else throw std::invalid_argument("wallet.mnemonic_scheme must be 12 or 24");
    }
    if (const auto value = file["search"]["prefix"].value<std::string>())
        config.search.pattern.prefix = *value;
    if (const auto value = file["search"]["suffix"].value<std::string>())
        config.search.pattern.suffix = *value;
    if (const auto value = file["search"]["contains"].value<std::string>())
        config.search.pattern.contains = *value;
    if (const auto value = file["search"]["ignore_case"].value<bool>())
        config.search.pattern.ignore_case = *value;
    if (const auto value = file["search"]["max_candidates"].value<std::int64_t>()) {
        if (*value < 0) throw std::invalid_argument("search.max_candidates must be nonnegative");
        config.search.max_candidates = static_cast<std::uint64_t>(*value);
    }
    if (const auto value = file["search"]["batch_size"].value<std::int64_t>()) {
        if (*value < 1 || *value > 1000000)
            throw std::invalid_argument("search.batch_size must be 1..1000000");
        config.search.batch_size = static_cast<std::uint32_t>(*value);
    }
    if (const auto value = file["search"]["cpu_threads"].value<std::int64_t>()) {
        if (*value < 0 || *value > 256)
            throw std::invalid_argument("search.cpu_threads must be 0..256");
        config.search.cpu_threads = static_cast<std::uint32_t>(*value);
    }
    if (const auto value = file["search"]["output_directory"].value<std::string>())
        config.search.output_directory = *value;
    return config;
}

}  // namespace vantix
