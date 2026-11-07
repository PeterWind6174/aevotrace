#include "aev_artifact.h"
#include "aev_guest.h"
#include "pe_session_core.hpp"
#include "program_identity.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace pe = aev::pe;

namespace {

enum PeRunStatus {
    PE_OK = 0,
    PE_INVALID_ARGUMENT = 2,
    PE_LOAD = 3,
    PE_ABI_MISMATCH = 4,
    PE_UNKNOWN_IMPORT = 5,
    PE_MEMORY = 6,
    PE_JIT = 7,
    PE_STEP_LIMIT = 8,
    PE_STALE_HANDLE = 9,
    PE_REENTRY = 10,
    PE_HOST = 11,
    PE_INTERNAL = 12
};

const char *status_name(int status) {
    return pe::session_status_name((uint32_t)status);
}

std::string json_escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string hex(uint64_t v) { return aev::hex64(v); }

struct Options {
    std::string dll;
    std::string registry;
    std::string aot;
    std::string load_base;
    std::string entry_export;
    std::string trace;
    std::string scenario;
    std::string dump_image;
    std::string dump_blocks;
    std::string dump_relocations;
    std::vector<std::string> calls;
    int reload = 1;
    bool info = false;
    bool expect_error = false;
};

struct CallSpec {
    std::string export_name;
    uint64_t args[4] = {0, 0, 0, 0};
};

std::string status_json(int status) {
    std::ostringstream os;
    os << "\"status\":" << status << ",\"status_name\":\""
       << status_name(status) << "\"";
    return os.str();
}

bool parse_u64(const std::string &text, uint64_t *out) {
    if (text.empty()) return false;
    char *end = nullptr;
    unsigned long long value = std::strtoull(text.c_str(), &end, 0);
    if (!end || *end != 0) return false;
    *out = (uint64_t)value;
    return true;
}

bool parse_args(const std::string &text, uint64_t args[4]) {
    if (text.empty()) return true;
    size_t start = 0;
    int index = 0;
    while (start <= text.size()) {
        size_t comma = text.find(',', start);
        std::string part = comma == std::string::npos
                               ? text.substr(start)
                               : text.substr(start, comma - start);
        if (part.empty() || index >= 4) return false;
        char *end = nullptr;
        long long value = std::strtoll(part.c_str(), &end, 0);
        if (!end || *end != 0) return false;
        args[index++] = (uint64_t)value;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return true;
}

bool parse_call(const std::string &text, CallSpec *out) {
    if (!out) return false;
    size_t colon = text.find(':');
    out->export_name = colon == std::string::npos ? text : text.substr(0, colon);
    if (out->export_name.empty()) return false;
    if (colon == std::string::npos) return true;
    return parse_args(text.substr(colon + 1), out->args);
}

void print_module_json(std::ostringstream &os, const pe::Session &session,
                       bool include_blocks) {
    const pe::Module &m = session.module();
    os << "\"module\":{";
    os << "\"kind\":\"" << AEV_MODULE_KIND_PE64 << "\"";
    os << ",\"file_sha256\":\"" << aev::hex_bytes(m.file_sha256, 32) << "\"";
    os << ",\"file_size\":" << m.file_size;
    os << ",\"file_fnv\":\"" << hex(m.file_fnv) << "\"";
    os << ",\"preferred_base\":\"" << hex(m.preferred_base) << "\"";
    os << ",\"load_base\":\"" << hex(m.load_base) << "\"";
    os << ",\"image_size\":" << m.size_of_image;
    os << ",\"image_sha256\":\"" << aev::hex_bytes(m.image_sha256, 32) << "\"";
    os << ",\"import_binding_sha256\":\""
       << aev::hex_bytes(m.import_binding_sha256, 32) << "\"";
    os << ",\"host_binding_sha256\":\""
       << aev::hex_bytes(m.host_binding_sha256, 32) << "\"";
    os << ",\"entry_exports_sha256\":\""
       << aev::hex_bytes(m.entry_exports_sha256, 32) << "\"";
    os << ",\"thunk_base\":\"" << hex(m.thunk_base) << "\"";
    os << ",\"import_count\":" << m.unique_imports.size();
    os << ",\"relocs_stripped\":" << (m.relocs_stripped ? 1 : 0);
    os << ",\"entry_export\":\"" << json_escape(session.entry_export()) << "\"";
    os << ",\"entry_va\":\"" << hex(m.entry_va) << "\"";
    os << ",\"stack_base\":\"" << hex(m.stack_base) << "\"";
    os << ",\"stack_top\":\"" << hex(m.stack_top) << "\"";
    os << ",\"entry_rsp\":\"" << hex(m.entry_rsp) << "\"";
    os << ",\"relocation_count\":" << m.relocations.size();
    os << ",\"relocation_delta\":\"" << hex(m.load_base - m.preferred_base) << "\"";
    os << ",\"pdata_count\":" << m.pdata.size();
    os << "}";

    os << ",\"sections\":[";
    for (size_t i = 0; i < m.sections.size(); ++i) {
        const auto &s = m.sections[i];
        if (i) os << ",";
        os << "{\"name\":\"" << json_escape(s.name)
           << "\",\"rva\":" << s.virtual_address
           << ",\"virtual_size\":" << s.effective_virtual_size
           << ",\"raw_size\":" << s.raw_size
           << ",\"mapped_size\":" << s.mapped_size
           << ",\"prot\":" << ((s.readable ? 1 : 0) | (s.writable ? 2 : 0) |
                               (s.executable ? 4 : 0)) << "}";
    }
    os << "]";

    os << ",\"exports\":[";
    for (size_t i = 0; i < m.exports.size(); ++i) {
        const auto &e = m.exports[i];
        if (i) os << ",";
        os << "{\"name\":\"" << json_escape(e.name) << "\",\"rva\":" << e.rva
           << ",\"va\":\"" << hex(m.load_base + e.rva) << "\",\"ordinal\":"
           << e.ordinal << "}";
    }
    os << "]";
    os << ",\"ordinal_only_exports\":[";
    for (size_t i = 0; i < m.ordinal_only_exports.size(); ++i) {
        const auto &e = m.ordinal_only_exports[i];
        if (i) os << ",";
        os << "{\"rva\":" << e.rva << ",\"ordinal\":" << e.ordinal << "}";
    }
    os << "]";

    os << ",\"imports\":[";
    for (size_t i = 0; i < m.unique_imports.size(); ++i) {
        const auto &imp = m.unique_imports[i];
        if (i) os << ",";
        os << "{\"module\":\"" << json_escape(imp.module)
           << "\",\"symbol\":\"" << json_escape(imp.symbol)
           << "\",\"thunk_va\":\"" << hex(imp.thunk_va)
           << "\",\"thunk_index\":" << imp.thunk_index
           << ",\"iat_rva\":" << imp.iat_rva
           << ",\"host_import_id\":" << imp.host_import_id
           << ",\"args\":" << imp.args
           << ",\"abi\":\"" << json_escape(imp.abi) << "\"}";
    }
    os << "]";

    os << ",\"host_imports\":[";
    for (size_t i = 0; i < m.unique_imports.size(); ++i) {
        const auto &imp = m.unique_imports[i];
        if (i) os << ",";
        os << "{\"id\":" << imp.host_import_id
           << ",\"thunk_index\":" << imp.thunk_index
           << ",\"module\":\"" << json_escape(imp.module)
           << "\",\"symbol\":\"" << json_escape(imp.symbol) << "\"}";
    }
    os << "]";
    os << ",\"relocations\":{\"count\":" << m.relocations.size()
       << ",\"delta\":\"" << hex(m.load_base - m.preferred_base)
       << "\",\"stripped\":" << (m.relocs_stripped ? 1 : 0);
    if (m.relocations.empty()) {
        os << ",\"first\":null,\"last\":null}";
    } else {
        os << ",\"first\":\"" << hex(m.load_base + m.relocations.front().rva)
           << "\",\"last\":\"" << hex(m.load_base + m.relocations.back().rva)
           << "\"}";
    }

    os << ",\"regions\":[";
    for (size_t i = 0; i < m.regions.size(); ++i) {
        const auto &r = m.regions[i];
        if (i) os << ",";
        os << "{\"guest_base\":\"" << hex(r.guest_base)
           << "\",\"size\":" << r.size << ",\"prot\":" << r.prot << "}";
    }
    os << "]";

    os << ",\"aot\":{\"present\":"
       << (session.aot_present() ? "true" : "false");
    os << ",\"sha256\":\"" << aev::hex_bytes(session.artifact_sha256(), 32)
       << "\"";
    os << ",\"selected_count\":" << session.selected_blocks().size();
    os << ",\"all_count\":" << session.all_blocks().size();
    os << "}";

    if (include_blocks) {
        os << ",\"blocks\":[";
        std::set<uint64_t> selected(session.selected_blocks().begin(),
                                    session.selected_blocks().end());
        bool first = true;
        for (uint64_t rip : session.all_blocks()) {
            aev::BlockIdentity id;
            std::string error;
            if (!aev::block_identity(session.program(), rip, &id, &error))
                continue;
            if (!first) os << ",";
            first = false;
            os << "{\"rip\":\"" << hex(rip) << "\",\"rva\":" << id.section_offset
               << ",\"size\":" << id.size
               << ",\"sha256\":\"" << aev::hex_bytes(id.sha256, 32)
               << "\",\"aot_selected\":"
               << (selected.count(rip) ? "true" : "false") << "}";
        }
        os << "]";
    }
}

void print_result_json(std::ostringstream &os, const CallSpec &spec,
                       const pe::SessionCallResult &r) {
    os << "{\"export\":\"" << json_escape(spec.export_name) << "\""
       << ",\"args\":[" << r.args[0] << "," << r.args[1] << ","
       << r.args[2] << "," << r.args[3] << "]"
       << "," << status_json((int)r.status)
       << ",\"rax\":" << (int64_t)r.rax
       << ",\"guest_error\":" << r.guest_error
       << ",\"error_pc\":\"" << hex(r.error_pc) << "\""
       << ",\"mem_fault_kind\":" << r.mem_fault_kind
       << ",\"mem_fault_addr\":\"" << hex(r.mem_fault_addr) << "\""
       << ",\"mem_fault_size\":" << r.mem_fault_size
       << ",\"mem_access\":" << r.mem_access
       << ",\"aot_blocks\":" << r.aot_blocks
       << ",\"jit_blocks\":" << r.jit_blocks
       << ",\"jit_compiles\":" << r.jit_compiles
       << ",\"jit_execs\":" << r.jit_execs
       << ",\"steps\":" << r.steps
       << ",\"rip\":\"" << hex(r.guest_rip) << "\""
       << ",\"rsp\":\"" << hex(r.guest_rsp) << "\"}";
}

void usage(const char *argv0) {
    std::fprintf(stderr,
                 "usage: %s --dll <path> --registry <path> [--load-base <hex>]\n"
                 "  [--aot <dylib>] [--entry-export <name>]\n"
                 "  [--call <export>[:a0,a1,a2,a3]]... [--reload N]\n"
                 "  [--expect-error] [--trace <path>] [--scenario <name>]\n"
                 "  [--info] [--dump-image <path>] [--dump-blocks <path>]\n"
                 "  [--dump-relocations <path>]\n",
                 argv0);
}

int emit_load_failure(std::ostringstream &out, const std::string &error,
                      bool expect_error, int failure_status = PE_LOAD) {
    out << status_json(failure_status) << ",\"error\":\""
        << json_escape(error) << "\",\"expected_error\":"
        << (expect_error ? "true" : "false")
        << ",\"any_failure\":true,\"failure_status\":" << failure_status;
    std::cout << out.str() << "}\n";
    return expect_error ? 0 : failure_status;
}

}  // namespace

int main(int argc, char **argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char *what) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                std::exit(2);
            }
            return std::string(argv[++i]);
        };
        if (a == "--dll") opt.dll = need("--dll");
        else if (a == "--registry") opt.registry = need("--registry");
        else if (a == "--aot") opt.aot = need("--aot");
        else if (a == "--load-base") opt.load_base = need("--load-base");
        else if (a == "--entry-export") opt.entry_export = need("--entry-export");
        else if (a == "--call") opt.calls.push_back(need("--call"));
        else if (a == "--reload") opt.reload = std::max(1, std::atoi(need("--reload").c_str()));
        else if (a == "--trace") opt.trace = need("--trace");
        else if (a == "--scenario") opt.scenario = need("--scenario");
        else if (a == "--dump-image") opt.dump_image = need("--dump-image");
        else if (a == "--dump-blocks") opt.dump_blocks = need("--dump-blocks");
        else if (a == "--dump-relocations") opt.dump_relocations = need("--dump-relocations");
        else if (a == "--info") opt.info = true;
        else if (a == "--expect-error") opt.expect_error = true;
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); usage(argv[0]); return 2; }
    }
    if (opt.dll.empty() || opt.registry.empty()) {
        usage(argv[0]);
        return 2;
    }
    if (!opt.info && opt.calls.empty()) {
        std::fprintf(stderr, "at least one --call is required unless --info\n");
        return 2;
    }
    if (opt.info && (opt.expect_error || opt.reload != 1)) {
        std::fprintf(stderr, "--info cannot be combined with --expect-error/--reload\n");
        return 2;
    }
    if (opt.aot.empty() && opt.load_base.empty()) {
        std::fprintf(stderr, "--load-base is required without --aot\n");
        return 2;
    }
    if (!opt.aot.empty() && opt.entry_export.empty()) {
        std::fprintf(stderr, "--entry-export is required with --aot\n");
        return 2;
    }
    std::vector<CallSpec> calls;
    for (const auto &text : opt.calls) {
        CallSpec spec;
        if (!parse_call(text, &spec)) {
            std::fprintf(stderr, "invalid --call: %s\n", text.c_str());
            return 2;
        }
        calls.push_back(spec);
    }

    uint64_t load_base = 0;
    if (!opt.load_base.empty() && !parse_u64(opt.load_base, &load_base)) {
        std::ostringstream out;
        out << "{";
        return emit_load_failure(out, "invalid --load-base", opt.expect_error);
    }

    std::ostringstream out;
    out << "{";
    bool first_iteration = true;
    uint64_t first_token = 0;
    int first_failure = PE_OK;
    bool any_failure = false;
    bool loaded_once = false;

    for (int iteration = 0; iteration < opt.reload; ++iteration) {
        pe::SessionConfig config;
        config.dll_path = opt.dll;
        config.registry_path = opt.registry;
        config.aot_path = opt.aot;
        config.entry_export = opt.entry_export;
        config.load_base = load_base;
        config.trace_path = opt.trace;
        config.scenario = opt.scenario;

        auto session = std::make_unique<pe::Session>();
        std::string error;
        pe::SessionStatus load_status = session->load(config, &error);
        if (load_status != pe::SessionStatus::Ok) {
            /* The runner's historical contract reports every load-stage
             * failure as status=3 LOAD; the C ABI exposes the finer class. */
            return emit_load_failure(out, error, opt.expect_error, PE_LOAD);
        }

        uint64_t token =
            pe::SessionTable::instance().add(std::move(session));
        if (token == 0) {
            return emit_load_failure(out, "handle table full",
                                     opt.expect_error, PE_INVALID_ARGUMENT);
        }
        if (first_token == 0) first_token = token;
        std::shared_ptr<pe::Session> inst =
            pe::SessionTable::instance().get(token);
        if (!inst) {
            return emit_load_failure(out, "session handle lost",
                                     opt.expect_error, PE_INTERNAL);
        }
        loaded_once = true;

        /* Preserve the historical --reload trace call-number offset without
         * changing the C ABI trace contract (which resets to 1 on open). */
        if (!opt.trace.empty() && iteration > 0)
            inst->set_trace_call_base((uint64_t)iteration * 100000);

        if (!first_iteration) out << ",";
        first_iteration = false;
        out << "\"iteration_" << iteration << "\":{";
        print_module_json(out, *inst, opt.info);

        std::vector<pe::SessionCallResult> results;
        bool failed = false;
        for (const auto &spec : calls) {
            pe::SessionCallResult r;
            std::string call_error;
            inst->call(spec.export_name, spec.args, &r, &call_error);
            results.push_back(r);
            if (r.status != PE_OK) {
                failed = true;
                if (first_failure == PE_OK) first_failure = (int)r.status;
            }
        }
        out << ",\"calls\":[";
        for (size_t i = 0; i < results.size(); ++i) {
            if (i) out << ",";
            print_result_json(out, calls[i], results[i]);
        }
        out << "]";
        out << ",\"host_import_calls\":" << inst->counts().host_import_calls;
        out << "}";

        if (failed) any_failure = true;

        if (!opt.dump_image.empty()) {
            const auto &image = inst->module().image;
            std::ofstream f(opt.dump_image, std::ios::binary);
            f.write(reinterpret_cast<const char *>(image.data()),
                    (std::streamsize)image.size());
        }
        if (!opt.dump_blocks.empty()) {
            std::ofstream f(opt.dump_blocks);
            f << "[";
            bool first = true;
            std::set<uint64_t> selected(inst->selected_blocks().begin(),
                                        inst->selected_blocks().end());
            for (const auto &fn : inst->program().functions) {
                for (const auto &b : fn.blocks) {
                    aev::BlockIdentity id;
                    std::string e;
                    if (!aev::block_identity(inst->program(), b.address, &id, &e))
                        continue;
                    if (!first) f << ",";
                    first = false;
                    f << "{\"rip\":\"" << hex(b.address) << "\",\"rva\":"
                      << id.section_offset << ",\"size\":" << id.size
                      << ",\"sha256\":\"" << aev::hex_bytes(id.sha256, 32)
                      << "\",\"aot_selected\":"
                      << (selected.count(b.address) ? "true" : "false") << "}";
                }
            }
            f << "]\n";
        }
        if (!opt.dump_relocations.empty()) {
            const pe::Module &m = inst->module();
            std::ofstream f(opt.dump_relocations);
            f << "{\"count\":" << m.relocations.size()
              << ",\"delta\":" << (m.load_base - m.preferred_base)
              << ",\"stripped\":" << (m.relocs_stripped ? 1 : 0)
              << ",\"sites\":[";
            for (size_t i = 0; i < m.relocations.size(); ++i) {
                if (i) f << ",";
                f << "{\"rva\":" << m.relocations[i].rva
                  << ",\"type\":" << m.relocations[i].type
                  << ",\"va\":\"" << hex(m.load_base + m.relocations[i].rva)
                  << "\"}";
            }
            f << "]}\n";
        }

        std::string destroy_error;
        pe::SessionStatus destroy_status =
            pe::SessionTable::instance().destroy(token, &destroy_error);
        if (destroy_status != pe::SessionStatus::Ok) {
            any_failure = true;
            if (first_failure == PE_OK) first_failure = (int)destroy_status;
        }
        if (iteration == 0 && opt.reload > 1) {
            std::shared_ptr<pe::Session> stale =
                pe::SessionTable::instance().get(first_token);
            out << ",\"stale_handle\":" << (stale ? PE_OK : PE_STALE_HANDLE)
                << ",\"stale_handle_status\":"
                << (stale ? PE_OK : PE_STALE_HANDLE);
        }
    }
    (void)loaded_once;

    int final_status = any_failure ? first_failure : PE_OK;
    out << ",\"reload_iterations\":" << opt.reload;
    out << ",\"expected_error\":" << (opt.expect_error ? "true" : "false");
    out << ",\"any_failure\":" << (any_failure ? "true" : "false");
    out << ",\"failure_status\":" << first_failure;
    out << ",\"status\":" << final_status << ",\"status_name\":\""
        << status_name(final_status) << "\"";
    out << "}\n";
    std::cout << out.str();
    if (opt.expect_error) {
        return any_failure ? 0 : 1;
    }
    return final_status;
}
