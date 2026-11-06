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

/* Record the first fault.  Subsequent helper calls in the same block must
 * not overwrite the originating instruction/fault fields. */
static void mem_fault(GuestState *s, uint64_t kind, uint64_t addr,
                      uint64_t len, uint64_t access) {
    if (!s->error_code) {
        s->error_code = AEV_ERR_MEMORY;
        s->error_pc = (access == AEV_MEM_ACCESS_EXEC) ? addr : s->rip;
        s->mem_fault_kind = kind;
        s->mem_fault_addr = addr;
        s->mem_fault_size = len;
        s->mem_access = access;
        fprintf(stderr,
                "AOT memory error: kind=%llu addr=0x%llx size=%llu access=%llu "
                "rip=0x%llx\n",
                (unsigned long long)kind, (unsigned long long)addr,
                (unsigned long long)len, (unsigned long long)access,
                (unsigned long long)s->rip);
    }
}

/*
 * Locate a checked Guest range.  Returns 0 on success and stores the Host
 * backing pointer.  Otherwise returns an AEV_MEM_FAULT_* kind.  A range must
 * be contained in exactly one region; crossing a region boundary fails.
 * Without a region table, the historical COFF identity mode is used (RWX over
 * [mem_base, mem_base+mem_size)); PE modules always install a region table.
 */
static uint64_t locate_range(const GuestState *s, uint64_t addr, uint64_t len,
                             uint32_t need, uint64_t *host) {
    if (!s || !host || len == 0)
        return AEV_MEM_FAULT_SIZE;
    if (addr > UINT64_MAX - (len - 1))
        return AEV_MEM_FAULT_RANGE;

    if (s->reserved1 && s->reserved2) {
        const AevMemRegion *regions =
            (const AevMemRegion *)(uintptr_t)s->reserved1;
        uint64_t count = s->reserved2;
        for (uint64_t i = 0; i < count; ++i) {
            const AevMemRegion *r = &regions[i];
            if (r->size == 0 || r->guest_base > UINT64_MAX - (r->size - 1))
                continue;
            if (addr < r->guest_base)
                continue;
            uint64_t off = addr - r->guest_base;
            if (off >= r->size)
                continue;
            if (r->size - off < len)
                return AEV_MEM_FAULT_RANGE;
            uint32_t prot = r->prot;
            if ((need & AEV_MEM_PROT_READ) && !(prot & AEV_MEM_PROT_READ))
                return AEV_MEM_FAULT_READ;
            if ((need & AEV_MEM_PROT_WRITE) && !(prot & AEV_MEM_PROT_WRITE))
                return AEV_MEM_FAULT_WRITE;
            if ((need & AEV_MEM_PROT_EXEC) && !(prot & AEV_MEM_PROT_EXEC))
                return AEV_MEM_FAULT_EXEC;
            *host = r->host_base + off;
            return AEV_MEM_FAULT_NONE;
        }
        return AEV_MEM_FAULT_UNMAPPED;
    }

    /* Legacy COFF identity mode: single RWX region backed by mem_base. */
    if (s->mem_base && s->mem_size) {
        if (addr < s->mem_base)
            return AEV_MEM_FAULT_UNMAPPED;
        uint64_t off = addr - s->mem_base;
        if (off > s->mem_size || s->mem_size - off < len)
            return AEV_MEM_FAULT_UNMAPPED;
        *host = addr;
        return AEV_MEM_FAULT_NONE;
    }
    return AEV_MEM_FAULT_UNMAPPED;
}

uint64_t aev_load(GuestState *s, uint64_t addr, uint32_t size, uint32_t sign_extend) {
    if (size == 0 || size > 8) {
        mem_fault(s, AEV_MEM_FAULT_SIZE, addr, size, AEV_MEM_ACCESS_READ);
        return 0;
    }
    uint64_t host = 0;
    uint64_t kind = locate_range(s, addr, size, AEV_MEM_PROT_READ, &host);
    if (kind != AEV_MEM_FAULT_NONE) {
        mem_fault(s, kind, addr, size, AEV_MEM_ACCESS_READ);
        return 0;
    }
    const uint8_t *p = (const uint8_t *)(uintptr_t)host;
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
    if (size == 0 || size > 8) {
        mem_fault(s, AEV_MEM_FAULT_SIZE, addr, size, AEV_MEM_ACCESS_WRITE);
        return;
    }
    uint64_t host = 0;
    uint64_t kind = locate_range(s, addr, size, AEV_MEM_PROT_WRITE, &host);
    if (kind != AEV_MEM_FAULT_NONE) {
        mem_fault(s, kind, addr, size, AEV_MEM_ACCESS_WRITE);
        return;
    }
    uint8_t *p = (uint8_t *)(uintptr_t)host;
    memcpy(p, &value, size);
}

int aev_guest_require_exec(GuestState *s, uint64_t rip, uint64_t len) {
    if (!s)
        return 0;
    /* Legacy COFF identity mode has synthetic Guest PCs outside the Host
     * mapping; keep the historical regressions working.  PE modules always
     * use a region table and get the real check. */
    if (!s->reserved1 || !s->reserved2)
        return 1;
    uint64_t host = 0;
    uint64_t kind = locate_range(s, rip, len ? len : 1, AEV_MEM_PROT_EXEC, &host);
    if (kind == AEV_MEM_FAULT_NONE)
        return 1;
    mem_fault(s, kind, rip, len ? len : 1, AEV_MEM_ACCESS_EXEC);
    return 0;
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
    __int128 ext;
    if (bits < 64) {
        __int128 low = (__int128)(r & mask);
        ext = (__int128)(int64_t)((uint64_t)low << (64 - bits)) >> (64 - bits);
    } else {
        /* r is the truncated 64-bit product.  It must be sign-extended to
         * __int128 before comparing with the full signed product; zero
         * extension makes -1 * 1 look like an overflow. */
        ext = (__int128)(int64_t)r;
    }
    set_flag(s, AEV_CF, full != ext);
    set_flag(s, AEV_OF, full != ext);
    /* ZF/SF/PF are architecturally undefined for imul; set them to zero. */
    set_flag(s, AEV_ZF, 0);
    set_flag(s, AEV_SF, 0);
    set_flag(s, AEV_PF, 0);
}

void aev_flags_shift(GuestState *s, uint32_t width, uint64_t value,
                     uint64_t count, uint64_t result, uint32_t kind) {
    uint32_t bits = width * 8;
    uint64_t mask = width_mask(width);
    uint64_t count_mask = bits - 1;
    uint64_t n = count & count_mask;
    if (n == 0)
        return;
    uint64_t v = value & mask;
    uint64_t r = result & mask;
    uint64_t cf = 0;
    uint64_t of = 0;
    if (kind == 0) { /* shl */
        cf = n < bits ? ((v >> (bits - n)) & 1) : 0;
        if (n == 1)
            of = ((r >> (bits - 1)) & 1) ^ cf;
    } else if (kind == 1) { /* shr */
        cf = n <= bits ? ((v >> (n - 1)) & 1) : 0;
        if (n == 1)
            of = (v >> (bits - 1)) & 1;
    } else { /* sar */
        cf = n <= bits ? ((v >> (n - 1)) & 1) : 0;
        of = 0;
    }
    set_flag(s, AEV_CF, cf != 0);
    set_flag(s, AEV_OF, of != 0);
    set_flag(s, AEV_AF, 0);
    set_zsp(s, width, r);
}

void aev_note_aot(GuestState *s, uint64_t rip) {
    s->aot_blocks++;
    if (s->trace_enabled)
        fprintf(stderr, "AOT  rip=0x%llx\n", (unsigned long long)rip);
    if (s->reserved0) {
        AevTraceSink *sink = (AevTraceSink *)(uintptr_t)s->reserved0;
        if (sink->abi_version == AEV_TRACE_SINK_ABI_VERSION && sink->on_block)
            sink->on_block(sink->ctx, AEV_TRACE_SINK_SOURCE_AOT, rip);
    }
}
