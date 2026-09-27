#!/usr/bin/env python3
"""Make ARM NEON assembly safe on a CPU that traps unaligned accesses.

RISC OS 5 runs ARMv7 with alignment checking on (SCTLR.A = 1). Then a NEON
load/store faults unless its address is a multiple of the ELEMENT size:
vld1.32 needs 4-byte alignment, vld1.64 8-byte, and so on; .8 never faults.
Linux runs with checking off, so codec assembly written for Linux freely
uses vld1.32 / vst1.64 on byte (pixel) pointers. This tool rewrites those
instructions, in place, into forms with the same effect that don't fault:

 1. vld1/vst1 of whole registers, e.g.  vld1.64 {d0-d1}, [r1], r2
    -> vld1.8 {d0-d1}, [r1], r2
    Identical on little-endian ARM: element i goes to lane i either way, so
    the register bytes are the same. No speed cost.

 2. one lane, e.g.  vst1.32 {d8[1]}, [r0], r1
    -> vst1.8 {d8[4]}, [r0]!  ... {d8[7]}, [r0], r1 ; sub r0, r0, #3
    (byte lanes 4k..4k+n-1 are the element's bytes on little-endian).

 3. all lanes ("dup"), e.g.  vld1.32 {d16[], d17[]}, [r3]
    -> byte loads into d16 lanes 0-3 as in 2, then vdup.32 d16, d16[0]
       and vmov d17, d16.

 4. vld2/3/4 and vst2/3/4 of one lane, e.g.  vld2.32 {d0[1], d1[1]}, [r3], r4
    -> as 2, register by register (element k of the structure belongs to
       the k-th register); and their "dup" forms, e.g. vld4.16 {d0[],...},
       each register getting its own element, as 3.

Instructions that carry an alignment qualifier ([r0, :128]) are left alone:
the qualifier promises alignment, and Linux would fault on them too. So are
.8 accesses. vld2/3/4 and vst2/3/4 with elements wider than 8 bits can't be
rewritten mechanically when they move whole registers (they de-interleave):
those are reported (exit status 2) unless listed in
--allow FILE:LINE (checked by hand: their data is aligned).

A register list that is a macro argument ({\\regs}) may be whole registers
or a lane, which only the macro's callers show; it is reported unless listed
with --whole FILE:LINE (rewrite as whole registers) or --allow.

Usage: neon-align.py [--check] [--allow FILE:LINE ...] [--whole FILE:LINE ...]
                     file.S ...
       The *.allow files hold "FILE:LINE" (allow) and "whole FILE:LINE" lines.
   --check   only report what would change; exit 1 if anything would.
"""
import argparse
import re
import sys

SIZE_OF = {"8": 8, "16": 16, "32": 32, "64": 64}
DTYPE = re.compile(r"^(?:[iusfp]?)(8|16|32|64)$", re.I)
INSN = re.compile(
    r"^(?P<indent>\s*)(?P<label>(?:[\w.$\\]+:\s*)?)"
    r"(?P<op>v(?:ld|st)(?P<n>[1-4]))\.(?P<dt>\\?\w+)(?P<sp>\s+)"
    r"(?:\{(?P<regs>[^}]*)\}|(?P<reg1>[^\s{,][^,]*?))\s*,\s*\[(?P<base>[^\]]*)\](?P<post>[^@;/]*?)"
    r"(?P<tail>\s*(?:(?:@|//|/\*).*)?)$", re.I)


def regs_of(m):
    """The register list without braces (GAS also takes "vst1.32 d0[0], ...")."""
    return m.group("regs") if m.group("regs") is not None else m.group("reg1").strip()


def split_regs(regs):
    return [r.strip() for r in regs.split(",") if r.strip()]


def lane_of(reg):
    """'d8[1]' -> ('d8', '1'); 'd8[]' -> ('d8', ''); 'd8' -> None"""
    m = re.match(r"^(.*?)\[(.*)\]$", reg)
    return (m.group(1).strip(), m.group(2).strip()) if m else None


def expand_bytes(indent, op, parts, base, post):
    """Byte-wise transfer of parts = [(dreg, first_byte_expr, nbytes), ...]
    in memory order, reproducing the original addressing mode's final base
    register (post: '', '!' or ', rM')."""
    out = []
    post = post.strip()
    rn = base.strip()
    seq = [(d, "%s+%d" % (f, i) if i else f) for d, f, n in parts for i in range(n)]
    total = len(seq)
    for i, (dreg, lane) in enumerate(seq):
        last = i == total - 1
        if not last or post == "!":
            addr = "[%s]!" % rn
        elif post.startswith(","):
            addr = "[%s]%s" % (rn, post)
        else:
            addr = "[%s]" % rn
        out.append("%s%s.8 {%s[%s]}, %s" % (indent, op, dreg, lane, addr))
    if post != "!" and total > 1:
        out.append("%ssub %s, %s, #%d" % (indent, rn, rn, total - 1))
    return out


def rewrite(line, whole=False):
    """Returns (new_lines or None if unchanged, problem or None). whole: the
    register list is a macro argument known to name whole registers."""
    m = INSN.match(line)
    if not m:
        return None, None
    dt = m.group("dt")
    dm = DTYPE.match(dt)
    if dm:
        size = int(dm.group(1))
        return (None, None) if size == 8 else rewrite_sized(m, line, size, whole)
    if not dt.startswith("\\"):
        return None, None
    # Element size from a macro argument (vld1.\wd): one branch per size.
    ind = m.group("indent")
    out = []
    for size in (16, 32, 64):
        new, problem = rewrite_sized(m, line, size, whole)
        if problem:
            return None, problem
        if new:
            out += ["%s.%s %s == %d" % (ind, "if" if not out else "elseif", dt, size)] + new
    if not out:
        return None, None
    return out + ["%s.else" % ind, line, "%s.endif" % ind], None


def rewrite_sized(m, line, size, whole=False):
    if size == 64 and any(lane_of(r) for r in split_regs(regs_of(m))):
        return None, None           # no 64-bit lanes: can't be this size
    base = m.group("base")
    if ":" in base:            # alignment qualifier: aligned by contract
        return None, None
    # A macro argument after the register ([r2\\align]) can only be an
    # alignment qualifier (",:32") or nothing. Rewrite for the "nothing" case
    # and keep the original under .ifb/.else for the aligned one.
    bm = re.match(r"^\s*([^\s,\\]+)\s*(\\.*)?$", base)
    qual_arg = bm.group(2) if bm and bm.group(2) else None
    orig_base = base
    if qual_arg:
        base = bm.group(1)
    op = m.group("op").lower()
    ind, label, tail = m.group("indent"), m.group("label"), m.group("tail").strip()
    regs = split_regs(regs_of(m))
    lanes = [lane_of(r) for r in regs]
    note = "  @ neon-align: was " + line.strip()
    nb = size // 8
    single = op[:3] + "1"     # vld1/vst1 for the byte transfers
    nregs = int(m.group("n"))

    if not whole and any("\\" in r and "[" not in r for r in regs):
        # {\regs}: a macro argument that may be whole registers or a lane
        # (d0[0]); can't tell here, so it's listed for a look.
        return None, "register list from a macro argument {%s}" % regs_of(m)

    if all(l is None for l in lanes):                      # rule 1
        if nregs != 1:
            return None, "%s.%s with %d-bit elements, unaligned-capable" % (op, m.group("dt"), size)
        new = "%s%s%s.8%s{%s}, [%s]%s" % (ind, label, m.group("op"), m.group("sp"),
                                          regs_of(m), orig_base, m.group("post").rstrip())
        return [new + ("  " + tail if tail else "")], None

    if any(l is None for l in lanes):
        return None, "unhandled register list {%s}" % regs_of(m)

    def lane_bytes(idx):
        return str(int(idx) * nb) if idx.isdigit() else "(%s)*%d" % (idx, nb)

    if all(l[1] != "" for l in lanes):                     # rules 2 and 4
        if len(regs) != nregs:
            return None, "unhandled register list {%s}" % regs_of(m)
        parts = [(d, lane_bytes(i), nb) for d, i in lanes]
        out = expand_bytes(ind, single, parts, base, m.group("post"))
    elif all(l[1] == "" for l in lanes) and op.startswith("vld"):  # rules 3 and 4
        if nregs == 1:
            parts = [(lanes[0][0], "0", nb)]
        elif len(regs) == nregs:
            parts = [(d, "0", nb) for d, _ in lanes]
        else:
            return None, "unhandled register list {%s}" % regs_of(m)
        out = expand_bytes(ind, "vld1", parts, base, m.group("post"))
        for d, _, _ in parts:
            out.append("%svdup.%d %s, %s[0]" % (ind, size, d, d))
        if nregs == 1:
            for other, _ in lanes[1:]:
                out.append("%svmov %s, %s" % (ind, other, lanes[0][0]))
    else:
        return None, "unhandled register list {%s}" % regs_of(m)
    out[0] += note
    if qual_arg:
        arg = re.match(r"\\(\w+)", qual_arg).group(0)
        out = ["%s.ifb %s" % (ind, arg)] + out + ["%s.else" % ind, "%s%s" % (ind, line.strip()[len(label.strip()):].strip()) if label else line, "%s.endif" % ind]
    if label:
        out.insert(0, ind + label.strip())
    return out, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--allow", action="append", default=[])
    ap.add_argument("--whole", action="append", default=[],
                    help="FILE:LINE whose {\\macro-arg} register list is whole registers")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    allowed = set(a.allow)
    changed = problems = 0
    for path in a.files:
        src = open(path).read().split("\n")
        out = []
        for no, line in enumerate(src, 1):
            key = "%s:%d" % (path.split("/")[-1], no)
            new, problem = rewrite(line, whole=key in a.whole)
            if problem:
                if key not in allowed and "%s:%d" % (path, no) not in allowed:
                    print("%s:%d: %s: %s" % (path, no, problem, line.strip()))
                    problems += 1
            if new:
                changed += 1
                out.extend(new)
            else:
                out.append(line)
        if not a.check and out != src:
            open(path, "w").write("\n".join(out))
    print("neon-align: %d instructions %s, %d need a look" %
          (changed, "to rewrite" if a.check else "rewritten", problems), file=sys.stderr)
    if problems:
        sys.exit(2)
    if a.check and changed:
        sys.exit(1)


if __name__ == "__main__":
    main()
