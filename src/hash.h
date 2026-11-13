// Portable SHA-1 / SHA-256 / MD5 (for the AF_ALG kernel crypto socket emulation).
#pragma once
#include <cstdint>
#include <cstddef>

struct Sha1 {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
    void init();
    void update(const uint8_t* p, size_t sz);
    void final(uint8_t out[20]);
};

struct Sha256 {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
    void init();
    void update(const uint8_t* p, size_t sz);
    void final(uint8_t out[32]);
};

struct Md5 {
    uint32_t h[4];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
    void init();
    void update(const uint8_t* p, size_t sz);
    void final(uint8_t out[16]);
};
