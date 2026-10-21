#!/usr/bin/env python3
"""symsum.py BINARY INSPROF.TXT [TOP]: instructions per function (the
binary's symbol table, nm -n), as a share of all counted in the binary."""
import bisect, subprocess, sys
binary, prof = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
syms = []
for line in subprocess.run(["arm-linux-gnueabihf-nm", "-n", "--defined-only", binary], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
tot, other, per = 0, 0, {}
for line in open(prof):
    pc, n = line.split()
    pc, n = int(pc, 16) & ~1, int(n)
    i = bisect.bisect_right(addrs, pc) - 1
    if i < 0 or pc > addrs[-1] + 0x100000:
        other += n
        continue
    name = syms[i][1]
    per[name] = per.get(name, 0) + n
    tot += n
print(f"{tot/1e6:.1f} M instructions in {binary.split('/')[-1]} ({other/1e6:.1f} M elsewhere: libc, ld.so)")
for name, n in sorted(per.items(), key=lambda x: -x[1])[:top]:
    print(f"{100*n/tot:6.2f}%  {n/1e6:9.2f} M  {name}")
