#include "aev_guest.h"

#include <stdio.h>
#include <string.h>

static uint64_t width_mask(uint32_t width) {
    return width >= 8 ? UINT64_MAX : ((1ULL << (width * 8)) - 1ULL);
}

static uint64_t width_sign(uint32_t width) {
    return 1ULL << (width * 8 - 1);
}

static void set_flag(GuestState *s, uint64_t bit, int value) {
    if (value)
        s->rflags |= bit;
    else
        s->rflags &= ~bit;
}

static int parity_even(uint64_t v) {
    return __builtin_parity((unsigned)(v & 0xffu)) == 0;
}

static void set_zsp(GuestState *s, uint32_t width, uint64_t r) {
    uint64_t mask = width_mask(width);
    uint64_t sign = width_sign(width);
    r &= mask;
    set_flag(s, AEV_ZF, r == 0);
    set_flag(s, AEV_SF, (r & sign) != 0);
    set_flag(s, AEV_PF, parity_even(r));
}

static void memory_error(GuestState *s, uint64_t addr, uint32_t size) {
    if (!s->error_code) {
        s->error_code = AEV_ERR_MEMORY;
        s->error_pc = s->rip;
        fprintf(stderr,
                "AOT memory error: addr=0x%llx size=%u base=0x%llx mem_size=0x%llx rip=0x%llx\n",
                (unsigned long long)addr, size,
                (unsigned long long)s->mem_base,
                (unsigned long long)s->mem_size,
                (unsigned long long)s->rip);
    }
}

static int range_ok(const GuestState *s, uint64_t addr, uint32_t size) {
    if (size == 0 || size > 8)
        return 0;
    if (addr < s->mem_base)
        return 0;
    uint64_t off = addr - s->mem_base;
    if (off > s->mem_size)
        return 0;
    if (s->mem_size - off < size)
        return 0;
    return 1;
}

uint64_t aev_load(GuestState *s, uint64_t addr, uint32_t size, uint32_t sign_extend) {
    if (!range_ok(s, addr, size)) {
        memory_error(s, addr, size);
        return 0;
    }
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
    uint64_t v = 0;
    memcpy(&v, p, size); /* host is little-endian ARM64; x86 is little-endian */
    if (sign_extend && size < 8) {
        uint32_t bits = size * 8;
        uint64_t sign = 1ULL << (bits - 1);
        if (v & sign)
            v |= ~width_mask(size);
    }
    return v;
}

void aev_store(GuestState *s, uint64_t addr, uint64_t value, uint32_t size) {
    if (!range_ok(s, addr, size)) {
        memory_error(s, addr, size);
        return;
    }
    uint8_t *p = (uint8_t *)(uintptr_t)addr;
    memcpy(p, &value, size);
}

void aev_flags_add(GuestState *s, uint32_t width, uint64_t a, uint64_t b, uint64_t r) {
    uint64_t mask = width_mask(width);
    uint64_t sign = width_sign(width);
    uint64_t ua = a & mask, ub = b & mask, ur = r & mask;
    set_flag(s, AEV_CF, ur < ua);
    set_flag(s, AEV_OF, ((~(ua ^ ub) & (ua ^ ur)) & sign) != 0);
    set_flag(s, AEV_AF, ((ua ^ ub ^ ur) & 0x10ULL) != 0);
    set_zsp(s, width, ur);
}

void aev_flags_sub(GuestState *s, uint32_t width, uint64_t a, uint64_t b, uint64_t r) {
    uint64_t mask = width_mask(width);
    uint64_t sign = width_sign(width);
    uint64_t ua = a & mask, ub = b & mask, ur = r & mask;
    set_flag(s, AEV_CF, ua < ub);
    set_flag(s, AEV_OF, (((ua ^ ub) & (ua ^ ur)) & sign) != 0);
    set_flag(s, AEV_AF, ((ua ^ ub ^ ur) & 0x10ULL) != 0);
    set_zsp(s, width, ur);
}

void aev_flags_logic(GuestState *s, uint32_t width, uint64_t r) {
    set_flag(s, AEV_CF, 0);
    set_flag(s, AEV_OF, 0);
    set_flag(s, AEV_AF, 0);
    set_zsp(s, width, r);
}

void aev_flags_inc(GuestState *s, uint32_t width, uint64_t a, uint64_t r) {
    uint64_t mask = width_mask(width);
    uint64_t sign = width_sign(width);
    uint64_t ua = a & mask, ur = r & mask;
    set_flag(s, AEV_OF, ur == sign);
    set_flag(s, AEV_AF, ((ua ^ 1ULL ^ ur) & 0x10ULL) != 0);
    set_zsp(s, width, ur);
}

void aev_flags_dec(GuestState *s, uint32_t width, uint64_t a, uint64_t r) {
    uint64_t mask = width_mask(width);
    uint64_t sign = width_sign(width);
    uint64_t ua = a & mask, ur = r & mask;
    set_flag(s, AEV_OF, ua == sign);
    set_flag(s, AEV_AF, ((ua ^ 1ULL ^ ur) & 0x10ULL) != 0);
    set_zsp(s, width, ur);
}

void aev_flags_neg(GuestState *s, uint32_t width, uint64_t a, uint64_t r) {
    uint64_t mask = width_mask(width);
    uint64_t sign = width_sign(width);
    uint64_t ua = a & mask, ur = r & mask;
    set_flag(s, AEV_CF, ua != 0);
    set_flag(s, AEV_OF, ua == sign);
    set_flag(s, AEV_AF, ((0ULL ^ ua ^ ur) & 0x10ULL) != 0);
    set_zsp(s, width, ur);
}

void aev_flags_imul(GuestState *s, uint32_t width, uint64_t a, uint64_t b, uint64_t r) {
    uint32_t bits = width * 8;
    uint64_t mask = width_mask(width);
    a &= mask;
    b &= mask;
    __int128 sa = (__int128)(int64_t)(a << (64 - bits)) >> (64 - bits);
    __int128 sb = (__int128)(int64_t)(b << (64 - bits)) >> (64 - bits);
    __int128 full = sa * sb;
    __int128 low = (__int128)(r & mask);
    __int128 ext;
    if (bits < 64) {
        ext = (__int128)(int64_t)((uint64_t)low << (64 - bits)) >> (64 - bits);
    } else {
        ext = low;
    }
    set_flag(s, AEV_CF, full != ext);
    set_flag(s, AEV_OF, full != ext);
    /* ZF/SF/PF are architecturally undefined for imul; set them to zero. */
    set_flag(s, AEV_ZF, 0);
    set_flag(s, AEV_SF, 0);
    set_flag(s, AEV_PF, 0);
}

void aev_note_aot(GuestState *s, uint64_t rip) {
    s->aot_blocks++;
    if (s->trace_enabled)
        fprintf(stderr, "AOT  rip=0x%llx\n", (unsigned long long)rip);
}
