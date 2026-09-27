#include "vantix/vendor_backend.hpp"

#include <openssl/crypto.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace vantix {
namespace {
constexpr unsigned batch_limit = 128;

class VendorLibrary {
public:
    explicit VendorLibrary(ComputeApi api) {
#ifdef _WIN32
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return;
        auto file = std::filesystem::path(path).parent_path() /
            (api == ComputeApi::cuda ? L"vantix_cuda.dll" : L"vantix_hip.dll");
        DLL_DIRECTORY_COOKIE hip_directory = nullptr;
        if (api == ComputeApi::hip) {
            wchar_t hip_path[MAX_PATH]{};
            const auto length = GetEnvironmentVariableW(L"HIP_PATH", hip_path, MAX_PATH);
            if (length > 0 && length < MAX_PATH) {
                const auto bin = std::filesystem::path(hip_path) / L"bin";
                hip_directory = AddDllDirectory(bin.c_str());
            }
        }
        handle_ = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                                     LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                                     LOAD_LIBRARY_SEARCH_USER_DIRS);
        if (hip_directory) RemoveDllDirectory(hip_directory);
#else
        char path[4096]{};
        auto length = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (length <= 0) return;
        path[length] = 0;
        auto file = std::filesystem::path(path).parent_path() /
            (api == ComputeApi::cuda ? "libvantix_cuda.so" : "libvantix_hip.so");
        handle_ = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_) {
            if (const auto* resources = std::getenv("VANTIX_RESOURCE_DIR")) {
                file = std::filesystem::path(resources) /
                    (api == ComputeApi::cuda ? "libvantix_cuda.so" : "libvantix_hip.so");
                handle_ = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
            }
        }
#endif
        if (!handle_) return;
        abi = reinterpret_cast<AbiFn>(symbol("vantix_vendor_abi_version"));
        build_id = reinterpret_cast<BuildFn>(symbol("vantix_vendor_build_id"));
        count = reinterpret_cast<CountFn>(symbol("vantix_vendor_device_count"));
        name = reinterpret_cast<InfoFn>(symbol("vantix_vendor_device_name"));
        id = reinterpret_cast<InfoFn>(symbol("vantix_vendor_device_id"));
        create = reinterpret_cast<CreateFn>(symbol("vantix_vendor_create"));
        derive = reinterpret_cast<DeriveFn>(symbol("vantix_vendor_derive"));
        error = reinterpret_cast<ErrorFn>(symbol("vantix_vendor_error"));
        destroy = reinterpret_cast<DestroyFn>(symbol("vantix_vendor_destroy"));
    }
    ~VendorLibrary() {
#ifdef _WIN32
        if (handle_) FreeLibrary(handle_);
#else
        if (handle_) dlclose(handle_);
#endif
    }
    bool ready() const {
        return abi && abi() == 1 && build_id && count && name && id && create && derive &&
               error && destroy;
    }
    using AbiFn = int (*)();
    using BuildFn = const char* (*)();
    using CountFn = int (*)();
    using InfoFn = int (*)(int, char*, std::size_t);
    using CreateFn = void* (*)(int);
    using DeriveFn = int (*)(void*, const unsigned char*, unsigned, unsigned char*);
    using ErrorFn = const char* (*)(void*);
    using DestroyFn = void (*)(void*);
    AbiFn abi = nullptr;
    BuildFn build_id = nullptr;
    CountFn count = nullptr;
    InfoFn name = nullptr, id = nullptr;
    CreateFn create = nullptr;
    DeriveFn derive = nullptr;
    ErrorFn error = nullptr;
    DestroyFn destroy = nullptr;
private:
    void* symbol(const char* key) const {
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(handle_, key));
#else
        return dlsym(handle_, key);
#endif
    }
#ifdef _WIN32
    HMODULE handle_ = nullptr;
#else
    void* handle_ = nullptr;
#endif
};

struct SecureBatch {
    std::vector<std::string> phrases;
    std::vector<unsigned char> input, seeds;
    ~SecureBatch() {
        for (auto& phrase : phrases) OPENSSL_cleanse(phrase.data(), phrase.size());
        if (!input.empty()) OPENSSL_cleanse(input.data(), input.size());
        if (!seeds.empty()) OPENSSL_cleanse(seeds.data(), seeds.size());
    }
};

class VendorBackend final : public IComputeBackend {
public:
    VendorBackend(std::shared_ptr<VendorLibrary> library, ComputeApi api,
                  int index, WalletVersion version)
        : library_(std::move(library)), api_(api), version_(version) {
        char raw_name[256]{}, raw_id[64]{};
        if (!library_->name(index, raw_name, sizeof(raw_name)) ||
            !library_->id(index, raw_id, sizeof(raw_id)))
            throw std::runtime_error("Cannot identify vendor GPU");
        name_ = std::string(api == ComputeApi::cuda ? "CUDA " : "HIP ") + raw_name;
        hardware_id_ = raw_id;
        std::vector<std::string> matches;
        for (const auto& device : DeviceManager::discover())
            if (device.name == raw_name &&
                std::find(device.available_apis.begin(), device.available_apis.end(), api_) !=
                    device.available_apis.end())
                matches.push_back(device.hardware_id);
        if (matches.size() == 1) hardware_id_ = matches.front();
        context_ = library_->create(index);
        if (!context_) throw std::runtime_error("Cannot initialize vendor GPU context");
    }
    ~VendorBackend() override { library_->destroy(context_); }
    BackendInfo info() const override {
        return {name_, api_, hardware_id_, library_->build_id()};
    }
    bool self_test() override {
        const std::string phrase =
            "abandon abandon abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon about";
        SecureBatch batch;
        batch.phrases.push_back(phrase);
        batch.input.resize(128);
        batch.seeds.resize(32);
        std::memcpy(batch.input.data(), phrase.data(), phrase.size());
        if (!library_->derive(context_, batch.input.data(), 1, batch.seeds.data())) return false;
        Bytes32 seed{};
        std::memcpy(seed.data(), batch.seeds.data(), 32);
        const auto actual = public_key_from_ed25519_seed(seed);
        const auto expected = multichain_public_key(phrase);
        OPENSSL_cleanse(seed.data(), seed.size());
        return actual == expected;
    }
    std::uint64_t run_batch(std::uint32_t size, const SearchPattern& pattern,
        const std::function<void(WalletCandidate&&)>& on_match) override {
        std::uint64_t checked = 0;
        while (checked < size) {
            const auto count = static_cast<unsigned>(std::min<std::uint64_t>(
                batch_limit, size - checked));
            SecureBatch batch;
            batch.phrases.reserve(count);
            batch.input.resize(count * 128);
            batch.seeds.resize(count * 32);
            for (unsigned i = 0; i < count; ++i) {
                batch.phrases.push_back(generate_multichain_mnemonic());
                if (batch.phrases.back().size() >= 128)
                    throw std::runtime_error("Mnemonic exceeds GPU input stride");
                std::memcpy(batch.input.data() + i * 128, batch.phrases.back().data(),
                            batch.phrases.back().size());
            }
            if (!library_->derive(context_, batch.input.data(), count, batch.seeds.data()))
                throw std::runtime_error(name_ + " derivation failed: " +
                                         library_->error(context_));
            for (unsigned i = 0; i < count; ++i) {
                Bytes32 seed{};
                std::memcpy(seed.data(), batch.seeds.data() + i * 32, 32);
                WalletCandidate candidate{};
                candidate.version = version_;
                candidate.scheme = MnemonicScheme::multichain12;
                candidate.public_key = public_key_from_ed25519_seed(seed);
                candidate.address_hash = version_ == WalletVersion::v5r1
                    ? v5r1_address_hash(candidate.public_key)
                    : v4r2_address_hash(candidate.public_key);
                candidate.address = friendly_address(candidate.address_hash);
                OPENSSL_cleanse(seed.data(), seed.size());
                if (pattern.matches(candidate.address)) {
                    if (multichain_public_key(batch.phrases[i]) != candidate.public_key)
                        throw std::runtime_error("Vendor GPU candidate failed CPU verification");
                    candidate.mnemonic = batch.phrases[i];
                    try { on_match(std::move(candidate)); }
                    catch (...) {
                        OPENSSL_cleanse(candidate.mnemonic.data(), candidate.mnemonic.size());
                        throw;
                    }
                    OPENSSL_cleanse(candidate.mnemonic.data(), candidate.mnemonic.size());
                }
            }
            checked += count;
        }
        return checked;
    }
private:
    std::shared_ptr<VendorLibrary> library_;
    ComputeApi api_;
    WalletVersion version_;
    std::string name_, hardware_id_;
    void* context_ = nullptr;
};
}  // namespace

std::vector<std::shared_ptr<IComputeBackend>> load_vendor_backends(
    ComputeApi api, WalletVersion version, MnemonicScheme scheme) {
    if (api != ComputeApi::cuda && api != ComputeApi::hip)
        throw std::invalid_argument("Vendor backend must be CUDA or HIP");
    if (scheme != MnemonicScheme::multichain12) return {};
    auto library = std::make_shared<VendorLibrary>(api);
    if (!library->ready())
        throw std::runtime_error(api == ComputeApi::cuda
            ? "vantix_cuda module or CUDA runtime is missing"
            : "vantix_hip module or HIP runtime is missing");
    std::vector<std::shared_ptr<IComputeBackend>> result;
    const auto count = library->count();
    if (count < 0 || count > 64) return {};
    for (int index = 0; index < count; ++index) {
        try { result.push_back(std::make_shared<VendorBackend>(library, api, index, version)); }
        catch (const std::exception&) { /* Other devices remain eligible. */ }
    }
    return result;
}
}  // namespace vantix
