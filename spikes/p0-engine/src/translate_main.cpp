#include "ir_emitter.hpp"
#include "pe.hpp"
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
    std::string pe;
    std::vector<std::string> functions;
    std::vector<std::string> exports;
    std::string entry;
    std::string registry;
    std::string load_base;
    std::string out_ll;
    std::string out_header;
    std::string out_manifest;
    std::set<uint64_t> aot_include;
    std::set<uint64_t> aot_exclude;
};

void usage(const char *argv0) {
    std::fprintf(stderr,
                 "usage:\n"
                 "  %s --input <obj> --function <name> [--function <name> ...]\n"
                 "     --entry <name> --out-ll <file> --out-header <file> [--out-manifest <file>]\n"
                 "  %s --pe <dll> --load-base <hex> --registry <file> --export <name> ...\n"
                 "     [--entry <export>] --out-ll <file> --out-header <file> [--out-manifest <file>]\n"
                 "  shared: [--aot-include-rip <hex> ...] [--aot-exclude-rip <hex> ...]\n",
                 argv0, argv0);
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

std::string hex_bytes32(const uint8_t *data) {
    return aev::hex_bytes(data, 32);
}

bool apply_coverage(std::vector<aev::GuestFunction> &functions,
                    const std::set<uint64_t> &include,
                    const std::set<uint64_t> &exclude,
                    std::string *error) {
    std::set<uint64_t> known;
    for (auto &fn : functions) {
        for (auto &block : fn.blocks) {
            known.insert(block.address);
            if (include.count(block.address) && exclude.count(block.address)) {
                if (error) *error = "coverage conflict for rip=0x" +
                                    aev::hex64(block.address);
                return false;
            }
            if (include.count(block.address)) {
                block.jit_only = false;
                block.jit_reason.clear();
            } else if (exclude.count(block.address)) {
                block.jit_only = true;
                block.jit_reason = "excluded by coverage";
            }
        }
    }
    for (uint64_t rip : include) {
        if (!known.count(rip)) {
            if (error) *error = "aot-include-rip not found: 0x" + aev::hex64(rip);
            return false;
        }
    }
    for (uint64_t rip : exclude) {
        if (!known.count(rip)) {
            if (error) *error = "aot-exclude-rip not found: 0x" + aev::hex64(rip);
            return false;
        }
    }
    return true;
}

int run_coff(const Options &opt) {
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
    const uint64_t input_size = object.content_size;
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
    if (!apply_coverage(program.functions, opt.aot_include, opt.aot_exclude, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    uint8_t function_name_sha256[32];
    aev::sha256(opt.entry.data(), opt.entry.size(), function_name_sha256);
    aev::AotModuleIdentity identity;
    identity.module_kind = AEV_MODULE_KIND_COFF;
    identity.module_file_size = input_size;
    std::string ll;
    if (!aev::emit_aot_module(program, input_hash, input_sha256, entry_address,
                              function_name_sha256, identity, &ll, &error)) {
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
        for (const auto &fn : program.functions)
            out << "#define AEV_ADDR_" << sanitize(fn.name) << " 0x" << std::hex
                << fn.address << "ULL\n";
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
            if (b.jit_only) ++jit_blocks; else ++aot_blocks;
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
        out << "  \"input_sha256\": \"" << hex_bytes32(input_sha256) << "\",\n";
        out << "  \"input_hash\": " << input_hash << ",\n";
        out << "  \"module_kind\": \"coff\",\n";
        out << "  \"module_file_sha256\": \"" << hex_bytes32(input_sha256) << "\",\n";
        out << "  \"module_file_size\": " << input_size << ",\n";
        out << "  \"preferred_base\": 0, \"load_base\": 0, \"image_size\": 0,\n";
        out << "  \"image_sha256\": \"" << std::string(64, '0') << "\",\n";
        out << "  \"import_binding_sha256\": \"" << std::string(64, '0') << "\",\n";
        out << "  \"host_binding_sha256\": \"" << std::string(64, '0') << "\",\n";
        out << "  \"entry_exports_sha256\": \"" << std::string(64, '0') << "\",\n";
        out << "  \"entry_exports\": [], \"entry_export\": \"" << json_escape(opt.entry) << "\",\n";
        out << "  \"thunk_base\": 0, \"import_count\": 0, \"imports\": [],\n";
        out << "  \"relocs_stripped\": 0, \"registry_sha256\": null,\n";
        out << "  \"entry\": \"" << json_escape(opt.entry) << "\",\n";
        out << "  \"entry_function_sha256\": \"" << hex_bytes32(function_name_sha256) << "\",\n";
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
    return 0;
}

int run_pe(const Options &opt) {
    std::string error;
    aev::pe::ImportRegistry registry;
    if (!registry.load(opt.registry, &error)) {
        std::fprintf(stderr, "registry load failed: %s\n", error.c_str());
        return 1;
    }
    char *end = nullptr;
    unsigned long long load_base = std::strtoull(opt.load_base.c_str(), &end, 0);
    if (!end || *end != 0 || load_base == 0) {
        std::fprintf(stderr, "invalid --load-base\n");
        return 1;
    }
    aev::pe::LoadOptions load_options;
    load_options.load_base = (uint64_t)load_base;
    aev::pe::Module module;
    if (!aev::pe::load_module(opt.pe, load_options, registry, &module, &error)) {
        std::fprintf(stderr, "PE load failed: %s\n", error.c_str());
        return 1;
    }
    std::vector<std::string> roots = opt.exports;
    if (roots.empty()) {
        for (const auto &e : module.exports)
            roots.push_back(e.name);
    }
    std::string entry_export = opt.entry;
    if (entry_export.empty())
        entry_export = roots.empty() ? "" : roots.front();
    const aev::pe::Export *entry = module.find_export(entry_export);
    if (!entry) {
        std::fprintf(stderr, "entry export not found: %s\n", entry_export.c_str());
        return 1;
    }
    module.primary_export = entry_export;
    module.entry_va = module.load_base + entry->rva;

    std::vector<aev::FunctionSpec> specs;
    if (!aev::pe::discover_functions(module, roots, &specs, &error)) {
        std::fprintf(stderr, "code discovery failed: %s\n", error.c_str());
        return 1;
    }
    aev::Program program;
    if (!aev::pe::build_program_for_module(module, specs, &program, &error)) {
        std::fprintf(stderr, "program setup failed: %s\n", error.c_str());
        return 1;
    }
    if (!program.find_function_by_name(entry_export)) {
        std::fprintf(stderr, "entry function not in translated program: %s\n",
                     entry_export.c_str());
        return 1;
    }
    if (!apply_coverage(program.functions, opt.aot_include, opt.aot_exclude, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    uint8_t function_name_sha256[32];
    aev::sha256(entry_export.data(), entry_export.size(), function_name_sha256);
    aev::AotModuleIdentity identity;
    identity.module_kind = AEV_MODULE_KIND_PE64;
    identity.module_file_size = module.file_size;
    identity.preferred_base = module.preferred_base;
    identity.load_base = module.load_base;
    identity.image_size = module.size_of_image;
    std::memcpy(identity.image_sha256, module.image_sha256, 32);
    std::memcpy(identity.import_binding_sha256, module.import_binding_sha256, 32);
    std::memcpy(identity.host_binding_sha256, module.host_binding_sha256, 32);
    std::memcpy(identity.entry_exports_sha256, module.entry_exports_sha256, 32);
    identity.thunk_base = module.thunk_base;
    identity.import_count = module.unique_imports.size();
    identity.import_thunks = module.import_thunks;
    identity.relocs_stripped = module.relocs_stripped ? 1 : 0;

    std::string ll;
    if (!aev::emit_aot_module(program, module.file_fnv, module.file_sha256,
                              module.entry_va, function_name_sha256, identity,
                              &ll, &error)) {
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
        out << "#define AEV_LOAD_BASE 0x" << std::hex << module.load_base << "ULL\n";
        out << "#define AEV_ENTRY_ADDR 0x" << std::hex << module.entry_va << "ULL\n";
        for (const auto &fn : program.functions)
            out << "#define AEV_ADDR_" << sanitize(fn.name) << " 0x" << std::hex
                << fn.address << "ULL\n";
    }

    size_t aot_blocks = 0, jit_blocks = 0, all_block_count = 0;
    for (const auto &fn : program.functions) {
        for (const auto &b : fn.blocks) {
            ++all_block_count;
            if (b.jit_only) ++jit_blocks; else ++aot_blocks;
        }
    }
    std::vector<std::string> entry_exports;
    for (const auto &e : module.exports)
        entry_exports.push_back(e.name);
    std::sort(entry_exports.begin(), entry_exports.end());

    if (!opt.out_manifest.empty()) {
        std::ofstream out(opt.out_manifest);
        if (!out) {
            std::fprintf(stderr, "cannot write %s\n", opt.out_manifest.c_str());
            return 1;
        }
        out << "{\n";
        out << "  \"input\": \"" << json_escape(opt.pe) << "\",\n";
        out << "  \"input_sha256\": \"" << hex_bytes32(module.file_sha256) << "\",\n";
        out << "  \"input_hash\": " << module.file_fnv << ",\n";
        out << "  \"module_kind\": \"" << AEV_MODULE_KIND_PE64 << "\",\n";
        out << "  \"module_file_sha256\": \"" << hex_bytes32(module.file_sha256) << "\",\n";
        out << "  \"module_file_size\": " << module.file_size << ",\n";
        out << "  \"preferred_base\": " << module.preferred_base << ",\n";
        out << "  \"load_base\": " << module.load_base << ",\n";
        out << "  \"image_size\": " << module.size_of_image << ",\n";
        out << "  \"image_sha256\": \"" << hex_bytes32(module.image_sha256) << "\",\n";
        out << "  \"import_binding_sha256\": \"" << hex_bytes32(module.import_binding_sha256) << "\",\n";
        out << "  \"host_binding_sha256\": \"" << hex_bytes32(module.host_binding_sha256) << "\",\n";
        out << "  \"entry_exports_sha256\": \"" << hex_bytes32(module.entry_exports_sha256) << "\",\n";
        out << "  \"entry_exports\": [";
        for (size_t i = 0; i < entry_exports.size(); ++i) {
            if (i) out << ", ";
            out << "\"" << json_escape(entry_exports[i]) << "\"";
        }
        out << "],\n";
        out << "  \"entry_export\": \"" << json_escape(entry_export) << "\",\n";
        out << "  \"entry\": \"" << json_escape(entry_export) << "\",\n";
        out << "  \"entry_function_sha256\": \"" << hex_bytes32(function_name_sha256) << "\",\n";
        out << "  \"entry_address\": " << module.entry_va << ",\n";
        out << "  \"thunk_base\": " << module.thunk_base << ",\n";
        out << "  \"import_count\": " << module.unique_imports.size() << ",\n";
        out << "  \"registry_sha256\": \"" << registry.file_sha256() << "\",\n";
        out << "  \"imports\": [";
        for (size_t i = 0; i < module.unique_imports.size(); ++i) {
            const auto &imp = module.unique_imports[i];
            if (i) out << ", ";
            out << "{\"module\": \"" << json_escape(imp.module)
                << "\", \"symbol\": \"" << json_escape(imp.symbol)
                << "\", \"thunk_va\": " << imp.thunk_va
                << ", \"thunk_index\": " << imp.thunk_index
                << ", \"iat_rva\": " << imp.iat_rva
                << ", \"host_import_id\": " << imp.host_import_id
                << ", \"args\": " << imp.args
                << ", \"abi\": \"" << json_escape(imp.abi) << "\"}";
        }
        out << "],\n";
        out << "  \"iat_slots\": [";
        for (size_t i = 0; i < module.imports.size(); ++i) {
            const auto &imp = module.imports[i];
            if (i) out << ", ";
            out << "{\"module\": \"" << json_escape(imp.module)
                << "\", \"symbol\": \"" << json_escape(imp.symbol)
                << "\", \"iat_rva\": " << imp.iat_rva
                << ", \"thunk_va\": " << imp.thunk_va << "}";
        }
        out << "],\n";
        out << "  \"relocs_stripped\": " << (module.relocs_stripped ? 1 : 0) << ",\n";
        out << "  \"relocation_count\": " << module.relocations.size() << ",\n";
        out << "  \"relocation_delta\": " << (module.load_base - module.preferred_base) << ",\n";
        out << "  \"pdata_count\": " << module.pdata.size() << ",\n";
        out << "  \"offset_kind\": \"rva\",\n";
        out << "  \"artifact_abi\": " << AEV_ARTIFACT_ABI_VERSION << ",\n";
        out << "  \"translator_abi\": " << AEV_TRANSLATOR_ABI_VERSION << ",\n";
        out << "  \"target\": \"" << AEV_AOT_TARGET_TRIPLE << "\",\n";
        out << "  \"all_block_count\": " << all_block_count << ",\n";
        out << "  \"aot_block_count\": " << aot_blocks << ",\n";
        out << "  \"jit_block_count\": " << jit_blocks << ",\n";
        out << "  \"sections\": [";
        for (size_t i = 0; i < module.sections.size(); ++i) {
            const auto &s = module.sections[i];
            if (i) out << ", ";
            out << "{\"name\": \"" << json_escape(s.name)
                << "\", \"rva\": " << s.virtual_address
                << ", \"virtual_size\": " << s.effective_virtual_size
                << ", \"raw_size\": " << s.raw_size
                << ", \"mapped_size\": " << s.mapped_size
                << ", \"prot\": " << ((s.readable ? 1 : 0) | (s.writable ? 2 : 0) |
                                      (s.executable ? 4 : 0)) << "}";
        }
        out << "],\n";
        out << "  \"exports\": [";
        for (size_t i = 0; i < module.exports.size(); ++i) {
            const auto &e = module.exports[i];
            if (i) out << ", ";
            out << "{\"name\": \"" << json_escape(e.name)
                << "\", \"rva\": " << e.rva
                << ", \"va\": " << (module.load_base + e.rva)
                << ", \"ordinal\": " << e.ordinal << "}";
        }
        out << "],\n";
        out << "  \"ordinal_only_exports\": [";
        for (size_t i = 0; i < module.ordinal_only_exports.size(); ++i) {
            const auto &e = module.ordinal_only_exports[i];
            if (i) out << ", ";
            out << "{\"rva\": " << e.rva << ", \"ordinal\": " << e.ordinal << "}";
        }
        out << "],\n";
        out << "  \"functions\": [\n";
        for (size_t fi = 0; fi < program.functions.size(); ++fi) {
            const auto &fn = program.functions[fi];
            const auto &spec = specs[fi];
            out << "    {\"name\": \"" << json_escape(fn.name)
                << "\", \"address\": " << fn.address
                << ", \"rva\": " << spec.rva
                << ", \"bytes\": " << fn.bytes.size()
                << ", \"pdata_begin\": " << spec.pdata_begin
                << ", \"pdata_end\": " << spec.pdata_end
                << ", \"code_size\": " << spec.size
                << ", \"blocks\": [";
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
                    << ", \"rva\": " << id.section_offset
                    << ", \"section_offset\": " << id.section_offset
                    << ", \"jit_only\": " << (b.jit_only ? "true" : "false")
                    << ", \"size\": " << id.size
                    << ", \"sha256\": \"" << aev::hex_bytes(id.sha256, 32) << "\""
                    << ", \"reason\": \"" << json_escape(b.jit_reason) << "\"}";
            }
            out << "]}" << (fi + 1 == program.functions.size() ? "" : ",") << "\n";
        }
        out << "  ]\n}\n";
    }
    std::printf("translated pe=%s entry=%s entry_addr=0x%llx load_base=0x%llx "
                "functions=%zu aot_blocks=%zu jit_blocks=%zu imports=%zu relocs=%zu\n",
                opt.pe.c_str(), entry_export.c_str(),
                (unsigned long long)module.entry_va,
                (unsigned long long)module.load_base, program.functions.size(),
                aot_blocks, jit_blocks, module.unique_imports.size(),
                module.relocations.size());
    std::printf("input_sha256=%s image_sha256=%s\n",
                aev::hex_bytes(module.file_sha256, 32).c_str(),
                aev::hex_bytes(module.image_sha256, 32).c_str());
    return 0;
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
        else if (a == "--pe")
            opt.pe = need("--pe");
        else if (a == "--function")
            opt.functions.push_back(need("--function"));
        else if (a == "--export")
            opt.exports.push_back(need("--export"));
        else if (a == "--entry")
            opt.entry = need("--entry");
        else if (a == "--registry")
            opt.registry = need("--registry");
        else if (a == "--load-base")
            opt.load_base = need("--load-base");
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

    if (!opt.pe.empty()) {
        if (opt.registry.empty() || opt.load_base.empty() || opt.out_ll.empty() ||
            opt.out_header.empty()) {
            usage(argv[0]);
            return 2;
        }
        if (opt.entry.empty() && !opt.exports.empty())
            opt.entry = opt.exports.front();
        return run_pe(opt);
    }
    if (opt.input.empty() || opt.functions.empty() || opt.out_ll.empty() ||
        opt.out_header.empty()) {
        usage(argv[0]);
        return 2;
    }
    if (opt.entry.empty())
        opt.entry = opt.functions[0];
    return run_coff(opt);
}
