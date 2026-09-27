#!/usr/bin/env python3
"""List functions in an ARM ELF whose stack frame is 4KB or more and that
don't probe the stack page by page (-fstack-clash-protection).
   check-stack-probes.py <elf> [objdump]"""
import re,subprocess,sys
b=sys.argv[1]
out=subprocess.run([(sys.argv[2] if len(sys.argv)>2 else 'arm-riscos-gnueabihf-objdump'),'-d','--no-show-raw-insn',b],capture_output=True,text=True).stdout
fn=None; res={}; regs={}; probed=set()
for line in out.splitlines():
    m=re.match(r'^[0-9a-f]+ <(.+)>:$',line)
    if m: fn=m.group(1); regs={}; continue
    if re.search(r'\tstr\s+r0, \[ip(, #-?\d+)?\]',line): probed.add(fn)
    m=re.search(r'\t(movw|mov)\s+(r\d+|ip|lr|fp|sl), #(\d+)',line)
    if m: regs[m.group(2)]=int(m.group(3)); continue
    m=re.search(r'\tmovt\s+(r\d+|ip|lr|fp|sl), #(\d+)',line)
    if m and m.group(1) in regs: regs[m.group(1)]+=int(m.group(2))<<16; continue
    m=re.search(r'\tsub\s+sp, sp, #(\d+)',line)
    n=None
    if m: n=int(m.group(1))
    m2=re.search(r'\tsub\s+sp, sp, (r\d+|ip|lr|fp|sl)$',line)
    if m2 and m2.group(1) in regs: n=regs[m2.group(1)]
    if n and n>=4096: res[fn]=max(res.get(fn,0),n)
bad={f:n for f,n in res.items() if f not in probed}
for f,n in sorted(bad.items(),key=lambda x:-x[1]): print(n,f)
print(len(res),'functions with frames >= 4096;',len(bad),'without stack probes')
