// VANTIX Vulkan seed derivation. One invocation handles one BIP39 phrase.
// SHA-512 follows FIPS 180-4; all arithmetic is unsigned modulo 2^64.
// The host verifies every exported match with its independent OpenSSL path.

[[vk::binding(0, 0)]] StructuredBuffer<uint> inputWords;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> outputWords;
struct Params { uint count; uint pad0; uint pad1; uint pad2; };
[[vk::binding(2, 0)]] ConstantBuffer<Params> params;

static const uint64_t K[80] = {
#include "sha512_constants.slang"
};

uint64_t rotr(uint64_t value, uint shift) {
    return (value >> shift) | (value << (64 - shift));
}

uint64_t load64(uint block[128], uint offset) {
    uint64_t result = 0;
    [unroll] for (uint i = 0; i < 8; ++i)
        result = (result << 8) | uint64_t(block[offset + i]);
    return result;
}

void store64(uint64_t value, inout uint bytes[64], uint offset) {
    [unroll] for (uint i = 0; i < 8; ++i)
        bytes[offset + i] = uint((value >> (56 - 8 * i)) & 255);
}

void compress(inout uint64_t state[8], uint block[128]) {
    uint64_t schedule[80];
    [unroll] for (uint i = 0; i < 16; ++i)
        schedule[i] = load64(block, i * 8);
    for (uint i = 16; i < 80; ++i) {
        uint64_t a = schedule[i - 15];
        uint64_t b = schedule[i - 2];
        uint64_t s0 = rotr(a, 1) ^ rotr(a, 8) ^ (a >> 7);
        uint64_t s1 = rotr(b, 19) ^ rotr(b, 61) ^ (b >> 6);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }
    uint64_t working[8];
    [unroll] for (uint i = 0; i < 8; ++i) working[i] = state[i];
    for (uint i = 0; i < 80; ++i) {
        uint64_t a = working[0], b = working[1], c = working[2], d = working[3];
        uint64_t e = working[4], f = working[5], g = working[6], h = working[7];
        uint64_t s1 = rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41);
        uint64_t choose = (e & f) ^ ((~e) & g);
        uint64_t t1 = h + s1 + choose + K[i] + schedule[i];
        uint64_t s0 = rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39);
        uint64_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = s0 + majority;
        working[0] = t1 + t2; working[1] = a; working[2] = b; working[3] = c;
        working[4] = d + t1; working[5] = e; working[6] = f; working[7] = g;
    }
    [unroll] for (uint i = 0; i < 8; ++i) state[i] += working[i];
}

void initState(out uint64_t state[8]) {
    state[0] = 0x6a09e667f3bcc908UL;
    state[1] = 0xbb67ae8584caa73bUL;
    state[2] = 0x3c6ef372fe94f82bUL;
    state[3] = 0xa54ff53a5f1d36f1UL;
    state[4] = 0x510e527fade682d1UL;
    state[5] = 0x9b05688c2b3e6c1fUL;
    state[6] = 0x1f83d9abfb41bd6bUL;
    state[7] = 0x5be0cd19137e2179UL;
}

void hmacPrefix(uint key[128], uint keyLength,
                out uint64_t inner[8], out uint64_t outer[8]) {
    initState(inner);
    initState(outer);
    uint block[128];
    [unroll] for (uint i = 0; i < 128; ++i)
        block[i] = (i < keyLength ? key[i] : 0) ^ 0x36;
    compress(inner, block);
    [unroll] for (uint i = 0; i < 128; ++i)
        block[i] = (i < keyLength ? key[i] : 0) ^ 0x5c;
    compress(outer, block);
}

void hmacFromPrefix(uint64_t innerPrefix[8], uint64_t outerPrefix[8],
                    uint message[64], uint messageLength, out uint digest[64]) {
    uint64_t inner[8], outer[8];
    [unroll] for (uint i = 0; i < 8; ++i) {
        inner[i] = innerPrefix[i];
        outer[i] = outerPrefix[i];
    }
    uint block[128];

    [unroll] for (uint i = 0; i < 128; ++i) block[i] = 0;
    for (uint i = 0; i < messageLength; ++i) block[i] = message[i];
    block[messageLength] = 0x80;
    uint64_t bitLength = uint64_t(128 + messageLength) * 8;
    [unroll] for (uint i = 0; i < 8; ++i)
        block[127 - i] = uint((bitLength >> (8 * i)) & 255);
    compress(inner, block);

    [unroll] for (uint i = 0; i < 128; ++i) block[i] = 0;
    [unroll] for (uint i = 0; i < 8; ++i) {
        uint64_t word = inner[i];
        [unroll] for (uint j = 0; j < 8; ++j)
            block[i * 8 + j] = uint((word >> (56 - j * 8)) & 255);
    }
    block[64] = 0x80;
    uint64_t outerLength = uint64_t(192) * 8;
    [unroll] for (uint i = 0; i < 8; ++i)
        block[127 - i] = uint((outerLength >> (8 * i)) & 255);
    compress(outer, block);
    [unroll] for (uint i = 0; i < 8; ++i)
        store64(outer[i], digest, i * 8);
}

void hmac512(uint key[128], uint keyLength, uint message[64],
             uint messageLength, out uint digest[64]) {
    uint64_t inner[8], outer[8];
    hmacPrefix(key, keyLength, inner, outer);
    hmacFromPrefix(inner, outer, message, messageLength, digest);
}

[numthreads(64, 1, 1)]
void ton_main(uint3 thread: SV_DispatchThreadID) {
    uint candidate = thread.x;
    if (candidate >= params.count) return;
    uint phrase[128];
    uint phraseLength = 128;
    [unroll] for (uint i = 0; i < 128; ++i) {
        uint packed = inputWords[candidate * 32 + i / 4];
        phrase[i] = (packed >> ((i & 3) * 8)) & 255;
        if (phrase[i] == 0 && phraseLength == 128) phraseLength = i;
    }
    uint salt[64];
    [unroll] for (uint i = 0; i < 64; ++i) salt[i] = 0;
    salt[0] = 109; salt[1] = 110; salt[2] = 101; salt[3] = 109;
    salt[4] = 111; salt[5] = 110; salt[6] = 105; salt[7] = 99;
    salt[11] = 1; // BIP39 salt "mnemonic" || INT_32_BE(1)
    uint current[64], derived[64], next[64];
    uint64_t innerPrefix[8], outerPrefix[8];
    hmacPrefix(phrase, phraseLength, innerPrefix, outerPrefix);
    hmacFromPrefix(innerPrefix, outerPrefix, salt, 12, current);
    [unroll] for (uint i = 0; i < 64; ++i) derived[i] = current[i];
    for (uint iteration = 1; iteration < 2048; ++iteration) {
        hmacFromPrefix(innerPrefix, outerPrefix, current, 64, next);
        [unroll] for (uint i = 0; i < 64; ++i) {
            derived[i] ^= next[i];
            current[i] = next[i];
        }
    }

    uint key[128];
    [unroll] for (uint i = 0; i < 128; ++i) key[i] = 0;
    key[0] = 101; key[1] = 100; key[2] = 50; key[3] = 53;
    key[4] = 53; key[5] = 49; key[6] = 57; key[7] = 32;
    key[8] = 115; key[9] = 101; key[10] = 101; key[11] = 100;
    hmac512(key, 12, derived, 64, current);
    const uint path[3] = {44, 607, 0};
    for (uint level = 0; level < 3; ++level) {
        uint child[64];
        [unroll] for (uint i = 0; i < 64; ++i) child[i] = 0;
        [unroll] for (uint i = 0; i < 32; ++i) {
            child[i + 1] = current[i];
            key[i] = current[i + 32];
        }
        uint hardened = 0x80000000 | path[level];
        child[33] = (hardened >> 24) & 255;
        child[34] = (hardened >> 16) & 255;
        child[35] = (hardened >> 8) & 255;
        child[36] = hardened & 255;
        hmac512(key, 32, child, 37, next);
        [unroll] for (uint i = 0; i < 64; ++i) current[i] = next[i];
    }
    [unroll] for (uint i = 0; i < 8; ++i) {
        uint base = i * 4;
        outputWords[candidate * 8 + i] =
            current[base] | (current[base + 1] << 8) |
            (current[base + 2] << 16) | (current[base + 3] << 24);
    }
}
