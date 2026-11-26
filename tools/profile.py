#!/usr/bin/env python3
"""Sums smrun's profile (-p) by function: profile.py smas.elf DIR/profile.txt [N]"""
import bisect
import subprocess
import sys

elf, prof = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
syms = []
for line in subprocess.run(['llvm-nm', '-n', '--defined-only', elf], capture_output=True,
                           text=True).stdout.splitlines():
    parts = line.split()
    if len(parts) == 3 and parts[1] in 'tTwW' and not parts[2].startswith('$'):
        syms.append((int(parts[0], 16) & ~1, parts[2]))
syms.sort()
addrs = [a for a, _ in syms]
totals = {}
total = 0
for line in open(prof):
    a, c = line.split()
    a, c = int(a, 16), int(c)
    i = bisect.bisect_right(addrs, a) - 1
    name = syms[i][1] if i >= 0 else '?'
    if name.startswith('__Thumbv4') or name.startswith('__ARMv4'):
        name = '(thunk)'
    totals[name] = totals.get(name, 0) + c
    total += c
for name, c in sorted(totals.items(), key=lambda x: -x[1])[:top]:
    print('%10d %5.1f%%  %s' % (c, 100.0 * c / total, name))
print('%10d total' % total)
