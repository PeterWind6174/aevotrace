#ifndef AEV_COFF_HPP
#define AEV_COFF_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace aev {

struct CoffSymbol {
    std::string name;
    uint32_t value = 0;
    int16_t section_number = 0;
    uint16_t type = 0;
    uint8_t storage_class = 0;
    uint8_t aux_count = 0;
};

struct CoffSection {
    std::string name;
    uint32_t virtual_size = 0;
    uint32_t virtual_address = 0;
    uint32_t raw_size = 0;
    uint32_t raw_ptr = 0;
    uint32_t characteristics = 0;
    std::vector<uint8_t> data;
};

class CoffObject {
  public:
    bool load(const std::string &path, std::string *error);
    const CoffSection *section(int index) const; /* 1-based COFF index */
    const CoffSymbol *find_symbol(const std::string &name) const;

    std::vector<CoffSection> sections;
    std::vector<CoffSymbol> symbols;
    uint16_t machine = 0;
    uint32_t timestamp = 0;
    /* 64-bit FNV-1a over the complete input file.  Used only as a
     * non-cryptographic identity check so a stale AOT artifact with the
     * same version cannot be silently paired with a different object. */
    uint64_t content_hash = 0;

  private:
    std::vector<uint8_t> file_;
};

}  // namespace aev

#endif
