// Test-only emitter defense-in-depth probe.  Normal translation rejects LOCK
// in build_program before any IR is generated.  This probe constructs a Program
// by hand with a LOCK-prefixed terminator (RET and JMP), which real x86-64
// decoding cannot produce, and checks that the emitter's terminator entry point
// rejects it instead of lowering it as an ordinary branch/return.
#include "ir_emitter.hpp"

#include <capstone/x86.h>

#include <cstdio>
#include <string>
#include <utility>

namespace {

aev::X64Insn make_insn(uint16_t id, const char *mnemonic, bool lock) {
    aev::X64Insn insn;
    insn.address = 0x140001000;
    insn.size = 1;
    insn.id = id;
    insn.lock_prefix = lock;
    insn.mnemonic = mnemonic;
    insn.bytes.push_back(0x90);
    return insn;
}

bool emit_one_insn(bool lock, uint16_t id, const char *mnemonic, bool expect_ok,
                   const char *label) {
    aev::Program program;
    aev::GuestFunction fn;
    fn.name = "probe";
    fn.address = 0x140001000;
    fn.bytes = {0x90};
    fn.insns.push_back(make_insn(id, mnemonic, lock));

    aev::Block block;
    block.address = fn.address;
    block.begin = 0;
    block.end = 1;
    block.jit_only = true;
    fn.blocks.push_back(block);
    program.functions.push_back(std::move(fn));

    std::string out;
    std::string error;
    const bool ok = aev::emit_jit_block(program, program.functions[0],
                                        program.functions[0].blocks[0], &out,
                                        &error);
    if (ok != expect_ok) {
        std::printf("FAIL: %s: emission %s (expected %s)\n", label,
                    ok ? "succeeded" : "failed", expect_ok ? "success" : "failure");
        if (!ok) std::printf("  error: %s\n", error.c_str());
        return false;
    }
    if (!expect_ok) {
        if (error.find("LOCK") == std::string::npos ||
            error.find("0x140001000") == std::string::npos) {
            std::printf("FAIL: %s: rejection lacks LOCK/address: %s\n", label,
                        error.c_str());
            return false;
        }
        std::printf("PASS: %s rejected: %s\n", label, error.c_str());
    } else {
        std::printf("PASS: %s emitted as a control\n", label);
    }
    return true;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= emit_one_insn(true, X86_INS_RET, "ret", false, "LOCK RET terminator");
    ok &= emit_one_insn(true, X86_INS_JMP, "jmp", false, "LOCK JMP terminator");
    ok &= emit_one_insn(false, X86_INS_RET, "ret", true, "plain RET terminator");
    return ok ? 0 : 1;
}
