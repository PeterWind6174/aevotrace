#include "aev_bridge.h"

#include "aev_guest.h"
#include "aev_artifact.h"
#include "coff.hpp"
#include "jit_runtime.hpp"
#include "program_identity.hpp"
#include "sha256.hpp"
#include "translator.hpp"

#include <dlfcn.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
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
    uint8_t input_sha256[32] = {};
    uint8_t aot_sha256[32] = {};
    std::vector<uint64_t> all_blocks;
    std::vector<uint64_t> aot_selected;
    uint32_t aot_table_count = 0;
    std::string target_triple;

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
    bool trace_open = false;
    std::ofstream trace_stream;
    std::string trace_scenario;
    AevTraceSink trace_sink{};
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

std::string json_escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

void trace_array(std::ostream &os, const std::vector<uint64_t> &values) {
    os << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) os << ",";
        os << "\"" << aev::hex64(values[i]) << "\"";
    }
    os << "]";
}

void trace_block(AevBridge *b, uint32_t source, uint64_t rip) {
    if (!b->trace_open)
        return;
    aev::BlockIdentity id;
    std::string error;
    const char *kind = source == AEV_TRACE_SINK_SOURCE_AOT
                           ? "aot"
                           : (source == AEV_TRACE_SINK_SOURCE_JIT_COMPILE
                                  ? "jit_compile"
                                  : "jit_exec");
    b->trace_stream << "{\"record\":\"" << kind << "\",\"call\":"
                    << b->call_index;
    if (!aev::block_identity(b->program, rip, &id, &error)) {
        b->trace_stream << ",\"rip\":\"" << aev::hex64(rip)
                        << "\",\"error\":\"" << json_escape(error) << "\"}\n";
    } else {
        b->trace_stream << ",\"rip\":\"" << aev::hex64(rip)
                        << "\",\"offset\":" << id.section_offset
                        << ",\"size\":" << id.size
                        << ",\"block_sha256\":\""
                        << aev::hex_bytes(id.sha256, 32) << "\"}\n";
    }
}

void trace_host_import(AevBridge *b, uint64_t import_id) {
    if (!b->trace_open)
        return;
    b->trace_stream << "{\"record\":\"host_import\",\"call\":"
                    << b->call_index << ",\"id\":" << import_id << "}\n";
}

void trace_dispatch_miss(AevBridge *b, uint64_t rip, AevBridgeStatus status) {
    if (!b->trace_open)
        return;
    b->trace_stream << "{\"record\":\"dispatch_miss\",\"call\":"
                    << b->call_index << ",\"rip\":\"" << aev::hex64(rip)
                    << "\",\"status\":" << (unsigned)status << "}\n";
}

void trace_call_end(AevBridge *b, AevBridgeStatus status) {
    if (!b->trace_open)
        return;
    b->trace_stream << "{\"record\":\"call_end\",\"call\":" << b->call_index
                    << ",\"status\":" << (unsigned)status
                    << ",\"error_pc\":\"" << aev::hex64(b->state.error_pc)
                    << "\",\"guest_error\":" << b->state.error_code << "}\n";
}

extern "C" void bridge_trace_on_block(void *ctx, uint32_t source, uint64_t rip) {
    trace_block(static_cast<AevBridge *>(ctx), source, rip);
}

void bridge_jit_trace(void *ctx, uint32_t source, uint64_t rip) {
    trace_block(static_cast<AevBridge *>(ctx), source, rip);
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
        case AEV_ERR_HOST:
            return AEV_BRIDGE_ERR_HOST;
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
            trace_dispatch_miss(b, rip, AEV_BRIDGE_ERR_UNKNOWN_IMPORT);
            return -1;
        }
        const HostImport &imp = it->second;
        b->last_import_id = imp.id;
        uint64_t ret = aev_load(s, s->rsp, 8, 0);
        if (s->error_code) {
            b->last_failure = AEV_BRIDGE_ERR_MEMORY;
            return -1;
        }
        s->rsp += 8;
        trace_host_import(b, imp.id);

        uint64_t out_rax = 0;
        AevBridgeStatus st = AEV_BRIDGE_ERR_HOST;
        try {
            st = imp.fn(imp.ctx, b, s->rcx, s->rdx, s->r8, s->r9, &out_rax);
        } catch (...) {
            st = AEV_BRIDGE_ERR_HOST;
        }
        if (st < AEV_BRIDGE_OK || st > AEV_BRIDGE_ERR_INTERNAL)
            st = AEV_BRIDGE_ERR_HOST;
        if (st != AEV_BRIDGE_OK) {
            b->last_failure = st;
            s->error_pc = rip;
            if (!s->error_code)
                s->error_code = AEV_ERR_HOST;
            return -1;
        }
        s->rip = ret;
        s->rax = out_rax;
        return 0;
    }

    const aev::GuestFunction *fn = b->program.find_function_at(rip);
    if (!fn) {
        b->last_failure = AEV_BRIDGE_ERR_NO_BLOCK;
        s->error_code = AEV_ERR_BAD_TARGET;
        s->error_pc = rip;
        trace_dispatch_miss(b, rip, AEV_BRIDGE_ERR_NO_BLOCK);
        return -1;
    }
    const aev::Block *block = b->program.find_block(rip);
    if (!block) {
        b->last_failure = AEV_BRIDGE_ERR_NO_BLOCK;
        s->error_code = AEV_ERR_BAD_TARGET;
        s->error_pc = rip;
        trace_dispatch_miss(b, rip, AEV_BRIDGE_ERR_NO_BLOCK);
        return -1;
    }
    if (!block->jit_only) {
        b->last_failure = AEV_BRIDGE_ERR_JIT;
        s->error_code = AEV_ERR_JIT;
        s->error_pc = rip;
        trace_dispatch_miss(b, rip, AEV_BRIDGE_ERR_JIT);
        return -1;
    }
    if (b->test_fault_mode == 1 && rip == b->test_fault_rip) {
        b->last_failure = AEV_BRIDGE_ERR_JIT;
        s->error_code = AEV_ERR_JIT;
        s->error_pc = rip;
        trace_dispatch_miss(b, rip, AEV_BRIDGE_ERR_JIT);
        return -1;
    }

    int64_t rc = b->jit->dispatch(s, rip);
    if (rc != 0) {
        if (!s->error_code) {
            b->last_failure = AEV_BRIDGE_ERR_JIT;
            s->error_code = AEV_ERR_JIT;
            s->error_pc = rip;
        }
        trace_dispatch_miss(b, rip, AEV_BRIDGE_ERR_JIT);
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
    std::memcpy(b->input_sha256, object.content_sha256, sizeof(b->input_sha256));

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
    std::string aot_hash_error;
    if (!aev::sha256_file(b->aot_path, b->aot_sha256, &aot_hash_error)) {
        set_error(err, err_cap, "AOT SHA-256 failed: " + aot_hash_error);
        return AEV_BRIDGE_ERR_LOAD;
    }
    b->aot_run = reinterpret_cast<AotRunFn>(dlsym(b->aot_handle, "aev_aot_run"));
    const uint64_t *aot_abi =
        static_cast<const uint64_t *>(dlsym(b->aot_handle, "aev_aot_abi_version"));
    const uint64_t *aot_guest_size =
        static_cast<const uint64_t *>(dlsym(b->aot_handle, "aev_guest_state_size"));
    const uint64_t *aot_input_hash =
        static_cast<const uint64_t *>(dlsym(b->aot_handle, "aev_input_hash"));
    const uint8_t *aot_input_sha =
        static_cast<const uint8_t *>(dlsym(b->aot_handle, "aev_input_sha256"));
    const uint64_t *aot_artifact_abi =
        static_cast<const uint64_t *>(dlsym(b->aot_handle, "aev_artifact_abi_version"));
    const uint64_t *aot_translator_abi =
        static_cast<const uint64_t *>(dlsym(b->aot_handle, "aev_translator_abi_version"));
    const uint64_t *aot_entry_addr =
        static_cast<const uint64_t *>(dlsym(b->aot_handle, "aev_entry_addr"));
    const uint8_t *aot_function_sha =
        static_cast<const uint8_t *>(dlsym(b->aot_handle, "aev_function_name_sha256"));
    const char *aot_target =
        static_cast<const char *>(dlsym(b->aot_handle, "aev_target_triple"));
    const char *aot_module_kind =
        static_cast<const char *>(dlsym(b->aot_handle, "aev_module_kind"));
    const AevBlock *aot_blocks =
        static_cast<const AevBlock *>(dlsym(b->aot_handle, "aev_blocks"));
    const uint64_t *aot_block_count =
        static_cast<const uint64_t *>(dlsym(b->aot_handle, "aev_block_count"));
    const uint64_t *aot_all_count =
        static_cast<const uint64_t *>(dlsym(b->aot_handle, "aev_all_block_count"));
    const uint64_t *aot_all_blocks =
        static_cast<const uint64_t *>(dlsym(b->aot_handle, "aev_all_blocks"));
    if (!b->aot_run || !aot_abi || !aot_guest_size || !aot_input_hash ||
        !aot_input_sha || !aot_artifact_abi || !aot_translator_abi ||
        !aot_entry_addr || !aot_function_sha || !aot_target || !aot_blocks ||
        !aot_block_count || !aot_all_count || !aot_all_blocks || !aot_module_kind) {
        set_error(err, err_cap, "AOT dylib is missing required ABI/identity symbols");
        return AEV_BRIDGE_ERR_ABI_MISMATCH;
    }
    if (std::string(aot_module_kind) != AEV_MODULE_KIND_COFF) {
        set_error(err, err_cap, std::string("AOT module kind is not COFF: ") + aot_module_kind);
        return AEV_BRIDGE_ERR_ABI_MISMATCH;
    }
    if (*aot_abi != AEV_GUEST_ABI_VERSION ||
        *aot_guest_size != (uint64_t)sizeof(GuestState) ||
        *aot_artifact_abi != AEV_ARTIFACT_ABI_VERSION ||
        *aot_translator_abi != AEV_TRANSLATOR_ABI_VERSION) {
        set_error(err, err_cap, "AOT dylib ABI/guest-state/artifact/translator mismatch");
        return AEV_BRIDGE_ERR_ABI_MISMATCH;
    }
    if (*aot_input_hash != b->input_hash ||
        std::memcmp(aot_input_sha, b->input_sha256, 32) != 0) {
        set_error(err, err_cap, "AOT dylib input identity does not match COFF bytes");
        return AEV_BRIDGE_ERR_ABI_MISMATCH;
    }
    if (*aot_entry_addr != b->entry_addr) {
        set_error(err, err_cap, "AOT dylib entry address mismatch");
        return AEV_BRIDGE_ERR_ABI_MISMATCH;
    }
    uint8_t expected_function_sha[32];
    aev::sha256(b->function_name.data(), b->function_name.size(), expected_function_sha);
    if (std::memcmp(aot_function_sha, expected_function_sha, 32) != 0) {
        set_error(err, err_cap, "AOT dylib function-name identity mismatch");
        return AEV_BRIDGE_ERR_ABI_MISMATCH;
    }
    if (std::string(aot_target) != AEV_AOT_TARGET_TRIPLE) {
        set_error(err, err_cap, std::string("AOT target mismatch: ") + aot_target);
        return AEV_BRIDGE_ERR_ABI_MISMATCH;
    }
    b->target_triple = aot_target;

    std::vector<uint64_t> expected_all;
    for (const auto &fn : b->program.functions)
        for (const auto &block : fn.blocks)
            expected_all.push_back(block.address);
    std::sort(expected_all.begin(), expected_all.end());
    if (*aot_all_count != expected_all.size()) {
        set_error(err, err_cap, "AOT all-block count does not match translated program");
        return AEV_BRIDGE_ERR_ABI_MISMATCH;
    }
    for (size_t i = 0; i < expected_all.size(); ++i) {
        if (aot_all_blocks[i] != expected_all[i]) {
            set_error(err, err_cap, "AOT all-block table does not match translated program");
            return AEV_BRIDGE_ERR_ABI_MISMATCH;
        }
    }
    b->all_blocks = expected_all;
    b->aot_selected.clear();
    if (*aot_block_count > expected_all.size() ||
        *aot_block_count > *aot_all_count) {
        set_error(err, err_cap, "AOT selected block count exceeds all-block table");
        return AEV_BRIDGE_ERR_ABI_MISMATCH;
    }
    std::set<uint64_t> selected_set;
    for (uint64_t i = 0; i < *aot_block_count; ++i) {
        uint64_t rip = aot_blocks[i].rip;
        if (!aot_blocks[i].fn ||
            !std::binary_search(expected_all.begin(), expected_all.end(), rip) ||
            (i > 0 && aot_blocks[i - 1].rip >= rip) ||
            !selected_set.insert(rip).second) {
            set_error(err, err_cap, "AOT selected block table is invalid or unsorted");
            return AEV_BRIDGE_ERR_ABI_MISMATCH;
        }
        char symbol[64];
        std::snprintf(symbol, sizeof(symbol), "aev_blk_%llx",
                      (unsigned long long)rip);
        void *expected_fn = dlsym(b->aot_handle, symbol);
        if (!expected_fn ||
            reinterpret_cast<void *>(aot_blocks[i].fn) != expected_fn) {
            set_error(err, err_cap, "AOT selected block fn/table mismatch");
            return AEV_BRIDGE_ERR_ABI_MISMATCH;
        }
        b->aot_selected.push_back(rip);
    }
    b->aot_table_count = (uint32_t)*aot_block_count;
    for (auto &fn : b->program.functions) {
        for (auto &block : fn.blocks) {
            if (!selected_set.count(block.address)) {
                block.jit_only = true;
                block.jit_reason = "not selected in AOT coverage";
            } else {
                block.jit_only = false;
                block.jit_reason.clear();
            }
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
    if (b->trace_open)
        (void)aev_bridge_trace_close(b);
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

AevBridgeStatus aev_bridge_artifact_info(const AevBridge *b, AevArtifactInfo *info,
                                         char *err, size_t err_cap) {
    if (!b || !info || info->struct_size != sizeof(AevArtifactInfo) ||
        info->abi_version != AEV_ARTIFACT_INFO_ABI_VERSION) {
        set_error(err, err_cap, "invalid artifact info struct");
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }
    std::memset(info, 0, sizeof(*info));
    info->struct_size = sizeof(AevArtifactInfo);
    info->abi_version = AEV_ARTIFACT_INFO_ABI_VERSION;
    info->aot_table_count = b->aot_table_count;
    info->all_block_count = (uint32_t)b->all_blocks.size();
    info->artifact_abi_version = AEV_ARTIFACT_ABI_VERSION;
    info->translator_abi_version = AEV_TRANSLATOR_ABI_VERSION;
    info->entry_addr = b->entry_addr;
    std::memcpy(info->input_sha256, b->input_sha256, sizeof(info->input_sha256));
    std::memcpy(info->aot_sha256, b->aot_sha256, sizeof(info->aot_sha256));
    std::snprintf(info->target_triple, sizeof(info->target_triple), "%s",
                  b->target_triple.c_str());
    return AEV_BRIDGE_OK;
}

int aev_bridge_selected_block_contains(const AevBridge *b, uint64_t rip) {
    if (!b)
        return -1;
    return std::binary_search(b->aot_selected.begin(), b->aot_selected.end(), rip)
               ? 1
               : 0;
}

uint32_t aev_bridge_selected_block_count(const AevBridge *b) {
    return b ? b->aot_table_count : 0;
}

int aev_bridge_selected_block_rip(const AevBridge *b, uint32_t index,
                                  uint64_t *rip) {
    if (!b || !rip || index >= b->aot_selected.size())
        return -1;
    *rip = b->aot_selected[index];
    return 0;
}

uint64_t aev_bridge_debug_reserved0(const AevBridge *b) {
    return b ? b->state.reserved0 : 0;
}

AevBridgeStatus aev_bridge_trace_open(AevBridge *b, const char *path,
                                      const char *scenario,
                                      char *err, size_t err_cap) {
    if (!b || !path || !scenario) {
        set_error(err, err_cap, "invalid trace path/scenario");
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }
    if (std::this_thread::get_id() != b->owner) {
        set_error(err, err_cap, "trace_open must run on the bridge owner thread");
        return AEV_BRIDGE_ERR_WRONG_THREAD;
    }
    if (b->in_call) {
        set_error(err, err_cap, "cannot open trace during a guest call");
        return AEV_BRIDGE_ERR_REENTRY;
    }
    if (b->trace_open) {
        set_error(err, err_cap, "trace already open");
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }
    b->trace_stream.open(path, std::ios::out | std::ios::trunc);
    if (!b->trace_stream) {
        set_error(err, err_cap, "cannot open trace file");
        return AEV_BRIDGE_ERR_LOAD;
    }
    b->trace_scenario = scenario;
    b->trace_open = true;
    b->trace_stream << "{\"record\":\"header\",\"trace_abi\":"
                    << AEV_TRACE_FILE_ABI_COFF << ",\"scenario\":\""
                    << json_escape(b->trace_scenario) << "\",\"input_sha256\":\""
                    << aev::hex_bytes(b->input_sha256, 32)
                    << "\",\"input_fnv\":\"" << aev::hex64(b->input_hash)
                    << "\",\"entry\":\"" << aev::hex64(b->entry_addr)
                    << "\",\"function\":\"" << json_escape(b->function_name)
                    << "\",\"aot_sha256\":\""
                    << aev::hex_bytes(b->aot_sha256, 32)
                    << "\",\"callback_abi\":" << AEV_CALLBACK_ABI_VERSION
                    << ",\"guest_abi\":" << AEV_GUEST_ABI_VERSION
                    << ",\"aot_artifact_abi\":" << AEV_ARTIFACT_ABI_VERSION
                    << ",\"translator_abi\":" << AEV_TRANSLATOR_ABI_VERSION
                    << ",\"target\":\"" << json_escape(b->target_triple)
                    << "\",\"translator_version\":\"aev-translate-v2\""
                    << ",\"trace_tool_version\":\"aev-trace-sink-v1\""
                    << ",\"all_blocks\":";
    trace_array(b->trace_stream, b->all_blocks);
    b->trace_stream << ",\"aot_selected\":";
    trace_array(b->trace_stream, b->aot_selected);
    b->trace_stream << "}\n";
    b->trace_stream.flush();
    if (!b->trace_stream) {
        b->trace_stream.close();
        b->trace_open = false;
        set_error(err, err_cap, "cannot write trace header");
        return AEV_BRIDGE_ERR_LOAD;
    }
    b->trace_sink.abi_version = AEV_TRACE_SINK_ABI_VERSION;
    b->trace_sink.reserved = 0;
    b->trace_sink.on_block = bridge_trace_on_block;
    b->trace_sink.ctx = b;
    b->jit->set_trace_hook(b, bridge_jit_trace);
    return AEV_BRIDGE_OK;
}

AevBridgeStatus aev_bridge_trace_close(AevBridge *b) {
    if (!b) {
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }
    if (std::this_thread::get_id() != b->owner) {
        return AEV_BRIDGE_ERR_WRONG_THREAD;
    }
    if (b->in_call) {
        return AEV_BRIDGE_ERR_REENTRY;
    }
    if (b->trace_open) {
        b->trace_stream.flush();
        b->trace_stream.close();
        b->trace_open = false;
        std::memset(&b->trace_sink, 0, sizeof(b->trace_sink));
        if (b->jit)
            b->jit->set_trace_hook(nullptr, nullptr);
        b->state.reserved0 = 0;
    }
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
    if (!b || !result) {
        set_error(err, err_cap, "invalid bridge/result pointer");
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }
    if (result->struct_size != sizeof(AevCallResult) ||
        result->abi_version != AEV_CALLBACK_ABI_VERSION) {
        set_error(err, err_cap, "invalid result struct size/ABI");
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }
    const uint32_t result_size = result->struct_size;
    const uint32_t result_abi = result->abi_version;
    std::memset(result, 0, sizeof(*result));
    result->struct_size = result_size;
    result->abi_version = result_abi;
    result->guest_mem_base = b->mem_base;
    result->guest_mem_size = b->mem_size;
    if (entry_addr != b->entry_addr) {
        result->status = AEV_BRIDGE_ERR_INVALID_ARGUMENT;
        set_error(err, err_cap, "invalid bridge call entry address");
        return AEV_BRIDGE_ERR_INVALID_ARGUMENT;
    }

    if (std::this_thread::get_id() != b->owner ||
        (b->require_main_thread && !pthread_main_np())) {
        result->status = AEV_BRIDGE_ERR_WRONG_THREAD;
        set_error(err, err_cap, "guest call must run on the bridge owner/main thread");
        return AEV_BRIDGE_ERR_WRONG_THREAD;
    }
    if (b->in_call) {
        result->status = AEV_BRIDGE_ERR_REENTRY;
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
    b->state.reserved0 = b->trace_open
                             ? (uint64_t)(uintptr_t)&b->trace_sink
                             : 0;
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
    trace_call_end(b, status);
    b->state.reserved0 = 0;

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
