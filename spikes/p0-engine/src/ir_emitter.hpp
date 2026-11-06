#ifndef AEV_IR_EMITTER_HPP
#define AEV_IR_EMITTER_HPP

#include "translator.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace aev {

/* Persistent module identity emitted into the AOT dylib.  COFF uses
 * module_kind="coff" and zero module fields; PE uses "pe32+amd64-dll" and
 * the values from the loaded module manifest.  No Host pointers. */
struct AotModuleIdentity {
    std::string module_kind;
    uint64_t module_file_size = 0;
    uint64_t preferred_base = 0;
    uint64_t load_base = 0;
    uint64_t image_size = 0;
    uint8_t image_sha256[32] = {};
    uint8_t import_binding_sha256[32] = {};
    uint8_t host_binding_sha256[32] = {};
    uint8_t entry_exports_sha256[32] = {};
    uint64_t thunk_base = 0;
    uint64_t import_count = 0;
    std::vector<uint64_t> import_thunks;
    uint64_t relocs_stripped = 0;
};

bool emit_aot_module(const Program &program, uint64_t input_hash,
                     const uint8_t input_sha256[32], uint64_t entry_addr,
                     const uint8_t function_name_sha256[32],
                     const AotModuleIdentity &identity,
                     std::string *out, std::string *error);
bool emit_jit_block(const Program &program,
                    const GuestFunction &fn,
                    const Block &block,
                    std::string *out,
                    std::string *error);

}  // namespace aev

#endif
