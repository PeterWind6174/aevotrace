#include "jit_runtime.hpp"

#include "ir_emitter.hpp"
#include <capstone/capstone.h>
#include <capstone/arm64.h>
#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/Support.h>
#include <llvm-c/Target.h>

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aev {
namespace {

void print_llvm_error(const char *what, LLVMErrorRef err) {
    char *msg = LLVMGetErrorMessage(err);
    std::fprintf(stderr, "%s: %s\n", what, msg ? msg : "(unknown LLVM error)");
    if (msg)
        LLVMDisposeErrorMessage(msg);
}

void dump_arm64(const void *addr, size_t max_bytes) {
    csh handle;
    if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle) != CS_ERR_OK)
        return;
    cs_insn *insn = nullptr;
    size_t count = cs_disasm(handle, (const uint8_t *)addr, max_bytes,
                             (uint64_t)(uintptr_t)addr, 0, &insn);
    for (size_t i = 0; i < count; ++i) {
        std::fprintf(stderr, "  JITCODE 0x%llx: %-10s %s\n",
                     (unsigned long long)insn[i].address,
                     insn[i].mnemonic, insn[i].op_str);
    }
    if (insn)
        cs_free(insn, count);
    cs_close(&handle);
}

}  // namespace

struct JitRuntime::Impl {
    LLVMOrcLLJITRef jit = nullptr;
    LLVMOrcJITDylibRef jd = nullptr;
    const Program *program = nullptr;
    std::unordered_map<uint64_t, void *> cache;
    bool trace = false;
    bool dump_jit = false;
    void *trace_ctx = nullptr;
    JitRuntime::TraceHook trace_hook = nullptr;
    uint64_t compile_count = 0;
    uint64_t exec_count = 0;
    std::string data_layout;
    std::string triple;
};

JitRuntime::JitRuntime(const Program *program) : impl_(new Impl()) {
    impl_->program = program;
}

JitRuntime::~JitRuntime() {
    if (impl_ && impl_->jit) {
        LLVMErrorRef err = LLVMOrcDisposeLLJIT(impl_->jit);
        if (err)
            print_llvm_error("LLVMOrcDisposeLLJIT", err);
    }
}

void JitRuntime::set_trace(bool enabled) { impl_->trace = enabled; }
void JitRuntime::set_dump_jit(bool enabled) { impl_->dump_jit = enabled; }
void JitRuntime::set_trace_hook(void *ctx, TraceHook hook) {
    impl_->trace_ctx = ctx;
    impl_->trace_hook = hook;
}
uint64_t JitRuntime::compile_count() const { return impl_->compile_count; }
uint64_t JitRuntime::exec_count() const { return impl_->exec_count; }

namespace {

bool register_absolute_symbols(LLVMOrcLLJITRef jit, LLVMOrcJITDylibRef jd) {
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
        pairs[i].Name = LLVMOrcLLJITMangleAndIntern(jit, syms[i].name);
        pairs[i].Sym.Address = (LLVMOrcExecutorAddress)(uintptr_t)syms[i].addr;
        pairs[i].Sym.Flags.GenericFlags = 0x30; /* Exported | Callable */
        pairs[i].Sym.Flags.TargetFlags = 0;
    }
    LLVMOrcMaterializationUnitRef mu = LLVMOrcAbsoluteSymbols(pairs.data(), n);
    LLVMErrorRef err = LLVMOrcJITDylibDefine(jd, mu);
    if (err) {
        print_llvm_error("LLVMOrcJITDylibDefine(absolute symbols)", err);
        return false;
    }
    return true;
}

}  // namespace

bool JitRuntime::init(std::string *error) {
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();
    LLVMErrorRef err = LLVMOrcCreateLLJIT(&impl_->jit, nullptr);
    if (err) {
        print_llvm_error("LLVMOrcCreateLLJIT", err);
        if (error) *error = "LLVMOrcCreateLLJIT failed";
        return false;
    }
    impl_->jd = LLVMOrcLLJITGetMainJITDylib(impl_->jit);
    impl_->data_layout = LLVMOrcLLJITGetDataLayoutStr(impl_->jit);
    impl_->triple = LLVMOrcLLJITGetTripleString(impl_->jit);
    std::fprintf(stderr, "JIT initialized triple=%s\n", impl_->triple.c_str());
    if (!register_absolute_symbols(impl_->jit, impl_->jd)) {
        if (error) *error = "failed to register absolute helper symbols";
        return false;
    }
    return true;
}

bool JitRuntime::compile_block(uint64_t rip, void **out_fn, std::string *error) {
    const GuestFunction *fn = impl_->program->find_function_at(rip);
    if (!fn) {
        if (error) *error = "no guest function contains rip";
        return false;
    }
    const Block *block = impl_->program->find_block(rip);
    if (!block) {
        if (error) *error = "rip is inside a function but not a basic-block boundary";
        return false;
    }
    if (!block->jit_only) {
        if (error) *error = "AOT-covered block is missing from the AOT table";
        return false;
    }

    std::string ir, emit_error;
    if (!emit_jit_block(*impl_->program, *fn, *block, &ir, &emit_error)) {
        if (error) *error = "JIT IR emission failed: " + emit_error;
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
    replace_line("target datalayout =",
                 "target datalayout = \"" + impl_->data_layout + "\"");
    replace_line("target triple =",
                 "target triple = \"" + impl_->triple + "\"");

    if (impl_->trace) {
        std::fprintf(stderr, "JIT IR-begin rip=0x%llx\n%sJIT IR-end rip=0x%llx\n",
                     (unsigned long long)rip, ir.c_str(),
                     (unsigned long long)rip);
    }

    LLVMContextRef llvm_ctx = LLVMContextCreate();
    LLVMMemoryBufferRef buf = LLVMCreateMemoryBufferWithMemoryRangeCopy(
        ir.data(), ir.size(), "aev-jit-block");
    LLVMModuleRef mod = nullptr;
    char *parse_msg = nullptr;
    if (LLVMParseIRInContext(llvm_ctx, buf, &mod, &parse_msg)) {
        std::fprintf(stderr, "JIT IR parse failed for 0x%llx: %s\n",
                     (unsigned long long)rip,
                     parse_msg ? parse_msg : "(no message)");
        if (parse_msg)
            LLVMDisposeMessage(parse_msg);
        LLVMContextDispose(llvm_ctx);
        if (error) *error = "JIT IR parse failed";
        return false;
    }
    if (parse_msg)
        LLVMDisposeMessage(parse_msg);

    LLVMOrcThreadSafeContextRef tsc =
        LLVMOrcCreateNewThreadSafeContextFromLLVMContext(llvm_ctx);
    LLVMOrcThreadSafeModuleRef tsm = LLVMOrcCreateNewThreadSafeModule(mod, tsc);
    LLVMErrorRef err = LLVMOrcLLJITAddLLVMIRModule(impl_->jit, impl_->jd, tsm);
    if (err) {
        print_llvm_error("LLVMOrcLLJITAddLLVMIRModule", err);
        if (error) *error = "LLVMOrcLLJITAddLLVMIRModule failed";
        return false;
    }

    char func_name[64];
    snprintf(func_name, sizeof(func_name), "aev_jit_%llx",
             (unsigned long long)rip);
    LLVMOrcExecutorAddress addr = 0;
    err = LLVMOrcLLJITLookup(impl_->jit, &addr, func_name);
    if (err) {
        print_llvm_error("LLVMOrcLLJITLookup", err);
        if (error) *error = "LLVMOrcLLJITLookup failed";
        return false;
    }

    impl_->compile_count++;
    std::fprintf(stderr,
                 "JIT compile rip=0x%llx symbol=%s host_addr=0x%llx "
                 "source=ORC(LLJIT) block_jit_only=%d\n",
                 (unsigned long long)rip, func_name,
                 (unsigned long long)addr, block->jit_only ? 1 : 0);
    if (impl_->dump_jit)
        dump_arm64((const void *)(uintptr_t)addr, 64);

    *out_fn = (void *)(uintptr_t)addr;
    if (impl_->trace_hook)
        impl_->trace_hook(impl_->trace_ctx, 1, rip);
    return true;
}

int64_t JitRuntime::dispatch(GuestState *s, uint64_t rip) {
    auto it = impl_->cache.find(rip);
    void *fn = nullptr;
    if (it == impl_->cache.end()) {
        std::string error;
        if (!compile_block(rip, &fn, &error)) {
            std::fprintf(stderr, "JIT ERROR: rip=0x%llx: %s\n",
                         (unsigned long long)rip, error.c_str());
            if (!s->error_code)
                s->error_code = AEV_ERR_JIT;
            if (!s->error_pc)
                s->error_pc = rip;
            return -1;
        }
        impl_->cache.emplace(rip, fn);
        s->jit_compiles++;
    } else {
        fn = it->second;
    }

    s->jit_execs++;
    impl_->exec_count++;
    if (impl_->trace || impl_->dump_jit) {
        std::fprintf(stderr,
                     "JIT exec rip=0x%llx host_addr=0x%llx rax_before=0x%llx "
                     "rsp_before=0x%llx\n",
                     (unsigned long long)rip, (unsigned long long)(uintptr_t)fn,
                     (unsigned long long)s->rax, (unsigned long long)s->rsp);
    }

    if (impl_->trace_hook)
        impl_->trace_hook(impl_->trace_ctx, 2, rip);
    using JitFn = void (*)(GuestState *);
    JitFn jf = reinterpret_cast<JitFn>(fn);
    jf(s);
    s->jit_blocks++;

    if (impl_->trace || impl_->dump_jit) {
        std::fprintf(stderr,
                     "JIT return rip=0x%llx next_rip=0x%llx rax_after=0x%llx "
                     "error=%llu\n",
                     (unsigned long long)rip, (unsigned long long)s->rip,
                     (unsigned long long)s->rax,
                     (unsigned long long)s->error_code);
    }
    return s->error_code ? -1 : 0;
}

}  // namespace aev
