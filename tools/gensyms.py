#!/usr/bin/env python3
"""Turn `nm -n` output into a C symbol table for kernel stack traces."""
import sys


def main():
    syms = []
    if len(sys.argv) > 1:
        for line in open(sys.argv[1]):
            parts = line.split()
            if len(parts) != 3 or parts[1] not in 'tTwW':
                continue
            addr = int(parts[0], 16)
            if addr < 0xFFFFFFFF80000000:
                continue
            syms.append((addr, parts[2]))
    print('struct ksym { unsigned long addr; const char *name; };')
    print('const struct ksym ksym_table[] = {')
    for a, n in sorted(syms):
        print(f'    {{ 0x{a:x}ul, "{n}" }},')
    if not syms:
        print('    { 0, "" },')
    print('};')
    print(f'const int ksym_count = {len(syms)};')


if __name__ == '__main__':
    main()
