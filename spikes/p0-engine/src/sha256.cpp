#include "sha256.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <cstdio>
#include <fstream>
#include <vector>

namespace aev {

void sha256(const void *data, size_t len, uint8_t out[32]) {
    CC_SHA256(data, static_cast<CC_LONG>(len), out);
}

std::string hex_bytes(const uint8_t *data, size_t len) {
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0x0f];
    }
    return out;
}

std::string sha256_hex(const void *data, size_t len) {
    uint8_t digest[32];
    sha256(data, len, digest);
    return hex_bytes(digest, sizeof(digest));
}

std::string sha256_hex(const std::string &data) {
    return sha256_hex(data.data(), data.size());
}

bool sha256_file(const std::string &path, uint8_t out[32], std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    std::vector<char> buffer(1024 * 1024);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize got = in.gcount();
        if (got > 0)
            CC_SHA256_Update(&ctx, buffer.data(), static_cast<CC_LONG>(got));
    }
    if (!in.eof()) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    CC_SHA256_Final(out, &ctx);
    return true;
}

std::string hex64(uint64_t value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)value);
    return buf;
}

}  // namespace aev
