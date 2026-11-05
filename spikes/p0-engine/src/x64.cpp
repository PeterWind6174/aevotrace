#include "x64.hpp"

#include <capstone/capstone.h>
#include <capstone/x86.h>

#include <cstdio>
#include <cstring>

namespace aev {

DecodeResult decode_x64(const std::vector<uint8_t> &bytes, uint64_t base_address) {
    DecodeResult result;
    csh handle = 0;
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK) {
        result.error = "cs_open(x86-64) failed";
        return result;
    }
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
    cs_option(handle, CS_OPT_SYNTAX, CS_OPT_SYNTAX_INTEL);

    cs_insn *insn = cs_malloc(handle);
    const uint8_t *code = bytes.data();
    size_t code_size = bytes.size();
    uint64_t addr = base_address;

    while (code_size > 0) {
        uint64_t before = addr;
        if (!cs_disasm_iter(handle, &code, &code_size, &addr, insn)) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "invalid x86-64 instruction at 0x%llx (next byte 0x%02x, %zu bytes left)",
                     (unsigned long long)before, code_size ? code[0] : 0, code_size);
            result.error = buf;
            cs_free(insn, 1);
            cs_close(&handle);
            return result;
        }

        X64Insn out;
        out.address = insn->address;
        out.size = insn->size;
        out.id = (uint16_t)insn->id;
        out.mnemonic = insn->mnemonic;
        out.op_str = insn->op_str;
        out.bytes.assign(insn->bytes, insn->bytes + insn->size);

        const cs_x86 &x86 = insn->detail->x86;
        out.eflags = x86.eflags;
        for (uint8_t i = 0; i < x86.op_count; ++i) {
            const cs_x86_op &op = x86.operands[i];
            X64Operand o;
            o.size = op.size;
            switch (op.type) {
                case X86_OP_REG:
                    o.kind = OperandKind::Register;
                    o.reg = (int)op.reg;
                    break;
                case X86_OP_IMM:
                    o.kind = OperandKind::Immediate;
                    o.imm = (int64_t)op.imm;
                    break;
                case X86_OP_MEM:
                    o.kind = OperandKind::Memory;
                    o.segment = (int)op.mem.segment;
                    o.base = (int)op.mem.base;
                    o.index = (int)op.mem.index;
                    o.scale = op.mem.scale ? op.mem.scale : 1;
                    o.disp = (int64_t)op.mem.disp;
                    break;
                default:
                    result.error = "unsupported Capstone operand type";
                    cs_free(insn, 1);
                    cs_close(&handle);
                    return result;
            }
            out.operands.push_back(o);
        }
        result.insns.push_back(std::move(out));
    }

    cs_free(insn, 1);
    cs_close(&handle);
    result.ok = true;
    return result;
}

}  // namespace aev
