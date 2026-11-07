#ifndef AEV_PE_SESSION_CORE_HPP
#define AEV_PE_SESSION_CORE_HPP

/*
 * Shared, persistent PE execution session used by both aev-pe-runner and the
 * versioned C ABI in libaev_pe_session.dylib.  This is the single owner of the
 * PE module, Program, AOT validation, JIT runtime, HostImport implementation
 * and trace writer; no second loader/dispatcher exists in either consumer.
 *
 * The C ABI wrapper adds generation handles, owner-thread checks, struct-size
 * validation and error buffers.  This header is internal (not installed).
 */

#include "aev_guest.h"
#include "pe.hpp"
#include "translator.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aev::pe {

struct SessionImpl;

enum class SessionStatus : uint32_t {
    Ok = 0,
    InvalidArgument = 2,
    Load = 3,
    AbiMismatch = 4,
    UnknownImport = 5,
    Memory = 6,
    Jit = 7,
    StepLimit = 8,
    StaleHandle = 9,
    Reentry = 10,
    Host = 11,
    Internal = 12,
    WrongThread = 13,
    Trace = 14,
};

const char *session_status_name(uint32_t status);

struct SessionConfig {
    std::string dll_path;
    std::string registry_path;
    std::string aot_path;
    std::string entry_export;
    uint64_t load_base = 0;
    std::string trace_path;
    std::string scenario;
};

struct SessionCallResult {
    uint32_t status = 0;
    uint32_t guest_error = 0;
    uint64_t args[4] = {0, 0, 0, 0};
    uint64_t rax = 0;
    int32_t rax_i32 = 0;
    uint64_t error_pc = 0;
    uint64_t mem_fault_kind = 0;
    uint64_t mem_fault_addr = 0;
    uint64_t mem_fault_size = 0;
    uint64_t mem_access = 0;
    uint64_t steps = 0;
    uint64_t aot_blocks = 0;
    uint64_t jit_blocks = 0;
    uint64_t jit_compiles = 0;
    uint64_t jit_execs = 0;
    uint64_t call_index = 0;
    uint64_t total_calls = 0;
    uint64_t total_aot_blocks = 0;
    uint64_t total_jit_blocks = 0;
    uint64_t total_jit_compiles = 0;
    uint64_t total_jit_execs = 0;
    uint64_t guest_rip = 0;
    uint64_t guest_rsp = 0;
};

struct SessionCounts {
    uint64_t load_count = 0;
    uint64_t calls_total = 0;
    uint64_t host_import_calls = 0;
    uint64_t aot_blocks_total = 0;
    uint64_t jit_blocks_total = 0;
    uint64_t jit_compiles_total = 0;
    uint64_t jit_execs_total = 0;
    uint64_t trace_records = 0;
    uint64_t trace_open = 0;
};

struct SessionObserver {
    void *ctx = nullptr;
    void (*on_call_end)(void *ctx, const SessionCallResult *result) = nullptr;
    void (*on_host_import)(void *ctx, uint64_t host_import_id,
                           uint32_t thunk_index) = nullptr;
};

class Session {
  public:
    Session();
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    /* Load one DLL.  The session is constructed on its owner thread; load must
     * be called before call/query.  A failed load releases every partially
     * initialised resource and leaves no usable module. */
    SessionStatus load(const SessionConfig &config, std::string *error);
    SessionStatus call(const std::string &export_name,
                       const uint64_t args[4], SessionCallResult *out,
                       std::string *error);

    SessionStatus trace_open(const std::string &path,
                             const std::string &scenario, std::string *error);
    SessionStatus trace_close(std::string *error);
    bool trace_is_open() const;

    /* Runner-only legacy compatibility: offset the on-disk trace call index
     * for --reload iterations without changing the C ABI session semantics. */
    void set_trace_call_base(uint64_t base);

    SessionStatus set_observer(const SessionObserver *observer,
                               std::string *error);

    /* Owner/thread checks used by the C ABI wrapper. */
    bool is_owner_thread() const;
    bool in_call() const;
    SessionStatus check_destroy() const;

    /* Read-only module/program queries. */
    const Module &module() const;
    const Program &program() const;
    const std::vector<uint64_t> &all_blocks() const;
    const std::vector<uint64_t> &selected_blocks() const;
    const std::string &entry_export() const;
    const uint8_t *artifact_sha256() const;
    bool aot_present() const;
    bool loaded() const;
    SessionCounts counts() const;

  private:
    std::unique_ptr<SessionImpl> impl_;
};

/* Process-wide generation-checked handle table (8 slots).  Handles look like
 * (slot + 1) | (generation << 32); destroying a slot increments the
 * generation so old tokens never alias a later session. */
class SessionTable {
  public:
    static SessionTable &instance();

    uint64_t add(std::unique_ptr<Session> session);
    std::shared_ptr<Session> get(uint64_t token) const;
    SessionStatus destroy(uint64_t token, std::string *error);

  private:
    SessionTable() = default;
    mutable std::mutex mutex_;
    std::array<std::shared_ptr<Session>, 8> slots_{};
    std::array<uint32_t, 8> generations_{};
};

}  // namespace aev::pe

#endif  // AEV_PE_SESSION_CORE_HPP
