#ifndef AEV_IR_EMITTER_HPP
#define AEV_IR_EMITTER_HPP

#include "translator.hpp"

#include <cstdint>
#include <string>

namespace aev {

bool emit_aot_module(const Program &program, uint64_t input_hash,
                     const uint8_t input_sha256[32], uint64_t entry_addr,
                     const uint8_t function_name_sha256[32],
                     std::string *out, std::string *error);
bool emit_jit_block(const Program &program,
                    const GuestFunction &fn,
                    const Block &block,
                    std::string *out,
                    std::string *error);

}  // namespace aev

#endif
