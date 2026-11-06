#include "ir_emitter.hpp"

#include "aev_artifact.h"
#include "program_identity.hpp"
#include "aev_guest.h"

#include <capstone/x86.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace aev {
namespace {

enum GuestField : int {
#define AEV_FIELD(name) GF_##name,
#include "aev_guest_fields.def"
#undef AEV_FIELD
    GF_COUNT
};

struct Unsupported : std::runtime_error {
    explicit Unsupported(const std::string &m) : std::runtime_error(m) {}
};

std::string u64(uint64_t v) { return std::to_string(v); }

std::string hex64(uint64_t v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%llx", (unsigned long long)v);
    return buf;
}

uint64_t width_mask(int width) {
    return width >= 8 ? UINT64_MAX : ((1ULL << (width * 8)) - 1ULL);
}

std::string format_insn(const X64Insn &insn) {
    char buf[160];
    snprintf(buf, sizeof(buf), "0x%llx: %s %s",
             (unsigned long long)insn.address, insn.mnemonic.c_str(),
             insn.op_str.c_str());
    return buf;
}

bool map_reg(int reg, int *field, int *natural_width, std::string *error) {
#define REG_CASE(r, f, w) \
    case X86_REG_##r:     \
        *field = GF_##f;  \
        *natural_width = w; \
        return true;
#define REG64(r, f) REG_CASE(r, f, 8)
#define REG32(r, f) REG_CASE(r, f, 4)
#define REG16(r, f) REG_CASE(r, f, 2)
#define REG8(r, f) REG_CASE(r, f, 1)
    switch (reg) {
        REG64(RAX, rax) REG32(EAX, rax) REG16(AX, rax) REG8(AL, rax)
        REG64(RBX, rbx) REG32(EBX, rbx) REG16(BX, rbx) REG8(BL, rbx)
        REG64(RCX, rcx) REG32(ECX, rcx) REG16(CX, rcx) REG8(CL, rcx)
        REG64(RDX, rdx) REG32(EDX, rdx) REG16(DX, rdx) REG8(DL, rdx)
        REG64(RSI, rsi) REG32(ESI, rsi) REG16(SI, rsi) REG8(SIL, rsi)
        REG64(RDI, rdi) REG32(EDI, rdi) REG16(DI, rdi) REG8(DIL, rdi)
        REG64(RBP, rbp) REG32(EBP, rbp) REG16(BP, rbp) REG8(BPL, rbp)
        REG64(RSP, rsp) REG32(ESP, rsp) REG16(SP, rsp) REG8(SPL, rsp)
        REG64(R8, r8) REG32(R8D, r8) REG16(R8W, r8) REG8(R8B, r8)
        REG64(R9, r9) REG32(R9D, r9) REG16(R9W, r9) REG8(R9B, r9)
        REG64(R10, r10) REG32(R10D, r10) REG16(R10W, r10) REG8(R10B, r10)
        REG64(R11, r11) REG32(R11D, r11) REG16(R11W, r11) REG8(R11B, r11)
        REG64(R12, r12) REG32(R12D, r12) REG16(R12W, r12) REG8(R12B, r12)
        REG64(R13, r13) REG32(R13D, r13) REG16(R13W, r13) REG8(R13B, r13)
        REG64(R14, r14) REG32(R14D, r14) REG16(R14W, r14) REG8(R14B, r14)
        REG64(R15, r15) REG32(R15D, r15) REG16(R15W, r15) REG8(R15B, r15)
        case X86_REG_AH:
        case X86_REG_BH:
        case X86_REG_CH:
        case X86_REG_DH:
            if (error) *error = "high-byte registers are not supported by this spike";
            return false;
        default:
            if (error) *error = "unsupported register operand";
            return false;
    }
#undef REG8
#undef REG16
#undef REG32
#undef REG64
#undef REG_CASE
}

enum Cond {
    CC_O = 0,
    CC_NO,
    CC_B,
    CC_AE,
    CC_E,
    CC_NE,
    CC_BE,
    CC_A,
    CC_S,
    CC_NS,
    CC_P,
    CC_NP,
    CC_L,
    CC_GE,
    CC_LE,
    CC_G
};

bool cond_for_insn(uint16_t id, Cond *cc) {
    switch (id) {
        case X86_INS_JO: case X86_INS_CMOVO: *cc = CC_O; return true;
        case X86_INS_JNO: case X86_INS_CMOVNO: *cc = CC_NO; return true;
        case X86_INS_JB: case X86_INS_CMOVB: *cc = CC_B; return true;
        case X86_INS_JAE: case X86_INS_CMOVAE: *cc = CC_AE; return true;
        case X86_INS_JE: case X86_INS_CMOVE: *cc = CC_E; return true;
        case X86_INS_JNE: case X86_INS_CMOVNE: *cc = CC_NE; return true;
        case X86_INS_JBE: case X86_INS_CMOVBE: *cc = CC_BE; return true;
        case X86_INS_JA: case X86_INS_CMOVA: *cc = CC_A; return true;
        case X86_INS_JS: case X86_INS_CMOVS: *cc = CC_S; return true;
        case X86_INS_JNS: case X86_INS_CMOVNS: *cc = CC_NS; return true;
        case X86_INS_JP: case X86_INS_CMOVP: *cc = CC_P; return true;
        case X86_INS_JNP: case X86_INS_CMOVNP: *cc = CC_NP; return true;
        case X86_INS_JL: case X86_INS_CMOVL: *cc = CC_L; return true;
        case X86_INS_JGE: case X86_INS_CMOVGE: *cc = CC_GE; return true;
        case X86_INS_JLE: case X86_INS_CMOVLE: *cc = CC_LE; return true;
        case X86_INS_JG: case X86_INS_CMOVG: *cc = CC_G; return true;
        default: return false;
    }
}

class Emitter {
  public:
    explicit Emitter(const Program &program, bool jit_mode)
        : program_(program), jit_mode_(jit_mode) {}

    std::string emit_block(const GuestFunction &fn, const Block &block) {
        body_.str("");
        body_.clear();
        tmp_ = 0;
        check_id_ = 0;
        fault_used_ = false;
        current_fn_ = &fn;
        current_block_ = &block;

        std::string name = (jit_mode_ ? "aev_jit_" : "aev_blk_") + hex64(block.address);
        if (!jit_mode_)
            emit("call void @aev_note_aot(ptr %s, i64 " + u64(block.address) + ")");

        bool terminated = false;
        for (size_t i = block.begin; i < block.end; ++i) {
            const X64Insn &insn = fn.insns[i];
            if (is_terminator(insn.id)) {
                emit_terminator(insn);
                terminated = true;
                break;
            }
            emit_instruction(insn);
        }
        if (!terminated) {
            uint64_t next = next_block_address(fn, block);
            store_field(GF_pending_transfer, u64(AEV_TRANSFER_NONE));
            store_field(GF_rip, u64(next));
        }
        emit("ret void");
        if (fault_used_) {
            body_ << "fault:\n";
            body_ << "  ret void\n";
        }

        std::string out = "define void @" + name + "(ptr %s) {\n";
        out += "entry:\n";
        out += body_.str();
        out += "}\n";
        return out;
    }

  private:
    const Program &program_;
    bool jit_mode_;
    const GuestFunction *current_fn_ = nullptr;
    const Block *current_block_ = nullptr;
    std::ostringstream body_;
    int tmp_ = 0;
    int check_id_ = 0;
    bool fault_used_ = false;

    static bool is_jcc_insn(uint16_t id) {
        switch (id) {
            case X86_INS_JO:
            case X86_INS_JNO:
            case X86_INS_JB:
            case X86_INS_JAE:
            case X86_INS_JE:
            case X86_INS_JNE:
            case X86_INS_JBE:
            case X86_INS_JA:
            case X86_INS_JS:
            case X86_INS_JNS:
            case X86_INS_JP:
            case X86_INS_JNP:
            case X86_INS_JL:
            case X86_INS_JGE:
            case X86_INS_JLE:
            case X86_INS_JG:
                return true;
            default:
                return false;
        }
    }

    static bool is_terminator(uint16_t id) {
        return id == X86_INS_JMP || id == X86_INS_CALL || id == X86_INS_RET ||
               is_jcc_insn(id);
    }

    std::string fresh(const char *prefix) {
        return std::string("%") + prefix + std::to_string(tmp_++);
    }

    void emit(const std::string &line) { body_ << "  " << line << "\n"; }

    void emit_label(const std::string &name) { body_ << name << ":\n"; }

    /* Every memory helper call is followed by a check of GuestState.error_code.
     * On failure control branches to a single per-block `fault` label that
     * returns immediately, so no register or memory side effect of the
     * faulting instruction (or of later instructions in the block) commits.
     * The guest RIP is set to the current instruction address before each
     * helper call, so aev_mem.c records an exact error_pc. */
    void emit_fault_check() {
        std::string err = fresh("err");
        emit(err + " = load i64, ptr " + gep(GF_error_code) + ", align 8");
        std::string bad = fresh("bad");
        emit(bad + " = icmp ne i64 " + err + ", 0");
        std::string cont = "cont" + std::to_string(check_id_++);
        emit("br i1 " + bad + ", label %fault, label %" + cont);
        emit_label(cont);
        fault_used_ = true;
    }

    std::string load_mem(const X64Insn &insn, const std::string &addr, int width,
                         bool sign) {
        store_field(GF_rip, u64(insn.address));
        std::string v = fresh("ld");
        emit(v + " = call i64 @aev_load(ptr %s, i64 " + addr + ", i32 " +
             std::to_string(width) + ", i32 " + (sign ? "1" : "0") + ")");
        emit_fault_check();
        return v;
    }

    void store_mem(const X64Insn &insn, const std::string &addr,
                   const std::string &value, int width) {
        store_field(GF_rip, u64(insn.address));
        emit("call void @aev_store(ptr %s, i64 " + addr + ", i64 " + value +
             ", i32 " + std::to_string(width) + ")");
        emit_fault_check();
    }

    std::string gep(int field) {
        std::string v = fresh("g");
        emit(v + " = getelementptr %GuestState, ptr %s, i32 0, i32 " + std::to_string(field));
        return v;
    }

    std::string load_field(int field) {
        std::string p = gep(field);
        std::string v = fresh("v");
        emit(v + " = load i64, ptr " + p + ", align 8");
        return v;
    }

    void store_field(int field, const std::string &value) {
        std::string p = gep(field);
        emit("store i64 " + value + ", ptr " + p + ", align 8");
    }

    std::string and_mask(const std::string &value, int width) {
        if (width >= 8)
            return value;
        std::string m = fresh("m");
        emit(m + " = and i64 " + value + ", " + u64(width_mask(width)));
        return m;
    }

    std::string sign_extend(const std::string &value, int width) {
        if (width >= 8)
            return value;
        int shift = 64 - width * 8;
        std::string sh = fresh("sh");
        emit(sh + " = shl i64 " + value + ", " + std::to_string(shift));
        std::string as = fresh("as");
        emit(as + " = ashr i64 " + sh + ", " + std::to_string(shift));
        return as;
    }

    std::string read_reg(int reg, int width) {
        int field = 0, natural = 0;
        std::string err;
        if (!map_reg(reg, &field, &natural, &err))
            throw Unsupported(err);
        std::string v = load_field(field);
        return and_mask(v, width);
    }

    void write_reg(int reg, int width, const std::string &value) {
        int field = 0, natural = 0;
        std::string err;
        if (!map_reg(reg, &field, &natural, &err))
            throw Unsupported(err);
        if (width >= 8) {
            store_field(field, value);
            return;
        }
        if (width == 4) {
            std::string z = and_mask(value, 4);
            store_field(field, z);
            return;
        }
        std::string old = load_field(field);
        uint64_t mask = width_mask(width);
        std::string keep = fresh("keep");
        emit(keep + " = and i64 " + old + ", " + u64(~mask));
        std::string ins = fresh("ins");
        emit(ins + " = and i64 " + value + ", " + u64(mask));
        std::string nv = fresh("nv");
        emit(nv + " = or i64 " + keep + ", " + ins);
        store_field(field, nv);
    }

    std::string mem_address(const X64Insn &insn, const X64Operand &op) {
        if (op.kind != OperandKind::Memory)
            throw Unsupported("internal: operand is not memory");
        if (op.segment != X86_REG_INVALID && op.segment != X86_REG_DS &&
            op.segment != X86_REG_SS && op.segment != X86_REG_ES &&
            op.segment != X86_REG_CS) {
            throw Unsupported("segment override is unsupported in " + format_insn(insn));
        }

        std::string addr = "0";
        if (op.base == X86_REG_RIP) {
            addr = u64((uint64_t)((int64_t)insn.address + (int64_t)insn.size + op.disp));
            return addr;
        }
        if (op.base != X86_REG_INVALID) {
            int field = 0, natural = 0;
            std::string err;
            if (!map_reg(op.base, &field, &natural, &err))
                throw Unsupported(err);
            addr = read_reg(op.base, natural);
        }
        if (op.index != X86_REG_INVALID) {
            int field = 0, natural = 0;
            std::string err;
            if (!map_reg(op.index, &field, &natural, &err))
                throw Unsupported(err);
            std::string idx = read_reg(op.index, natural);
            if (op.scale != 1) {
                std::string scaled = fresh("idx");
                emit(scaled + " = mul i64 " + idx + ", " + std::to_string(op.scale));
                idx = scaled;
            }
            if (addr == "0") {
                addr = idx;
            } else {
                std::string sum = fresh("addr");
                emit(sum + " = add i64 " + addr + ", " + idx);
                addr = sum;
            }
        }
        if (op.disp != 0) {
            if (addr == "0") {
                addr = u64((uint64_t)op.disp);
            } else {
                std::string sum = fresh("addr");
                emit(sum + " = add i64 " + addr + ", " + u64((uint64_t)op.disp));
                addr = sum;
            }
        }
        return addr;
    }

    std::string read_operand(const X64Insn &insn, const X64Operand &op, int width,
                             bool sign = false) {
        switch (op.kind) {
            case OperandKind::Register: {
                std::string v = read_reg(op.reg, width);
                return sign ? sign_extend(v, width) : v;
            }
            case OperandKind::Immediate: {
                uint64_t v = (uint64_t)op.imm & width_mask(width);
                return u64(v);
            }
            case OperandKind::Memory: {
                std::string addr = mem_address(insn, op);
                return load_mem(insn, addr, width, sign);
            }
        }
        throw Unsupported("internal: bad operand kind");
    }

    void write_operand(const X64Insn &insn, const X64Operand &op, int width,
                       const std::string &value) {
        switch (op.kind) {
            case OperandKind::Register:
                write_reg(op.reg, width, value);
                return;
            case OperandKind::Memory: {
                std::string addr = mem_address(insn, op);
                store_mem(insn, addr, value, width);
                return;
            }
            case OperandKind::Immediate:
                throw Unsupported("cannot write an immediate operand");
        }
    }

    std::string flag_bit(uint64_t bit) {
        std::string rf = load_field(GF_rflags);
        std::string masked = fresh("f");
        emit(masked + " = and i64 " + rf + ", " + u64(bit));
        std::string c = fresh("c");
        emit(c + " = icmp ne i64 " + masked + ", 0");
        return c;
    }

    std::string not1(const std::string &v) {
        std::string n = fresh("n");
        emit(n + " = xor i1 " + v + ", true");
        return n;
    }

    std::string cond_value(Cond cc) {
        if (cc == CC_O) return flag_bit(AEV_OF);
        if (cc == CC_NO) return not1(flag_bit(AEV_OF));
        if (cc == CC_B) return flag_bit(AEV_CF);
        if (cc == CC_AE) return not1(flag_bit(AEV_CF));
        if (cc == CC_E) return flag_bit(AEV_ZF);
        if (cc == CC_NE) return not1(flag_bit(AEV_ZF));
        if (cc == CC_BE) {
            std::string c = fresh("c");
            emit(c + " = or i1 " + flag_bit(AEV_CF) + ", " + flag_bit(AEV_ZF));
            return c;
        }
        if (cc == CC_A) {
            std::string c = fresh("c");
            emit(c + " = or i1 " + flag_bit(AEV_CF) + ", " + flag_bit(AEV_ZF));
            return not1(c);
        }
        if (cc == CC_S) return flag_bit(AEV_SF);
        if (cc == CC_NS) return not1(flag_bit(AEV_SF));
        if (cc == CC_P) return flag_bit(AEV_PF);
        if (cc == CC_NP) return not1(flag_bit(AEV_PF));
        if (cc == CC_L) {
            std::string c = fresh("c");
            emit(c + " = icmp ne i1 " + flag_bit(AEV_SF) + ", " + flag_bit(AEV_OF));
            return c;
        }
        if (cc == CC_GE) return not1(cond_value(CC_L));
        if (cc == CC_LE) {
            std::string c = fresh("c");
            emit(c + " = or i1 " + flag_bit(AEV_ZF) + ", " + cond_value(CC_L));
            return c;
        }
        if (cc == CC_G) return not1(cond_value(CC_LE));
        throw Unsupported("internal: bad condition code");
    }

    void emit_push(const X64Insn &insn, const std::string &value) {
        if (insn.id == X86_INS_PUSH &&
            (insn.operands.empty() || insn.operands[0].size == 2))
            throw Unsupported("unsupported 16-bit push " + format_insn(insn));
        std::string rsp = load_field(GF_rsp);
        std::string nrsp = fresh("rsp");
        emit(nrsp + " = sub i64 " + rsp + ", 8");
        store_mem(insn, nrsp, value, 8);
        store_field(GF_rsp, nrsp);
    }

    void emit_pop(const X64Insn &insn, int reg) {
        if (insn.operands.empty() || insn.operands[0].size == 2)
            throw Unsupported("unsupported 16-bit pop " + format_insn(insn));
        int field = 0, natural = 0;
        std::string err;
        if (!map_reg(reg, &field, &natural, &err))
            throw Unsupported(err);
        const int width = insn.operands[0].size;
        std::string rsp = load_field(GF_rsp);
        std::string v = load_mem(insn, rsp, 8, false);
        write_reg(reg, width, v);
        /* POP RSP: the popped value is the new stack pointer.  The ordinary
         * RSP+8 update would overwrite it, so skip that update for RSP. */
        if (field != GF_rsp) {
            std::string nrsp = fresh("rsp");
            emit(nrsp + " = add i64 " + rsp + ", 8");
            store_field(GF_rsp, nrsp);
        }
    }

    void emit_flags_call(const char *name, int width, const std::string &a,
                         const std::string &b, const std::string &r) {
        emit("call void @" + std::string(name) + "(ptr %s, i32 " +
             std::to_string(width) + ", i64 " + a + ", i64 " + b + ", i64 " + r +
             ")");
    }

    void emit_flags_1(const char *name, int width, const std::string &a,
                      const std::string &r) {
        emit("call void @" + std::string(name) + "(ptr %s, i32 " +
             std::to_string(width) + ", i64 " + a + ", i64 " + r + ")");
    }

    void emit_binary_flags(const X64Insn &insn, const X64Operand &dst,
                           const X64Operand &src, const char *op, const char *flagfn) {
        int width = dst.size;
        std::string a = read_operand(insn, dst, width);
        std::string b = read_operand(insn, src, width);
        std::string raw = fresh("r");
        emit(raw + " = " + op + " i64 " + a + ", " + b);
        std::string r = and_mask(raw, width);
        /* For a memory destination the store can fault.  Write first and let
         * the checked store branch to the block's fault exit; only then commit
         * flags, so a faulting write leaves RFLAGS untouched. */
        write_operand(insn, dst, width, r);
        emit_flags_call(flagfn, width, a, b, r);
    }

    void emit_logic(const X64Insn &insn, const X64Operand &dst,
                    const X64Operand &src, const char *op) {
        int width = dst.size;
        std::string a = read_operand(insn, dst, width);
        std::string b = read_operand(insn, src, width);
        std::string raw = fresh("r");
        emit(raw + " = " + op + " i64 " + a + ", " + b);
        std::string r = and_mask(raw, width);
        write_operand(insn, dst, width, r);
        emit("call void @aev_flags_logic(ptr %s, i32 " + std::to_string(width) +
             ", i64 " + r + ")");
    }

    void emit_test_cmp(const X64Insn &insn, const X64Operand &aop,
                       const X64Operand &bop, bool is_test) {
        int width = aop.size;
        std::string a = read_operand(insn, aop, width);
        std::string b = read_operand(insn, bop, width);
        std::string raw = fresh("r");
        emit(raw + " = " + (is_test ? "and" : "sub") + " i64 " + a + ", " + b);
        std::string r = and_mask(raw, width);
        if (is_test)
            emit("call void @aev_flags_logic(ptr %s, i32 " + std::to_string(width) +
                 ", i64 " + r + ")");
        else
            emit_flags_call("aev_flags_sub", width, a, b, r);
    }

    void emit_terminator(const X64Insn &insn) {
        /* Terminators are dispatched before emit_instruction(), so the LOCK
         * guard there would not see a caller-constructed LOCK RET/JMP block.
         * Check at this entry point as well: normal translation rejects LOCK
         * in build_program, but the emitter must not silently lower it either. */
        if (insn.lock_prefix)
            throw Unsupported("unsupported instruction (LOCK prefix; atomic semantics are not implemented) " +
                              format_insn(insn));
        if (insn.id == X86_INS_JMP) {
            std::string target;
            if (!insn.operands.empty() && insn.operands[0].kind == OperandKind::Immediate)
                target = u64((uint64_t)insn.operands[0].imm);
            else if (!insn.operands.empty())
                target = read_operand(insn, insn.operands[0], 8);
            else
                throw Unsupported("jmp without operand in " + format_insn(insn));
            store_field(GF_pending_transfer, u64(AEV_TRANSFER_JMP));
            store_field(GF_rip, target);
            return;
        }
        if (insn.id == X86_INS_CALL) {
            std::string target;
            if (!insn.operands.empty() && insn.operands[0].kind == OperandKind::Immediate)
                target = u64((uint64_t)insn.operands[0].imm);
            else if (!insn.operands.empty())
                target = read_operand(insn, insn.operands[0], 8);
            else
                throw Unsupported("call without operand in " + format_insn(insn));
            emit_push(insn, u64(insn.address + insn.size));
            store_field(GF_pending_transfer, u64(AEV_TRANSFER_CALL));
            store_field(GF_rip, target);
            return;
        }
        if (insn.id == X86_INS_RET) {
            std::string rsp = load_field(GF_rsp);
            std::string target = load_mem(insn, rsp, 8, false);
            store_field(GF_pending_transfer, u64(AEV_TRANSFER_RET));
            store_field(GF_rip, target);
            std::string nrsp = fresh("rsp");
            emit(nrsp + " = add i64 " + rsp + ", 8");
            if (!insn.operands.empty() && insn.operands[0].kind == OperandKind::Immediate) {
                std::string nrsp2 = fresh("rsp");
                emit(nrsp2 + " = add i64 " + nrsp + ", " +
                     u64((uint64_t)insn.operands[0].imm));
                nrsp = nrsp2;
            }
            store_field(GF_rsp, nrsp);
            return;
        }
        Cond cc;
        if (cond_for_insn(insn.id, &cc)) {
            if (insn.operands.empty() || insn.operands[0].kind != OperandKind::Immediate)
                throw Unsupported("conditional branch without immediate target in " +
                                  format_insn(insn));
            std::string target = u64((uint64_t)insn.operands[0].imm);
            std::string fallthrough = u64(insn.address + insn.size);
            std::string c = cond_value(cc);
            std::string sel = fresh("rip");
            emit(sel + " = select i1 " + c + ", i64 " + target + ", i64 " +
                 fallthrough);
            store_field(GF_pending_transfer, u64(AEV_TRANSFER_JCC));
            store_field(GF_rip, sel);
            return;
        }
        throw Unsupported("unsupported terminator in " + format_insn(insn));
    }

    void emit_instruction(const X64Insn &insn) {
        if (insn.lock_prefix)
            throw Unsupported("unsupported instruction (LOCK prefix; atomic semantics are not implemented) " +
                              format_insn(insn));
        switch (insn.id) {
            case X86_INS_NOP:
            case X86_INS_ENDBR64:
                return;
            case X86_INS_MOV: {
                if (insn.operands.size() != 2)
                    throw Unsupported("mov operand count in " + format_insn(insn));
                const X64Operand &dst = insn.operands[0];
                const X64Operand &src = insn.operands[1];
                int width = dst.size;
                if (dst.kind == OperandKind::Immediate)
                    throw Unsupported("mov to immediate in " + format_insn(insn));
                std::string v = read_operand(insn, src, width);
                write_operand(insn, dst, width, v);
                return;
            }
            case X86_INS_LEA: {
                if (insn.operands.size() != 2 ||
                    insn.operands[0].kind != OperandKind::Register ||
                    insn.operands[1].kind != OperandKind::Memory)
                    throw Unsupported("lea form in " + format_insn(insn));
                std::string addr = mem_address(insn, insn.operands[1]);
                write_operand(insn, insn.operands[0], insn.operands[0].size, addr);
                return;
            }
            case X86_INS_PUSH: {
                if (insn.operands.size() != 1)
                    throw Unsupported("push operand count in " + format_insn(insn));
                std::string v = read_operand(insn, insn.operands[0], 8, true);
                emit_push(insn, v);
                return;
            }
            case X86_INS_POP: {
                if (insn.operands.size() != 1 ||
                    insn.operands[0].kind != OperandKind::Register)
                    throw Unsupported("pop form in " + format_insn(insn));
                emit_pop(insn, insn.operands[0].reg);
                return;
            }
            case X86_INS_ADD:
                emit_binary_flags(insn, insn.operands[0], insn.operands[1], "add",
                                  "aev_flags_add");
                return;
            case X86_INS_SUB:
                emit_binary_flags(insn, insn.operands[0], insn.operands[1], "sub",
                                  "aev_flags_sub");
                return;
            case X86_INS_AND:
                emit_logic(insn, insn.operands[0], insn.operands[1], "and");
                return;
            case X86_INS_OR:
                emit_logic(insn, insn.operands[0], insn.operands[1], "or");
                return;
            case X86_INS_XOR:
                emit_logic(insn, insn.operands[0], insn.operands[1], "xor");
                return;
            case X86_INS_CMP:
                emit_test_cmp(insn, insn.operands[0], insn.operands[1], false);
                return;
            case X86_INS_TEST:
                emit_test_cmp(insn, insn.operands[0], insn.operands[1], true);
                return;
            case X86_INS_INC:
            case X86_INS_DEC: {
                if (insn.operands.size() != 1)
                    throw Unsupported("inc/dec operand count in " + format_insn(insn));
                const X64Operand &dst = insn.operands[0];
                int width = dst.size;
                std::string a = read_operand(insn, dst, width);
                std::string raw = fresh("r");
                emit(raw + " = " + (insn.id == X86_INS_INC ? "add" : "sub") +
                     " i64 " + a + ", 1");
                std::string r = and_mask(raw, width);
                write_operand(insn, dst, width, r);
                emit_flags_1(insn.id == X86_INS_INC ? "aev_flags_inc" : "aev_flags_dec",
                             width, a, r);
                return;
            }
            case X86_INS_NEG: {
                if (insn.operands.size() != 1)
                    throw Unsupported("neg operand count in " + format_insn(insn));
                const X64Operand &dst = insn.operands[0];
                int width = dst.size;
                std::string a = read_operand(insn, dst, width);
                std::string raw = fresh("r");
                emit(raw + " = sub i64 0, " + a);
                std::string r = and_mask(raw, width);
                write_operand(insn, dst, width, r);
                emit_flags_1("aev_flags_neg", width, a, r);
                return;
            }
            case X86_INS_IMUL: {
                if (insn.operands.size() < 2 ||
                    insn.operands[0].kind != OperandKind::Register)
                    throw Unsupported("unsupported imul form (one-operand IMUL is not implemented) " +
                                      format_insn(insn));
                const X64Operand &dst = insn.operands[0];
                int width = dst.size;
                const bool three_operand = insn.operands.size() >= 3;
                /* Two-operand: dst = dst * src.
                 * Three-operand: dst = src * imm; operand[0] is only the
                 * destination and must not be read as the multiplicand. */
                std::string a = read_operand(insn, three_operand ? insn.operands[1] : dst,
                                             width);
                std::string b = read_operand(insn, three_operand ? insn.operands[2]
                                                                 : insn.operands[1],
                                             width);
                std::string raw = fresh("r");
                emit(raw + " = mul i64 " + a + ", " + b);
                std::string r = and_mask(raw, width);
                write_operand(insn, dst, width, r);
                emit_flags_call("aev_flags_imul", width, a, b, r);
                return;
            }
            case X86_INS_SHL:
            case X86_INS_SHR:
            case X86_INS_SAR: {
                if (insn.operands.empty() || insn.operands.size() > 2)
                    throw Unsupported("shift form in " + format_insn(insn));
                const X64Operand &dst = insn.operands[0];
                int width = dst.size;
                std::string a = read_operand(insn, dst, width);
                std::string count;
                if (insn.operands.size() == 1) {
                    count = "1";
                } else {
                    const X64Operand &cop = insn.operands[1];
                    if (cop.kind == OperandKind::Immediate)
                        count = u64((uint64_t)cop.imm & 0xffULL);
                    else if (cop.kind == OperandKind::Register &&
                             cop.reg == X86_REG_CL)
                        count = read_reg(X86_REG_CL, 1);
                    else
                        throw Unsupported("unsupported shift count in " +
                                          format_insn(insn));
                }
                std::string bits = fresh("shc");
                /* x86 masks shift counts to 5 bits for all operand sizes. */
                emit(bits + " = and i64 " + count + ", 31");
                std::string zero = fresh("shz");
                emit(zero + " = icmp eq i64 " + bits + ", 0");
                std::string shifted;
                if (insn.id == X86_INS_SHL) {
                    shifted = fresh("shv");
                    emit(shifted + " = shl i64 " + a + ", " + bits);
                } else if (insn.id == X86_INS_SHR) {
                    shifted = fresh("shv");
                    emit(shifted + " = lshr i64 " + a + ", " + bits);
                } else {
                    std::string sa = sign_extend(a, width);
                    shifted = fresh("shv");
                    emit(shifted + " = ashr i64 " + sa + ", " + bits);
                }
                std::string r = and_mask(shifted, width);
                std::string sel = fresh("shr");
                emit(sel + " = select i1 " + zero + ", i64 " + a + ", i64 " + r);
                write_operand(insn, dst, width, sel);
                int kind = insn.id == X86_INS_SHL ? 0
                           : (insn.id == X86_INS_SHR ? 1 : 2);
                emit("call void @aev_flags_shift(ptr %s, i32 " +
                     std::to_string(width) + ", i64 " + a + ", i64 " + bits +
                     ", i64 " + sel + ", i32 " + std::to_string(kind) + ")");
                return;
            }
            case X86_INS_CDQE: {
                std::string v = load_field(GF_rax);
                std::string sh = fresh("cdq");
                emit(sh + " = shl i64 " + v + ", 32");
                std::string as = fresh("cdq");
                emit(as + " = ashr i64 " + sh + ", 32");
                store_field(GF_rax, as);
                return;
            }
            case X86_INS_MOVZX:
            case X86_INS_MOVSX:
            case X86_INS_MOVSXD: {
                if (insn.operands.size() != 2 ||
                    insn.operands[0].kind != OperandKind::Register)
                    throw Unsupported("movzx/movsx form in " + format_insn(insn));
                int src_width = insn.operands[1].size;
                bool sign = insn.id != X86_INS_MOVZX;
                std::string v = read_operand(insn, insn.operands[1], src_width, sign);
                write_operand(insn, insn.operands[0], insn.operands[0].size, v);
                return;
            }
            default: {
                Cond cc;
                if (insn.id != X86_INS_JMP && insn.id != X86_INS_CALL &&
                    insn.id != X86_INS_RET && cond_for_insn(insn.id, &cc)) {
                    /* CMOVcc: select, do not touch flags. */
                    if (insn.operands.size() != 2 ||
                        insn.operands[0].kind != OperandKind::Register)
                        throw Unsupported("cmov form in " + format_insn(insn));
                    int width = insn.operands[0].size;
                    std::string a = read_operand(insn, insn.operands[0], width);
                    std::string b = read_operand(insn, insn.operands[1], width);
                    std::string c = cond_value(cc);
                    std::string sel = fresh("sel");
                    emit(sel + " = select i1 " + c + ", i64 " + b + ", i64 " + a);
                    write_operand(insn, insn.operands[0], width, sel);
                    return;
                }
                throw Unsupported("unimplemented instruction " + format_insn(insn));
            }
        }
    }

    uint64_t next_block_address(const GuestFunction &fn, const Block &block) {
        for (const auto &b : fn.blocks) {
            if (b.address > block.address)
                return b.address;
        }
        throw Unsupported("block at 0x" + hex64(block.address) +
                          " has no fallthrough target");
    }
};

std::string module_prefix() {
    std::ostringstream os;
    os << "target datalayout = \"e-m:o-i64:64-i128:128-n32:64-S128\"\n";
    os << "target triple = \"arm64-apple-macosx14.0.0\"\n\n";
    os << "%GuestState = type { ";
    for (int i = 0; i < GF_COUNT; ++i)
        os << (i ? ", i64" : "i64");
    os << " }\n\n";
    os << "declare i64 @aev_load(ptr, i64, i32, i32)\n";
    os << "declare void @aev_store(ptr, i64, i64, i32)\n";
    os << "declare void @aev_flags_add(ptr, i32, i64, i64, i64)\n";
    os << "declare void @aev_flags_sub(ptr, i32, i64, i64, i64)\n";
    os << "declare void @aev_flags_logic(ptr, i32, i64)\n";
    os << "declare void @aev_flags_inc(ptr, i32, i64, i64)\n";
    os << "declare void @aev_flags_dec(ptr, i32, i64, i64)\n";
    os << "declare void @aev_flags_neg(ptr, i32, i64, i64)\n";
    os << "declare void @aev_flags_imul(ptr, i32, i64, i64, i64)\n";
    os << "declare void @aev_flags_shift(ptr, i32, i64, i64, i64, i32)\n";
    os << "declare void @aev_note_aot(ptr, i64)\n\n";
    return os.str();
}

}  // namespace

bool emit_aot_module(const Program &program, uint64_t input_hash,
                     const uint8_t input_sha256[32], uint64_t entry_addr,
                     const uint8_t function_name_sha256[32],
                     const AotModuleIdentity &identity,
                     std::string *out, std::string *error) {
    try {
        std::ostringstream os;
        os << module_prefix();
        os << "%BlockEntry = type { i64, ptr }\n\n";
        os << "@aev_input_hash = constant i64 " << input_hash << "\n";

        std::vector<BlockIdentity> all_blocks;
        if (!all_block_identities(program, &all_blocks, error))
            return false;

        auto emit_i8_array = [&](const char *name, const uint8_t *data, size_t len) {
            os << "@" << name << " = constant [" << len << " x i8] [";
            for (size_t i = 0; i < len; ++i) {
                if (i) os << ", ";
                os << "i8 " << (unsigned)data[i];
            }
            os << "]\n";
        };
        emit_i8_array("aev_input_sha256", input_sha256, 32);
        emit_i8_array("aev_function_name_sha256", function_name_sha256, 32);
        std::string kind = identity.module_kind;
        std::vector<uint8_t> kind_bytes(kind.begin(), kind.end());
        kind_bytes.push_back(0);
        emit_i8_array("aev_module_kind", kind_bytes.data(), kind_bytes.size());
        os << "@aev_module_file_size = constant i64 " << identity.module_file_size << "\n";
        os << "@aev_preferred_base = constant i64 " << identity.preferred_base << "\n";
        os << "@aev_load_base = constant i64 " << identity.load_base << "\n";
        os << "@aev_image_size = constant i64 " << identity.image_size << "\n";
        emit_i8_array("aev_image_sha256", identity.image_sha256, 32);
        emit_i8_array("aev_import_binding_sha256", identity.import_binding_sha256, 32);
        emit_i8_array("aev_host_binding_sha256", identity.host_binding_sha256, 32);
        emit_i8_array("aev_entry_exports_sha256", identity.entry_exports_sha256, 32);
        os << "@aev_thunk_base = constant i64 " << identity.thunk_base << "\n";
        os << "@aev_import_count = constant i64 " << identity.import_count << "\n";
        os << "@aev_import_thunks = constant [" << identity.import_thunks.size()
           << " x i64] ";
        if (identity.import_thunks.empty()) {
            os << "zeroinitializer";
        } else {
            os << "[";
            for (size_t i = 0; i < identity.import_thunks.size(); ++i) {
                if (i) os << ", ";
                os << "i64 " << identity.import_thunks[i];
            }
            os << "]";
        }
        os << "\n";
        os << "@aev_relocs_stripped = constant i64 " << identity.relocs_stripped << "\n";
        std::string target = AEV_AOT_TARGET_TRIPLE;
        emit_i8_array("aev_target_triple",
                      reinterpret_cast<const uint8_t *>(target.data()),
                      target.size() + 1);
        os << "@aev_entry_addr = constant i64 " << entry_addr << "\n";
        os << "@aev_all_block_count = constant i64 " << all_blocks.size() << "\n";
        os << "@aev_all_blocks = constant [" << all_blocks.size() << " x i64] [";
        for (size_t i = 0; i < all_blocks.size(); ++i) {
            if (i) os << ", ";
            os << "i64 " << all_blocks[i].rip;
        }
        os << "]\n";

        struct Entry {
            uint64_t rip;
            std::string fn;
        };
        std::vector<Entry> entries;

        for (const auto &fn : program.functions) {
            for (const auto &block : fn.blocks) {
                if (block.jit_only)
                    continue;
                Emitter emitter(program, false);
                os << emitter.emit_block(fn, block) << "\n";
                entries.push_back({block.address, "aev_blk_" + hex64(block.address)});
            }
        }

        std::sort(entries.begin(), entries.end(),
                  [](const Entry &a, const Entry &b) { return a.rip < b.rip; });

        os << "@aev_block_count = constant i64 " << entries.size() << "\n";
        /* A zero-length array cannot be written with the bracketed
         * `[zeroinitializer]` form; the valid spelling is the bare
         * `zeroinitializer` for the whole aggregate.  A function whose blocks
         * are all JIT-only therefore still produces a compilable module. */
        os << "@aev_blocks = constant [" << entries.size() << " x %BlockEntry] ";
        if (entries.empty()) {
            os << "zeroinitializer";
        } else {
            os << "[";
            for (size_t i = 0; i < entries.size(); ++i) {
                if (i) os << ", ";
                os << "%BlockEntry { i64 " << entries[i].rip << ", ptr @" << entries[i].fn << " }";
            }
            os << "]";
        }
        os << "\n";

        *out = os.str();
        return true;
    } catch (const std::exception &e) {
        if (error) *error = e.what();
        return false;
    }
}

bool emit_jit_block(const Program &program,
                    const GuestFunction &fn,
                    const Block &block,
                    std::string *out,
                    std::string *error) {
    try {
        Emitter emitter(program, true);
        std::string body = emitter.emit_block(fn, block);
        *out = module_prefix() + body;
        return true;
    } catch (const std::exception &e) {
        if (error) *error = e.what();
        return false;
    }
}

}  // namespace aev
