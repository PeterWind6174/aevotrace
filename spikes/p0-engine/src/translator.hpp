#ifndef AEV_TRANSLATOR_HPP
#define AEV_TRANSLATOR_HPP

#include "coff.hpp"
#include "x64.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace aev {

struct FunctionSpec {
    std::string name;
    int section_number = 0;
    uint64_t guest_va = 0;
    uint32_t rva = 0; /* section-relative offset; PE uses image RVA */
    uint32_t size = 0;
    uint32_t pdata_begin = 0;
    uint32_t pdata_end = 0;
};

struct Block {
    uint64_t address = 0;
    size_t begin = 0; /* index into GuestFunction::insns */
    size_t end = 0;   /* one past last instruction */
    bool jit_only = false;
    std::string jit_reason;
};

struct GuestFunction {
    std::string name;
    int section_number = 0;
    uint32_t section_offset = 0;
    uint64_t address = 0;
    std::vector<uint8_t> bytes;
    std::vector<X64Insn> insns;
    std::vector<Block> blocks;
};

struct Program {
    CoffObject object;
    std::vector<GuestFunction> functions;

    const GuestFunction *find_function_at(uint64_t rip) const;
    const Block *find_block(uint64_t rip) const;
    GuestFunction *find_function_by_name(const std::string &name);
    const GuestFunction *find_function_by_name(const std::string &name) const;
};

bool build_program(CoffObject &&object,
                   const std::vector<std::string> &function_names,
                   const std::vector<uint64_t> &addresses,
                   Program *program,
                   std::string *error);
bool build_program_from_specs(CoffObject &&object,
                              const std::vector<FunctionSpec> &specs,
                              const std::set<uint64_t> *external_jmp_targets,
                              Program *program,
                              std::string *error);

/* True when the block needs the JIT because AOT deliberately does not cover
 * one of its instructions (currently imul). */
bool block_is_aot_covered(const GuestFunction &fn, const Block &block);
bool is_block_terminator(uint16_t insn_id);

}  // namespace aev

#endif
