#include "program_identity.hpp"

#include "sha256.hpp"

#include <algorithm>
#include <cstring>

namespace aev {
namespace {

const CoffSection *section_for(const Program &program, const GuestFunction &fn) {
    return program.object.section(fn.section_number);
}

}  // namespace

bool block_bytes(const Program &program, uint64_t rip, const uint8_t **data,
                 uint32_t *section_offset, uint32_t *size, std::string *error) {
    for (const auto &fn : program.functions) {
        if (rip < fn.address || rip >= fn.address + fn.bytes.size())
            continue;
        const Block *block = nullptr;
        for (const auto &candidate : fn.blocks) {
            if (candidate.address == rip) {
                block = &candidate;
                break;
            }
        }
        if (!block) {
            if (error) *error = "rip is not a block boundary";
            return false;
        }
        uint64_t end_addr;
        if (block->end < fn.insns.size()) {
            end_addr = fn.insns[block->end].address;
        } else {
            end_addr = fn.address + fn.bytes.size();
        }
        if (end_addr <= rip) {
            if (error) *error = "invalid block end";
            return false;
        }
        uint64_t rel = rip - fn.address;
        uint64_t off = (uint64_t)fn.section_offset + rel;
        uint64_t len = end_addr - rip;
        const CoffSection *section = section_for(program, fn);
        if (!section || off > section->data.size() ||
            len > section->data.size() - off) {
            if (error) *error = "block bytes outside COFF section";
            return false;
        }
        if (data) *data = section->data.data() + off;
        if (section_offset) *section_offset = (uint32_t)off;
        if (size) *size = (uint32_t)len;
        return true;
    }
    if (error) *error = "rip not inside translated functions";
    return false;
}

bool block_identity(const Program &program, uint64_t rip, BlockIdentity *out,
                    std::string *error) {
    const uint8_t *data = nullptr;
    uint32_t off = 0, len = 0;
    if (!block_bytes(program, rip, &data, &off, &len, error))
        return false;
    if (out) {
        out->rip = rip;
        out->section_offset = off;
        out->size = len;
        sha256(data, len, out->sha256);
    }
    return true;
}

bool all_block_identities(const Program &program, std::vector<BlockIdentity> *out,
                          std::string *error) {
    std::vector<BlockIdentity> blocks;
    for (const auto &fn : program.functions) {
        for (const auto &block : fn.blocks) {
            BlockIdentity id;
            if (!block_identity(program, block.address, &id, error))
                return false;
            blocks.push_back(id);
        }
    }
    std::sort(blocks.begin(), blocks.end(),
              [](const BlockIdentity &a, const BlockIdentity &b) {
                  return a.rip < b.rip;
              });
    for (size_t i = 1; i < blocks.size(); ++i) {
        if (blocks[i].rip == blocks[i - 1].rip) {
            if (error) *error = "duplicate block RIP";
            return false;
        }
    }
    if (out) *out = std::move(blocks);
    return true;
}

}  // namespace aev
