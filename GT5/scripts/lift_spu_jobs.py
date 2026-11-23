#!/usr/bin/env python3
"""Lift every captured raw SPU image and regenerate src/gen/spu_jobs.c.

GT5's job binaries and its Wws_Job policy module are raw code+data blobs, not the
ELFs build_spu_workloads.py discovers. Capture them first, then lift and rebuild:

    run.ps1 -Env @{SPU_DUMP_MISS="...\\analysis\\spu_miss"}
    python scripts/lift_spu_jobs.py

The dump name carries the fingerprint the runtime computed, which is the value to
register. Run again after each round: a job only dumps once it gets dispatched,
and earlier jobs have to work for later ones to be reached.

The Wws_Job policy module streams a second family of jobs into LS 0x4000 from the
EBOOT's own data. Those are found by scanning the EBOOT (wws_jobs below) and registered
as overlays, so they run compiled instead of through the interpreter.
"""
import concurrent.futures
import hashlib
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT.parent / "ps3recomp" / "tools"
DUMPS = ROOT / "analysis" / "spu_miss"
OUT = ROOT / "spu" / "jobs"
REG = ROOT / "src" / "gen" / "spu_jobs.c"

# Policy modules run at LS 0xA00 (spurs_policy.c); jobs load at LS 0 (spurs_job.c).
PM_BASE = {"75BFE68AE96D7577": 0xA00}  # Wws_Job, embedded in the EBOOT at 0x0177F200

# A Wws job: 16 bytes of `ila $r2` fingerprint, then {entry, text, data, bss}, then
# {0xC0DEC0DE, ?, link base, 5}. The DMA covers text - 0x20 bytes when there's no data.
EBOOT = ROOT / "input" / "EBOOT.elf"
WWS_DIR = ROOT / "analysis" / "spu_wws"
WWS_BASE = 0x4000
WWS_IMAGE0 = 100
WWS_SAMPLE = 64  # code bytes checked at first dispatch; pins the overlay to its load address

NAME_RE = re.compile(r"^spujob_([0-9A-Fa-f]{16})_(\d+)\.bin$")

# Branches whose target is known when lifting (the interrupt window around each DMA, `bid`/`bie` to the
# next instruction, and the like) go there directly instead of through the dispatcher; the pc getter
# `brsl $rX, .+4` no longer splits a function; code entered only from outside gets a C function of its
# own; jump tables become switches; the interrupt test at labels reads the event word first. See the
# options' help in spu_lifter.py. The Wws jobs (lift_wws) and their job manager (PM_OPTS, lift); not
# the SPURS jobs. Natives (src/native/natives.txt) keep what they rely on: the functions
# of an image with a lift-sha1 pin are lifted exactly as before (--frozen-funcs), the others stay
# entered as their own functions (--keep-entries).
LIFT_OPTS = ["--const-branches", "--getpc-inline", "--seed-entries", "--jump-switch", "--irq-macro", "--irq-oneword"]
PM_OPTS = ["--const-branches", "--jump-switch", "--irq-macro", "--irq-oneword"]
# n_static, 22 Nov 2026: every image also gets its any-address form (spu_anyaddr.c: every instruction address
# enterable, used by the runtime only where the lift above has no function), registered from spu_jobs.c right
# after the image's own functions, so no address of a known image is ever interpreted.
ANY_OPTS = ["--any-address", "spu_anyaddr.c"]   # no merged groups there: the rest do nothing
NATIVES = ROOT / "src" / "native" / "natives.txt"


def native_opts(image):
    """--frozen-funcs / --keep-entries for one image, from natives.txt."""
    addrs, pinned = set(), False
    if NATIVES.exists():
        for ln in NATIVES.read_text(encoding="utf-8").splitlines():
            p = ln.split()
            if len(p) < 2 or p[0] != image:
                continue
            if p[1] == "lift-sha1":
                pinned = True
            elif re.fullmatch(r"[0-9A-Fa-f]{8}", p[1]):
                addrs.add(int(p[1], 16))
    if not addrs:
        return []
    return ["--frozen-funcs" if pinned else "--keep-entries", ",".join(hex(a) for a in sorted(addrs))]


# Addresses reached only through a computed branch (scripts/spu_static_entries.txt: `<image> <address> ...`),
# lifted as entries so the runtime never has to interpret the bytes there (spu_lifter.py --entries).
STATIC_ENTRIES = ROOT / "scripts" / "spu_static_entries.txt"


# The Wws jobs' entry stub builds its own return point at 0x40C8, `ila $r0,0xDC ; a $r0,$r0,$r126`, and
# branches to the job's main, whose `bi $r0` lands at 0x40DC with no caller waiting: an entry in every job that
# has the stub, whatever the release (the list names it for the European disc's jobs).
WWS_STUB = bytes.fromhex("42006E00181F8000")


def static_entries(image, blob=None):
    addrs = set()
    if blob is not None and blob[0xC8:0xD0] == WWS_STUB:
        addrs.add(WWS_BASE + 0xDC)
    if STATIC_ENTRIES.exists():
        for ln in STATIC_ENTRIES.read_text(encoding="utf-8").splitlines():
            p = ln.split("#", 1)[0].split()
            if len(p) >= 2 and p[0] == image and re.fullmatch(r"[0-9A-Fa-f]{1,8}", p[1]):
                addrs.add(int(p[1], 16))
    return ["--entries", ",".join(hex(a) for a in sorted(addrs))] if addrs else []


# A Wws job image is named after its address in the EBOOT (w<address>), and that address differs between
# releases: the game updates keep some of the disc's images byte for byte, somewhere else. The lists that go
# by name (spu_static_entries.txt, src/native/natives.txt and the native code's symbols) would then miss them.
# `<image> = <SHA-1 of the image's bytes>` lines in spu_static_entries.txt give those bytes that name in
# every release, so the same code gets the same entries and the same native functions wherever it sits.
def known_names():
    names = {}
    if STATIC_ENTRIES.exists():
        for ln in STATIC_ENTRIES.read_text(encoding="utf-8").splitlines():
            p = ln.split("#", 1)[0].split()
            if len(p) >= 3 and p[1] == "=" and re.fullmatch(r"[0-9A-Fa-f]{40}", p[2]):
                names.setdefault(p[2].lower(), p[0])
    return names


def name_jobs(jobs):
    """(name, ea, entry, code, blob) for each Wws job: the listed name of its bytes, else w<its address>."""
    known = known_names()
    own = {f"w{ea:08X}" for ea, *_ in jobs}
    out, taken = [], set()
    for ea, entry, code, blob in jobs:
        name = f"w{ea:08X}"
        alias = known.get(hashlib.sha1(blob).hexdigest())
        # never a name another image of this EBOOT has by its own address, nor one given out already
        if alias and alias != name and alias not in own and alias not in taken:
            name = alias
        taken.add(name)
        out.append((name, ea, entry, code, blob))
    return out


def run(*args):
    r = subprocess.run([sys.executable, *map(str, args)], capture_output=True, text=True, cwd=ROOT)
    if r.returncode:
        print(r.stdout[-2000:], r.stderr[-2000:], sep="\n", file=sys.stderr)
        raise SystemExit(f"failed: {' '.join(map(str, args))}")
    return r.stdout


def lift(blob, fp):
    base = PM_BASE.get(fp, 0)
    outdir = OUT / f"j{fp[:8]}"
    outdir.mkdir(parents=True, exist_ok=True)
    funcs = OUT / f"j{fp[:8]}_funcs.json"
    out = run(TOOLS / "find_spu_functions.py", "--raw", "--base", hex(base), "--out", funcs, blob)
    m = re.search(r"Detected ([\d,]+) function", out)
    # The Wws job manager (the policy module) opens its interrupt window the same way (`ila $rX, next ;
    # bie $rX`, then `bid` to the next), between functions: those become direct tail calls (PM_OPTS). The
    # SPURS jobs are left as they were.
    run(TOOLS / "spu_lifter.py", blob, "--base", hex(base), "--functions", funcs,
        "--symbol-prefix", f"gt5_j{fp[:8]}", "--output", outdir, "--source-name", "spu_recomp.c", *ANY_OPTS,
        *(PM_OPTS if fp in PM_BASE else []), *static_entries(f"j{fp[:8]}"))
    return int(m.group(1).replace(",", "")) if m else 0


def wws_jobs():
    """Every Wws job in the EBOOT, as (ea, entry offset, code size, blob)."""
    d = EBOOT.read_bytes()
    phoff = struct.unpack(">Q", d[0x20:0x28])[0]
    phnum = struct.unpack(">H", d[0x38:0x3A])[0]
    jobs = []
    for i in range(phnum):
        kind, _, off, va, _, size, _, _ = struct.unpack(">IIQQQQQQ", d[phoff + i * 56:phoff + i * 56 + 56])
        if kind != 1:
            continue
        for o in range(off, off + size - 0x40, 16):
            w = struct.unpack(">12I", d[o:o + 0x30])
            if w[8] == 0xC0DEC0DE and all((x >> 25) == 0x21 and (x & 0x7F) == 2 for x in w[:4]):
                code = w[5] - 0x20
                jobs.append((va + o - off, w[4], code, d[o:o + code]))
    return jobs


def lift_wws(job):
    name, ea, entry, code, blob = job
    WWS_DIR.mkdir(parents=True, exist_ok=True)
    path = WWS_DIR / f"{name}.bin"
    path.write_bytes(blob)
    outdir = OUT / name
    outdir.mkdir(parents=True, exist_ok=True)
    funcs = OUT / f"{name}_funcs.json"
    out = run(TOOLS / "find_spu_functions.py", "--raw", "--base", hex(WWS_BASE), "--entry", hex(WWS_BASE + entry),
              "--out", funcs, path)
    m = re.search(r"Detected ([\d,]+) function", out)
    run(TOOLS / "spu_lifter.py", path, "--base", hex(WWS_BASE), "--functions", funcs, "--code-end", hex(WWS_BASE + code),
        "--return-entries", "--merge-chunks", "--symbol-prefix", f"gt5_{name}", "--output", outdir, "--source-name", "spu_recomp.c", *ANY_OPTS,
        *LIFT_OPTS, *native_opts(name), *static_entries(name, blob))
    return int(m.group(1).replace(",", "")) if m else 0


def main():
    dumps = sorted(p for p in DUMPS.glob("spujob_*.bin") if NAME_RE.match(p.name))
    if not dumps:
        raise SystemExit(f"no spujob_*.bin in {DUMPS} -- run with SPU_DUMP_MISS first")
    fps = [NAME_RE.match(p.name).group(1).upper() for p in dumps]
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        counts = list(ex.map(lift, dumps, fps))
    for p, n in zip(dumps, counts):
        print(f"  {p.name}: {n} functions")

    jobs = name_jobs(wws_jobs())
    # a name's folder from an earlier lift of another release would be compiled in as well: only this lift's stay
    names = {j[0] for j in jobs}
    for old in OUT.glob("w????????"):
        if old.is_dir() and old.name not in names:
            shutil.rmtree(old, ignore_errors=True)
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        wcounts = list(ex.map(lift_wws, jobs))
    print(f"  {len(jobs)} Wws jobs, {sum(wcounts)} functions, {sum(j[3] for j in jobs)} bytes of code")
    moved = [(j[0], j[1]) for j in jobs if j[0] != f"w{j[1]:08X}"]
    if moved:
        print("  known by another release's name: " + ", ".join(f"{n} (here at 0x{ea:08X})" for n, ea in moved))

    externs, regs = [], []
    for img, (p, fp) in enumerate(zip(dumps, fps), start=2):
        pre = f"gt5_j{fp[:8]}"
        entry = f"{pre}_spu_func_{PM_BASE.get(fp, 0):08X}"
        externs += [f"extern void {entry}(spu_context*);", f"extern void {pre}_spu_recomp_register(void);", f"extern void {pre}_any_register(void);"]
        regs += [f"    /* {p.name} */",
                 f"    spu_begin_image({img});",
                 f"    {pre}_spu_recomp_register();", f"    {pre}_any_register();",
                 f'    spu_workload_register_img(0x{fp}ULL, {entry}, {img}, "j{fp[:8]}");']
    for img, (name, ea, entry, code, blob) in enumerate(jobs, start=WWS_IMAGE0):
        pre = f"gt5_{name}"
        sample = blob[0x30:0x30 + WWS_SAMPLE]
        externs += [f"extern void {pre}_spu_recomp_register(void);", f"extern void {pre}_any_register(void);",
                    f"static const uint8_t {pre}_sample[{len(sample)}] = {{{', '.join(str(b) for b in sample)}}};"]
        regs += [f"    spu_begin_image({img});",
                 f"    {pre}_spu_recomp_register();", f"    {pre}_any_register();",
                 f"    {{ spu_overlay_code_segment s = {{0x{WWS_BASE + 0x30:X}, {len(sample)}, {pre}_sample}};",
                 f"      spu_overlay_register_verified_region(0x{ea:08X}u, 0x{WWS_BASE:X}u, 0x{code:X}u, {img}, &s, 1); }}"]
    REG.parent.mkdir(parents=True, exist_ok=True)
    REG.write_text(TEMPLATE.format(externs="\n".join(externs), regs="\n".join(regs), n=len(dumps)), encoding="utf-8")
    print(f"{len(dumps)} images, {sum(counts)} SPU functions -> {REG.relative_to(ROOT)}")


TEMPLATE = """/* Raw SPU images (SPURS jobs + the Wws_Job policy module), GENERATED by
 * scripts/lift_spu_jobs.py. The runtime matches an image by content fingerprint;
 * without a match it logs `dispatch MISS`, and that job never runs. */
#include <stdint.h>
#include "spu_workload.h"
#include "spu_overlay_residency.h"

extern void spu_begin_image(int image_id);

{externs}

/* {n} images. Image ids 2.. leave room for build_spu_workloads.py's hashed ids. */
__attribute__((constructor)) static void gt5_spu_jobs_register(void)
{{
{regs}
    spu_begin_image(0);
}}
"""

if __name__ == "__main__":
    main()
