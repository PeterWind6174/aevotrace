// Test-only AOT/JIT probe: load a translated ARM64 dylib, call its
// aev_aot_run dispatcher with a real LLVM ORC JIT callback, and print the
// resulting GuestState.  This exercises the hand-off from the AOT runtime into
// JIT-compiled ARM64 blocks, including an empty AOT table (every block is
// JIT-only) and a JIT block that faults after a committed instruction.
//
// It deliberately mirrors the minimum of runner.cpp's ORC setup; it is not
// shipped runtime code and does not change the product dispatch path.
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

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

void die(const char *msg) {
    std::fprintf(stderr, "aev-aot-jit-probe: %s\n", msg);
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
        std::fprintf(stderr, "aev-aot-jit-probe: unknown register %s\n", name.c_str());
        std::exit(2);
    }
}

struct JitCtx {
    LLVMOrcLLJITRef jit = nullptr;
    LLVMOrcJITDylibRef jd = nullptr;
    const aev::Program *program = nullptr;
    std::unordered_map<uint64_t, void *> cache;
    uint64_t compile_events = 0;
    uint64_t exec_events = 0;
    std::string data_layout;
    std::string triple;
};

bool register_absolute_symbols(JitCtx *ctx) {
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
        pairs[i].Name = LLVMOrcLLJITMangleAndIntern(ctx->jit, syms[i].name);
        pairs[i].Sym.Address = (LLVMOrcExecutorAddress)(uintptr_t)syms[i].addr;
        pairs[i].Sym.Flags.GenericFlags = 0x30; /* Exported | Callable */
        pairs[i].Sym.Flags.TargetFlags = 0;
    }
    LLVMOrcMaterializationUnitRef mu = LLVMOrcAbsoluteSymbols(pairs.data(), n);
    LLVMErrorRef err = LLVMOrcJITDylibDefine(ctx->jd, mu);
    if (err) {
        print_llvm_error("LLVMOrcJITDylibDefine", err);
        return false;
    }
    return true;
}

bool jit_init(JitCtx *ctx) {
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();
    LLVMErrorRef err = LLVMOrcCreateLLJIT(&ctx->jit, nullptr);
    if (err) {
        print_llvm_error("LLVMOrcCreateLLJIT", err);
        return false;
    }
    ctx->jd = LLVMOrcLLJITGetMainJITDylib(ctx->jit);
    ctx->data_layout = LLVMOrcLLJITGetDataLayoutStr(ctx->jit);
    ctx->triple = LLVMOrcLLJITGetTripleString(ctx->jit);
    std::fprintf(stderr, "JIT initialized triple=%s\n", ctx->triple.c_str());
    return register_absolute_symbols(ctx);
}

bool jit_compile_block(JitCtx *ctx, uint64_t rip, void **out_fn) {
    const aev::GuestFunction *fn = ctx->program->find_function_at(rip);
    if (!fn) {
        std::fprintf(stderr, "JIT ERROR: no guest function contains rip=0x%llx\n",
                     (unsigned long long)rip);
        return false;
    }
    const aev::Block *block = ctx->program->find_block(rip);
    if (!block) {
        std::fprintf(stderr,
                     "JIT ERROR: rip=0x%llx is inside %s but is not a basic-block boundary\n",
                     (unsigned long long)rip, fn->name.c_str());
        return false;
    }
    if (!block->jit_only) {
        std::fprintf(stderr,
                     "JIT ERROR: block 0x%llx is AOT-covered but missing from the AOT table\n",
                     (unsigned long long)rip);
        return false;
    }

    std::string ir, error;
    if (!aev::emit_jit_block(*ctx->program, *fn, *block, &ir, &error)) {
        std::fprintf(stderr, "JIT IR emission failed for 0x%llx: %s\n",
                     (unsigned long long)rip, error.c_str());
        return false;
    }
    auto replace_line = [&](const std::string &key, const std::string &replacement) {
        size_t pos = ir.find(key);
        if (pos == std::string::npos) return;
        size_t end = ir.find('\n', pos);
        if (end == std::string::npos) end = ir.size();
        ir.replace(pos, end - pos, replacement);
    };
    replace_line("target datalayout =", "target datalayout = \"" + ctx->data_layout + "\"");
    replace_line("target triple =", "target triple = \"" + ctx->triple + "\"");

    LLVMContextRef llvm_ctx = LLVMContextCreate();
    LLVMMemoryBufferRef buf = LLVMCreateMemoryBufferWithMemoryRangeCopy(
        ir.data(), ir.size(), "aev-aot-jit-probe");
    LLVMModuleRef mod = nullptr;
    char *parse_msg = nullptr;
    if (LLVMParseIRInContext(llvm_ctx, buf, &mod, &parse_msg)) {
        std::fprintf(stderr, "JIT IR parse failed for 0x%llx: %s\n",
                     (unsigned long long)rip, parse_msg ? parse_msg : "(no message)");
        if (parse_msg) LLVMDisposeMessage(parse_msg);
        LLVMContextDispose(llvm_ctx);
        return false;
    }
    if (parse_msg) LLVMDisposeMessage(parse_msg);

    LLVMOrcThreadSafeContextRef tsc =
        LLVMOrcCreateNewThreadSafeContextFromLLVMContext(llvm_ctx);
    LLVMOrcThreadSafeModuleRef tsm = LLVMOrcCreateNewThreadSafeModule(mod, tsc);
    LLVMErrorRef err = LLVMOrcLLJITAddLLVMIRModule(ctx->jit, ctx->jd, tsm);
    if (err) {
        print_llvm_error("LLVMOrcLLJITAddLLVMIRModule", err);
        return false;
    }

    char func_name[64];
    std::snprintf(func_name, sizeof(func_name), "aev_jit_%llx",
                  (unsigned long long)rip);
    LLVMOrcExecutorAddress addr = 0;
    err = LLVMOrcLLJITLookup(ctx->jit, &addr, func_name);
    if (err) {
        print_llvm_error("LLVMOrcLLJITLookup", err);
        return false;
    }

    ctx->compile_events++;
    std::fprintf(stderr,
                 "JIT compile rip=0x%llx symbol=%s host_addr=0x%llx triple=%s\n",
                 (unsigned long long)rip, func_name, (unsigned long long)addr,
                 ctx->triple.c_str());
    *out_fn = (void *)(uintptr_t)addr;
    return true;
}

int64_t jit_dispatch(void *opaque, GuestState *s, uint64_t rip) {
    JitCtx *ctx = static_cast<JitCtx *>(opaque);
    auto it = ctx->cache.find(rip);
    void *fn = nullptr;
    if (it == ctx->cache.end()) {
        if (!jit_compile_block(ctx, rip, &fn)) {
            if (!s->error_code) s->error_code = AEV_ERR_JIT;
            if (!s->error_pc) s->error_pc = rip;
            return -1;
        }
        ctx->cache.emplace(rip, fn);
        s->jit_compiles++;
    } else {
        fn = it->second;
    }

    s->jit_execs++;
    ctx->exec_events++;
    std::fprintf(stderr, "JIT exec rip=0x%llx host_addr=0x%llx\n",
                 (unsigned long long)rip, (unsigned long long)(uintptr_t)fn);
    using JitFn = void (*)(GuestState *);
    JitFn jf = reinterpret_cast<JitFn>(fn);
    jf(s);
    s->jit_blocks++;
    std::fprintf(stderr, "JIT return rip=0x%llx next_rip=0x%llx rax_after=0x%llx error=%llu\n",
                 (unsigned long long)rip, (unsigned long long)s->rip,
                 (unsigned long long)s->rax,
                 (unsigned long long)s->error_code);
    return s->error_code ? -1 : 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 5) {
        std::fprintf(stderr,
                     "usage: %s <dylib> <obj> <function> <rip> [key=value ...]\n"
                     "  registers: rax..r15, rip, rflags\n"
                     "  memory:    mem=addr:size:value, dump=addr:size\n"
                     "  addresses: mem+OFF, mem-OFF, mem_end+OFF, mem_end-OFF\n",
                     argv[0]);
        return 2;
    }
    const std::string dylib_path = argv[1];
    const std::string obj_path = argv[2];
    const std::string func_name = argv[3];
    const uint64_t rip = parse_u64(argv[4]);

    void *handle = dlopen(dylib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 2;
    }
    auto aot_run = reinterpret_cast<int (*)(GuestState *, const AevRuntime *)>(
        dlsym(handle, "aev_aot_run"));
    if (!aot_run) {
        std::fprintf(stderr, "dlsym(aev_aot_run) failed: %s\n", dlerror());
        return 2;
    }
    const uint64_t *table_count_ptr =
        reinterpret_cast<const uint64_t *>(dlsym(handle, "aev_block_count"));
    if (!table_count_ptr) {
        std::fprintf(stderr, "dlsym(aev_block_count) failed: %s\n", dlerror());
        return 2;
    }
    const uint64_t table_count = *table_count_ptr;
    const AevBlock *aot_blocks =
        static_cast<const AevBlock *>(dlsym(handle, "aev_blocks"));
    if (table_count != 0 && !aot_blocks) {
        std::fprintf(stderr, "dlsym(aev_blocks) failed: %s\n", dlerror());
        return 2;
    }
    std::set<uint64_t> aot_selected;
    for (uint64_t i = 0; i < table_count; ++i)
        aot_selected.insert(aot_blocks[i].rip);

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
    if (!program.find_function_at(rip) || !program.find_block(rip)) {
        std::fprintf(stderr, "no function/block at rip=0x%llx\n",
                     (unsigned long long)rip);
        return 2;
    }
    /* The AOT table is the coverage authority: blocks not in it must take
     * the JIT path even if the generic COFF builder would call them
     * AOT-coverable.  This makes the empty-table probe exercise real JIT
     * fallback across all blocks. */
    for (auto &fn : program.functions) {
        for (auto &block : fn.blocks) {
            block.jit_only = !aot_selected.count(block.address);
            block.jit_reason = block.jit_only ? "not selected in AOT table" : "";
        }
    }

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
    for (int i = 5; i < argc; ++i) {
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
                std::fprintf(stderr, "aev-aot-jit-probe: mem write outside region\n");
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
                std::fprintf(stderr, "aev-aot-jit-probe: dump outside region\n");
                return 2;
            }
            dumps.push_back({addr, size});
        } else {
            set_reg(&state, key, parse_value(val, state));
        }
    }
    const uint64_t rsp_in = state.rsp;

    JitCtx jit;
    jit.program = &program;
    if (!jit_init(&jit)) {
        dlclose(handle);
        return 2;
    }

    AevRuntime rt;
    rt.jit_dispatch = jit_dispatch;
    rt.ctx = &jit;

    int rc = aot_run(&state, &rt);

    std::printf("rip_in=0x%llx mem_base=0x%llx mem_size=0x%llx rsp_in=0x%llx "
                "rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx "
                "rsi=0x%llx rdi=0x%llx rbp=0x%llx rsp=0x%llx "
                "r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx "
                "r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx "
                "rip_out=0x%llx rflags=0x%llx rc=%d error=%llu error_pc=0x%llx "
                "steps=%llu aot_table_count=%llu aot_blocks=%llu "
                "jit_compiles=%llu jit_execs=%llu jit_compile_events=%llu "
                "jit_exec_events=%llu\n",
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
                rc, (unsigned long long)state.error_code,
                (unsigned long long)state.error_pc,
                (unsigned long long)state.steps, (unsigned long long)table_count,
                (unsigned long long)state.aot_blocks,
                (unsigned long long)state.jit_compiles,
                (unsigned long long)state.jit_execs,
                (unsigned long long)jit.compile_events,
                (unsigned long long)jit.exec_events);
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

    LLVMErrorRef dispose_err = LLVMOrcDisposeLLJIT(jit.jit);
    if (dispose_err) print_llvm_error("LLVMOrcDisposeLLJIT", dispose_err);
    dlclose(handle);
    munmap(mem, mem_size);
    return 0;
}
