// Portable SHA-1 / SHA-256 / MD5 implementations (FIPS 180-4, RFC 1321).
#include "hash.h"
#include <cstring>

namespace {
inline uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline uint32_t le32(const uint8_t* p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }

// Shared Merkle–Damgård buffering. `block` processes one 64-byte block.
template <typename Ctx, typename F> void md_update(Ctx& c, const uint8_t* p, size_t sz, F block) {
    c.len += sz;
    while (sz) {
        size_t k = 64 - c.n;
        if (k > sz) k = sz;
        memcpy(c.buf + c.n, p, k);
        c.n += k;
        p += k;
        sz -= k;
        if (c.n == 64) {
            block(c, c.buf);
            c.n = 0;
        }
    }
}
template <typename Ctx, typename F> void md_pad(Ctx& c, bool big_endian, F block) {
    uint64_t bits = c.len * 8;
    c.buf[c.n++] = 0x80;
    if (c.n > 56) {
        memset(c.buf + c.n, 0, 64 - c.n);
        block(c, c.buf);
        c.n = 0;
    }
    memset(c.buf + c.n, 0, 56 - c.n);
    for (int i = 0; i < 8; i++) c.buf[56 + i] = (uint8_t)(big_endian ? bits >> (56 - 8 * i) : bits >> (8 * i));
    block(c, c.buf);
}

void sha1_block(Sha1& c, const uint8_t* b) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = be32(b + 4 * i);
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c.h[0], bb = c.h[1], cc = c.h[2], d = c.h[3], e = c.h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (bb & cc) | (~bb & d); k = 0x5a827999; }
        else if (i < 40) { f = bb ^ cc ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (bb & cc) | (bb & d) | (cc & d); k = 0x8f1bbcdc; }
        else { f = bb ^ cc ^ d; k = 0xca62c1d6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(bb, 30); bb = a; a = t;
    }
    c.h[0] += a; c.h[1] += bb; c.h[2] += cc; c.h[3] += d; c.h[4] += e;
}

const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void sha256_block(Sha256& c, const uint8_t* b) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = be32(b + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c.h[0], bb = c.h[1], cc = c.h[2], d = c.h[3], e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t mj = (a & bb) ^ (a & cc) ^ (bb & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = bb; bb = a; a = t1 + t2;
    }
    c.h[0] += a; c.h[1] += bb; c.h[2] += cc; c.h[3] += d; c.h[4] += e; c.h[5] += f; c.h[6] += g; c.h[7] += h;
}

const uint32_t KMD5[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
    0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
    0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
const int RMD5[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                      4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

void md5_block(Md5& c, const uint8_t* b) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++) m[i] = le32(b + 4 * i);
    uint32_t a = c.h[0], bb = c.h[1], cc = c.h[2], d = c.h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (bb & cc) | (~bb & d); g = i; }
        else if (i < 32) { f = (d & bb) | (~d & cc); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = bb ^ cc ^ d; g = (3 * i + 5) & 15; }
        else { f = cc ^ (bb | ~d); g = (7 * i) & 15; }
        uint32_t t = d;
        d = cc;
        cc = bb;
        bb = bb + rol(a + f + KMD5[i] + m[g], RMD5[i]);
        a = t;
    }
    c.h[0] += a; c.h[1] += bb; c.h[2] += cc; c.h[3] += d;
}
}  // namespace

void Sha1::init() {
    const uint32_t iv[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    memcpy(h, iv, sizeof iv);
    len = 0;
    n = 0;
}
void Sha1::update(const uint8_t* p, size_t sz) { md_update(*this, p, sz, sha1_block); }
void Sha1::final(uint8_t out[20]) {
    md_pad(*this, true, sha1_block);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

void Sha256::init() {
    const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h, iv, sizeof iv);
    len = 0;
    n = 0;
}
void Sha256::update(const uint8_t* p, size_t sz) { md_update(*this, p, sz, sha256_block); }
void Sha256::final(uint8_t out[32]) {
    md_pad(*this, true, sha256_block);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

void Md5::init() {
    const uint32_t iv[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    memcpy(h, iv, sizeof iv);
    len = 0;
    n = 0;
}
void Md5::update(const uint8_t* p, size_t sz) { md_update(*this, p, sz, md5_block); }
void Md5::final(uint8_t out[16]) {
    md_pad(*this, false, md5_block);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (8 * j));
}
