#include "translator.hpp"

#include <capstone/x86.h>

#include <algorithm>
#include <cstdio>
#include <set>

namespace aev {
namespace {

bool is_jcc(uint16_t id) {
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

bool is_cmov(uint16_t id) {
    switch (id) {
        case X86_INS_CMOVO:
        case X86_INS_CMOVNO:
        case X86_INS_CMOVB:
        case X86_INS_CMOVAE:
        case X86_INS_CMOVE:
        case X86_INS_CMOVNE:
        case X86_INS_CMOVBE:
        case X86_INS_CMOVA:
        case X86_INS_CMOVS:
        case X86_INS_CMOVNS:
        case X86_INS_CMOVP:
        case X86_INS_CMOVNP:
        case X86_INS_CMOVL:
        case X86_INS_CMOVGE:
        case X86_INS_CMOVLE:
        case X86_INS_CMOVG:
            return true;
        default:
            return false;
    }
}

bool supported_insn(uint16_t id) {
    switch (id) {
        case X86_INS_MOV:
        case X86_INS_LEA:
        case X86_INS_PUSH:
        case X86_INS_POP:
        case X86_INS_ADD:
        case X86_INS_SUB:
        case X86_INS_AND:
        case X86_INS_OR:
        case X86_INS_XOR:
        case X86_INS_CMP:
        case X86_INS_TEST:
        case X86_INS_INC:
        case X86_INS_DEC:
        case X86_INS_NEG:
        case X86_INS_IMUL:
        case X86_INS_NOP:
        case X86_INS_ENDBR64:
        case X86_INS_JMP:
        case X86_INS_CALL:
        case X86_INS_RET:
        case X86_INS_MOVZX:
        case X86_INS_MOVSX:
        case X86_INS_MOVSXD:
            return true;
        default:
            return is_jcc(id) || is_cmov(id);
    }
}

std::string format_insn(const X64Insn &insn) {
    char buf[160];
    snprintf(buf, sizeof(buf), "0x%llx: %s %s (%u bytes)",
             (unsigned long long)insn.address, insn.mnemonic.c_str(),
             insn.op_str.c_str(), insn.size);
    return buf;
}

}  // namespace

bool is_block_terminator(uint16_t insn_id) {
    return is_jcc(insn_id) || insn_id == X86_INS_JMP || insn_id == X86_INS_RET;
}

const GuestFunction *Program::find_function_at(uint64_t rip) const {
    for (const auto &fn : functions) {
        if (rip >= fn.address && rip < fn.address + fn.bytes.size())
            return &fn;
    }
    return nullptr;
}

const Block *Program::find_block(uint64_t rip) const {
    for (const auto &fn : functions) {
        for (const auto &b : fn.blocks) {
            if (b.address == rip)
                return &b;
        }
    }
    return nullptr;
}

GuestFunction *Program::find_function_by_name(const std::string &name) {
    for (auto &fn : functions)
        if (fn.name == name)
            return &fn;
    return nullptr;
}

const GuestFunction *Program::find_function_by_name(const std::string &name) const {
    for (const auto &fn : functions)
        if (fn.name == name)
            return &fn;
    return nullptr;
}

bool block_is_aot_covered(const GuestFunction &fn, const Block &block) {
    for (size_t i = block.begin; i < block.end; ++i) {
        if (fn.insns[i].id == X86_INS_IMUL)
            return false;
    }
    return true;
}

bool build_program(CoffObject &&object,
                   const std::vector<std::string> &function_names,
                   const std::vector<uint64_t> &addresses,
                   Program *program,
                   std::string *error) {
    if (function_names.size() != addresses.size()) {
        if (error) *error = "function names and addresses have different sizes";
        return false;
    }
    program->object = std::move(object);
    program->functions.clear();

    std::set<uint64_t> used_addresses;
    for (size_t fi = 0; fi < function_names.size(); ++fi) {
        const std::string &name = function_names[fi];
        const CoffSymbol *sym = program->object.find_symbol(name);
        if (!sym) {
            if (error) *error = "COFF symbol not found: " + name;
            return false;
        }
        if (sym->section_number <= 0) {
            if (error) *error = "symbol has no section: " + name;
            return false;
        }
        const CoffSection *section = program->object.section(sym->section_number);
        if (!section) {
            if (error) *error = "invalid section for symbol: " + name;
            return false;
        }
        if (sym->value >= section->data.size()) {
            if (error) *error = "symbol offset outside section: " + name;
            return false;
        }
        if (!used_addresses.insert(addresses[fi]).second) {
            if (error) *error = "duplicate synthetic function address";
            return false;
        }

        GuestFunction fn;
        fn.name = name;
        fn.section_number = sym->section_number;
        fn.section_offset = sym->value;
        fn.address = addresses[fi];
        fn.bytes.assign(section->data.begin() + sym->value, section->data.end());

        DecodeResult dr = decode_x64(fn.bytes, fn.address);
        if (!dr.ok) {
            if (error) *error = "decode failed for " + name + ": " + dr.error;
            return false;
        }
        fn.insns = std::move(dr.insns);
        if (fn.insns.empty()) {
            if (error) *error = "function has no instructions: " + name;
            return false;
        }

        std::set<uint64_t> leaders;
        leaders.insert(fn.address);
        uint64_t fn_end = fn.address + fn.bytes.size();
        auto in_range = [&](uint64_t a) { return a >= fn.address && a < fn_end; };

        for (size_t i = 0; i < fn.insns.size(); ++i) {
            const X64Insn &insn = fn.insns[i];
            /* LOCK is decoded but atomics are not implemented.  Reject every
             * LOCK-prefixed form before IR emission so a LOCK ADD/INC/AND/...
             * can never be silently lowered as a plain load/store. */
            if (insn.lock_prefix) {
                if (error)
                    *error = "unsupported instruction (LOCK prefix; atomic semantics are not implemented) " +
                             format_insn(insn);
                return false;
            }
            /* The spike's push/pop implementation is 64-bit only.  Reject a
             * 16-bit form explicitly instead of accepting it and moving RSP by
             * eight bytes. */
            if ((insn.id == X86_INS_PUSH || insn.id == X86_INS_POP) &&
                !insn.operands.empty() && insn.operands[0].size == 2) {
                if (error) *error = "unsupported 16-bit push/pop " + format_insn(insn);
                return false;
            }
            if (!supported_insn(insn.id)) {
                if (error) *error = "unsupported instruction " + format_insn(insn);
                return false;
            }
            /* One-operand IMUL is outside this spike's subset at every width.
             * The explicit width in the diagnostic makes the unsupported form
             * unambiguous instead of calling every width an 8-bit form. */
            if (insn.id == X86_INS_IMUL && insn.operands.size() == 1) {
                if (error) {
                    unsigned bits = insn.operands[0].size * 8;
                    *error = "unsupported one-operand imul (" +
                             std::to_string(bits) + "-bit IMUL form) " +
                             format_insn(insn);
                }
                return false;
            }
            uint64_t next = insn.address + insn.size;
            if (is_jcc(insn.id)) {
                if (!insn.operands.empty() &&
                    insn.operands[0].kind == OperandKind::Immediate) {
                    uint64_t target = (uint64_t)insn.operands[0].imm;
                    if (!in_range(target)) {
                        if (error) *error = "branch target outside function in " + format_insn(insn);
                        return false;
                    }
                    leaders.insert(target);
                }
                if (in_range(next))
                    leaders.insert(next);
            } else if (insn.id == X86_INS_JMP) {
                if (!insn.operands.empty() &&
                    insn.operands[0].kind == OperandKind::Immediate) {
                    uint64_t target = (uint64_t)insn.operands[0].imm;
                    if (!in_range(target)) {
                        if (error) *error = "jump target outside function in " + format_insn(insn);
                        return false;
                    }
                    leaders.insert(target);
                }
            } else if (insn.id == X86_INS_CALL) {
                if (!insn.operands.empty() &&
                    insn.operands[0].kind == OperandKind::Immediate) {
                    uint64_t target = (uint64_t)insn.operands[0].imm;
                    if (in_range(target))
                        leaders.insert(target);
                }
                if (in_range(next))
                    leaders.insert(next);
            } else if (insn.id == X86_INS_RET) {
                /* no fallthrough */
            }
        }

        std::vector<uint64_t> sorted_leaders(leaders.begin(), leaders.end());
        std::sort(sorted_leaders.begin(), sorted_leaders.end());

        for (const auto &leader : sorted_leaders) {
            auto it = std::lower_bound(
                fn.insns.begin(), fn.insns.end(), leader,
                [](const X64Insn &insn, uint64_t addr) { return insn.address < addr; });
            if (it == fn.insns.end() || it->address != leader) {
                if (error) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "branch target 0x%llx is not an instruction boundary in %s",
                             (unsigned long long)leader, name.c_str());
                    *error = buf;
                }
                return false;
            }
        }

        for (size_t li = 0; li < sorted_leaders.size(); ++li) {
            Block block;
            block.address = sorted_leaders[li];
            size_t begin = (size_t)(std::lower_bound(
                fn.insns.begin(), fn.insns.end(), block.address,
                [](const X64Insn &insn, uint64_t addr) { return insn.address < addr; }) -
                fn.insns.begin());
            size_t end = fn.insns.size();
            if (li + 1 < sorted_leaders.size()) {
                end = (size_t)(std::lower_bound(
                    fn.insns.begin(), fn.insns.end(), sorted_leaders[li + 1],
                    [](const X64Insn &insn, uint64_t addr) { return insn.address < addr; }) -
                    fn.insns.begin());
            }
            block.begin = begin;
            block.end = end;
            block.jit_only = !block_is_aot_covered(fn, block);
            if (block.jit_only)
                block.jit_reason = "AOT coverage boundary: imul delegated to JIT";
            fn.blocks.push_back(block);
        }

        program->functions.push_back(std::move(fn));
    }

    return true;
}

}  // namespace aev
