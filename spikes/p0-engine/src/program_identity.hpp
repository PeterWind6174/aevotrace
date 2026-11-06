#ifndef AEV_PROGRAM_IDENTITY_HPP
#define AEV_PROGRAM_IDENTITY_HPP

#include "translator.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace aev {

struct BlockIdentity {
    uint64_t rip = 0;
    uint32_t section_offset = 0; /* section-relative */
    uint32_t size = 0;
    uint8_t sha256[32] = {};
};

/* All blocks in stable RIP order.  The identity hashes the actual COFF
 * section bytes at [section_offset, section_offset+size), not fn.bytes[offset]. */
bool all_block_identities(const Program &program, std::vector<BlockIdentity> *out,
                          std::string *error);
bool block_identity(const Program &program, uint64_t rip, BlockIdentity *out,
                    std::string *error);
bool block_bytes(const Program &program, uint64_t rip, const uint8_t **data,
                 uint32_t *section_offset, uint32_t *size, std::string *error);

}  // namespace aev

#endif
