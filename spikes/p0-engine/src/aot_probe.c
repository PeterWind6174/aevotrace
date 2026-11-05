/* Test-only AOT probe: dlopen a translated ARM64 dylib, initialize GuestState,
 * call aev_aot_run, and print the result.  Used by
 * scripts/run_semantics_tests.sh; not shipped runtime code. */
#include "aev_guest.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint64_t parse_u64(const char *s) {
    return (uint64_t)strtoull(s, NULL, 0);
}

static uint64_t parse_value(const char *s, const GuestState *st) {
    if (strncmp(s, "mem_end+", 8) == 0) return st->mem_base + st->mem_size + parse_u64(s + 8);
    if (strncmp(s, "mem_end-", 8) == 0) return st->mem_base + st->mem_size - parse_u64(s + 8);
    if (strncmp(s, "mem+", 4) == 0) return st->mem_base + parse_u64(s + 4);
    if (strncmp(s, "mem-", 4) == 0) return st->mem_base - parse_u64(s + 4);
    return parse_u64(s);
}

static void set_reg(GuestState *s, const char *name, uint64_t value) {
    if (!strcmp(name, "rax")) s->rax = value;
    else if (!strcmp(name, "rbx")) s->rbx = value;
    else if (!strcmp(name, "rcx")) s->rcx = value;
    else if (!strcmp(name, "rdx")) s->rdx = value;
    else if (!strcmp(name, "rsi")) s->rsi = value;
    else if (!strcmp(name, "rdi")) s->rdi = value;
    else if (!strcmp(name, "rbp")) s->rbp = value;
    else if (!strcmp(name, "rsp")) s->rsp = value;
    else if (!strcmp(name, "r8")) s->r8 = value;
    else if (!strcmp(name, "r9")) s->r9 = value;
    else if (!strcmp(name, "r10")) s->r10 = value;
    else if (!strcmp(name, "r11")) s->r11 = value;
    else if (!strcmp(name, "r12")) s->r12 = value;
    else if (!strcmp(name, "r13")) s->r13 = value;
    else if (!strcmp(name, "r14")) s->r14 = value;
    else if (!strcmp(name, "r15")) s->r15 = value;
    else if (!strcmp(name, "rip")) s->rip = value;
    else if (!strcmp(name, "rflags")) s->rflags = value;
    else {
        fprintf(stderr, "aev-aot-probe: unknown register %s\n", name);
        exit(2);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <dylib> <rip> [key=value ...]\n"
                "  registers: rax..r15, rip, rflags\n"
                "  memory:    mem=addr:size:value, dump=addr:size\n"
                "  addresses: mem+OFF, mem-OFF, mem_end+OFF, mem_end-OFF\n",
                argv[0]);
        return 2;
    }
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 2;
    }
    int (*aot_run)(GuestState *, const AevRuntime *) =
        (int (*)(GuestState *, const AevRuntime *))dlsym(h, "aev_aot_run");
    if (!aot_run) {
        fprintf(stderr, "dlsym(aev_aot_run) failed: %s\n", dlerror());
        return 2;
    }

    const long page = sysconf(_SC_PAGESIZE);
    const size_t mem_size = (size_t)page * 16;
    void *mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    GuestState s;
    memset(&s, 0, sizeof(s));
    s.mem_base = (uint64_t)(uintptr_t)mem;
    s.mem_size = mem_size;
    s.rsp = (s.mem_base + mem_size - 0x100) & ~0xFULL;
    s.rip = parse_u64(argv[2]);
    *(uint64_t *)(uintptr_t)s.rsp = AEV_RETURN_SENTINEL;

    uint64_t dump_addr[8];
    size_t dump_size[8];
    int dump_count = 0;

    for (int i = 3; i < argc; ++i) {
        char *arg = strdup(argv[i]);
        char *eq = strchr(arg, '=');
        if (!eq) {
            fprintf(stderr, "aev-aot-probe: expected key=value\n");
            return 2;
        }
        *eq = 0;
        char *val = eq + 1;
        if (!strcmp(arg, "mem")) {
            char *c1 = strchr(val, ':');
            char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
            if (!c1 || !c2) {
                fprintf(stderr, "aev-aot-probe: mem=addr:size:value\n");
                return 2;
            }
            *c1 = 0;
            *c2 = 0;
            uint64_t addr = parse_value(val, &s);
            size_t size = (size_t)parse_u64(c1 + 1);
            uint64_t value = parse_value(c2 + 1, &s);
            if (addr < s.mem_base || addr - s.mem_base > mem_size ||
                mem_size - (addr - s.mem_base) < size) {
                fprintf(stderr, "aev-aot-probe: mem write outside region\n");
                return 2;
            }
            memcpy((void *)(uintptr_t)addr, &value, size);
        } else if (!strcmp(arg, "dump")) {
            char *c = strchr(val, ':');
            if (!c || dump_count >= 8) {
                fprintf(stderr, "aev-aot-probe: dump=addr:size\n");
                return 2;
            }
            *c = 0;
            dump_addr[dump_count] = parse_value(val, &s);
            dump_size[dump_count] = (size_t)parse_u64(c + 1);
            uint64_t a = dump_addr[dump_count];
            size_t n = dump_size[dump_count];
            if (a < s.mem_base || a - s.mem_base > mem_size ||
                mem_size - (a - s.mem_base) < n) {
                fprintf(stderr, "aev-aot-probe: dump outside region\n");
                return 2;
            }
            dump_count++;
        } else {
            set_reg(&s, arg, parse_value(val, &s));
        }
        free(arg);
    }

    const uint64_t rsp_in = s.rsp;
    int rc = aot_run(&s, NULL);

    printf("rip_in=0x%llx mem_base=0x%llx mem_size=0x%llx rsp_in=0x%llx "
           "rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx "
           "rsi=0x%llx rdi=0x%llx rbp=0x%llx rsp=0x%llx "
           "r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx "
           "r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx "
           "rip_out=0x%llx rflags=0x%llx rc=%d error=%llu error_pc=0x%llx "
           "steps=%llu aot=%llu\n",
           (unsigned long long)parse_u64(argv[2]),
           (unsigned long long)s.mem_base,
           (unsigned long long)s.mem_size,
           (unsigned long long)rsp_in,
           (unsigned long long)s.rax, (unsigned long long)s.rbx,
           (unsigned long long)s.rcx, (unsigned long long)s.rdx,
           (unsigned long long)s.rsi, (unsigned long long)s.rdi,
           (unsigned long long)s.rbp, (unsigned long long)s.rsp,
           (unsigned long long)s.r8, (unsigned long long)s.r9,
           (unsigned long long)s.r10, (unsigned long long)s.r11,
           (unsigned long long)s.r12, (unsigned long long)s.r13,
           (unsigned long long)s.r14, (unsigned long long)s.r15,
           (unsigned long long)s.rip, (unsigned long long)s.rflags, rc,
           (unsigned long long)s.error_code, (unsigned long long)s.error_pc,
           (unsigned long long)s.steps, (unsigned long long)s.aot_blocks);
    for (int i = 0; i < dump_count; ++i) {
        printf("dump addr=0x%llx size=%zu bytes=",
               (unsigned long long)dump_addr[i], dump_size[i]);
        const uint8_t *p = (const uint8_t *)(uintptr_t)dump_addr[i];
        for (size_t j = 0; j < dump_size[i]; ++j)
            printf("%s%02x", j ? ":" : "", p[j]);
        printf("\n");
    }
    printf("CF=%d OF=%d ZF=%d SF=%d PF=%d\n",
           !!(s.rflags & AEV_CF), !!(s.rflags & AEV_OF),
           !!(s.rflags & AEV_ZF), !!(s.rflags & AEV_SF),
           !!(s.rflags & AEV_PF));
    dlclose(h);
    munmap(mem, mem_size);
    return 0;
}
