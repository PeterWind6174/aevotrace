#include "aev_bridge.h"

#include "aev_guest.h"
#include "coff.hpp"
#include "jit_runtime.hpp"
#include "translator.hpp"

#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using AotRunFn = int (*)(GuestState *, const AevRuntime *);

struct HostImport {
    uint64_t id = 0;
    AevHostImportFn fn = nullptr;
    void *ctx = nullptr;
};

}  // namespace

struct AevBridge {
    std::string coff_path;
    std::string function_name;
    std::string aot_path;
    uint64_t entry_addr = 0;
    uint64_t input_hash = 0;

    aev::Program program;
    std::unique_ptr<aev::JitRuntime> jit;
    void *aot_handle = nullptr;
    AotRunFn aot_run = nullptr;

    uint64_t mem_base = 0;
    uint64_t mem_size = 0;
    uint64_t stack_base = 0;
    uint64_t stack_top = 0;
    uint64_t entry_rsp = 0;

    GuestState state{};
    std::map<uint64_t, HostImport> imports;
    std::map<uint64_t, uint64_t> import_ids;

    std::thread::id owner;
    bool require_main_thread = false;
    bool in_call = false;
    uint64_t call_index = 0;
    uint64_t total_calls = 0;
    uint64_t total_aot_blocks = 0;
    uint64_t total_jit_blocks = 0;
    uint64_t total_jit_compiles = 0;
    uint64_t total_jit_execs = 0;

    AevBridgeStatus last_failure = AEV_BRIDGE_OK;
    uint64_t last_import_id = 0;
    uint32_t test_fault_mode = 0;
    uint64_t test_fault_rip = 0;
    bool trace = false;
    bool dump_jit = false;
    bool trace_aot = false;
};

namespace {

void set_error(char *err, size_t cap, const std::string &msg) {
    if (!err || cap == 0)
        return;
    size_t n = msg.size();
    if (n >= cap)
        n = cap - 1;
    std::memcpy(err, msg.data(), n);
    err[n] = '\0';
}

bool in_import_range(uint64_t addr) {
    return addr >= AEV_BRIDGE_IMPORT_BASE && addr < AEV_BRIDGE_IMPORT_LIMIT;
}

bool range_ok(const AevBridge *b, uint64_t addr, uint64_t len) {
    if (!b || len == 0)
        return false;
    if (addr < b->mem_base)
        return false;
    uint64_t off = addr - b->mem_base;
    if (off > b->mem_size)
        return false;
    return len <= b->mem_size - off;
}

void destroy_resources(AevBridge *b) {
    if (!b)
        return;
    b->jit.reset();
    if (b->aot_handle) {
        dlclose(b->aot_handle);
        b->aot_handle = nullptr;
    }
    if (b->mem_base && b->mem_size) {
        munmap(reinterpret_cast<void *>(static_cast<uintptr_t>(b->mem_base)),
               static_cast<size_t>(b->mem_size));
        b->mem_base = 0;
        b->mem_size = 0;
    }
}

AevBridgeStatus map_guest_error(const AevBridge *b, int rc) {
    if (b->last_failure != AEV_BRIDGE_OK)
        return b->last_failure;
    if (rc == 0) {
        if (b->state.rip == AEV_GUEST_SENTINEL) {
            if (b->state.rsp == b->entry_rsp + 8)
                return AEV_BRIDGE_OK;
            return AEV_BRIDGE_ERR_INTERNAL;
        }
        return AEV_BRIDGE_ERR_INTERNAL;
    }
    switch (b->state.error_code) {
        case AEV_ERR_MEMORY:
            return AEV_BRIDGE_ERR_MEMORY;
        case AEV_ERR_STEP_LIMIT:
            return AEV_BRIDGE_ERR_STEP_LIMIT;
        case AEV_ERR_JIT:
            return AEV_BRIDGE_ERR_JIT;
        case AEV_ERR_NO_BLOCK:
        case AEV_ERR_BAD_TARGET:
            return AEV_BRIDGE_ERR_NO_BLOCK;
        default:
            return AEV_BRIDGE_ERR_INTERNAL;
    }
}

int64_t bridge_jit_dispatch(void *opaque, GuestState *s, uint64_t rip) {
    AevBridge *b = static_cast<AevBridge *>(opaque);

    if (in_import_range(rip)) {
        auto it = b->imports.find(rip);
        if (it == b->imports.end()) {
            b->last_failure = AEV_BRIDGE_ERR_UNKNOWN_IMPORT;
            s->error_code = AEV_ERR_BAD_TARGET;
            s->error_pc = rip;
            return -1;
        }
        const HostImport &imp = it->second;
        uint64_t ret = aev_load(s, s->rsp, 8, 0);
        if (s->error_code) {
            b->last_failure = AEV_BRIDGE_ERR_MEMORY;
            return -1;
        }
        s->rsp += 8;
        s->rip = ret;

        uint64_t out_rax = 0;
        AevBridgeStatus st = AEV_BRIDGE_ERR_HOST;
        try {
            st = imp.fn(imp.ctx, b, s->rcx, s->rdx, s->r8, s->r9, &out_rax);
        } catch (...) {
            st = AEV_BRIDGE_ERR_HOST;
        }
        if (st != AEV_BRIDGE_OK) {
            b->last_failure =
                (st == AEV_BRIDGE_OK || st > AEV_BRIDGE_ERR_INTERNAL)
                    ? AEV_BRIDGE_ERR_HOST
                    : st;
            s->error_pc = rip;
            if (!s->error_code)
                s->error_code = AEV_ERR_INTERNAL;
            return -1;
        }
        s->rax = out_rax;
        b->last_import_id = imp.id;
        return 0;
    }

    const aev::GuestFunction *fn = b->program.find_function_at(rip);
    if (!fn) {
        b->last_failure = AEV_BRIDGE_ERR_NO_BLOCK;
        s->error_code = AEV_ERR_BAD_TARGET;
        s->error_pc = rip;
        return -1;
    }
    const aev::Block *block = b->program.find_block(rip);
    if (!block) {
        b->last_failure = AEV_BRIDGE_ERR_NO_BLOCK;
        s->error_code = AEV_ERR_BAD_TARGET;
        s->error_pc = rip;
        return -1;
    }
    if (!block->jit_only) {
        b->last_failure = AEV_BRIDGE_ERR_JIT;
        s->error_code = AEV_ERR_JIT;
        s->error_pc = rip;
        return -1;
    }
    if (b->test_fault_mode == 1 && rip == b->test_fault_rip) {
        b->last_failure = AEV_BRIDGE_ERR_JIT;
        s->error_code = AEV_ERR_JIT;
        s->error_pc = rip;
        return -1;
    }

    int64_t rc = b->jit->dispatch(s, rip);
    if (rc != 0) {
        if (!s->error_code) {
            b->last_failure = AEV_BRIDGE_ERR_JIT;
            s->error_code = AEV_ERR_JIT;
            s->error_pc = rip;
        }
    }
    return rc;
}

}  // namespace

extern "C" {

int aev_bridge_abi_info(uint32_t *abi_version, uint32_t *guest_state_size,
                        uint32_t *call_result_size, uint32_t *config_size) {
    if (!abi_version || !guest_state_size || !call_result_size || !config_size)
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    *abi_version = AEV_CALLBACK_ABI_VERSION;
    *guest_state_size = (uint32_t)sizeof(GuestState);
    *call_result_size = (uint32_t)sizeof(AevCallResult);
    *config_size = (uint32_t)sizeof(AevBridgeConfig);
    return AEV_BRIDGE_OK;
}

AevBridgeStatus aev_bridge_create(const AevBridgeConfig *cfg,
                                  AevBridge **out_bridge,
                                  char *err, size_t err_cap) {
    if (!out_bridge)
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    *out_bridge = nullptr;
    if (!cfg || cfg->struct_size != sizeof(AevBridgeConfig) ||
        cfg->abi_version != AEV_CALLBACK_ABI_VERSION || !cfg->coff_path ||
        !cfg->function_name || !cfg->aot_dylib_path ||
        cfg->guest_stack_size < 64ULL * 1024ULL ||
        cfg->guest_mem_size < cfg->guest_stack_size + 64ULL * 1024ULL ||
        in_import_range(cfg->entry_addr)) {
        set_error(err, err_cap, "invalid bridge configuration");
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }
    if (cfg->require_main_thread && !pthread_main_np()) {
        set_error(err, err_cap, "bridge must be created on the process main thread");
        return AEV_BRIDGE_ERR_WRONG_THREAD;
    }

    std::unique_ptr<AevBridge, void (*)(AevBridge *)> guard(
        new AevBridge(), [](AevBridge *b) {
            destroy_resources(b);
            delete b;
        });
    AevBridge *b = guard.get();
    b->coff_path = cfg->coff_path;
    b->function_name = cfg->function_name;
    b->aot_path = cfg->aot_dylib_path;
    b->entry_addr = cfg->entry_addr;
    b->trace = cfg->trace != 0;
    b->dump_jit = cfg->dump_jit != 0;
    b->trace_aot = cfg->trace_aot != 0;
    b->require_main_thread = cfg->require_main_thread != 0;

    std::string load_error;
    aev::CoffObject object;
    if (!object.load(b->coff_path, &load_error)) {
        set_error(err, err_cap, "COFF load failed: " + load_error);
        return AEV_BRIDGE_ERR_LOAD;
    }
    b->input_hash = object.content_hash;

    std::vector<std::string> names{b->function_name};
    std::vector<uint64_t> addresses{b->entry_addr};
    if (!aev::build_program(std::move(object), names, addresses, &b->program,
                            &load_error)) {
        set_error(err, err_cap, "program setup failed: " + load_error);
        return AEV_BRIDGE_ERR_LOAD;
    }
    if (!b->program.find_block(b->entry_addr)) {
        set_error(err, err_cap, "entry address is not a translated basic block");
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }

    b->aot_handle = dlopen(b->aot_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!b->aot_handle) {
        set_error(err, err_cap, std::string("dlopen failed: ") + dlerror());
        return AEV_BRIDGE_ERR_LOAD;
    }
    b->aot_run = reinterpret_cast<AotRunFn>(dlsym(b->aot_handle, "aev_aot_run"));
    const uint64_t *aot_abi =
        static_cast<const uint64_t *>(dlsym(b->aot_handle, "aev_aot_abi_version"));
    const uint64_t *aot_guest_size =
        static_cast<const uint64_t *>(dlsym(b->aot_handle, "aev_guest_state_size"));
    const uint64_t *aot_input_hash =
        static_cast<const uint64_t *>(dlsym(b->aot_handle, "aev_input_hash"));
    const AevBlock *aot_blocks =
        static_cast<const AevBlock *>(dlsym(b->aot_handle, "aev_blocks"));
    const uint64_t *aot_block_count =
        static_cast<const uint64_t *>(dlsym(b->aot_handle, "aev_block_count"));
    if (!b->aot_run || !aot_abi || !aot_guest_size || !aot_input_hash ||
        !aot_blocks || !aot_block_count) {
        set_error(err, err_cap, "AOT dylib is missing required ABI/identity symbols");
        return AEV_BRIDGE_ERR_ABI_MISMATCH;
    }
    if (*aot_abi != AEV_GUEST_ABI_VERSION ||
        *aot_guest_size != (uint64_t)sizeof(GuestState)) {
        set_error(err, err_cap, "AOT dylib ABI/guest-state size mismatch");
        return AEV_BRIDGE_ERR_ABI_MISMATCH;
    }
    if (*aot_input_hash != b->input_hash) {
        set_error(err, err_cap, "AOT dylib input hash does not match COFF bytes");
        return AEV_BRIDGE_ERR_ABI_MISMATCH;
    }

    std::vector<uint64_t> expected_blocks;
    for (const auto &fn : b->program.functions)
        for (const auto &block : fn.blocks)
            if (!block.jit_only)
                expected_blocks.push_back(block.address);
    std::sort(expected_blocks.begin(), expected_blocks.end());
    if (*aot_block_count != expected_blocks.size()) {
        set_error(err, err_cap, "AOT block count does not match translated program");
        return AEV_BRIDGE_ERR_ABI_MISMATCH;
    }
    for (size_t i = 0; i < expected_blocks.size(); ++i) {
        if (!aot_blocks[i].fn || aot_blocks[i].rip != expected_blocks[i]) {
            set_error(err, err_cap, "AOT block table does not match translated program");
            return AEV_BRIDGE_ERR_ABI_MISMATCH;
        }
    }

    const long page = sysconf(_SC_PAGESIZE);
    const uint64_t page_size = page > 0 ? (uint64_t)page : 4096ULL;
    uint64_t mem_size = cfg->guest_mem_size;
    mem_size = (mem_size + page_size - 1) & ~(page_size - 1);
    void *mem = mmap(nullptr, static_cast<size_t>(mem_size),
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        set_error(err, err_cap, "guest mmap failed");
        return AEV_BRIDGE_ERR_LOAD;
    }
    b->mem_base = (uint64_t)(uintptr_t)mem;
    b->mem_size = mem_size;
    b->stack_top = b->mem_base + b->mem_size;
    b->stack_base = b->stack_top - cfg->guest_stack_size;
    b->entry_rsp = b->stack_top - 0x108;
    if (b->entry_rsp < b->stack_base + 0x100 ||
        b->entry_rsp + 0x30 > b->stack_top) {
        set_error(err, err_cap, "guest stack is too small for the entry frame");
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }

    b->jit = std::make_unique<aev::JitRuntime>(&b->program);
    b->jit->set_trace(b->trace);
    b->jit->set_dump_jit(b->dump_jit);
    std::string jit_error;
    if (!b->jit->init(&jit_error)) {
        set_error(err, err_cap, "JIT init failed: " + jit_error);
        return AEV_BRIDGE_ERR_LOAD;
    }

    b->owner = std::this_thread::get_id();
    *out_bridge = guard.release();
    return AEV_BRIDGE_OK;
}

AevBridgeStatus aev_bridge_destroy(AevBridge *b) {
    if (!b)
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    if (b->in_call)
        return AEV_BRIDGE_ERR_REENTRY;
    destroy_resources(b);
    delete b;
    return AEV_BRIDGE_OK;
}

int aev_bridge_guest_info(const AevBridge *b, uint64_t *base, uint64_t *size,
                          uint64_t *stack_base, uint64_t *stack_top) {
    if (!b || !base || !size || !stack_base || !stack_top)
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    *base = b->mem_base;
    *size = b->mem_size;
    *stack_base = b->stack_base;
    *stack_top = b->stack_top;
    return AEV_BRIDGE_OK;
}

int aev_bridge_register_import(AevBridge *b, uint64_t guest_addr, uint64_t import_id,
                               AevHostImportFn fn, void *host_ctx,
                               char *err, size_t err_cap) {
    if (!b || !fn || import_id == 0 ||
        guest_addr != AEV_BRIDGE_IMPORT_BASE + import_id * AEV_BRIDGE_IMPORT_STRIDE ||
        !in_import_range(guest_addr)) {
        set_error(err, err_cap, "invalid host import address/id");
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }
    if (b->in_call) {
        set_error(err, err_cap, "cannot register an import during a guest call");
        return AEV_BRIDGE_ERR_REENTRY;
    }
    if (b->imports.count(guest_addr) || b->import_ids.count(import_id)) {
        set_error(err, err_cap, "duplicate host import address or id");
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }
    if (guest_addr >= b->mem_base && guest_addr < b->mem_base + b->mem_size) {
        set_error(err, err_cap, "host import address overlaps guest memory");
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }
    b->imports.emplace(guest_addr, HostImport{import_id, fn, host_ctx});
    b->import_ids.emplace(import_id, guest_addr);
    return AEV_BRIDGE_OK;
}

int aev_bridge_guest_write(AevBridge *b, uint64_t guest_addr, const void *src,
                           uint64_t len, char *err, size_t err_cap) {
    if (!b || !src || !range_ok(b, guest_addr, len)) {
        set_error(err, err_cap, "guest write outside checked memory");
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }
    std::memcpy(reinterpret_cast<void *>(static_cast<uintptr_t>(guest_addr)),
                src, static_cast<size_t>(len));
    return AEV_BRIDGE_OK;
}

int aev_bridge_guest_read(AevBridge *b, uint64_t guest_addr, void *dst,
                          uint64_t len, char *err, size_t err_cap) {
    if (!b || !dst || !range_ok(b, guest_addr, len)) {
        set_error(err, err_cap, "guest read outside checked memory");
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }
    std::memcpy(dst, reinterpret_cast<const void *>(static_cast<uintptr_t>(guest_addr)),
                static_cast<size_t>(len));
    return AEV_BRIDGE_OK;
}

int aev_bridge_call(AevBridge *b, uint64_t entry_addr,
                    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                    AevCallResult *result, char *err, size_t err_cap) {
    if (!b || !result || result->struct_size != sizeof(AevCallResult) ||
        result->abi_version != AEV_CALLBACK_ABI_VERSION ||
        entry_addr != b->entry_addr) {
        set_error(err, err_cap, "invalid bridge call arguments/result struct");
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }
    const uint32_t result_size = result->struct_size;
    const uint32_t result_abi = result->abi_version;
    std::memset(result, 0, sizeof(*result));
    result->struct_size = result_size;
    result->abi_version = result_abi;

    if (std::this_thread::get_id() != b->owner ||
        (b->require_main_thread && !pthread_main_np())) {
        set_error(err, err_cap, "guest call must run on the bridge owner/main thread");
        return AEV_BRIDGE_ERR_WRONG_THREAD;
    }
    if (b->in_call) {
        set_error(err, err_cap, "nested guest call is not supported");
        return AEV_BRIDGE_ERR_REENTRY;
    }

    b->in_call = true;
    b->last_failure = AEV_BRIDGE_OK;
    b->last_import_id = 0;
    b->call_index++;
    std::memset(&b->state, 0, sizeof(b->state));
    b->state.mem_base = b->mem_base;
    b->state.mem_size = b->mem_size;
    b->state.rsp = b->entry_rsp;
    b->state.rip = b->entry_addr;
    b->state.rcx = a0;
    b->state.rdx = a1;
    b->state.r8 = a2;
    b->state.r9 = a3;
    b->state.trace_enabled = b->trace_aot ? 1 : 0;
    const uint64_t sentinel = AEV_GUEST_SENTINEL;
    std::memcpy(reinterpret_cast<void *>(static_cast<uintptr_t>(b->entry_rsp)),
                &sentinel, sizeof(sentinel));

    AevBridgeStatus status = AEV_BRIDGE_ERR_INTERNAL;
    try {
        AevRuntime rt;
        rt.jit_dispatch = bridge_jit_dispatch;
        rt.ctx = b;
        int rc = b->aot_run(&b->state, &rt);
        if (b->test_fault_mode == 2 && rc == 0)
            b->state.rsp += 8; /* test-only sentinel/RSP mismatch */
        status = map_guest_error(b, rc);
    } catch (...) {
        status = AEV_BRIDGE_ERR_HOST;
        b->last_failure = AEV_BRIDGE_ERR_HOST;
        if (!b->state.error_pc)
            b->state.error_pc = b->state.rip;
    }

    result->status = (uint32_t)status;
    result->guest_error = (uint32_t)b->state.error_code;
    result->guest_rax = (status == AEV_BRIDGE_OK) ? b->state.rax : 0;
    result->guest_rip = b->state.rip;
    result->guest_rsp = b->state.rsp;
    result->error_pc = b->state.error_pc;
    result->steps = b->state.steps;
    result->aot_blocks = b->state.aot_blocks;
    result->jit_blocks = b->state.jit_blocks;
    result->jit_compiles = b->state.jit_compiles;
    result->jit_execs = b->state.jit_execs;
    result->host_import_id = b->last_import_id;
    result->call_index = b->call_index;
    result->guest_mem_base = b->mem_base;
    result->guest_mem_size = b->mem_size;
    result->total_calls = b->call_index;
    result->total_aot_blocks = b->total_aot_blocks + b->state.aot_blocks;
    result->total_jit_blocks = b->total_jit_blocks + b->state.jit_blocks;
    result->total_jit_compiles = b->total_jit_compiles + b->state.jit_compiles;
    result->total_jit_execs = b->total_jit_execs + b->state.jit_execs;

    b->total_calls = b->call_index;
    b->total_aot_blocks += b->state.aot_blocks;
    b->total_jit_blocks += b->state.jit_blocks;
    b->total_jit_compiles += b->state.jit_compiles;
    b->total_jit_execs += b->state.jit_execs;
    b->in_call = false;

    if (status != AEV_BRIDGE_OK) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "guest call failed: status=%u guest_error=%llu error_pc=0x%llx",
                      (unsigned)status, (unsigned long long)b->state.error_code,
                      (unsigned long long)b->state.error_pc);
        set_error(err, err_cap, msg);
    }
    return (int)status;
}

int aev_bridge_set_test_fault(AevBridge *b, uint32_t mode, uint64_t rip,
                              char *err, size_t err_cap) {
    if (!b || mode > 2) {
        set_error(err, err_cap, "invalid test fault mode");
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }
    if (b->in_call) {
        set_error(err, err_cap, "cannot set a test fault during a guest call");
        return AEV_BRIDGE_ERR_REENTRY;
    }
    b->test_fault_mode = mode;
    b->test_fault_rip = rip;
    return AEV_BRIDGE_OK;
}

}  // extern "C"
