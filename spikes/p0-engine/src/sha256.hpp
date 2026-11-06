#ifndef AEV_SHA256_HPP
#define AEV_SHA256_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace aev {

void sha256(const void *data, size_t len, uint8_t out[32]);
std::string sha256_hex(const void *data, size_t len);
std::string sha256_hex(const std::string &data);
bool sha256_file(const std::string &path, uint8_t out[32], std::string *error);
std::string hex64(uint64_t value);
std::string hex_bytes(const uint8_t *data, size_t len);

}  // namespace aev

#endif
