#include "vantix/tuning_cache.hpp"
#include "vantix/resources.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace vantix {
namespace {
std::filesystem::path cache_file() {
#ifdef _WIN32
    const auto* base = std::getenv("LOCALAPPDATA");
    if (!base) return {};
    return std::filesystem::path(base) / "VANTIX" / "tuning.profile";
#else
    if (const auto* base = std::getenv("XDG_CACHE_HOME"))
        return std::filesystem::path(base) / "vantix" / "tuning.profile";
    const auto* home = std::getenv("HOME");
    return home ? std::filesystem::path(home) / ".cache" / "vantix" /
                      "tuning.profile" : std::filesystem::path{};
#endif
}

std::string key_for(const BackendRegistry& registry,
                    const std::vector<DeviceInfo>& devices,
                    WalletVersion wallet, MnemonicScheme scheme) {
    std::vector<std::string> parts;
    for (const auto& device : devices) {
        std::ostringstream part;
        part << device.hardware_id << '|' << device.name << '|' << device.driver
             << '|' << device.memory_bytes;
        for (const auto api : device.available_apis) part << '|' << api_name(api);
        parts.push_back(part.str());
    }
    std::sort(parts.begin(), parts.end());
    std::ostringstream input;
    input << "VANTIX/profile-2/" << static_cast<int>(wallet) << '/'
          << static_cast<int>(scheme) << '\n';
    for (const auto& part : parts) input << part << '\n';
    std::vector<std::string> backends;
    for (const auto& backend : registry.backends()) {
        const auto info = backend->info();
        backends.push_back(api_name(info.api) + "|" + info.hardware_id + "|" +
                           info.build_id);
    }
    std::sort(backends.begin(), backends.end());
    for (const auto& part : backends) input << part << '\n';
    std::ifstream file(resource_path("gpu/vulkan/derive.spv"), std::ios::binary);
    input << std::string((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    const auto payload = input.str();
    unsigned char hash[32]{};
    unsigned int length = 0;
    if (EVP_Digest(payload.data(), payload.size(), hash, &length,
                   EVP_sha256(), nullptr) != 1 || length != 32) return {};
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (auto byte : hash) result << std::setw(2) << static_cast<unsigned>(byte);
    return result.str();
}
}

std::vector<IComputeBackend*> load_tuning_profile(
    const BackendRegistry& registry, const std::vector<DeviceInfo>& devices,
    WalletVersion wallet, MnemonicScheme scheme) {
    try {
        const auto file_path = cache_file();
        if (file_path.empty()) return {};
        std::ifstream file(file_path);
        std::string key, count_text;
        if (!std::getline(file, key) || !std::getline(file, count_text) ||
            key != key_for(registry, devices, wallet, scheme)) return {};
        const auto count = std::stoul(count_text);
        if (!count || count > registry.backends().size()) return {};
        std::vector<IComputeBackend*> selected;
        std::unordered_set<std::string> seen_devices;
        for (std::size_t i = 0; i < count; ++i) {
            std::string identity;
            if (!std::getline(file, identity)) return {};
            const auto matched = std::find_if(registry.backends().begin(),
                registry.backends().end(), [&](const auto& backend) {
                    const auto info = backend->info();
                    return identity == api_name(info.api) + "|" + info.hardware_id;
                });
            if (matched == registry.backends().end()) return {};
            if (!seen_devices.insert((*matched)->info().hardware_id).second) return {};
            selected.push_back(matched->get());
        }
        return selected;
    } catch (...) {
        // A missing or malformed cache must not stop address generation.
    }
    return {};
}

void save_tuning_profile(
    const BackendRegistry& registry,
    const std::vector<IComputeBackend*>& backends,
    const std::vector<DeviceInfo>& devices,
    WalletVersion wallet, MnemonicScheme scheme) {
    try {
        const auto file_path = cache_file();
        if (file_path.empty()) return;
        std::filesystem::create_directories(file_path.parent_path());
        const auto temporary = file_path.string() + ".tmp";
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return;
        file << key_for(registry, devices, wallet, scheme) << '\n'
             << backends.size() << '\n';
        for (const auto* backend : backends) {
            const auto info = backend->info();
            file << api_name(info.api) << '|' << info.hardware_id << '\n';
        }
        file.close();
        if (!file) return;
        std::error_code ignored;
        std::filesystem::remove(file_path, ignored);
        std::filesystem::rename(temporary, file_path);
    } catch (...) {
        // A read-only profile directory should only cause recalibration.
    }
}
}
