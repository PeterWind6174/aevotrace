#include "aev_guest.h"
#include "ir_emitter.hpp"
#include "jit_runtime.hpp"
#include "translator.hpp"

#include <dlfcn.h>
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

int64_t runner_jit_dispatch(void *opaque, GuestState *s, uint64_t rip) {
    if (rip == 0xDEAD0000ULL || rip == 0) {
        std::fprintf(stderr,
                     "JIT ERROR: unresolved guest indirect target rip=0x%llx; "
                     "no translated function or JIT block exists\n",
                     (unsigned long long)rip);
        s->error_code = AEV_ERR_BAD_TARGET;
        s->error_pc = rip;
        return -1;
    }
    return static_cast<aev::JitRuntime *>(opaque)->dispatch(s, rip);
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

    aev::JitRuntime jit(&program);
    jit.set_trace(trace);
    jit.set_dump_jit(dump_jit);
    std::string jit_error;
    if (!jit.init(&jit_error)) {
        std::fprintf(stderr, "JIT init failed: %s\n", jit_error.c_str());
        dlclose(handle);
        return 1;
    }

    AevRuntime rt;
    rt.jit_dispatch = runner_jit_dispatch;
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
                 (unsigned long long)jit.compile_count(),
                 (unsigned long long)jit.exec_count(),
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

    dlclose(handle);
    return rc == 0 ? 0 : 3;
}
