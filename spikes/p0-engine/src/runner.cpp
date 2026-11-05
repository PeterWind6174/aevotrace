#include "aev_guest.h"
#include "ir_emitter.hpp"
#include "translator.hpp"

#include <capstone/capstone.h>
#include <capstone/arm64.h>
#include <dlfcn.h>
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
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "guest_layout.h"

using AotRunFn = int (*)(GuestState *, const AevRuntime *);

namespace {

struct CaseInput {
    const char *name;
    int values[8];
    int n;
    int seed;
    bool unaligned;
    bool cross_page;
};

const CaseInput kCases[] = {
    {"prep_a", {11, -4, 7, 19, 0, -13, 21, 5}, 8, (int)0x12345678u, false, false},
    {"prep_b", {-9, 2, -3, 4, 5, -6, 7, 8}, 6, (int)0x01020304u, false, false},
    {"hidden", {100, -101, 102, -103, 104, -105, 106, -107}, 7, (int)0x0BADF00Du, false, false},
    {"unaligned", {13, -8, 21, 0, -5, 34, 55, -89}, 7, (int)0x55AA33CCu, true, false},
    {"crosspage", {17, -22, 31, -44, 53, -66, 77, -88}, 4, (int)0x0F1E2D3Cu, false, true},
};

const CaseInput *find_case(const std::string &name) {
    for (const auto &c : kCases)
        if (name == c.name)
            return &c;
    return nullptr;
}

std::string symbol_for_block(uint64_t rip) {
    char buf[64];
    snprintf(buf, sizeof(buf), "aev_blk_%llx", (unsigned long long)rip);
    return buf;
}

void print_llvm_error(const char *what, LLVMErrorRef err) {
    char *msg = LLVMGetErrorMessage(err);
    std::fprintf(stderr, "%s: %s\n", what, msg ? msg : "(unknown LLVM error)");
    if (msg)
        LLVMDisposeErrorMessage(msg);
}

struct JitCtx {
    LLVMOrcLLJITRef jit = nullptr;
    LLVMOrcJITDylibRef jd = nullptr;
    const aev::Program *program = nullptr;
    std::unordered_map<uint64_t, void *> cache;
    bool trace = false;
    bool dump_jit = false;
    bool require_jit = true;
    uint64_t compile_count = 0;
    uint64_t exec_count = 0;
    std::string data_layout;
    std::string triple;
};

void dump_arm64(const void *addr, size_t max_bytes) {
    csh handle;
    if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle) != CS_ERR_OK)
        return;
    cs_insn *insn = nullptr;
    size_t count = cs_disasm(handle, (const uint8_t *)addr, max_bytes, (uint64_t)(uintptr_t)addr, 0, &insn);
    for (size_t i = 0; i < count; ++i) {
        std::fprintf(stderr, "  JITCODE 0x%llx: %-10s %s\n",
                     (unsigned long long)insn[i].address, insn[i].mnemonic,
                     insn[i].op_str);
    }
    if (insn)
        cs_free(insn, count);
    cs_close(&handle);
}

bool register_absolute_symbols(JitCtx *ctx) {
    struct Sym {
        const char *name;
        void *addr;
    };
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
        print_llvm_error("LLVMOrcJITDylibDefine(absolute symbols)", err);
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
        if (pos == std::string::npos)
            return;
        size_t end = ir.find('\n', pos);
        if (end == std::string::npos)
            end = ir.size();
        ir.replace(pos, end - pos, replacement);
    };
    replace_line("target datalayout =", "target datalayout = \"" + ctx->data_layout + "\"");
    replace_line("target triple =", "target triple = \"" + ctx->triple + "\"");

    if (ctx->trace) {
        std::fprintf(stderr, "JIT IR-begin rip=0x%llx\n%sJIT IR-end rip=0x%llx\n",
                     (unsigned long long)rip, ir.c_str(), (unsigned long long)rip);
    }

    LLVMContextRef llvm_ctx = LLVMContextCreate();
    LLVMMemoryBufferRef buf = LLVMCreateMemoryBufferWithMemoryRangeCopy(
        ir.data(), ir.size(), "aev-jit-block");
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
    snprintf(func_name, sizeof(func_name), "aev_jit_%llx", (unsigned long long)rip);
    LLVMOrcExecutorAddress addr = 0;
    err = LLVMOrcLLJITLookup(ctx->jit, &addr, func_name);
    if (err) {
        print_llvm_error("LLVMOrcLLJITLookup", err);
        return false;
    }

    ctx->compile_count++;
    std::fprintf(stderr,
                 "JIT compile rip=0x%llx symbol=%s host_addr=0x%llx source=ORC(LLJIT) "
                 "block_jit_only=%d\n",
                 (unsigned long long)rip, func_name, (unsigned long long)addr,
                 block->jit_only ? 1 : 0);
    if (ctx->dump_jit)
        dump_arm64((const void *)(uintptr_t)addr, 64);

    *out_fn = (void *)(uintptr_t)addr;
    return true;
}

int64_t jit_dispatch(void *opaque, GuestState *s, uint64_t rip) {
    JitCtx *ctx = static_cast<JitCtx *>(opaque);
    if (rip == 0xDEAD0000ULL || rip == 0) {
        std::fprintf(stderr,
                     "JIT ERROR: unresolved guest indirect target rip=0x%llx; "
                     "no translated function or JIT block exists\n",
                     (unsigned long long)rip);
        s->error_code = AEV_ERR_BAD_TARGET;
        s->error_pc = rip;
        return -1;
    }

    auto it = ctx->cache.find(rip);
    void *fn = nullptr;
    if (it == ctx->cache.end()) {
        if (!jit_compile_block(ctx, rip, &fn))
            return -1;
        ctx->cache.emplace(rip, fn);
        s->jit_compiles++;
    } else {
        fn = it->second;
    }

    s->jit_execs++;
    ctx->exec_count++;
    if (ctx->trace || ctx->dump_jit) {
        std::fprintf(stderr,
                     "JIT exec rip=0x%llx host_addr=0x%llx rax_before=0x%llx "
                     "rsp_before=0x%llx\n",
                     (unsigned long long)rip, (unsigned long long)(uintptr_t)fn,
                     (unsigned long long)s->rax, (unsigned long long)s->rsp);
    }

    using JitFn = void (*)(GuestState *);
    JitFn jf = reinterpret_cast<JitFn>(fn);
    jf(s);
    s->jit_blocks++;

    if (ctx->trace || ctx->dump_jit) {
        std::fprintf(stderr,
                     "JIT return rip=0x%llx next_rip=0x%llx rax_after=0x%llx "
                     "error=%llu\n",
                     (unsigned long long)rip, (unsigned long long)s->rip,
                     (unsigned long long)s->rax,
                     (unsigned long long)s->error_code);
    }
    return s->error_code ? -1 : 0;
}

void usage(const char *argv0) {
    std::fprintf(stderr,
                 "usage: %s --aot <dylib> --input <obj> --function <name> "
                 "--function <name> --case <name> [--trace] [--dump-jit] "
                 "[--print-symbols] [--bad-op]\n",
                 argv0);
}

}  // namespace

int main(int argc, char **argv) {
    std::string aot_path, input_path, case_name;
    std::vector<std::string> function_names;
    bool trace = false, dump_jit = false, print_symbols = false, bad_op = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char *what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--aot")
            aot_path = need("--aot");
        else if (a == "--input")
            input_path = need("--input");
        else if (a == "--function")
            function_names.push_back(need("--function"));
        else if (a == "--case")
            case_name = need("--case");
        else if (a == "--trace")
            trace = true;
        else if (a == "--dump-jit")
            dump_jit = true;
        else if (a == "--print-symbols")
            print_symbols = true;
        else if (a == "--bad-op")
            bad_op = true;
        else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    if (aot_path.empty() || input_path.empty() || function_names.empty() ||
        case_name.empty()) {
        usage(argv[0]);
        return 2;
    }
    const CaseInput *test = find_case(case_name);
    if (!test) {
        std::fprintf(stderr, "unknown case: %s\n", case_name.c_str());
        return 2;
    }

    std::string error;
    aev::CoffObject object;
    if (!object.load(input_path, &error)) {
        std::fprintf(stderr, "coff load failed: %s\n", error.c_str());
        return 1;
    }
    std::vector<uint64_t> addresses;
    for (const auto &name : function_names) {
        if (name == "guest_kernel")
            addresses.push_back(AEV_ADDR_guest_kernel);
        else if (name == "guest_op")
            addresses.push_back(AEV_ADDR_guest_op);
        else {
            std::fprintf(stderr, "runner has no generated address for function %s\n",
                         name.c_str());
            return 1;
        }
    }
    aev::Program program;
    if (!aev::build_program(std::move(object), function_names, addresses, &program,
                            &error)) {
        std::fprintf(stderr, "program setup failed: %s\n", error.c_str());
        return 1;
    }

    void *handle = dlopen(aot_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    auto aot_run = reinterpret_cast<AotRunFn>(dlsym(handle, "aev_aot_run"));
    if (!aot_run) {
        std::fprintf(stderr, "dlsym(aev_aot_run) failed: %s\n", dlerror());
        return 1;
    }

    if (print_symbols) {
        Dl_info info;
        if (dladdr(reinterpret_cast<void *>(aot_run), &info))
            std::printf("aot_load_base=0x%llx aot_run_host=0x%llx dylib=%s\n",
                        (unsigned long long)(uintptr_t)info.dli_fbase,
                        (unsigned long long)(uintptr_t)aot_run,
                        info.dli_fname ? info.dli_fname : "(unknown)");
        for (const auto &fn : program.functions) {
            for (const auto &b : fn.blocks) {
                if (b.jit_only)
                    continue;
                std::string sym = symbol_for_block(b.address);
                void *p = dlsym(handle, sym.c_str());
                std::printf("AOT block guest_rip=0x%llx symbol=%s host_addr=0x%llx\n",
                            (unsigned long long)b.address, sym.c_str(),
                            (unsigned long long)(uintptr_t)p);
            }
        }
    }

    const long page = sysconf(_SC_PAGESIZE);
    const size_t mem_size = (size_t)page * 8;
    void *mem = mmap(nullptr, mem_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        std::perror("mmap");
        return 1;
    }
    uint8_t *base = static_cast<uint8_t *>(mem);
    uint64_t stack_top = ((uint64_t)(uintptr_t)(base + mem_size - 0x100)) & ~0xFULL;
    uint64_t entry_rsp = stack_top - 0x48; /* as if called: rsp % 16 == 8 */
    *reinterpret_cast<uint64_t *>((void *)(uintptr_t)entry_rsp) = AEV_RETURN_SENTINEL;

    int *arr = nullptr;
    if (test->unaligned)
        arr = reinterpret_cast<int *>(base + 1);
    else if (test->cross_page)
        arr = reinterpret_cast<int *>(base + page - 6);
    else
        arr = reinterpret_cast<int *>(base + 0x40);

    int initial[8];
    for (int i = 0; i < 8; ++i)
        initial[i] = test->values[i];
    std::memcpy(arr, initial, sizeof(initial));

    GuestState s;
    std::memset(&s, 0, sizeof(s));
    s.mem_base = (uint64_t)(uintptr_t)base;
    s.mem_size = mem_size;
    s.rsp = entry_rsp;
    s.rip = AEV_ENTRY_ADDR;
    s.rcx = (uint64_t)(uintptr_t)arr;
    s.rdx = (uint32_t)test->n;
    s.r8 = (uint32_t)test->seed;
    s.r9 = bad_op ? 0xDEAD0000ULL : (uint64_t)AEV_OP_ADDR;
    s.trace_enabled = trace ? 1 : 0;

    JitCtx jit;
    jit.program = &program;
    jit.trace = trace;
    jit.dump_jit = dump_jit;
    if (!jit_init(&jit)) {
        dlclose(handle);
        return 1;
    }

    AevRuntime rt;
    rt.jit_dispatch = jit_dispatch;
    rt.ctx = &jit;

    std::fprintf(stderr,
                 "RUN pid=%ld case=%s entry=0x%llx arr=0x%llx n=%d seed=0x%x bad_op=%d\n",
                 (long)getpid(), test->name, (unsigned long long)s.rip,
                 (unsigned long long)(uintptr_t)arr, test->n,
                 (unsigned)test->seed, bad_op ? 1 : 0);

    int rc = aot_run(&s, &rt);

    std::fprintf(stderr,
                 "STATS rc=%d aot_blocks=%llu jit_blocks=%llu jit_compiles=%llu "
                 "jit_execs=%llu jit_compile_events=%llu jit_exec_events=%llu "
                 "steps=%llu error=%llu error_pc=0x%llx\n",
                 rc, (unsigned long long)s.aot_blocks,
                 (unsigned long long)s.jit_blocks,
                 (unsigned long long)s.jit_compiles,
                 (unsigned long long)s.jit_execs,
                 (unsigned long long)jit.compile_count,
                 (unsigned long long)jit.exec_count,
                 (unsigned long long)s.steps,
                 (unsigned long long)s.error_code,
                 (unsigned long long)s.error_pc);

    if (rc == 0) {
        int32_t result = (int32_t)s.rax;
        std::printf("case=%s result=%d arr=", test->name, result);
        for (int i = 0; i < 8; ++i) {
            int v = 0;
            std::memcpy(&v, reinterpret_cast<const uint8_t *>(arr) + i * sizeof(int),
                        sizeof(v));
            std::printf("%s%d", i ? "," : "", v);
        }
        std::printf("\n");
    } else {
        std::printf("case=%s ERROR rc=%d error_code=%llu error_pc=0x%llx\n",
                    test->name, rc, (unsigned long long)s.error_code,
                    (unsigned long long)s.error_pc);
    }

    LLVMErrorRef dispose_err = LLVMOrcDisposeLLJIT(jit.jit);
    if (dispose_err) {
        print_llvm_error("LLVMOrcDisposeLLJIT", dispose_err);
    }
    dlclose(handle);
    return rc == 0 ? 0 : 3;
}
