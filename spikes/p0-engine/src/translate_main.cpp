#include "ir_emitter.hpp"
#include "translator.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string input;
    std::vector<std::string> functions;
    std::string entry;
    std::string out_ll;
    std::string out_header;
    std::string out_manifest;
};

void usage(const char *argv0) {
    std::fprintf(stderr,
                 "usage: %s --input <obj> --function <name> [--function <name> ...]\n"
                 "          --entry <name> --out-ll <file> --out-header <file>\n"
                 "          [--out-manifest <file>]\n",
                 argv0);
}

std::string sanitize(const std::string &name) {
    std::string s;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9'))
            s.push_back(c);
        else
            s.push_back('_');
    }
    return s;
}

std::string json_escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

}  // namespace

int main(int argc, char **argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char *what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--input")
            opt.input = need("--input");
        else if (a == "--function")
            opt.functions.push_back(need("--function"));
        else if (a == "--entry")
            opt.entry = need("--entry");
        else if (a == "--out-ll")
            opt.out_ll = need("--out-ll");
        else if (a == "--out-header")
            opt.out_header = need("--out-header");
        else if (a == "--out-manifest")
            opt.out_manifest = need("--out-manifest");
        else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    if (opt.input.empty() || opt.functions.empty() || opt.out_ll.empty() ||
        opt.out_header.empty()) {
        usage(argv[0]);
        return 2;
    }
    if (opt.entry.empty())
        opt.entry = opt.functions[0];

    std::string error;
    aev::CoffObject object;
    if (!object.load(opt.input, &error)) {
        std::fprintf(stderr, "coff load failed: %s\n", error.c_str());
        return 1;
    }

    const uint64_t image_base = 0x140000000ULL;
    const uint64_t first_function = image_base + 0x1000ULL;
    const uint64_t stride = 0x1000ULL;
    const uint64_t input_hash = object.content_hash;
    std::vector<uint64_t> addresses;
    for (size_t i = 0; i < opt.functions.size(); ++i)
        addresses.push_back(first_function + i * stride);

    aev::Program program;
    if (!aev::build_program(std::move(object), opt.functions, addresses, &program,
                            &error)) {
        std::fprintf(stderr, "translation setup failed: %s\n", error.c_str());
        return 1;
    }

    bool entry_found = false;
    uint64_t entry_address = 0;
    for (const auto &fn : program.functions) {
        if (fn.name == opt.entry) {
            entry_found = true;
            entry_address = fn.address;
        }
    }
    if (!entry_found) {
        std::fprintf(stderr, "entry function not found: %s\n", opt.entry.c_str());
        return 1;
    }

    std::string ll;
    if (!aev::emit_aot_module(program, input_hash, &ll, &error)) {
        std::fprintf(stderr, "AOT IR emission failed: %s\n", error.c_str());
        return 1;
    }

    {
        std::ofstream out(opt.out_ll);
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", opt.out_ll.c_str());
            return 1;
        }
        out << ll;
    }

    {
        std::ofstream out(opt.out_header);
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", opt.out_header.c_str());
            return 1;
        }
        out << "#pragma once\n";
        out << "#define AEV_IMAGE_BASE 0x" << std::hex << image_base << "ULL\n";
        for (const auto &fn : program.functions) {
            out << "#define AEV_ADDR_" << sanitize(fn.name) << " 0x" << std::hex
                << fn.address << "ULL\n";
        }
        out << "#define AEV_ENTRY_ADDR AEV_ADDR_" << sanitize(opt.entry) << "\n";
        const aev::GuestFunction *op = program.find_function_by_name("guest_op");
        if (op)
            out << "#define AEV_OP_ADDR AEV_ADDR_guest_op\n";
        else
            out << "#define AEV_OP_ADDR 0ULL\n";
        out << "#define AEV_GUEST_KERNEL_ADDR AEV_ADDR_guest_kernel\n";
        out << "#define AEV_GUEST_OP_ADDR AEV_OP_ADDR\n";
    }

    size_t aot_blocks = 0, jit_blocks = 0;
    for (const auto &fn : program.functions) {
        for (const auto &b : fn.blocks) {
            if (b.jit_only)
                ++jit_blocks;
            else
                ++aot_blocks;
        }
    }

    if (!opt.out_manifest.empty()) {
        std::ofstream out(opt.out_manifest);
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", opt.out_manifest.c_str());
            return 1;
        }
        out << "{\n";
        out << "  \"input\": \"" << json_escape(opt.input) << "\",\n";
        out << "  \"entry\": \"" << json_escape(opt.entry) << "\",\n";
        out << "  \"entry_address\": " << entry_address << ",\n";
        out << "  \"aot_block_count\": " << aot_blocks << ",\n";
        out << "  \"jit_block_count\": " << jit_blocks << ",\n";
        out << "  \"functions\": [\n";
        for (size_t fi = 0; fi < program.functions.size(); ++fi) {
            const auto &fn = program.functions[fi];
            out << "    {\"name\": \"" << json_escape(fn.name)
                << "\", \"address\": " << fn.address << ", \"bytes\": "
                << fn.bytes.size() << ", \"blocks\": [";
            for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
                const auto &b = fn.blocks[bi];
                if (bi) out << ", ";
                out << "{\"rip\": " << b.address
                    << ", \"jit_only\": " << (b.jit_only ? "true" : "false")
                    << ", \"reason\": \"" << json_escape(b.jit_reason) << "\"}";
            }
            out << "]}" << (fi + 1 == program.functions.size() ? "" : ",") << "\n";
        }
        out << "  ]\n}\n";
    }

    std::printf("translated input=%s entry=%s entry_addr=0x%llx functions=%zu "
                "aot_blocks=%zu jit_blocks=%zu input_hash=0x%llx\n",
                opt.input.c_str(), opt.entry.c_str(),
                (unsigned long long)entry_address, program.functions.size(),
                aot_blocks, jit_blocks, (unsigned long long)input_hash);
    for (const auto &fn : program.functions) {
        std::printf("function %s addr=0x%llx bytes=%zu insns=%zu blocks=%zu\n",
                    fn.name.c_str(), (unsigned long long)fn.address,
                    fn.bytes.size(), fn.insns.size(), fn.blocks.size());
        for (const auto &b : fn.blocks) {
            if (b.jit_only)
                std::printf("  JIT-only block 0x%llx: %s\n",
                            (unsigned long long)b.address, b.jit_reason.c_str());
        }
    }
    return 0;
}
