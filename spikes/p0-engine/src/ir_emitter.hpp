#ifndef AEV_IR_EMITTER_HPP
#define AEV_IR_EMITTER_HPP

#include "translator.hpp"

#include <string>

namespace aev {

bool emit_aot_module(const Program &program, std::string *out, std::string *error);
bool emit_jit_block(const Program &program,
                    const GuestFunction &fn,
                    const Block &block,
                    std::string *out,
                    std::string *error);

}  // namespace aev

#endif
