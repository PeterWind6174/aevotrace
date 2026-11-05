#include "aev_bridge.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t kEntry = 0x140001000ULL;
constexpr uint64_t kImportUpdate = 0x140010010ULL;
constexpr uint64_t kImportReentry = 0x140010020ULL;
constexpr uint64_t kImportFail = 0x140010030ULL;
constexpr uint64_t kImportThrow = 0x140010040ULL;
constexpr uint64_t kImportBadStatus = 0x140010050ULL;
constexpr uint64_t kUnknownImport = 0x1400100f0ULL;
constexpr uint64_t kBadAddress = 0xdead0000ULL;
constexpr uint64_t kTextOffset = 0x1000;
constexpr uint64_t kCtxOffset = 0x2000;
constexpr uint64_t kCounterOffset = 0x3000;

struct HostState {
    AevBridge *bridge = nullptr;
    uint64_t calls = 0;
    uint64_t last_hash = 0;
    uint64_t last_len = 0;
    uint64_t last_op = 0;
    uint64_t last_a3 = 0;
    bool reentry_next = false;
    AevBridgeStatus reentry_status = AEV_BRIDGE_OK;
    AevBridgeStatus destroy_status = AEV_BRIDGE_OK;
    bool throw_next = false;
    bool fail_next = false;
    bool bad_status_next = false;
    bool pending_valid = false;
    uint64_t pending_result = 0;
};

struct Fixture {
    AevBridge *bridge = nullptr;
    HostState host;
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t stack_base = 0;
    uint64_t stack_top = 0;
    uint64_t counter = 0;
    std::string err;
};

uint32_t fnv1a32(const std::vector<uint16_t> &units) {
    uint32_t h = 2166136261u;
    for (uint16_t unit : units) {
        h ^= unit;
        h *= 16777619u;
    }
    return h;
}

const char *status_name(AevBridgeStatus s) {
    switch (s) {
        case AEV_BRIDGE_OK: return "OK";
        case AEV_BRIDGE_ERR_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case AEV_BRIDGE_ERR_ABI_MISMATCH: return "ABI_MISMATCH";
        case AEV_BRIDGE_ERR_LOAD: return "LOAD";
        case AEV_BRIDGE_ERR_NO_BLOCK: return "NO_BLOCK";
        case AEV_BRIDGE_ERR_UNKNOWN_IMPORT: return "UNKNOWN_IMPORT";
        case AEV_BRIDGE_ERR_MEMORY: return "MEMORY";
        case AEV_BRIDGE_ERR_JIT: return "JIT";
        case AEV_BRIDGE_ERR_STEP_LIMIT: return "STEP_LIMIT";
        case AEV_BRIDGE_ERR_REENTRY: return "REENTRY";
        case AEV_BRIDGE_ERR_WRONG_THREAD: return "WRONG_THREAD";
        case AEV_BRIDGE_ERR_HOST: return "HOST";
        case AEV_BRIDGE_ERR_INTERNAL: return "INTERNAL";
        default: return "UNKNOWN";
    }
}

AevBridgeStatus host_update(void *opaque, AevBridge *bridge,
                            uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t *out_rax) {
    HostState *h = static_cast<HostState *>(opaque);
    h->calls++;
    h->pending_valid = false;
    h->pending_result = 0;
    h->last_hash = a0;
    h->last_len = a1;
    h->last_op = a2;
    h->last_a3 = a3;
    h->pending_valid = true;
    h->pending_result = a0 ^ a2;
    if (h->throw_next) {
        h->throw_next = false;
        throw std::runtime_error("injected host exception");
    }
    if (h->fail_next) {
        h->fail_next = false;
        return AEV_BRIDGE_ERR_HOST;
    }
    if (h->bad_status_next) {
        h->bad_status_next = false;
        return static_cast<AevBridgeStatus>(-1);
    }
    if (h->reentry_next) {
        h->reentry_next = false;
        AevCallResult nested{};
        nested.struct_size = sizeof(AevCallResult);
        nested.abi_version = AEV_CALLBACK_ABI_VERSION;
        h->reentry_status = (AevBridgeStatus)aev_bridge_call(
            bridge, kEntry, 0, 0, 0, 0, &nested, nullptr, 0);
        h->destroy_status = aev_bridge_destroy(bridge);
    }
    *out_rax = a2; /* frozen helper contract: return op */
    return AEV_BRIDGE_OK;
}

uint64_t read_u64(const Fixture &f, uint64_t addr) {
    uint64_t value = 0;
    if (aev_bridge_guest_read(f.bridge, addr, &value, sizeof(value), nullptr, 0) !=
        AEV_BRIDGE_OK) {
        throw std::runtime_error("guest_read failed");
    }
    return value;
}

void write_ctx(Fixture &f, uint64_t update_addr, uint64_t counter_addr) {
    struct Ctx {
        uint64_t update;
        uint64_t counter;
    } ctx{update_addr, counter_addr};
    if (aev_bridge_guest_write(f.bridge, f.base + kCtxOffset, &ctx, sizeof(ctx),
                               nullptr, 0) != AEV_BRIDGE_OK) {
        throw std::runtime_error("guest_write ctx failed");
    }
}

void write_text(Fixture &f, uint64_t text_addr,
                const std::vector<uint16_t> &units) {
    if (!units.empty() &&
        aev_bridge_guest_write(f.bridge, text_addr, units.data(),
                               units.size() * sizeof(uint16_t), nullptr, 0) !=
            AEV_BRIDGE_OK) {
        throw std::runtime_error("guest_write text failed");
    }
}

AevBridgeStatus call_fixture_ctx(Fixture &f, uint32_t op,
                                 const std::vector<uint16_t> &units,
                                 uint64_t text_addr, uint64_t update_addr,
                                 uint64_t counter_addr, AevCallResult *out) {
    write_text(f, text_addr, units);
    write_ctx(f, update_addr, counter_addr);
    AevCallResult result{};
    result.struct_size = sizeof(result);
    result.abi_version = AEV_CALLBACK_ABI_VERSION;
    AevBridgeStatus status = (AevBridgeStatus)aev_bridge_call(
        f.bridge, kEntry, op, text_addr, units.size(),
        f.base + kCtxOffset, &result, nullptr, 0);
    if (out)
        *out = result;
    return status;
}

AevBridgeStatus call_fixture(Fixture &f, uint32_t op,
                             const std::vector<uint16_t> &units,
                             AevCallResult *out) {
    return call_fixture_ctx(f, op, units, f.base + kTextOffset, kImportUpdate,
                            f.base + kCounterOffset, out);
}

int failures = 0;
void check(const char *name, bool condition, const std::string &detail = "") {
    std::printf("CHECK %s %s%s%s\n", name, condition ? "PASS" : "FAIL",
                detail.empty() ? "" : " ", detail.c_str());
    if (!condition)
        failures++;
}

Fixture make_fixture(const std::string &coff, const std::string &aot,
                     bool trace_aot = false) {
    Fixture f;
    f.host.bridge = nullptr;
    AevBridgeConfig cfg{};
    cfg.struct_size = sizeof(cfg);
    cfg.abi_version = AEV_CALLBACK_ABI_VERSION;
    cfg.coff_path = coff.c_str();
    cfg.function_name = "aev_guest_callback";
    cfg.entry_addr = kEntry;
    cfg.aot_dylib_path = aot.c_str();
    cfg.guest_mem_size = 4ULL * 1024 * 1024;
    cfg.guest_stack_size = 64ULL * 1024;
    cfg.trace = 0;
    cfg.dump_jit = 1;
    cfg.trace_aot = trace_aot ? 1 : 0;
    cfg.require_main_thread = 1;
    char err[512] = {0};
    AevBridgeStatus status = aev_bridge_create(&cfg, &f.bridge, err, sizeof(err));
    if (status != AEV_BRIDGE_OK) {
        throw std::runtime_error(std::string("bridge create failed: ") +
                                 err);
    }
    f.host.bridge = f.bridge;
    if (aev_bridge_guest_info(f.bridge, &f.base, &f.size, &f.stack_base,
                              &f.stack_top) != AEV_BRIDGE_OK) {
        throw std::runtime_error("guest_info failed");
    }
    write_ctx(f, kImportUpdate, f.base + kCounterOffset);
    if (aev_bridge_register_import(f.bridge, kImportUpdate, 1, host_update,
                                   &f.host, nullptr, 0) != AEV_BRIDGE_OK ||
        aev_bridge_register_import(f.bridge, kImportReentry, 2, host_update,
                                   &f.host, nullptr, 0) != AEV_BRIDGE_OK ||
        aev_bridge_register_import(f.bridge, kImportFail, 3, host_update,
                                   &f.host, nullptr, 0) != AEV_BRIDGE_OK ||
        aev_bridge_register_import(f.bridge, kImportThrow, 4, host_update,
                                   &f.host, nullptr, 0) != AEV_BRIDGE_OK ||
        aev_bridge_register_import(f.bridge, kImportBadStatus, 5, host_update,
                                   &f.host, nullptr, 0) != AEV_BRIDGE_OK) {
        throw std::runtime_error("register_import failed");
    }
    return f;
}

void run_positive_cases(Fixture &f) {
    const std::vector<uint16_t> alpha = {0x0041, 0x0065, 0x0076, 0x006f,
                                         0x03b2, 0x4e2d};
    const std::vector<uint16_t> beta = {0x006d, 0x0061, 0x0063, 0x004f,
                                        0x0053, 0x002d, 0x0034, 0x0037};
    const std::vector<uint16_t> hidden = {0x72ec, 0x7acb, 0x002d, 0x03a9,
                                          0x002d, 0x0039, 0x0033};
    AevCallResult r{};
    AevBridgeStatus s = call_fixture(f, 7, alpha, &r);
    check("alpha_status", s == AEV_BRIDGE_OK, status_name(s));
    check("alpha_result", r.guest_rax == 3828036654ULL,
          std::to_string((unsigned long long)r.guest_rax));
    check("alpha_counter", read_u64(f, f.base + kCounterOffset) == 1);
    check("alpha_aot", r.aot_blocks > 0);
    check("alpha_jit_compile", r.jit_compiles == 1);
    check("alpha_jit_exec", r.jit_execs > 0);
    check("alpha_helper_args", f.host.last_hash == fnv1a32(alpha) &&
                                    f.host.last_len == alpha.size() &&
                                    f.host.last_op == 7);
    check("alpha_call_index", r.call_index == 1);
    std::printf("CASE alpha op=7 len=6 result=%llu counter=%llu\n",
                (unsigned long long)r.guest_rax,
                (unsigned long long)read_u64(f, f.base + kCounterOffset));

    s = call_fixture(f, 9, beta, &r);
    check("beta_status", s == AEV_BRIDGE_OK, status_name(s));
    check("beta_result", r.guest_rax == 614625053ULL,
          std::to_string((unsigned long long)r.guest_rax));
    check("beta_jit_cached", r.jit_compiles == 0);
    check("beta_counter", read_u64(f, f.base + kCounterOffset) == 2);
    std::printf("CASE beta op=9 len=8 result=%llu counter=%llu\n",
                (unsigned long long)r.guest_rax,
                (unsigned long long)read_u64(f, f.base + kCounterOffset));

    s = call_fixture(f, 0x5a5a, hidden, &r);
    check("hidden_status", s == AEV_BRIDGE_OK, status_name(s));
    check("hidden_result", r.guest_rax == 651115151ULL,
          std::to_string((unsigned long long)r.guest_rax));
    check("hidden_counter", read_u64(f, f.base + kCounterOffset) == 3);
    std::printf("CASE hidden op=23130 len=7 result=%llu counter=%llu\n",
                (unsigned long long)r.guest_rax,
                (unsigned long long)read_u64(f, f.base + kCounterOffset));
}

void run_100_calls(Fixture &f) {
    const std::vector<uint16_t> text = {'l', 'o', 'o', 'p', '-', '1', '0', '0'};
    const uint32_t expected = fnv1a32(text) ^ 3u;
    const uint64_t before = read_u64(f, f.base + kCounterOffset);
    bool all_ok = true;
    bool cached = true;
    bool jit_executed = false;
    for (int i = 0; i < 100; ++i) {
        AevCallResult r{};
        AevBridgeStatus s = call_fixture(f, 3, text, &r);
        if (s != AEV_BRIDGE_OK || r.guest_rax != expected ||
            r.jit_compiles != 0) {
            all_ok = false;
            std::printf("CHECK loop_failure_%d FAIL status=%s rax=%llu compiles=%llu\n",
                        i, status_name(s), (unsigned long long)r.guest_rax,
                        (unsigned long long)r.jit_compiles);
            break;
        }
        jit_executed = jit_executed || r.jit_execs > 0;
        cached = cached && r.jit_compiles == 0;
    }
    const uint64_t after = read_u64(f, f.base + kCounterOffset);
    check("loop_100_ok", all_ok);
    check("loop_100_counter", after == before + 100,
          std::to_string((unsigned long long)(after - before)));
    check("loop_100_jit_cached", cached);
    check("loop_100_jit_exec", jit_executed);
}

void run_negative_cases(Fixture &f) {
    const std::vector<uint16_t> text = {'n', 'e', 'g', '-', '0', '1'};
    const uint64_t counter_before = read_u64(f, f.base + kCounterOffset);
    const uint64_t calls_before = f.host.calls;
    AevCallResult r{};

    // Unknown import: reserved range but not registered.
    write_ctx(f, kUnknownImport, f.base + kCounterOffset);
    write_text(f, f.base + kTextOffset, text);
    r = {};
    r.struct_size = sizeof(r);
    r.abi_version = AEV_CALLBACK_ABI_VERSION;
    AevBridgeStatus s = (AevBridgeStatus)aev_bridge_call(
        f.bridge, kEntry, 1, f.base + kTextOffset, text.size(),
        f.base + kCtxOffset, &r, nullptr, 0);
    check("unknown_import", s == AEV_BRIDGE_ERR_UNKNOWN_IMPORT, status_name(s));
    check("unknown_import_counter", read_u64(f, f.base + kCounterOffset) == counter_before);
    check("unknown_import_no_host", f.host.calls == calls_before);

    // Inside-function but not a block boundary.
    s = call_fixture_ctx(f, 1, text, f.base + kTextOffset, kEntry + 1,
                         f.base + kCounterOffset, &r);
    check("no_block_boundary", s == AEV_BRIDGE_ERR_NO_BLOCK, status_name(s));
    check("no_block_counter", read_u64(f, f.base + kCounterOffset) == counter_before);

    // Zero / DEAD0000 targets.
    s = call_fixture_ctx(f, 1, text, f.base + kTextOffset, 0,
                         f.base + kCounterOffset, &r);
    check("no_block_zero", s == AEV_BRIDGE_ERR_NO_BLOCK, status_name(s));
    s = call_fixture_ctx(f, 1, text, f.base + kTextOffset, kBadAddress,
                         f.base + kCounterOffset, &r);
    check("no_block_dead", s == AEV_BRIDGE_ERR_NO_BLOCK, status_name(s));

    // Illegal Guest memory read.
    write_ctx(f, kImportUpdate, f.base + kCounterOffset);
    r = {};
    r.struct_size = sizeof(r);
    r.abi_version = AEV_CALLBACK_ABI_VERSION;
    s = (AevBridgeStatus)aev_bridge_call(
        f.bridge, kEntry, 1, kBadAddress, 4, f.base + kCtxOffset, &r, nullptr, 0);
    check("memory_fault", s == AEV_BRIDGE_ERR_MEMORY, status_name(s));
    check("memory_fault_counter", read_u64(f, f.base + kCounterOffset) == counter_before);
    check("memory_fault_no_host", f.host.calls == calls_before);

    // Injected JIT failure, then recovery.
    write_ctx(f, kImportUpdate, f.base + kCounterOffset);
    aev_bridge_set_test_fault(f.bridge, 1, 0x140001020ULL, nullptr, 0);
    s = call_fixture(f, 1, text, &r);
    check("injected_jit", s == AEV_BRIDGE_ERR_JIT, status_name(s));
    check("injected_jit_counter", read_u64(f, f.base + kCounterOffset) == counter_before);
    aev_bridge_set_test_fault(f.bridge, 0, 0, nullptr, 0);
    s = call_fixture(f, 1, text, &r);
    check("jit_recovery", s == AEV_BRIDGE_OK, status_name(s));
    check("jit_recovery_result", r.guest_rax == (uint64_t)(fnv1a32(text) ^ 1u));

    // Injected sentinel/RSP mismatch.
    aev_bridge_set_test_fault(f.bridge, 2, 0, nullptr, 0);
    s = call_fixture(f, 1, text, &r);
    check("internal_rsp", s == AEV_BRIDGE_ERR_INTERNAL, status_name(s));
    aev_bridge_set_test_fault(f.bridge, 0, 0, nullptr, 0);
    s = call_fixture(f, 1, text, &r);
    check("internal_recovery", s == AEV_BRIDGE_OK, status_name(s));

    // Reentry and destroy-during-call are rejected without corrupting the outer call.
    f.host.reentry_next = true;
    s = call_fixture_ctx(f, 5, text, f.base + kTextOffset, kImportReentry,
                         f.base + kCounterOffset, &r);
    check("reentry_outer_ok", s == AEV_BRIDGE_OK, status_name(s));
    check("reentry_rejected", f.host.reentry_status == AEV_BRIDGE_ERR_REENTRY,
          status_name(f.host.reentry_status));
    check("destroy_during_call_rejected",
          f.host.destroy_status == AEV_BRIDGE_ERR_REENTRY,
          status_name(f.host.destroy_status));

    // Host failure and host exception must not commit RAX/counter.
    const uint64_t fail_counter = read_u64(f, f.base + kCounterOffset);
    f.host.fail_next = true;
    s = call_fixture_ctx(f, 6, text, f.base + kTextOffset, kImportFail,
                         f.base + kCounterOffset, &r);
    check("host_failure", s == AEV_BRIDGE_ERR_HOST, status_name(s));
    check("host_failure_counter", read_u64(f, f.base + kCounterOffset) == fail_counter);
    check("host_failure_import_id", r.host_import_id == 3,
          std::to_string((unsigned long long)r.host_import_id));
    check("host_failure_rip", r.guest_rip == kImportFail,
          std::to_string((unsigned long long)r.guest_rip));
    check("host_failure_error_pc", r.error_pc == kImportFail,
          std::to_string((unsigned long long)r.error_pc));
    f.host.throw_next = true;
    s = call_fixture_ctx(f, 6, text, f.base + kTextOffset, kImportThrow,
                         f.base + kCounterOffset, &r);
    check("host_exception", s == AEV_BRIDGE_ERR_HOST, status_name(s));
    check("host_exception_counter", read_u64(f, f.base + kCounterOffset) == fail_counter);
    check("host_exception_import_id", r.host_import_id == 4,
          std::to_string((unsigned long long)r.host_import_id));
    f.host.bad_status_next = true;
    s = call_fixture_ctx(f, 6, text, f.base + kTextOffset, kImportBadStatus,
                         f.base + kCounterOffset, &r);
    check("host_bad_status", s == AEV_BRIDGE_ERR_HOST, status_name(s));
    check("host_bad_status_import_id", r.host_import_id == 5,
          std::to_string((unsigned long long)r.host_import_id));

    // Helper succeeds, then Guest counter pointer faults: pending must not commit.
    const uint64_t helper_calls = f.host.calls;
    s = call_fixture_ctx(f, 7, text, f.base + kTextOffset, kImportUpdate,
                         kBadAddress, &r);
    check("helper_then_fault", s == AEV_BRIDGE_ERR_MEMORY, status_name(s));
    check("helper_then_fault_helper_called", f.host.calls == helper_calls + 1);
    check("helper_then_fault_pending_only", f.host.pending_valid);
    write_ctx(f, kImportUpdate, f.base + kCounterOffset);

    // Wrong thread.
    std::atomic<int> thread_status{999};
    std::thread worker([&]() {
        AevCallResult tr{};
        tr.struct_size = sizeof(tr);
        tr.abi_version = AEV_CALLBACK_ABI_VERSION;
        thread_status = aev_bridge_call(f.bridge, kEntry, 1, f.base + kTextOffset,
                                        text.size(), f.base + kCtxOffset,
                                        &tr, nullptr, 0);
    });
    worker.join();
    check("wrong_thread", thread_status == AEV_BRIDGE_ERR_WRONG_THREAD,
          std::to_string(thread_status.load()));

    // Invalid arguments / load.
    AevCallResult bad_result{};
    bad_result.struct_size = sizeof(bad_result) - 1;
    bad_result.abi_version = AEV_CALLBACK_ABI_VERSION;
    check("bad_result_size",
          aev_bridge_call(f.bridge, kEntry, 0, 0, 0, 0, &bad_result,
                          nullptr, 0) == AEV_BRIDGE_ERR_INVALID_ARGUMENT);
    check("bad_guest_write",
          aev_bridge_guest_write(f.bridge, kBadAddress, "x", 1, nullptr, 0) ==
              AEV_BRIDGE_ERR_INVALID_ARGUMENT);
    AevCallResult bad_entry{};
    bad_entry.struct_size = sizeof(bad_entry);
    bad_entry.abi_version = AEV_CALLBACK_ABI_VERSION;
    check("bad_entry",
          aev_bridge_call(f.bridge, kEntry + 0x1000, 0, 0, 0, 0, &bad_entry,
                          nullptr, 0) == AEV_BRIDGE_ERR_INVALID_ARGUMENT);
    check("bad_entry_status", bad_entry.status == AEV_BRIDGE_ERR_INVALID_ARGUMENT,
          std::to_string(bad_entry.status));
    check("bad_entry_call_index", bad_entry.call_index == 0,
          std::to_string((unsigned long long)bad_entry.call_index));
    check("import_id_zero",
          aev_bridge_register_import(f.bridge, AEV_BRIDGE_IMPORT_BASE, 0,
                                     host_update, &f.host, nullptr, 0) ==
              AEV_BRIDGE_ERR_INVALID_ARGUMENT);
    check("import_addr_mismatch",
          aev_bridge_register_import(f.bridge, kImportUpdate + 0x10, 1,
                                     host_update, &f.host, nullptr, 0) ==
              AEV_BRIDGE_ERR_INVALID_ARGUMENT);
    check("import_duplicate",
          aev_bridge_register_import(f.bridge, kImportUpdate, 1, host_update,
                                     &f.host, nullptr, 0) ==
              AEV_BRIDGE_ERR_INVALID_ARGUMENT);
    AevBridgeConfig bad_cfg{};
    bad_cfg.struct_size = sizeof(bad_cfg) - 1;
    bad_cfg.abi_version = AEV_CALLBACK_ABI_VERSION;
    AevBridge *bad_bridge = nullptr;
    check("bad_config",
          aev_bridge_create(&bad_cfg, &bad_bridge, nullptr, 0) ==
              AEV_BRIDGE_ERR_INVALID_ARGUMENT);
}

void run_step_limit(const std::string &coff, const std::string &aot) {
    Fixture f = make_fixture(coff, aot);
    const uint64_t text_addr = f.base + 0x100000;
    const uint64_t units = 1100000;
    std::vector<uint16_t> text(units, 0x0041);
    if (aev_bridge_guest_write(f.bridge, text_addr, text.data(),
                               text.size() * sizeof(uint16_t), nullptr, 0) !=
        AEV_BRIDGE_OK) {
        throw std::runtime_error("step-limit text write failed");
    }
    write_ctx(f, kImportUpdate, f.base + kCounterOffset);
    AevCallResult r{};
    r.struct_size = sizeof(r);
    r.abi_version = AEV_CALLBACK_ABI_VERSION;
    AevBridgeStatus s = (AevBridgeStatus)aev_bridge_call(
        f.bridge, kEntry, 1, text_addr, units, f.base + kCtxOffset,
        &r, nullptr, 0);
    check("step_limit", s == AEV_BRIDGE_ERR_STEP_LIMIT, status_name(s));
    check("step_limit_steps", r.steps >= AEV_BRIDGE_STEP_LIMIT);
    check("step_limit_counter", read_u64(f, f.base + kCounterOffset) == 0);
    AevCallResult good{};
    AevBridgeStatus s2 = call_fixture(f, 1, {0x41, 0x42, 0x43, 0x44}, &good);
    check("step_limit_recovery", s2 == AEV_BRIDGE_OK, status_name(s2));
    aev_bridge_destroy(f.bridge);
}

void run_abi_negative(const std::string &coff, const std::string &mutated_aot) {
    AevBridgeConfig cfg{};
    cfg.struct_size = sizeof(cfg);
    cfg.abi_version = AEV_CALLBACK_ABI_VERSION;
    cfg.coff_path = coff.c_str();
    cfg.function_name = "aev_guest_callback";
    cfg.entry_addr = kEntry;
    cfg.aot_dylib_path = mutated_aot.c_str();
    cfg.guest_mem_size = 4ULL * 1024 * 1024;
    cfg.guest_stack_size = 64ULL * 1024;
    cfg.require_main_thread = 1;
    AevBridge *b = nullptr;
    char err[256] = {0};
    AevBridgeStatus s = aev_bridge_create(&cfg, &b, err, sizeof(err));
    check("mutated_aot_rejected", s == AEV_BRIDGE_ERR_ABI_MISMATCH,
          status_name(s));
    check("mutated_aot_null", b == nullptr);

    AevBridgeConfig load_cfg = cfg;
    load_cfg.aot_dylib_path = "/nonexistent/libcallback_aot.dylib";
    b = nullptr;
    s = aev_bridge_create(&load_cfg, &b, err, sizeof(err));
    check("bad_aot_load", s == AEV_BRIDGE_ERR_LOAD, status_name(s));
}

}  // namespace

int main(int argc, char **argv) {
    std::string coff, aot, mutated;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need = [&](const char *what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--coff")
            coff = need("--coff");
        else if (arg == "--aot")
            aot = need("--aot");
        else if (arg == "--mutated-aot")
            mutated = need("--mutated-aot");
        else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }
    if (coff.empty() || aot.empty() || mutated.empty()) {
        std::fprintf(stderr, "usage: %s --coff <obj> --aot <dylib> --mutated-aot <dylib>\n",
                     argv[0]);
        return 2;
    }

    uint32_t abi = 0, guest_size = 0, result_size = 0, config_size = 0;
    aev_bridge_abi_info(&abi, &guest_size, &result_size, &config_size);
    check("abi_version", abi == AEV_CALLBACK_ABI_VERSION);
    check("guest_state_size", guest_size == 256);
    check("result_struct_size", result_size == 160);
    check("config_struct_size", config_size == sizeof(AevBridgeConfig));

    try {
        Fixture f = make_fixture(coff, aot, true);
        check("stack_partition", f.stack_base < f.stack_top &&
                                     f.stack_base >= f.base &&
                                     f.stack_top == f.base + f.size);
        run_positive_cases(f);
        run_100_calls(f);
        run_negative_cases(f);
        AevCallResult totals{};
        AevBridgeStatus s = call_fixture(f, 1, {0x41, 0x42, 0x43, 0x44}, &totals);
        check("final_recovery", s == AEV_BRIDGE_OK, status_name(s));
        check("total_calls", totals.total_calls == totals.call_index);
        check("total_jit_compiles", totals.total_jit_compiles == 1);
        check("total_jit_blocks", totals.total_jit_blocks > 0);
        aev_bridge_destroy(f.bridge);
        run_step_limit(coff, aot);
        run_abi_negative(coff, mutated);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        return 1;
    }

    std::printf("FAILURES %d\n", failures);
    return failures == 0 ? 0 : 1;
}
