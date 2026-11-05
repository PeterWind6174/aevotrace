#ifndef AEV_X64_HPP
#define AEV_X64_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace aev {

enum class OperandKind { Register, Immediate, Memory };

struct X64Operand {
    OperandKind kind = OperandKind::Immediate;
    int reg = 0;          /* x86_reg */
    int64_t imm = 0;
    uint8_t size = 0;     /* operand width in bytes */
    /* memory form */
    int segment = 0;
    int base = 0;
    int index = 0;
    int scale = 1;
    int64_t disp = 0;
};

struct X64Insn {
    uint64_t address = 0;
    uint32_t size = 0;
    uint16_t id = 0;      /* x86_insn */
    uint16_t eflags = 0;
    bool lock_prefix = false; /* X86_PREFIX_LOCK in cs_x86.prefix[0] */
    std::string mnemonic;
    std::string op_str;
    std::vector<uint8_t> bytes;
    std::vector<X64Operand> operands;
};

struct DecodeResult {
    std::vector<X64Insn> insns;
    bool ok = false;
    std::string error;
};

DecodeResult decode_x64(const std::vector<uint8_t> &bytes, uint64_t base_address);

}  // namespace aev

#endif
