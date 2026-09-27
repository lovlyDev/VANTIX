// SHA-512 with explicit 32-bit high/low words for GPUs with slow Int64.
// uint2.x is the high word and uint2.y is the low word.
static const uint2 K32[80] = {
#include "sha512_constants32.slang"
};

uint2 add64(uint2 a, uint2 b) {
    uint low = a.y + b.y;
    return uint2(a.x + b.x + uint(low < a.y), low);
}

uint2 rotr64(uint2 value, uint amount) {
    if (amount == 0) return value;
    if (amount < 32)
        return uint2((value.x >> amount) | (value.y << (32 - amount)),
                     (value.y >> amount) | (value.x << (32 - amount)));
    if (amount == 32) return value.yx;
    uint step = amount - 32;
    return uint2((value.y >> step) | (value.x << (32 - step)),
                 (value.x >> step) | (value.y << (32 - step)));
}

uint2 shr64(uint2 value, uint amount) {
    if (amount < 32)
        return uint2(value.x >> amount,
                     (value.y >> amount) | (value.x << (32 - amount)));
    if (amount == 32) return uint2(0, value.x);
    return uint2(0, value.x >> (amount - 32));
}

uint2 load64(uint block[128], uint offset) {
    uint high = 0, low = 0;
    for (uint i = 0; i < 4; ++i)
        high = (high << 8) | block[offset + i];
    for (uint i = 4; i < 8; ++i)
        low = (low << 8) | block[offset + i];
    return uint2(high, low);
}

void store64(uint2 value, inout uint bytes[64], uint offset) {
    for (uint i = 0; i < 4; ++i)
        bytes[offset + i] = (value.x >> (24 - i * 8)) & 255;
    for (uint i = 0; i < 4; ++i)
        bytes[offset + i + 4] = (value.y >> (24 - i * 8)) & 255;
}

void initState(out uint2 state[8]) {
    state[0] = uint2(0x6a09e667, 0xf3bcc908);
    state[1] = uint2(0xbb67ae85, 0x84caa73b);
    state[2] = uint2(0x3c6ef372, 0xfe94f82b);
    state[3] = uint2(0xa54ff53a, 0x5f1d36f1);
    state[4] = uint2(0x510e527f, 0xade682d1);
    state[5] = uint2(0x9b05688c, 0x2b3e6c1f);
    state[6] = uint2(0x1f83d9ab, 0xfb41bd6b);
    state[7] = uint2(0x5be0cd19, 0x137e2179);
}

void compress(inout uint2 state[8], uint block[128]) {
    uint2 schedule[80];
    for (uint i = 0; i < 16; ++i)
        schedule[i] = load64(block, i * 8);
    for (uint i = 16; i < 80; ++i) {
        uint2 a = schedule[i - 15], b = schedule[i - 2];
        uint2 s0 = rotr64(a, 1) ^ rotr64(a, 8) ^ shr64(a, 7);
        uint2 s1 = rotr64(b, 19) ^ rotr64(b, 61) ^ shr64(b, 6);
        schedule[i] = add64(add64(add64(schedule[i - 16], s0),
                                  schedule[i - 7]), s1);
    }
    uint2 working[8];
    for (uint i = 0; i < 8; ++i) working[i] = state[i];
    for (uint i = 0; i < 80; ++i) {
        uint2 a = working[0], b = working[1], c = working[2], d = working[3];
        uint2 e = working[4], f = working[5], g = working[6], h = working[7];
        uint2 sigma1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        uint2 choose = (e & f) ^ ((~e) & g);
        uint2 t1 = add64(add64(add64(add64(h, sigma1), choose), K32[i]),
                         schedule[i]);
        uint2 sigma0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        uint2 majority = (a & b) ^ (a & c) ^ (b & c);
        uint2 t2 = add64(sigma0, majority);
        working[0] = add64(t1, t2);
        working[1] = a;
        working[2] = b;
        working[3] = c;
        working[4] = add64(d, t1);
        working[5] = e;
        working[6] = f;
        working[7] = g;
    }
    for (uint i = 0; i < 8; ++i) state[i] = add64(state[i], working[i]);
}
