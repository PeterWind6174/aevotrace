#ifndef AEV_PE_HPP
#define AEV_PE_HPP

#include "aev_guest.h"
#include "translator.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace aev::pe {

constexpr uint64_t kMaxFileSize = 64ULL << 20;
constexpr uint64_t kMaxImageSize = 256ULL << 20;
constexpr uint32_t kMaxSections = 96;
constexpr uint32_t kMaxExports = 4096;
constexpr uint32_t kMaxPdata = 4096;
constexpr uint32_t kMaxRelocations = 1000000;
constexpr uint32_t kMaxImportDescriptors = 256;
constexpr uint32_t kMaxUniqueImports = 256;
constexpr uint32_t kMaxIatSlots = 4096;
constexpr uint32_t kMaxFunctions = 64;
constexpr uint64_t kMaxTotalCode = 1ULL << 20;
constexpr uint64_t kMaxFunctionCode = 256ULL << 10;
constexpr uint32_t kMaxName = 512;
constexpr uint64_t kThunkStride = 16;
constexpr uint64_t kStackSize = 1ULL << 20;

struct Section {
    std::string name;
    uint32_t virtual_address = 0;
    uint32_t virtual_size = 0;
    uint32_t raw_size = 0;
    uint32_t raw_ptr = 0;
    uint32_t characteristics = 0;
    uint32_t effective_virtual_size = 0;
    uint32_t mapped_size = 0;
    bool readable = false;
    bool writable = false;
    bool executable = false;
};

struct Export {
    std::string name;
    uint32_t rva = 0;
    uint32_t ordinal = 0;
};

struct OrdinalExport {
    uint32_t rva = 0;
    uint32_t ordinal = 0;
};

struct Import {
    std::string module;
    std::string symbol;
    uint32_t iat_rva = 0;
    uint64_t thunk_va = 0;
    uint64_t host_import_id = 0;
    uint32_t args = 0;
    std::string abi;
    uint32_t thunk_index = 0;
};

struct Relocation {
    uint32_t rva = 0;
    uint16_t type = 0;
};

struct PdataEntry {
    uint32_t begin = 0;
    uint32_t end = 0;
};

struct HostImportDecl {
    uint64_t id = 0;
    uint32_t args = 0;
    std::string abi;
};

class ImportRegistry {
  public:
    bool load(const std::string &path, std::string *error);
    bool resolve(const std::string &module, const std::string &symbol,
                 HostImportDecl *out) const;
    const std::vector<std::pair<std::pair<std::string, std::string>, HostImportDecl>> &
    entries() const {
        return entries_;
    }
    const uint8_t *binding_sha256() const { return binding_sha256_; }
    const std::string &file_sha256() const { return file_sha256_; }

  private:
    std::vector<std::pair<std::pair<std::string, std::string>, HostImportDecl>> entries_;
    uint8_t binding_sha256_[32] = {};
    std::string file_sha256_;
};

struct Module {
    Module() = default;
    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;
    Module(Module &&) = default;
    Module &operator=(Module &&) = default;

    std::vector<uint8_t> file;
    uint8_t file_sha256[32] = {};
    uint64_t file_fnv = 0;
    uint64_t file_size = 0;

    uint16_t machine = 0;
    uint16_t magic = 0;
    uint16_t characteristics = 0;
    uint32_t size_of_image = 0;
    uint32_t size_of_headers = 0;
    uint32_t section_alignment = 0;
    uint32_t file_alignment = 0;
    uint64_t preferred_base = 0;
    uint64_t load_base = 0;
    uint32_t entry_rva = 0;

    std::vector<Section> sections;
    std::vector<Export> exports;
    std::vector<OrdinalExport> ordinal_only_exports;
    std::vector<Import> imports;
    std::vector<Import> unique_imports;
    std::vector<Relocation> relocations;
    std::vector<PdataEntry> pdata;
    bool relocs_stripped = false;

    std::vector<uint8_t> image;      /* post-relocation + IAT, SizeOfImage */
    std::vector<uint8_t> thunk_host; /* runtime thunk backing */
    std::vector<uint8_t> stack_host; /* runtime stack backing */
    std::vector<AevMemRegion> regions;

    uint64_t thunk_base = 0;
    uint64_t stack_base = 0;
    uint64_t stack_top = 0;
    uint64_t entry_rsp = 0;
    std::vector<uint64_t> import_thunks;

    uint8_t image_sha256[32] = {};
    uint8_t import_binding_sha256[32] = {};
    uint8_t host_binding_sha256[32] = {};
    uint8_t entry_exports_sha256[32] = {};

    std::string primary_export;
    uint64_t entry_va = 0;

    const Section *section_for_rva(uint32_t rva) const;
    const Export *find_export(const std::string &name) const;
};

struct LoadOptions {
    uint64_t load_base = 0;
    uint64_t stack_size = kStackSize;
};

/* Parse, map, relocate and bind one PE32+ AMD64 DLL.  Any failure leaves no
 * usable Module and releases all backing storage. */
bool load_module(const std::string &path, const LoadOptions &options,
                 const ImportRegistry &registry, Module *module,
                 std::string *error);

/* Bounded code discovery from named exports.  Uses .pdata for function
 * extents and follows direct CALL/JMP targets inside the image. */
bool discover_functions(const Module &module,
                        const std::vector<std::string> &root_exports,
                        std::vector<FunctionSpec> *specs,
                        std::string *error);

/* Build the shared Program from the already loaded image.  The synthetic
 * CoffObject exposes the image as section 1 and uses the original DLL
 * SHA/FNV/size as content identity. */
bool build_program_for_module(Module &module,
                              const std::vector<FunctionSpec> &specs,
                              Program *program, std::string *error);

/* Helpers shared with tests/runner. */
bool rva_to_file(const Module &module, uint32_t rva, uint32_t len,
                 uint32_t *offset, std::string *error);
std::string hex_u64(uint64_t value);

}  // namespace aev::pe

#endif
