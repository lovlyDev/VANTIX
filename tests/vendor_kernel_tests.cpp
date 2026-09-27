#include "vantix/ton.hpp"

#include <openssl/crypto.h>

#include <array>
#include <cstring>
#include <iostream>

// Compile the exact CUDA/HIP derivation body as ordinary C++ for one thread.
// This checks its math without pretending to validate SDK compilation or GPU execution.
#define __device__
#define __constant__
#define __forceinline__ inline
#define __global__
struct ThreadCoordinate { unsigned x; };
ThreadCoordinate blockIdx{0}, blockDim{64}, threadIdx{0};
#include "../gpu/vendor/derive_kernel.hpp"

int main() {
    constexpr char phrase[] =
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon about";
    std::array<unsigned char, 128> input{};
    std::array<unsigned char, 32> output{};
    std::memcpy(input.data(), phrase, sizeof(phrase) - 1);
    vantix_gpu::derive_kernel(input.data(), output.data(), 1);
    vantix::Bytes32 seed{};
    std::memcpy(seed.data(), output.data(), seed.size());
    const auto actual = vantix::public_key_from_ed25519_seed(seed);
    const auto expected = vantix::multichain_public_key(phrase);
    OPENSSL_cleanse(seed.data(), seed.size());
    OPENSSL_cleanse(output.data(), output.size());
    OPENSSL_cleanse(input.data(), input.size());
    if (actual != expected) {
        std::cerr << "Vendor derivation differs from CPU reference\n";
        return 1;
    }
    return 0;
}
