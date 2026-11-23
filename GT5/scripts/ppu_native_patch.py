#!/usr/bin/env python3
"""Switch native functions in for lifted PPU functions at build time (no re-lift).

    ppu_native_patch.py <lifted ppu_recomp_NNN.cpp> <patched copy> <ppu_natives.txt>
    ppu_native_patch.py --where <ppu_natives.txt> <generated folder>     (CMake: the files that get a copy)
    ppu_native_patch.py --pin <generated folder> <ADDR8>...               (list lines for these functions)

GT5/src/native/ppu_natives.txt lists the functions, one a line:

    <ADDR8> leaf <sha1>

`leaf`: the native form is made from this lift's own text of the function by src/native/gen_ppu_leaf.py
(its registers held in C locals; see there). <sha1> is the SHA-1 of the lifted function's text (its
`void func_<ADDR8>(ppu_context* ctx) {` line to its closing brace, LF line ends) the native was proven
on: a function whose text is another (another release or update, another lifter) stays lifted, and so
does one the generator turns away; the build says which and why.

In the copy, the lifted definition is renamed `func_<ADDR8>_lifted` and a function with the old name is
added that picks one at run time (GT5_PPU_NATIVE: 1 = native, the default; 0 = lifted; 2 = compare: the
native's compare form on a copy of the registers with its stores held back, then the lifted function for
real, registers and stored bytes compared, the first differences logged). Every caller, the function
table and the trampolines keep the old name, so nothing else changes. src/native/ppu_native.cpp has the
switch, the counters (the "[ppu-native]" log line) and the compare.
"""
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def load_gen(listing):
    path = os.path.join(os.path.dirname(os.path.abspath(listing)), "gen_ppu_leaf.py")
    if not os.path.exists(path):
        path = os.path.join(HERE, "..", "src", "native", "gen_ppu_leaf.py")
    spec = importlib.util.spec_from_file_location("gen_ppu_leaf", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_list(listing):
    out = []
    for n, line in enumerate(open(listing, encoding="utf-8"), 1):
        f = line.split("#", 1)[0].split()
        if not f:
            continue
        if len(f) != 3 or f[1] != "leaf" or not re.fullmatch(r"[0-9A-Fa-f]{1,8}", f[0]) \
                or not re.fullmatch(r"[0-9a-f]{40}", f[2]):
            sys.exit(f"[ppu-native] {listing}:{n}: not `<ADDR8> leaf <sha1>`: {line.strip()}")
        out.append((f[0].upper().rjust(8, "0"), f[1], f[2]))
    return out


def patch(src, dst, listing):
    gen = load_gen(listing)
    text = open(src, encoding="latin-1", newline="").read()
    nl = "\r\n" if "\r\n" in text else "\n"
    tail, done, kept = [], [], []
    for addr, kind, sha in read_list(listing):
        name = f"func_{addr}"
        span = gen.find_function(text, name)
        if not span:
            continue                                   # in another file of this lift
        ftext = text[span[0]:span[1]]
        if gen.fn_sha1(ftext) != sha:
            kept.append(f"{addr} (its lifted text is not the one the native was proven on)")
            continue
        nat, why = gen.generate(ftext.replace("\r\n", "\n"), name, f"gt5_ppu_native_{addr}", f"gt5_ppu_shadow_{addr}")
        if nat is None:
            kept.append(f"{addr} ({why})")
            continue
        text = text[:span[0]] + ftext.replace(f"void {name}(ppu_context* ctx) {{", f"void {name}_lifted(ppu_context* ctx) {{", 1) \
            + text[span[1]:]
        k = f"gt5_ppu_nat_k_{addr}"
        shadow = f"gt5_ppu_shadow_{addr}" if why is None else "nullptr"
        tail += [nat,
                 f"static const int {k} = gt5_ppu_native_register(\"{addr}\", {0 if why is None else 1});",
                 f"void {name}(ppu_context* ctx) {{",
                 "    const int m = g_gt5_ppu_native;",
                 f"    if (__builtin_expect(m == 1, 1)) {{ gt5_ppu_nat_count({k}); gt5_ppu_native_{addr}(ctx); return; }}",
                 f"    if (m == 2) {{ gt5_ppu_native_compare(ctx, {shadow}, {name}_lifted, {k}); return; }}",
                 f"    gt5_ppu_nat_lifted({k});",
                 f"    {name}_lifted(ctx);",
                 "}", ""]
        done.append(addr)
    if tail:
        body = "\n".join(["", "/* ppu_native_patch.py: native forms of the functions GT5/src/native/ppu_natives.txt lists */",
                          '#include "ppu_native.h"', ""] + tail)
        text += body.replace("\n", nl)
    open(dst, "w", encoding="latin-1", newline="").write(text)
    msg = f"[ppu-native] {os.path.basename(src)}: {len(done)} function(s) native"
    if kept:
        msg += "; stay lifted: " + ", ".join(kept)
    print(msg)


def where(listing, gdir):
    gen = load_gen(listing)
    want = {f"func_{a}": s for a, k, s in read_list(listing)}
    heads = re.compile(r"^void (func_[0-9A-F]{8})\(ppu_context\* ctx\) \{", re.M)
    files = []
    for fn in sorted(os.listdir(gdir)):
        if not re.fullmatch(r"ppu_recomp_\d+\.cpp", fn):
            continue
        text = open(os.path.join(gdir, fn), encoding="latin-1", newline="").read()
        hit = False
        for m in heads.finditer(text):
            if m.group(1) in want:
                span = gen.find_function(text, m.group(1))
                if span and gen.fn_sha1(text[span[0]:span[1]]) == want[m.group(1)]:
                    hit = True
        if hit:
            files.append(fn)
    sys.stdout.write(";".join(files))


def pin(gdir, addrs):
    gen = load_gen(os.path.join(HERE, "..", "src", "native", "ppu_natives.txt"))
    want = {"func_" + a.upper().rjust(8, "0") for a in addrs}
    for fn in sorted(os.listdir(gdir)):
        if not re.fullmatch(r"ppu_recomp_\d+\.cpp", fn):
            continue
        text = open(os.path.join(gdir, fn), encoding="latin-1", newline="").read()
        for name in sorted(want):
            span = gen.find_function(text, name)
            if span:
                ftext = text[span[0]:span[1]]
                nat, why = gen.generate(ftext.replace("\r\n", "\n"), name, "n", "s")
                if nat is None:
                    print(f"# {name[5:]} refused: {why}")
                else:
                    print(f"{name[5:]} leaf {gen.fn_sha1(ftext)}" + (f"   # {why}" if why else ""))


def main():
    a = sys.argv[1:]
    if a and a[0] == "--where":
        where(a[1], a[2])
    elif a and a[0] == "--pin":
        pin(a[1], a[2:])
    elif len(a) == 3:
        patch(*a)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
