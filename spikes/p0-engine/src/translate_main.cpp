#include "ir_emitter.hpp"
#include "translator.hpp"
#include "sha256.hpp"
#include "aev_artifact.h"
#include "program_identity.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
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
    std::set<uint64_t> aot_include;
    std::set<uint64_t> aot_exclude;
};

void usage(const char *argv0) {
    std::fprintf(stderr,
                 "usage: %s --input <obj> --function <name> [--function <name> ...]\n"
                 "          --entry <name> --out-ll <file> --out-header <file>\n"
                 "          [--out-manifest <file>]\n"
          "          [--aot-include-rip <hex> ...] [--aot-exclude-rip <hex> ...]\n",
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
        else if (a == "--aot-include-rip")
            opt.aot_include.insert(std::strtoull(need("--aot-include-rip").c_str(), nullptr, 0));
        else if (a == "--aot-exclude-rip")
            opt.aot_exclude.insert(std::strtoull(need("--aot-exclude-rip").c_str(), nullptr, 0));
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
    uint8_t input_sha256[32];
    std::memcpy(input_sha256, object.content_sha256, sizeof(input_sha256));
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

    std::set<uint64_t> known_blocks;
    for (auto &fn : program.functions) {
        for (auto &block : fn.blocks) {
            known_blocks.insert(block.address);
            if (opt.aot_include.count(block.address) &&
                opt.aot_exclude.count(block.address)) {
                std::fprintf(stderr, "coverage conflict for rip=0x%llx\n",
                             (unsigned long long)block.address);
                return 1;
            }
            if (opt.aot_include.count(block.address)) {
                block.jit_only = false;
                block.jit_reason.clear();
            } else if (opt.aot_exclude.count(block.address)) {
                block.jit_only = true;
                block.jit_reason = "excluded by coverage";
            }
        }
    }
    for (uint64_t rip : opt.aot_include) {
        if (!known_blocks.count(rip)) {
            std::fprintf(stderr, "aot-include-rip not found: 0x%llx\n",
                         (unsigned long long)rip);
            return 1;
        }
    }
    for (uint64_t rip : opt.aot_exclude) {
        if (!known_blocks.count(rip)) {
            std::fprintf(stderr, "aot-exclude-rip not found: 0x%llx\n",
                         (unsigned long long)rip);
            return 1;
        }
    }

    uint8_t function_name_sha256[32];
    aev::sha256(opt.entry.data(), opt.entry.size(), function_name_sha256);
    std::string ll;
    if (!aev::emit_aot_module(program, input_hash, input_sha256, entry_address,
                              function_name_sha256, &ll, &error)) {
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

    size_t aot_blocks = 0, jit_blocks = 0, all_block_count = 0;
    for (const auto &fn : program.functions) {
        for (const auto &b : fn.blocks) {
            ++all_block_count;
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
        out << "  \"input_sha256\": \"" << aev::hex_bytes(input_sha256, 32) << "\",\n";
        out << "  \"input_hash\": " << input_hash << ",\n";
        out << "  \"entry\": \"" << json_escape(opt.entry) << "\",\n";
        out << "  \"entry_function_sha256\": \"" << aev::hex_bytes(function_name_sha256, 32) << "\",\n";
        out << "  \"artifact_abi\": " << AEV_ARTIFACT_ABI_VERSION << ",\n";
        out << "  \"translator_abi\": " << AEV_TRANSLATOR_ABI_VERSION << ",\n";
        out << "  \"target\": \"" << AEV_AOT_TARGET_TRIPLE << "\",\n";
        out << "  \"all_block_count\": " << all_block_count << ",\n";
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
                aev::BlockIdentity id;
                std::string identity_error;
                if (!aev::block_identity(program, b.address, &id, &identity_error)) {
                    std::fprintf(stderr, "block identity failed: %s\n", identity_error.c_str());
                    return 1;
                }
                if (bi) out << ", ";
                out << "{\"rip\": " << b.address
                    << ", \"jit_only\": " << (b.jit_only ? "true" : "false")
                    << ", \"section_offset\": " << id.section_offset
                    << ", \"size\": " << id.size
                    << ", \"sha256\": \"" << aev::hex_bytes(id.sha256, 32) << "\""
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
    std::printf("input_sha256=%s\n", aev::hex_bytes(input_sha256, 32).c_str());
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
