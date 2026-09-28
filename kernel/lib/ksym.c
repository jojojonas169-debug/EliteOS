/* Kernel symbol lookup for stack traces (table generated at link time). */
#include <kernel.h>

struct ksym {
    uint64_t addr;
    const char *name;
};

extern const struct ksym ksym_table[];
extern const int ksym_count;

const char *ksym_lookup(uint64_t addr, uint64_t *off)
{
    int lo = 0, hi = ksym_count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ksym_table[mid].addr <= addr) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return NULL;
    if (off) *off = addr - ksym_table[best].addr;
    return ksym_table[best].name;
}
