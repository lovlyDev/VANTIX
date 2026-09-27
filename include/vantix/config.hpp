#pragma once

#include "vantix/compute.hpp"

#include <string>

namespace vantix {

struct AppConfig {
    WalletVersion wallet = WalletVersion::v5r1;
    MnemonicScheme mnemonic_scheme = MnemonicScheme::multichain12;
    SearchOptions search;
};

AppConfig load_config(const std::string& path);

}  // namespace vantix
