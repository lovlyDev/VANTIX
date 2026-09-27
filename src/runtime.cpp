#include "vantix/compute.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <condition_variable>
#include <map>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace vantix {
void SearchPattern::validate() const {
    const auto allowed = [](const std::string& part) {
        return part.size() <= 48 &&
               std::all_of(part.begin(), part.end(), [](char ch) {
                   return (ch >= 'A' && ch <= 'Z') ||
                          (ch >= 'a' && ch <= 'z') ||
                          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
               });
    };
    if (!allowed(prefix) || !allowed(suffix) || !allowed(contains)) {
        throw std::invalid_argument(
            "TON friendly addresses only contain A-Z, a-z, 0-9, '-' and '_'; "
            "each pattern must be at most 48 characters");
    }
}

namespace {

std::string hex(const Bytes32& bytes) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto byte : bytes) stream << std::setw(2) << static_cast<unsigned>(byte);
    return stream.str();
}

std::string base64_encode(const std::uint8_t* data, std::size_t size) {
    std::string result(4 * ((size + 2) / 3), '\0');
    const auto written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(result.data()), data,
        static_cast<int>(size));
    if (written < 0) throw std::runtime_error("Base64 encoding failed");
    result.resize(written);
    return result;
}

#ifndef _WIN32
const char* linux_wallet_password() {
    const auto* password = std::getenv("VANTIX_WALLET_PASSWORD");
    if (!password || std::strlen(password) < 12 || std::strlen(password) > 1024)
        throw std::runtime_error(
            "Linux wallet encryption requires VANTIX_WALLET_PASSWORD (12..1024 characters)");
    return password;
}

std::array<unsigned char, 32> linux_wallet_key(const char* password,
                                                const unsigned char* salt) {
    std::array<unsigned char, 32> key{};
    if (EVP_PBE_scrypt(password, std::strlen(password), salt, 16,
                      1 << 15, 8, 1, 64ULL * 1024 * 1024,
                      key.data(), key.size()) != 1)
        throw std::runtime_error("Linux wallet key derivation failed");
    return key;
}

std::string protect_mnemonic(const std::string& mnemonic) {
    // Format: VNTX1 || 16-byte salt || 12-byte nonce || 16-byte tag || ciphertext.
    std::vector<unsigned char> payload(5 + 16 + 12 + 16 + mnemonic.size());
    std::memcpy(payload.data(), "VNTX1", 5);
    auto* salt = payload.data() + 5;
    auto* nonce = salt + 16;
    auto* tag = nonce + 12;
    auto* ciphertext = tag + 16;
    if (RAND_bytes(salt, 16) != 1 || RAND_bytes(nonce, 12) != 1)
        throw std::runtime_error("Linux wallet random generation failed");
    auto key = linux_wallet_key(linux_wallet_password(), salt);
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(
        EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int written = 0, final_written = 0;
    const bool ok = context &&
        EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce) == 1 &&
        EVP_EncryptUpdate(context.get(), ciphertext, &written,
                          reinterpret_cast<const unsigned char*>(mnemonic.data()),
                          static_cast<int>(mnemonic.size())) == 1 &&
        EVP_EncryptFinal_ex(context.get(), ciphertext + written, &final_written) == 1 &&
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
    OPENSSL_cleanse(key.data(), key.size());
    if (!ok || written + final_written != static_cast<int>(mnemonic.size()))
        throw std::runtime_error("Linux wallet encryption failed");
    auto encoded = base64_encode(payload.data(), payload.size());
    OPENSSL_cleanse(payload.data(), payload.size());
    return encoded;
}

std::string unprotect_mnemonic(const std::string& encoded) {
    if (encoded.empty() || encoded.size() > 16384 || encoded.size() % 4 != 0)
        throw std::runtime_error("Invalid encrypted wallet data");
    std::vector<unsigned char> payload(encoded.size() * 3 / 4);
    auto size = EVP_DecodeBlock(payload.data(),
        reinterpret_cast<const unsigned char*>(encoded.data()),
        static_cast<int>(encoded.size()));
    if (size < 0) throw std::runtime_error("Invalid encrypted wallet encoding");
    if (encoded.ends_with("==")) size -= 2;
    else if (encoded.ends_with("=")) size -= 1;
    if (size < 49 || std::memcmp(payload.data(), "VNTX1", 5) != 0)
        throw std::runtime_error("Unknown encrypted wallet format");
    const auto* salt = payload.data() + 5;
    const auto* nonce = salt + 16;
    auto* tag = payload.data() + 33;
    auto* ciphertext = tag + 16;
    auto key = linux_wallet_key(linux_wallet_password(), salt);
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(
        EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    std::string phrase(static_cast<std::size_t>(size) - 49, '\0');
    int written = 0, final_written = 0;
    const bool ok = context &&
        EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce) == 1 &&
        EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char*>(phrase.data()),
                          &written, ciphertext, static_cast<int>(phrase.size())) == 1 &&
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, 16, tag) == 1 &&
        EVP_DecryptFinal_ex(context.get(),
                            reinterpret_cast<unsigned char*>(phrase.data()) + written,
                            &final_written) == 1;
    OPENSSL_cleanse(key.data(), key.size());
    OPENSSL_cleanse(payload.data(), payload.size());
    if (!ok || written + final_written != static_cast<int>(phrase.size())) {
        OPENSSL_cleanse(phrase.data(), phrase.size());
        throw std::runtime_error("Invalid Linux wallet password or corrupted result");
    }
    return phrase;
}
#endif

#ifdef _WIN32
std::string protect_mnemonic(const std::string& mnemonic) {
    DATA_BLOB input{static_cast<DWORD>(mnemonic.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(mnemonic.data()))};
    DATA_BLOB encrypted{};
    if (!CryptProtectData(&input, L"VANTIX TON recovery phrase", nullptr,
                          nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &encrypted))
        throw std::runtime_error("Windows wallet encryption failed");
    try {
        auto result = base64_encode(encrypted.pbData, encrypted.cbData);
        OPENSSL_cleanse(encrypted.pbData, encrypted.cbData);
        LocalFree(encrypted.pbData);
        return result;
    } catch (...) {
        OPENSSL_cleanse(encrypted.pbData, encrypted.cbData);
        LocalFree(encrypted.pbData);
        throw;
    }
}

std::string unprotect_mnemonic(const std::string& encoded) {
    if (encoded.empty() || encoded.size() > 16384 || encoded.size() % 4 != 0)
        throw std::runtime_error("Invalid encrypted wallet data");
    std::vector<unsigned char> binary(encoded.size() * 3 / 4);
    auto size = EVP_DecodeBlock(binary.data(),
        reinterpret_cast<const unsigned char*>(encoded.data()),
        static_cast<int>(encoded.size()));
    if (size < 0) throw std::runtime_error("Invalid encrypted wallet encoding");
    if (encoded.ends_with("==")) size -= 2;
    else if (encoded.ends_with("=")) size -= 1;
    DATA_BLOB input{static_cast<DWORD>(size), binary.data()};
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr,
                            nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("Cannot decrypt this wallet for the current Windows user");
    std::string phrase(reinterpret_cast<char*>(output.pbData), output.cbData);
    OPENSSL_cleanse(output.pbData, output.cbData);
    LocalFree(output.pbData);
    OPENSSL_cleanse(binary.data(), binary.size());
    return phrase;
}
#endif

std::filesystem::path save_match(
    const WalletCandidate& candidate, const SearchOptions& options) {
    const std::filesystem::path directory(options.output_directory);
    std::filesystem::create_directories(directory);
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    const auto final_path = directory / (candidate.address + ".txt");
    const auto temp_path = directory / (candidate.address + ".tmp");
    if (std::filesystem::exists(final_path) || std::filesystem::exists(temp_path)) {
        throw std::runtime_error("Refusing to overwrite an existing wallet result");
    }

    {
        std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot open wallet result file");
        std::filesystem::permissions(temp_path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);
#ifdef _WIN32
        output << "WARNING: This file contains a Windows-user encrypted recovery phrase.\n";
#else
        output << "WARNING: This file contains a password-encrypted recovery phrase.\n";
#endif
        output << "wallet=" << (candidate.version == WalletVersion::v5r1 ? "V5R1" : "V4R2")
               << "\nworkchain=0\nnetwork=mainnet\n";
        output << "mnemonic_scheme=" <<
            (candidate.scheme == MnemonicScheme::multichain12 ? "BIP39-12" : "TON-24") << '\n';
        output << "address=" << candidate.address << '\n';
        output << "public_key=" << hex(candidate.public_key) << '\n';
#ifdef _WIN32
        output << "mnemonic_dpapi=" << protect_mnemonic(candidate.mnemonic) << '\n';
#else
        output << "mnemonic_aes256gcm=" << protect_mnemonic(candidate.mnemonic) << '\n';
#endif
        output.flush();
        if (!output) throw std::runtime_error("Cannot write wallet result file");
    }
    std::filesystem::rename(temp_path, final_path);
    return final_path;
}

}  // namespace

std::string reveal_saved_mnemonic(const std::string& file_path) {
    std::ifstream input(file_path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() > 16384)
        throw std::runtime_error("Wallet result file is missing or too large");
    input.seekg(0);
    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator != std::string::npos)
            fields[line.substr(0, separator)] = line.substr(separator + 1);
    }
    const auto wallet = fields.at("wallet") == "V5R1" ? WalletVersion::v5r1
        : fields.at("wallet") == "V4R2" ? WalletVersion::v4r2
        : throw std::runtime_error("Unknown wallet version");
    const auto scheme = fields.at("mnemonic_scheme") == "BIP39-12"
        ? MnemonicScheme::multichain12
        : fields.at("mnemonic_scheme") == "TON-24" ? MnemonicScheme::ton24
        : throw std::runtime_error("Unknown mnemonic scheme");
#ifdef _WIN32
    auto phrase = unprotect_mnemonic(fields.at("mnemonic_dpapi"));
#else
    auto phrase = unprotect_mnemonic(fields.at("mnemonic_aes256gcm"));
#endif
    try {
        const auto candidate = candidate_from_mnemonic(phrase, wallet, scheme);
        if (candidate.address != fields.at("address") ||
            hex(candidate.public_key) != fields.at("public_key"))
            throw std::runtime_error("Saved wallet failed address verification");
    } catch (...) {
        OPENSSL_cleanse(phrase.data(), phrase.size());
        throw;
    }
    return phrase;
}

void PerformanceMonitor::record(std::uint64_t count) {
    total_.fetch_add(count, std::memory_order_relaxed);
}

std::uint64_t PerformanceMonitor::total() const {
    return total_.load(std::memory_order_relaxed);
}

double PerformanceMonitor::rate() const {
    const auto seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started_).count();
    return seconds > 0 ? static_cast<double>(total()) / seconds : 0;
}

std::uint64_t ComputeScheduler::search(
    IComputeBackend& backend, const SearchOptions& options, PerformanceMonitor& monitor) {
    return search(std::vector<IComputeBackend*>{&backend}, options, monitor);
}

std::uint64_t ComputeScheduler::search(
    const std::vector<IComputeBackend*>& backends,
    const SearchOptions& options, PerformanceMonitor& monitor) {
    if (backends.empty()) throw std::invalid_argument("No compute backends selected");
    if (options.pattern.prefix.empty() && options.pattern.suffix.empty() &&
        options.pattern.contains.empty()) {
        throw std::invalid_argument("A prefix, suffix, or contains pattern is required");
    }
    options.pattern.validate();
    if (options.batch_size == 0) throw std::invalid_argument("Batch size must be positive");
    if (options.gpu_duty_percent < 1 || options.gpu_duty_percent > 100)
        throw std::invalid_argument("GPU duty percentage must be 1..100");
    std::vector<IComputeBackend*> worker_backends;
    for (auto* backend : backends) {
        if (!backend) throw std::invalid_argument("Null compute backend");
        const auto threads = backend->info().api == ComputeApi::cpu
            ? (options.cpu_threads ? options.cpu_threads
                                   : std::max(1u, std::thread::hardware_concurrency()))
            : 1u;
        for (unsigned thread = 0; thread < threads; ++thread)
            worker_backends.push_back(backend);
    }
    std::atomic<std::uint64_t> assigned = 0;
    std::atomic<bool> found = false;
    std::mutex result_mutex;
    std::exception_ptr failure;
    std::vector<std::jthread> workers;
    std::mutex progress_mutex;
    std::condition_variable_any progress_cv;
    std::jthread reporter;
    if (options.progress) {
        reporter = std::jthread([&](std::stop_token stop) {
            std::unique_lock progress_lock(progress_mutex);
            auto previous_time = std::chrono::steady_clock::now();
            std::uint64_t previous_total = monitor.total();
            while (!stop.stop_requested()) {
                progress_cv.wait_for(progress_lock, stop,
                                     std::chrono::seconds(1), [] { return false; });
                if (stop.stop_requested()) break;
                const auto now = std::chrono::steady_clock::now();
                const auto total = monitor.total();
                const auto seconds = std::chrono::duration<double>(now - previous_time).count();
                const auto rate = seconds > 0 ? (total - previous_total) / seconds : 0.0;
                previous_total = total;
                previous_time = now;
                std::lock_guard output_lock(result_mutex);
                std::cout << "PROGRESS " << total << ' ' << rate << '\n' << std::flush;
            }
        });
    }
    workers.reserve(worker_backends.size());
    for (auto* backend : worker_backends) {
        workers.emplace_back([&, backend](std::stop_token stop) {
            try {
                auto stopping = [&] {
                    return stop.stop_requested() ||
                           (options.stop_requested &&
                            options.stop_requested->load(std::memory_order_acquire));
                };
                while (!stopping() && !found.load(std::memory_order_acquire)) {
                    while (options.paused && options.paused->load(std::memory_order_acquire) &&
                           !stopping() && !found.load(std::memory_order_acquire))
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    if (stopping() || found.load(std::memory_order_acquire)) break;
                    const auto start = assigned.fetch_add(options.batch_size);
                    if (options.max_candidates && start >= options.max_candidates) break;
                    const auto remaining = options.max_candidates
                        ? options.max_candidates - start : options.batch_size;
                    const auto size = static_cast<std::uint32_t>(
                        std::min<std::uint64_t>(options.batch_size, remaining));
                    const auto batch_started = std::chrono::steady_clock::now();
                    const auto checked = backend->run_batch(size, options.pattern,
                        [&](WalletCandidate&& candidate) {
                        bool expected = false;
                        if (!found.compare_exchange_strong(expected, true)) return;
                        try {
                            const auto path = save_match(candidate, options);
                            std::lock_guard lock(result_mutex);
                            std::cout << "MATCH " << candidate.address << '\n';
                            std::cout << "Saved wallet: " << path.string() << '\n';
                        } catch (const std::exception& error) {
                            found.store(false);
                            throw WalletPersistenceError(error.what());
                        }
                        });
                    monitor.record(checked);
                    if (backend->info().api != ComputeApi::cpu &&
                        options.gpu_duty_percent < 100) {
                        const auto active = std::chrono::steady_clock::now() - batch_started;
                        const auto ratio = 100.0 / options.gpu_duty_percent - 1.0;
                        std::this_thread::sleep_for(active * ratio);
                    }
                }
            } catch (...) {
                std::lock_guard lock(result_mutex);
                if (!failure) failure = std::current_exception();
                found.store(true);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    if (reporter.joinable()) {
        reporter.request_stop();
        progress_cv.notify_all();
        reporter.join();
    }
    if (failure) std::rethrow_exception(failure);
    return monitor.total();
}

}  // namespace vantix
