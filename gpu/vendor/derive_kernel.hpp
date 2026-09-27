#pragma once

#include <cstdint>

// Shared CUDA/HIP implementation. One thread derives the Ed25519 seed for one
// BIP39-12 phrase using PBKDF2-HMAC-SHA512 and hardened SLIP-0010 m/44'/607'/0'.
namespace vantix_gpu {
__device__ __constant__ std::uint64_t sha512_k[80] = {
#include "../vulkan/sha512_constants.slang"
};

__device__ __forceinline__ std::uint64_t rotr(std::uint64_t x, unsigned n) {
    return (x >> n) | (x << (64 - n));
}

__device__ void compress(std::uint64_t state[8], const unsigned char block[128]) {
    std::uint64_t w[80];
    for (unsigned i = 0; i < 16; ++i) {
        std::uint64_t value = 0;
        for (unsigned j = 0; j < 8; ++j)
            value = (value << 8) | block[i * 8 + j];
        w[i] = value;
    }
    for (unsigned i = 16; i < 80; ++i) {
        const auto a = w[i - 15], b = w[i - 2];
        const auto s0 = rotr(a, 1) ^ rotr(a, 8) ^ (a >> 7);
        const auto s1 = rotr(b, 19) ^ rotr(b, 61) ^ (b >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint64_t v[8];
    for (unsigned i = 0; i < 8; ++i) v[i] = state[i];
    for (unsigned i = 0; i < 80; ++i) {
        const auto s1 = rotr(v[4], 14) ^ rotr(v[4], 18) ^ rotr(v[4], 41);
        const auto choose = (v[4] & v[5]) ^ (~v[4] & v[6]);
        const auto t1 = v[7] + s1 + choose + sha512_k[i] + w[i];
        const auto s0 = rotr(v[0], 28) ^ rotr(v[0], 34) ^ rotr(v[0], 39);
        const auto majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        const auto t2 = s0 + majority;
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (unsigned i = 0; i < 8; ++i) state[i] += v[i];
}

__device__ void init_state(std::uint64_t state[8]) {
    state[0] = 0x6a09e667f3bcc908ULL;
    state[1] = 0xbb67ae8584caa73bULL;
    state[2] = 0x3c6ef372fe94f82bULL;
    state[3] = 0xa54ff53a5f1d36f1ULL;
    state[4] = 0x510e527fade682d1ULL;
    state[5] = 0x9b05688c2b3e6c1fULL;
    state[6] = 0x1f83d9abfb41bd6bULL;
    state[7] = 0x5be0cd19137e2179ULL;
}

__device__ void hmac_prefix(const unsigned char* key, unsigned key_length,
                            std::uint64_t inner[8], std::uint64_t outer[8]) {
    init_state(inner);
    init_state(outer);
    unsigned char block[128];
    for (unsigned i = 0; i < 128; ++i)
        block[i] = static_cast<unsigned char>((i < key_length ? key[i] : 0) ^ 0x36);
    compress(inner, block);
    for (unsigned i = 0; i < 128; ++i)
        block[i] = static_cast<unsigned char>((i < key_length ? key[i] : 0) ^ 0x5c);
    compress(outer, block);
}

__device__ void hmac_from_prefix(const std::uint64_t inner_prefix[8],
                                 const std::uint64_t outer_prefix[8],
                                 const unsigned char* message,
                                 unsigned message_length, unsigned char digest[64]) {
    std::uint64_t inner[8], outer[8];
    for (unsigned i = 0; i < 8; ++i) {
        inner[i] = inner_prefix[i];
        outer[i] = outer_prefix[i];
    }
    unsigned char block[128];
    for (unsigned i = 0; i < 128; ++i) block[i] = 0;
    for (unsigned i = 0; i < message_length; ++i) block[i] = message[i];
    block[message_length] = 0x80;
    const auto inner_bits = static_cast<std::uint64_t>(128 + message_length) * 8;
    for (unsigned i = 0; i < 8; ++i)
        block[127 - i] = static_cast<unsigned char>(inner_bits >> (8 * i));
    compress(inner, block);
    for (unsigned i = 0; i < 128; ++i) block[i] = 0;
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned j = 0; j < 8; ++j)
            block[i * 8 + j] = static_cast<unsigned char>(inner[i] >> (56 - 8 * j));
    block[64] = 0x80;
    const auto outer_bits = std::uint64_t(192) * 8;
    for (unsigned i = 0; i < 8; ++i)
        block[127 - i] = static_cast<unsigned char>(outer_bits >> (8 * i));
    compress(outer, block);
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned j = 0; j < 8; ++j)
            digest[i * 8 + j] = static_cast<unsigned char>(outer[i] >> (56 - 8 * j));
}

__device__ void hmac512(const unsigned char* key, unsigned key_length,
                        const unsigned char* message, unsigned message_length,
                        unsigned char digest[64]) {
    std::uint64_t inner[8], outer[8];
    hmac_prefix(key, key_length, inner, outer);
    hmac_from_prefix(inner, outer, message, message_length, digest);
}

__global__ void derive_kernel(const unsigned char* input, unsigned char* output,
                              unsigned count) {
    const auto id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= count) return;
    const auto* phrase = input + id * 128;
    unsigned phrase_length = 0;
    while (phrase_length < 128 && phrase[phrase_length] != 0) ++phrase_length;
    unsigned char salt[12] = {'m','n','e','m','o','n','i','c',0,0,0,1};
    unsigned char current[64], derived[64], next[64];
    std::uint64_t inner_prefix[8], outer_prefix[8];
    hmac_prefix(phrase, phrase_length, inner_prefix, outer_prefix);
    hmac_from_prefix(inner_prefix, outer_prefix, salt, 12, current);
    for (unsigned i = 0; i < 64; ++i) derived[i] = current[i];
    for (unsigned iteration = 1; iteration < 2048; ++iteration) {
        hmac_from_prefix(inner_prefix, outer_prefix, current, 64, next);
        for (unsigned i = 0; i < 64; ++i) {
            derived[i] ^= next[i];
            current[i] = next[i];
        }
    }
    unsigned char key[128] = {};
    const unsigned char seed_key[12] = {'e','d','2','5','5','1','9',' ','s','e','e','d'};
    for (unsigned i = 0; i < 12; ++i) key[i] = seed_key[i];
    hmac512(key, 12, derived, 64, current);
    const unsigned path[3] = {44, 607, 0};
    for (unsigned level = 0; level < 3; ++level) {
        unsigned char child[37] = {};
        for (unsigned i = 0; i < 32; ++i) {
            child[i + 1] = current[i];
            key[i] = current[i + 32];
        }
        const auto hardened = 0x80000000U | path[level];
        child[33] = static_cast<unsigned char>(hardened >> 24);
        child[34] = static_cast<unsigned char>(hardened >> 16);
        child[35] = static_cast<unsigned char>(hardened >> 8);
        child[36] = static_cast<unsigned char>(hardened);
        hmac512(key, 32, child, 37, next);
        for (unsigned i = 0; i < 64; ++i) current[i] = next[i];
    }
    for (unsigned i = 0; i < 32; ++i) output[id * 32 + i] = current[i];
}
}  // namespace vantix_gpu
