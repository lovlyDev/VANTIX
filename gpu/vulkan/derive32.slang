// VANTIX portable 32-bit SHA-512 Vulkan path. One invocation per BIP39 phrase.
[[vk::binding(0, 0)]] StructuredBuffer<uint> inputWords;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> outputWords;
struct Params { uint count; uint pad0; uint pad1; uint pad2; };
[[vk::binding(2, 0)]] ConstantBuffer<Params> params;
#include "sha512_32.slang"

void hmac512(uint key[128], uint keyLength, uint message[64],
             uint messageLength, out uint digest[64]) {
    uint2 inner[8], outer[8];
    initState(inner);
    initState(outer);
    uint block[128];
    for (uint i = 0; i < 128; ++i)
        block[i] = (i < keyLength ? key[i] : 0) ^ 0x36;
    compress(inner, block);
    for (uint i = 0; i < 128; ++i)
        block[i] = (i < keyLength ? key[i] : 0) ^ 0x5c;
    compress(outer, block);

    for (uint i = 0; i < 128; ++i) block[i] = 0;
    for (uint i = 0; i < messageLength; ++i) block[i] = message[i];
    block[messageLength] = 0x80;
    uint bitLength = (128 + messageLength) * 8;
    for (uint i = 0; i < 4; ++i)
        block[127 - i] = (bitLength >> (8 * i)) & 255;
    compress(inner, block);

    for (uint i = 0; i < 128; ++i) block[i] = 0;
    for (uint i = 0; i < 8; ++i) {
        uint2 word = inner[i];
        for (uint j = 0; j < 4; ++j) {
            block[i * 8 + j] = (word.x >> (24 - j * 8)) & 255;
            block[i * 8 + j + 4] = (word.y >> (24 - j * 8)) & 255;
        }
    }
    block[64] = 0x80;
    uint outerLength = 192 * 8;
    for (uint i = 0; i < 4; ++i)
        block[127 - i] = (outerLength >> (8 * i)) & 255;
    compress(outer, block);
    for (uint i = 0; i < 8; ++i)
        store64(outer[i], digest, i * 8);
}

[numthreads(64, 1, 1)]
void ton_main(uint3 thread: SV_DispatchThreadID) {
    uint candidate = thread.x;
    if (candidate >= params.count) return;
    uint phrase[128];
    uint phraseLength = 128;
    for (uint i = 0; i < 128; ++i) {
        uint packed = inputWords[candidate * 32 + i / 4];
        phrase[i] = (packed >> ((i & 3) * 8)) & 255;
        if (phrase[i] == 0 && phraseLength == 128) phraseLength = i;
    }
    uint salt[64];
    for (uint i = 0; i < 64; ++i) salt[i] = 0;
    salt[0] = 109; salt[1] = 110; salt[2] = 101; salt[3] = 109;
    salt[4] = 111; salt[5] = 110; salt[6] = 105; salt[7] = 99;
    salt[11] = 1; // BIP39 salt "mnemonic" || INT_32_BE(1)
    uint current[64], derived[64], next[64];
    hmac512(phrase, phraseLength, salt, 12, current);
    for (uint i = 0; i < 64; ++i) derived[i] = current[i];
    for (uint iteration = 1; iteration < 2048; ++iteration) {
        hmac512(phrase, phraseLength, current, 64, next);
        for (uint i = 0; i < 64; ++i) {
            derived[i] ^= next[i];
            current[i] = next[i];
        }
    }

    uint key[128];
    for (uint i = 0; i < 128; ++i) key[i] = 0;
    key[0] = 101; key[1] = 100; key[2] = 50; key[3] = 53;
    key[4] = 53; key[5] = 49; key[6] = 57; key[7] = 32;
    key[8] = 115; key[9] = 101; key[10] = 101; key[11] = 100;
    hmac512(key, 12, derived, 64, current);
    const uint path[3] = {44, 607, 0};
    for (uint level = 0; level < 3; ++level) {
        uint child[64];
        for (uint i = 0; i < 64; ++i) child[i] = 0;
        for (uint i = 0; i < 32; ++i) {
            child[i + 1] = current[i];
            key[i] = current[i + 32];
        }
        uint hardened = 0x80000000 | path[level];
        child[33] = (hardened >> 24) & 255;
        child[34] = (hardened >> 16) & 255;
        child[35] = (hardened >> 8) & 255;
        child[36] = hardened & 255;
        hmac512(key, 32, child, 37, next);
        for (uint i = 0; i < 64; ++i) current[i] = next[i];
    }
    for (uint i = 0; i < 8; ++i) {
        uint base = i * 4;
        outputWords[candidate * 8 + i] =
            current[base] | (current[base + 1] << 8) |
            (current[base + 2] << 16) | (current[base + 3] << 24);
    }
}
