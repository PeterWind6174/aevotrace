#ifndef AEV_JIT_RUNTIME_HPP
#define AEV_JIT_RUNTIME_HPP

#include "aev_guest.h"
#include "translator.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace aev {

/* Shared ORC LLJIT runtime used by both the legacy runner and the callback
 * bridge.  It owns the LLVM JIT, the per-rip block cache and the absolute
 * helper symbols.  Host-import dispatch and test-specific target handling
 * belong to the caller, not to this class. */
class JitRuntime {
  public:
    explicit JitRuntime(const Program *program);
    ~JitRuntime();
    JitRuntime(const JitRuntime &) = delete;
    JitRuntime &operator=(const JitRuntime &) = delete;

    bool init(std::string *error);
    void set_trace(bool enabled);
    void set_dump_jit(bool enabled);

    /* Compiles/caches a jit_only block and executes it.  On failure sets
     * GuestState.error_code/error_pc and returns -1. */
    int64_t dispatch(GuestState *s, uint64_t rip);

    uint64_t compile_count() const;
    uint64_t exec_count() const;

  private:
    bool compile_block(uint64_t rip, void **out_fn, std::string *error);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aev

#endif
