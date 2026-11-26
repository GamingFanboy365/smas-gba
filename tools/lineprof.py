#!/usr/bin/env python3
"""Cycles by source line within one function, from smrun's profile (-p):
lineprof.py smas.elf DIR/profile.txt FUNCTION [N]   (the build needs -g)"""
import collections
import subprocess
import sys

elf, prof, func = sys.argv[1:4]
top = int(sys.argv[4]) if len(sys.argv) > 4 else 15
syms = []
for line in subprocess.run(['llvm-nm', '-n', '--defined-only', elf], capture_output=True,
                           text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in 'tT' and not p[2].startswith('$'):
        syms.append((int(p[0], 16) & ~1, p[2]))
start = next(a for a, n in syms if n == func)
end = min(a for a, n in syms if a > start)
rows = []
for line in open(prof):
    a, c = line.split()
    a, c = int(a, 16), int(c)
    if start <= a < end:
        rows.append((a, c))
out = subprocess.run(['llvm-symbolizer', '--obj=' + elf, '--no-inlines'],
                     input='\n'.join('0x%x' % a for a, _ in rows), capture_output=True,
                     text=True).stdout.split('\n\n')
by = collections.Counter()
for (a, c), o in zip(rows, out):
    loc = o.strip().split('\n')[-1].split('/')[-1]
    by[loc.rsplit(':', 1)[0]] += c
print('%s: %d cycles' % (func, sum(c for _, c in rows)))
for loc, c in by.most_common(top):
    print('%9d  %s' % (c, loc))
