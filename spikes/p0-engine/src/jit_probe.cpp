// Test-only JIT probe: decode a COFF function, emit one basic block with the
// engine's own emitter, compile it with LLVM ORC, execute the returned ARM64
// code, and print the resulting GuestState.  It is used by
// scripts/run_semantics_tests.sh; it is not shipped runtime code.
#include "aev_guest.h"
#include "ir_emitter.hpp"
#include "translator.hpp"

#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/Support.h>
#include <llvm-c/Target.h>

#include <sys/mman.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

void die(const char *msg) {
    std::fprintf(stderr, "aev-jit-probe: %s\n", msg);
    std::exit(2);
}

void print_llvm_error(const char *what, LLVMErrorRef err) {
    char *msg = LLVMGetErrorMessage(err);
    std::fprintf(stderr, "%s: %s\n", what, msg ? msg : "(unknown LLVM error)");
    if (msg) LLVMDisposeErrorMessage(msg);
}

uint64_t parse_u64(const std::string &s) {
    return (uint64_t)std::strtoull(s.c_str(), nullptr, 0);
}

uint64_t parse_value(const std::string &s, const GuestState &st) {
    if (s.rfind("mem_end+", 0) == 0)
        return st.mem_base + st.mem_size + parse_u64(s.substr(8));
    if (s.rfind("mem_end-", 0) == 0)
        return st.mem_base + st.mem_size - parse_u64(s.substr(8));
    if (s.rfind("mem+", 0) == 0) return st.mem_base + parse_u64(s.substr(4));
    if (s.rfind("mem-", 0) == 0) return st.mem_base - parse_u64(s.substr(4));
    return parse_u64(s);
}

void set_reg(GuestState *s, const std::string &name, uint64_t value) {
    if (name == "rax") s->rax = value;
    else if (name == "rbx") s->rbx = value;
    else if (name == "rcx") s->rcx = value;
    else if (name == "rdx") s->rdx = value;
    else if (name == "rsi") s->rsi = value;
    else if (name == "rdi") s->rdi = value;
    else if (name == "rbp") s->rbp = value;
    else if (name == "rsp") s->rsp = value;
    else if (name == "r8") s->r8 = value;
    else if (name == "r9") s->r9 = value;
    else if (name == "r10") s->r10 = value;
    else if (name == "r11") s->r11 = value;
    else if (name == "r12") s->r12 = value;
    else if (name == "r13") s->r13 = value;
    else if (name == "r14") s->r14 = value;
    else if (name == "r15") s->r15 = value;
    else if (name == "rip") s->rip = value;
    else if (name == "rflags") s->rflags = value;
    else {
        std::fprintf(stderr, "aev-jit-probe: unknown register %s\n", name.c_str());
        std::exit(2);
    }
}

bool register_absolute_symbols(LLVMOrcLLJITRef jit, LLVMOrcJITDylibRef jd) {
    struct Sym { const char *name; void *addr; };
    const Sym syms[] = {
        {"aev_load", reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&aev_load))},
        {"aev_store", reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&aev_store))},
        {"aev_flags_add", reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&aev_flags_add))},
        {"aev_flags_sub", reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&aev_flags_sub))},
        {"aev_flags_logic", reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&aev_flags_logic))},
        {"aev_flags_inc", reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&aev_flags_inc))},
        {"aev_flags_dec", reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&aev_flags_dec))},
        {"aev_flags_neg", reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&aev_flags_neg))},
        {"aev_flags_imul", reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(&aev_flags_imul))},
    };
    const size_t n = sizeof(syms) / sizeof(syms[0]);
    std::vector<LLVMOrcCSymbolMapPair> pairs(n);
    for (size_t i = 0; i < n; ++i) {
        pairs[i].Name = LLVMOrcLLJITMangleAndIntern(jit, syms[i].name);
        pairs[i].Sym.Address = (LLVMOrcExecutorAddress)(uintptr_t)syms[i].addr;
        pairs[i].Sym.Flags.GenericFlags = 0x30; /* Exported | Callable */
        pairs[i].Sym.Flags.TargetFlags = 0;
    }
    LLVMOrcMaterializationUnitRef mu = LLVMOrcAbsoluteSymbols(pairs.data(), n);
    LLVMErrorRef err = LLVMOrcJITDylibDefine(jd, mu);
    if (err) {
        print_llvm_error("LLVMOrcJITDylibDefine", err);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: %s <obj> <function> <rip> [key=value ...]\n"
                     "  registers: rax..r15, rip, rflags\n"
                     "  memory:    mem=addr:size:value, dump=addr:size\n"
                     "  addresses: mem+OFF, mem-OFF, mem_end+OFF, mem_end-OFF\n",
                     argv[0]);
        return 2;
    }
    const std::string obj_path = argv[1];
    const std::string func_name = argv[2];
    const uint64_t rip = parse_u64(argv[3]);

    std::string error;
    aev::CoffObject object;
    if (!object.load(obj_path, &error)) {
        std::fprintf(stderr, "coff load failed: %s\n", error.c_str());
        return 2;
    }
    aev::Program program;
    std::vector<std::string> names{func_name};
    std::vector<uint64_t> addrs{0x140001000ULL};
    if (!aev::build_program(std::move(object), names, addrs, &program, &error)) {
        std::fprintf(stderr, "program setup failed: %s\n", error.c_str());
        return 2;
    }
    const aev::GuestFunction *fn = program.find_function_at(rip);
    const aev::Block *block = program.find_block(rip);
    if (!fn || !block) {
        std::fprintf(stderr, "no function/block at rip=0x%llx\n",
                     (unsigned long long)rip);
        return 2;
    }

    std::string ir;
    if (!aev::emit_jit_block(program, *fn, *block, &ir, &error)) {
        std::fprintf(stderr, "JIT IR emission failed: %s\n", error.c_str());
        return 3;
    }

    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();
    LLVMOrcLLJITRef jit = nullptr;
    LLVMErrorRef llvm_err = LLVMOrcCreateLLJIT(&jit, nullptr);
    if (llvm_err) {
        print_llvm_error("LLVMOrcCreateLLJIT", llvm_err);
        return 2;
    }
    LLVMOrcJITDylibRef jd = LLVMOrcLLJITGetMainJITDylib(jit);
    const char *dl = LLVMOrcLLJITGetDataLayoutStr(jit);
    const char *triple = LLVMOrcLLJITGetTripleString(jit);
    auto replace_line = [&](const std::string &key, const std::string &replacement) {
        size_t pos = ir.find(key);
        if (pos == std::string::npos) return;
        size_t end = ir.find('\n', pos);
        if (end == std::string::npos) end = ir.size();
        ir.replace(pos, end - pos, replacement);
    };
    replace_line("target datalayout =", std::string("target datalayout = \"") + dl + "\"");
    replace_line("target triple =", std::string("target triple = \"") + triple + "\"");
    if (!register_absolute_symbols(jit, jd)) return 2;

    const long page = sysconf(_SC_PAGESIZE);
    const size_t mem_size = (size_t)page * 16;
    void *mem = mmap(nullptr, mem_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        std::perror("mmap");
        return 2;
    }

    GuestState state;
    std::memset(&state, 0, sizeof(state));
    state.mem_base = (uint64_t)(uintptr_t)mem;
    state.mem_size = mem_size;
    state.rsp = (state.mem_base + mem_size - 0x100) & ~0xFULL;
    state.rip = rip;
    *reinterpret_cast<uint64_t *>((void *)(uintptr_t)state.rsp) = AEV_RETURN_SENTINEL;

    std::vector<std::pair<uint64_t, size_t>> dumps;
    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];
        size_t eq = arg.find('=');
        if (eq == std::string::npos) die("expected key=value");
        std::string key = arg.substr(0, eq);
        std::string val = arg.substr(eq + 1);
        if (key == "mem") {
            size_t c1 = val.find(':');
            size_t c2 = val.find(':', c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos)
                die("mem=addr:size:value");
            uint64_t addr = parse_value(val.substr(0, c1), state);
            size_t size = (size_t)parse_u64(val.substr(c1 + 1, c2 - c1 - 1));
            uint64_t value = parse_value(val.substr(c2 + 1), state);
            if (size != 1 && size != 2 && size != 4 && size != 8)
                die("bad mem size");
            if (addr < state.mem_base || addr - state.mem_base > mem_size ||
                mem_size - (addr - state.mem_base) < size) {
                std::fprintf(stderr, "aev-jit-probe: mem write outside region\n");
                return 2;
            }
            std::memcpy((void *)(uintptr_t)addr, &value, size);
        } else if (key == "dump") {
            size_t c = val.find(':');
            if (c == std::string::npos) die("dump=addr:size");
            uint64_t addr = parse_value(val.substr(0, c), state);
            size_t size = (size_t)parse_u64(val.substr(c + 1));
            if (addr < state.mem_base || addr - state.mem_base > mem_size ||
                mem_size - (addr - state.mem_base) < size) {
                std::fprintf(stderr, "aev-jit-probe: dump outside region\n");
                return 2;
            }
            dumps.push_back({addr, size});
        } else {
            set_reg(&state, key, parse_value(val, state));
        }
    }
    const uint64_t rsp_in = state.rsp;

    LLVMContextRef ctx = LLVMContextCreate();
    LLVMMemoryBufferRef buf = LLVMCreateMemoryBufferWithMemoryRangeCopy(
        ir.data(), ir.size(), "aev-jit-probe");
    LLVMModuleRef mod = nullptr;
    char *parse_msg = nullptr;
    if (LLVMParseIRInContext(ctx, buf, &mod, &parse_msg)) {
        std::fprintf(stderr, "JIT IR parse failed: %s\n",
                     parse_msg ? parse_msg : "(no message)");
        if (parse_msg) LLVMDisposeMessage(parse_msg);
        return 2;
    }
    if (parse_msg) LLVMDisposeMessage(parse_msg);
    LLVMOrcThreadSafeContextRef tsc =
        LLVMOrcCreateNewThreadSafeContextFromLLVMContext(ctx);
    LLVMOrcThreadSafeModuleRef tsm = LLVMOrcCreateNewThreadSafeModule(mod, tsc);
    llvm_err = LLVMOrcLLJITAddLLVMIRModule(jit, jd, tsm);
    if (llvm_err) {
        print_llvm_error("LLVMOrcLLJITAddLLVMIRModule", llvm_err);
        return 2;
    }
    char sym[64];
    std::snprintf(sym, sizeof(sym), "aev_jit_%llx", (unsigned long long)rip);
    LLVMOrcExecutorAddress addr = 0;
    llvm_err = LLVMOrcLLJITLookup(jit, &addr, sym);
    if (llvm_err) {
        print_llvm_error("LLVMOrcLLJITLookup", llvm_err);
        return 2;
    }
    std::fprintf(stderr, "JIT compile rip=0x%llx symbol=%s host_addr=0x%llx triple=%s\n",
                 (unsigned long long)rip, sym, (unsigned long long)addr, triple);

    using JitFn = void (*)(GuestState *);
    JitFn jf = reinterpret_cast<JitFn>((uintptr_t)addr);
    jf(&state);

    std::printf("rip_in=0x%llx mem_base=0x%llx mem_size=0x%llx rsp_in=0x%llx "
                "rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx "
                "rsi=0x%llx rdi=0x%llx rbp=0x%llx rsp=0x%llx "
                "r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx "
                "r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx "
                "rip_out=0x%llx rflags=0x%llx error=%llu error_pc=0x%llx\n",
                (unsigned long long)rip,
                (unsigned long long)state.mem_base,
                (unsigned long long)state.mem_size,
                (unsigned long long)rsp_in,
                (unsigned long long)state.rax, (unsigned long long)state.rbx,
                (unsigned long long)state.rcx, (unsigned long long)state.rdx,
                (unsigned long long)state.rsi, (unsigned long long)state.rdi,
                (unsigned long long)state.rbp, (unsigned long long)state.rsp,
                (unsigned long long)state.r8, (unsigned long long)state.r9,
                (unsigned long long)state.r10, (unsigned long long)state.r11,
                (unsigned long long)state.r12, (unsigned long long)state.r13,
                (unsigned long long)state.r14, (unsigned long long)state.r15,
                (unsigned long long)state.rip, (unsigned long long)state.rflags,
                (unsigned long long)state.error_code,
                (unsigned long long)state.error_pc);
    for (const auto &d : dumps) {
        std::printf("dump addr=0x%llx size=%zu bytes=",
                    (unsigned long long)d.first, d.second);
        const uint8_t *p = reinterpret_cast<const uint8_t *>((uintptr_t)d.first);
        for (size_t i = 0; i < d.second; ++i)
            std::printf("%s%02x", i ? ":" : "", p[i]);
        std::printf("\n");
    }
    std::printf("CF=%d OF=%d ZF=%d SF=%d PF=%d\n",
                !!(state.rflags & AEV_CF), !!(state.rflags & AEV_OF),
                !!(state.rflags & AEV_ZF), !!(state.rflags & AEV_SF),
                !!(state.rflags & AEV_PF));

    LLVMErrorRef dispose_err = LLVMOrcDisposeLLJIT(jit);
    if (dispose_err) print_llvm_error("LLVMOrcDisposeLLJIT", dispose_err);
    munmap(mem, mem_size);
    return 0;
}
