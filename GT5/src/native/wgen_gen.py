#!/usr/bin/env python3
r"""wgen_gen.py: an SPU job image (a WWS job, a SPURS job, the WWS job manager) as native code, made at build time
from this copy's own game files.

    wgen_gen.py <lifted spu_recomp.c of the image> <output folder> [--image-from <folder>]   (natives.txt `generate`)

Writes <output folder>/wgen_<image>_gen.inc, which src/native/wgen_<image>.c includes. Nothing of the game's code
is in this script or in the repository: the instructions are read from this copy's EBOOT.elf (GT5/input) or job
image files (GT5/analysis/spu_miss), their
meaning from this copy's SPU lifter (ps3recomp/tools/spu_lifter.py, the same `_translate` the lift uses, so the
arithmetic is the lift's by construction), and the lifted file supplies the lift's entry points.

What it makes. The lift cuts a job into thousands of small functions that reach each other through the
runtime: every call is a host call plus a drain of pending transfers, every return or register branch goes
through the dispatcher, every label tests for an interrupt, and a return to an address the lift has no
function for unwinds the host stack and runs the SPU interpreter. Here each procedure of the job (its entry,
the targets of its calls, the addresses it builds as function pointers) is ONE C function:
  - the SPU registers it touches are C locals (loaded at its entry, stored before it leaves);
  - branches inside it are gotos, a jump through a register is a switch over its own labels;
  - a call (brsl, or bisl to one of the job's procedures) is a C call that passes the return address; the
    callee returns the address its code branched to, and the caller goes on natively when that is the
    return address (as the SPU does: a return is a branch to the link);
  - a call into anything else (the job manager's API below the job, code that is not a procedure) is made
    exactly as the lift makes it (the runtime's bracket), with every register in the context;
  - interrupts are tested only where one can have become pending (after a channel access, a call into the
    runtime or a procedure that does one, a branch that enables them), at the same addresses as the lift;
  - every other instruction is the lift's own C expression for it.
Hand-back: where the native code cannot go on (an interrupt to take, a branch to an address that is neither
the caller's return address nor one of its labels or procedures, a stop or halt, an instruction it does not
translate), it stores its registers and returns that address; each caller stores its own and returns it
too, and the job's entry gives it to the runtime's dispatcher: the rest of that job runs as lifted code, as
it does today. Counted with the reason (native_wgen.h).

Register sets. A procedure that reaches the runtime's bracket (directly or through its callees) is called
with every register stored and reloads every register after; any other is called with only the registers
it (with its callees) reads or writes stored, and only those it writes reloaded.

Pins: the image's SHA-1 (IMAGES, JIMAGES), and the SHA-1s of the lifters and disassembler it was proven with. Any
other gives a file that leaves the image lifted ("[wgen] <image> stays lifted code (...)" in the log).

Images. IMAGES: the WWS job images (loaded at LS 0x4000 from the EBOOT, 0xC0DEC0DE header), proven on 24 real
captures each. JIMAGES: the SPURS job images, read from the image file the lift itself compiled
(analysis/spu_miss/spujob_<fingerprint>_<size>.bin): the job-chain jobs at LS 0 (the sound jobs; a branch to LS 0
ends such a job, as the runtime's drain does: WGEN_END_PC) and the WWS job manager at LS 0xA00 (the kernel's
services below it are reached through the runtime, as the lift reaches them). In a SPURS job image a call of a
hand-written native's address calls that function by its name, as the lift does. natives.txt turns an image on only
where it was faster than the build's code before it (lift + hand-written natives; n_mdl notes): w018CAD00,
w0163C080, w01606C00. w01695680 (faster than the earlier lifts), the sound jobs and the manager reproduce their
captures but are not faster than n_static's lift: off.
"""
import hashlib
import re
import struct
import sys
from collections import defaultdict
from pathlib import Path

# WWS job images: image -> (EA in the EBOOT, SHA-1 of the code bytes loaded at LS 0x4000[, the job's entry])
IMAGES = {
    "w01695680": (0x01695680, "690d958c848dde4fbcffaaf7bfc88237241cc5b0"),
    "w01603480": (0x01603480, "83718a5206161589917c86efa13bea07eaec8576"),
    "w01642000": (0x01642000, "3f679c812eed53110d006cd47b782c40c2fdd3b1"),
    "w0163C080": (0x0163C080, "8a79a821bf37252a9ce2633154a6724919f6a322"),
    "w01606C00": (0x01606C00, "b0c4ba1bc90ab4d4313f2a3dc6e2b5910ab17f9a"),
    "w018C3A80": (0x018C3A80, "2eefc895904e79b17ad735320bea778562fd9360"),
    "w018CAD00": (0x018CAD00, "7bc7c3368148df7f60d8e8d2fe5eceff87780da2", 0x107D8),
}
# the lifters / disassemblers the generated code was proven with (SHA-1, line endings ignored): v0.1.109's lift and
# n_static's re-lift (22 Nov 2026); 24 real job captures per image reproduced with each. 68f0016f = n_static round 3
# (the any-address form): every image's generated file is byte for byte the one 170a1230 gives (n_mdl round 4)
LIFTER_SHA1 = {"854543cc5b1dc614cfc8b917b27b291d657bc9d3", "170a12303b793f81db68ba8288e391d7a21f3a37",
               "68f0016f610a664678055fc026d3de1b572eba06"}
DISASM_SHA1 = {"27cd4992c16f86e56ebf9f5004af6feaeb6ed04c"}
BASE = 0x4000
ENTRY = 0x4030            # the lowest code address of a WWS job image (its crt); the job's entry is per image
# The SPURS job images (n_mdl round 4): image -> (kind, SHA-1 of the image file the lift compiled, load address,
# entry). The image file is this copy's analysis/spu_miss/spujob_<fingerprint>_<size>.bin (scripts/
# spu_jobs_from_eboot.py writes it from the EBOOT, lift_spu_jobs.py lifts it). "chain": a job-chain job loaded at
# LS 0 (spurs_job.c: entered at 0 with the return address 0x3FF00 in r0; a branch to LS 0 ends it too); "policy":
# the WWS job manager at LS 0xA00 (spurs_policy.c; the kernel's services below it are the runtime's).
JIMAGES = {
    "jD58063CD": ("chain", "e934703d61aca2e7f025503e3326dd8d57c21f01", 0x0, 0x0),
    "jFE031AF6": ("chain", "65c1baa23a23c6a15eed04ae15137270fcc4d86c", 0x0, 0x0),
    "jEB79FDFC": ("chain", "38b144965d2b3a89bc29cc8a6e1970ea26424120", 0x0, 0x0),
    "j75BFE68A": ("policy", "3cad156b9d0d985e4788140db9728da0bf8cd734", 0xA00, 0xA00),
}

# helpers whose result depends on the float mode or on float value classes (spu_lifter.py _FP_HELPERS): their
# register operands go through spu_opq (Gen.fp_barrier)
FP_HELPERS = {"spu_" + n for n in (
    "fa fs fm fma fms fnms fi frest frsqest fceq fcgt fcmeq fcmgt fesd frds "
    "dfa dfs dfm dfma dfms dfnms dfnma dfceq dfcgt dfcmeq dfcmgt dftsv csflt cuflt cflts cfltu").split()}
COND_BR = {"brz", "brnz", "brhz", "brhnz"}
BI_FAMILY = {"bi", "biz", "binz", "bihz", "bihnz"}
HALTS = {"heq", "heqi", "hgt", "hgti", "hlgt", "hlgti"}
CHANNEL = {"rdch", "wrch", "rchcnt"}
NOP = {"nop", "lnop", "hbr", "hbra", "hbrr"}
STOPS = {"iret", "bisled", "stop", "stopd"}
ALL = frozenset(range(128))
DEBUG = "--debug" in sys.argv     # every procedure called with every register stored; registers stored before channel ops


def sha1_text(p):
    return hashlib.sha1(Path(p).read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def image_from_eboot(eboot, ea):
    """The code a WWS job image loads at LS 0x4000, from the EBOOT (its 0xC0DEC0DE header)."""
    d = Path(eboot).read_bytes()
    phoff = struct.unpack(">Q", d[0x20:0x28])[0]
    phnum = struct.unpack(">H", d[0x38:0x3A])[0]
    for i in range(phnum):
        kind, _, off, va, _, size, _, _ = struct.unpack(">IIQQQQQQ", d[phoff + i * 56:phoff + i * 56 + 56])
        if kind == 1 and va <= ea < va + size:
            o = off + ea - va
            w = struct.unpack(">12I", d[o:o + 0x30])
            if w[8] != 0xC0DEC0DE:
                return None
            return d[o:o + w[5] - 0x20]
    return None


class Insn:
    __slots__ = ("addr", "raw", "mn", "ops", "d")

    def __init__(self, addr, raw, d):
        self.addr, self.raw, self.d = addr, raw, d
        self.mn = d.mnemonic
        self.ops = [o.strip() for o in d.operands.split(",") if o.strip()]


def stub(out, img, why):
    out.write_text("/* wgen_gen.py: no native code for %s in this copy (%s): the job stays lifted. */\n"
                   "#define WGEN_NOT_BUILT \"%s\"\n" % (img, why, why), encoding="utf-8")
    print(f"[wgen] {img}: {why}: the job stays lifted")


def main():
    args = sys.argv[1:]
    src = None
    if "--image-from" in args:
        i = args.index("--image-from"); src = Path(args[i + 1]); del args[i:i + 2]
    pin = "--pin" in args
    args = [a for a in args if not a.startswith("--")]
    lifted, outdir = Path(args[0]), Path(args[1])
    img = lifted.resolve().parent.name
    outdir.mkdir(parents=True, exist_ok=True)
    out = outdir / f"wgen_{img}_gen.inc"
    gt5 = lifted.resolve().parents[3]
    tools = gt5.parent / "ps3recomp" / "tools"
    if img not in IMAGES and img not in JIMAGES:
        return stub(out, img, "an image this generator does not know")
    blob = None
    if img in JIMAGES:
        want = JIMAGES[img][1]
        files = sorted((gt5 / "analysis" / "spu_miss").glob(f"spujob_{img[1:]}*.bin"))
        if src is not None and (src / f"{img}.bin").exists():
            blob = (src / f"{img}.bin").read_bytes()
        elif files:
            blob = files[0].read_bytes()
    else:
        ea, want = IMAGES[img][:2]
        # the bytes lift_spu_jobs.py lifted under this name: in a game update the same job sits at another address
        # than the disc's (scripts/spu_static_entries.txt, the `=` lines), so the EBOOT is only the fallback
        own = gt5 / "analysis" / "spu_wws" / f"{img}.bin"
        if src is not None and (src / f"{img}.bin").exists():
            blob = (src / f"{img}.bin").read_bytes()
        elif own.exists():
            blob = own.read_bytes()
        elif (gt5 / "input" / "EBOOT.elf").exists():
            blob = image_from_eboot(gt5 / "input" / "EBOOT.elf", ea)
    if blob is None:
        return stub(out, img, "no job image")
    if pin:
        print("IMAGE_SHA1", hashlib.sha1(blob).hexdigest())
        print("LIFTER_SHA1", sha1_text(tools / "spu_lifter.py"))
        print("DISASM_SHA1", sha1_text(tools / "spu_disasm.py"))
        return
    if hashlib.sha1(blob).hexdigest() != want:
        return stub(out, img, "another job image: re-pin after the capture proof, wgen_gen.py --pin")
    if LIFTER_SHA1 and sha1_text(tools / "spu_lifter.py") not in LIFTER_SHA1:
        return stub(out, img, "another SPU lifter: re-pin after the capture proof, wgen_gen.py --pin")
    if DISASM_SHA1 and sha1_text(tools / "spu_disasm.py") not in DISASM_SHA1:
        return stub(out, img, "another SPU disassembler: re-pin after the capture proof, wgen_gen.py --pin")
    sys.path.insert(0, str(tools))
    import spu_disasm
    import spu_lifter
    gen = Gen(img, blob, spu_disasm, spu_lifter, lifted)
    out.write_text(gen.emit(), encoding="utf-8", newline="\n")
    s = gen.stats
    print(f"[wgen] {img}: {s['procs']} procedures ({s['rt']} reach the runtime, {s['pure']} pure), {s['insns']} "
          f"instructions, {s['switch']} jump tables, {s['checks']} interrupt tests, {s['handback']} hand-back points")


class Gen:
    def __init__(self, img, blob, dis, lif, lifted):
        self.img = img
        if img in JIMAGES:
            self.kind, _, self.base, self.entry = JIMAGES[img]
            self.lo = self.base               # the lowest code address
        else:
            self.kind, self.base, self.lo = "wws", BASE, ENTRY
            self.entry = IMAGES[img][2] if len(IMAGES[img]) > 2 else ENTRY
        # a job-chain job ends where it branches to LS 0 (its entry: the runtime's drain ends the job there, before
        # anything at 0 runs again), as well as at its return to 0x3FF00
        self.endpc = 0 if self.kind == "chain" else None
        # Hand-written natives of this image (natives.txt next to this script, `hooked` lines of other sources):
        # their addresses are never procedures here. A call goes to them as the lift calls them (the runtime's
        # bracket: their switches and counters work as before), and code that runs into one is handed to the
        # lifted code there in place, which is the native.
        self.hooked = set()
        nt = Path(__file__).resolve().parent / "natives.txt"
        if nt.exists():
            for ln in nt.read_text(encoding="utf-8", errors="replace").splitlines():
                f = ln.split("#", 1)[0].split()
                if len(f) >= 5 and f[0] == img and "hooked" in f[4:] and not f[3].startswith("wgen_"):
                    try:
                        self.hooked.add(int(f[1], 16))
                    except ValueError:
                        pass
        self.blob = blob
        self.end = self.base + len(blob)
        self.L = lif
        self.lifter = lif.SPULifter()
        self.lifter.reg_locals = True
        self.ins = {}
        for i in range(len(blob) // 4):
            a = self.base + 4 * i
            raw = struct.unpack(">I", blob[4 * i:4 * i + 4])[0]
            self.ins[a] = Insn(a, raw, dis.spu_decode(raw, a))
        t = lifted.read_text(encoding="latin-1")
        self.lift_starts = sorted(set(int(x, 16) for x in re.findall(r"\{ 0x([0-9A-F]{8})u, gt5_%s_spu_func_" % img, t)))
        self.lset = set(self.lift_starts)
        self.stats = defaultdict(int)
        self.lines = {}
        self.find_entries()
        self.procs = {e: self.walk(e) for e in sorted(self.entries)}
        self.stats["procs"] = len(self.procs)
        self.stats["insns"] = sum(len(p["body"]) for p in self.procs.values())
        self.contracts()

    # ---------------------------------------------------------------- analysis
    def code(self, a):
        return a is not None and self.lo <= a < self.end and not (a & 3)

    def target(self, insn):
        tok = insn.ops[-1] if insn.ops else ""
        return int(tok, 16) if tok.startswith("0x") else None

    def reg(self, tok):
        return int(self.L._reg(tok))

    def find_entries(self):
        """Strong entries: the job's entry and the targets of its calls. Weak ones: addresses the code builds
        with ila/il, or the image holds as a word, that are lift entries and start like a procedure (a stack
        store or stack adjustment in the first three instructions, or a leaf after a flow break that no
        branch targets), and the constants loaded into r0 (return points the job makes itself)."""
        strong = {self.entry}
        dtargets = set()
        self.links = defaultdict(set)
        for a, i in self.ins.items():
            if i.mn in ("brsl", "brasl"):
                t = self.target(i)
                if t is not None and t != a and t != a + 4 and self.code(t) and t not in self.hooked:
                    strong.add(t)
                    self.links[t].add(i.raw & 0x7F)
            elif i.mn in ("br", "bra") or i.mn in COND_BR:
                t = self.target(i)
                if t is not None:
                    dtargets.add(t)
        self.strong = strong

        def prologue(a):
            for k in range(3):
                i = self.ins.get(a + 4 * k)
                if i and i.mn == "stqd" and i.ops and i.ops[-1].endswith("($r1)"):
                    return True
                if i and i.mn == "ai" and len(i.ops) == 3 and i.ops[0] == "$r1" and i.ops[1] == "$r1":
                    return True
            return False

        def after_break(a):
            b = a - 4
            while b >= self.lo and self.ins[b].mn in ("nop", "lnop"):
                b -= 4
            j = self.ins.get(b)
            return j is not None and j.mn in ("bi", "br", "bra", "stop", "stopd")

        def funclike(a):
            return a in self.lset and (prologue(a) or (after_break(a) and a not in dtargets))
        cands = set()
        self.entries = set(strong)
        for a, i in self.ins.items():
            if i.mn in ("ila", "il") and len(i.ops) == 2:
                try:
                    v = int(i.ops[1], 0)
                except ValueError:
                    continue
                for c in (v, v + self.base):
                    if self.code(c) and (funclike(c) or (i.raw & 0x7F) == 0):
                        cands.add(c)
        # function pointers in the image's data, past the code the calls reach (vtables, the constructor table)
        self.entries = set(strong)
        sbody = set()
        for e in strong:
            sbody |= self.walk(e)["body"]
        code_end = max(sbody) + 4
        for o in range(code_end - self.base, len(self.blob) - 3, 4):
            v = struct.unpack(">I", self.blob[o:o + 4])[0]
            for c in (v, v + self.base):
                if self.code(c) and (after_break(c) or prologue(c)) and self.ins[c].raw >= 0x10000:
                    cands.add(c)
        # every lift entry that starts like a procedure (a stack store or adjustment first): reached through a
        # pointer the code computes, a procedure of its own
        for c in self.lift_starts:
            if self.code(c) and prologue(c) and c not in dtargets:
                cands.add(c)
        for c in self.ins:                  # (and where the lift has none: code after a break that saves registers)
            if c > self.lo and prologue(c) and after_break(c) and c not in dtargets and self.ins[c].raw >= 0x10000:
                cands.add(c)
        # tables of branch stubs (`br X; lnop` at 8-byte steps) that calls go through, and where they go
        for a, i in self.ins.items():
            if i.mn != "br" or a < self.lo + 8:
                continue
            nxt = self.ins.get(a + 8)
            pad = self.ins.get(a + 4)
            prev = self.ins[a - 4]
            starts = not self.falls(prev) or (prev.mn in ("lnop", "nop") and not self.falls(self.ins[a - 8]))
            if starts and pad is not None and pad.mn in ("lnop", "nop") and nxt is not None and nxt.mn == "br":
                k = a
                while self.ins.get(k) is not None and self.ins[k].mn == "br" and self.ins.get(k + 4) is not None                         and self.ins[k + 4].mn in ("lnop", "nop"):
                    cands.add(k)
                    t = self.target(self.ins[k])
                    if self.code(t):
                        cands.add(t)
                    k += 8
        self.entries = (strong | cands) - self.hooked
        self.stats["weak"] = len(cands - strong)

    def table_targets(self, e, a):
        """The labels a jump through a register at a can go to: the targets of an offset table right after the
        branch (words below 0x10000: a case's distance from the table, or its address), and the blocks of the
        stretch between the procedures before and after the branch that only such a jump can reach (a lift entry
        right after a branch, a return or a stop); with no table there, every lift entry of the stretch. A target
        that is a procedure of its own is reached through wg_call when it runs; any other one is handed back."""
        out = set()
        k = a + 4
        while k < a + 4 + 4 * 512 and k in self.ins and self.ins[k].raw < 0x10000:
            for t in (a + 4 + self.ins[k].raw, self.ins[k].raw):   # an offset from the table, or the address itself
                if self.code(t) and self.ins[t].raw >= 0x10000:
                    out.add(t)
            k += 4
        lo = max([x for x in self.entries if x <= a] + [self.lo])
        hi = min([x for x in self.entries if x > a] + [self.end])
        if not out:
            out = set(x for x in self.lift_starts if lo <= x < hi)
        else:
            for x in self.lift_starts:
                if lo <= x < hi and self.after_flow_break(x):
                    out.add(x)
        return sorted(x for x in out if x not in self.entries and self.code(x) and self.ins[x].raw >= 0x10000)

    def after_flow_break(self, x):
        """nothing falls into x (the instruction before it, past nop padding, never goes on to the next)"""
        b = x - 4
        while b >= self.lo and self.ins[b].mn in ("nop", "lnop"):
            b -= 4
        return b >= self.lo and not self.falls(self.ins[b])

    def extent_end(self, e):
        later = [s for s in self.strong if s > e]
        return min(later) if later else self.end

    def block_const(self, i, reg, limit=12):
        """The value of `reg` at instruction i when the straight-line code before it sets it to a constant
        (brsl rX,.+4 / il / ila, then ai / ori)."""
        chain = []
        a = i.addr - 4
        n = 0
        while n < limit and a >= self.lo:
            j = self.ins[a]
            if j.mn in ("bi", "br", "bra", "iret", "stop", "stopd") or j.mn in COND_BR or j.mn in BI_FAMILY:
                return None
            if reg in self.writes(j):
                if j.mn in ("brsl", "brasl"):
                    if self.target(j) != j.addr + 4:
                        return None
                    v = (j.addr + 4) & 0x3FFFC
                elif j.mn == "ila":
                    v = int(j.ops[1], 0)
                elif j.mn == "il":
                    v = int(j.ops[1], 0) & 0xFFFFFFFF
                elif j.mn in ("ai", "ori") and self.reg(j.ops[1]) == reg:
                    chain.append((j.mn, int(self.L._imm(j.ops[2]), 0)))
                    a -= 4
                    n += 1
                    continue
                else:
                    return None
                for mn, k in reversed(chain):
                    v = (v + k) & 0xFFFFFFFF if mn == "ai" else (v | (k & 0xFFFFFFFF))
                return v
            a -= 4
            n += 1
        return None

    def writes(self, j):
        if j.mn in self.L._NO_RT_WRITE or j.mn in NOP:
            return set()
        if j.mn in ("bisl", "brsl", "brasl", "bisled"):
            return {j.raw & 0x7F}
        return {self.L._dest_reg(j.d)}

    def is_link_return(self, i):
        return i.mn in BI_FAMILY and self.reg(i.ops[0] if i.mn == "bi" else i.ops[1]) == 0

    def walk(self, e):
        """The procedure at e: its body (instructions reached from e without leaving through a call, a return
        or a branch to another procedure), its labels, its jump tables."""
        body, work, labels = set(), [e], {e}
        switches = {}
        while work:
            a = work.pop()
            if a in body or not self.code(a):
                continue
            body.add(a)
            i = self.ins[a]
            mn = i.mn

            def go(t, fall=False):
                if not self.code(t) or t == self.endpc:      # (a job-chain job's branch to LS 0: its end)
                    return
                if (t in self.entries or t in self.hooked) and t != e:
                    return                              # a tail call (or a hand-written native: handed over there)
                if not fall:
                    labels.add(t)
                work.append(t)
            if mn in ("br", "bra"):
                go(self.target(i))
            elif mn in COND_BR:
                go(self.target(i))
                go(a + 4, True)
            elif mn in ("brsl", "brasl", "bisl"):
                labels.add(a + 4)
                go(a + 4, True)
            elif mn in BI_FAMILY:
                if mn != "bi":
                    go(a + 4, True)
                reg = self.reg(i.ops[0] if mn == "bi" else i.ops[1])
                v = self.block_const(i, reg)
                if v is not None:
                    switches[a] = ("const", v & 0x3FFFC)
                    go(v & 0x3FFFC)
                elif reg != 0 and reg not in self.links.get(e, ()):
                    # a jump through a register: the targets of an offset table right after it are labels here;
                    # anything else is decided when it runs (a return, one of the job's procedures, or handed back)
                    cand = self.table_targets(e, a)
                    switches[a] = ("table", cand)
                    for s in cand:
                        go(s)
            elif mn in STOPS:
                continue
            else:
                go(a + 4, True)
        return {"entry": e, "body": body, "labels": labels, "switches": switches}

    # ---------------------------------------------------------------- C text of one data instruction
    def data_line(self, i):
        """(C text, registers read, registers written, kind) of a non-control instruction."""
        key = i.addr
        if key in self.lines:
            return self.lines[key]
        mn = i.mn
        if mn in NOP:
            res = ("", set(), set(), "nop")
        elif mn in ("br", "bra", "brsl", "brasl", "bisl") or mn in COND_BR or mn in BI_FAMILY or mn in HALTS \
                or mn in STOPS:
            res = ("", set(), set(), "control")
        else:
            func = self.L.LiftedFunction(name="x", start_addr=i.addr, end_addr=i.addr + 4)
            try:
                c = self.lifter._translate(i.d, func)
            except Exception:
                c = "spu_unsupported"
            c = c.replace("@SY@", "").replace("@WB@", "").replace("@RL@", "").strip()
            if "spu_unsupported" in c or "TODO" in c or "return" in c or "goto" in c or "@" in c:
                res = ("", set(), set(), "handback")
            else:
                c = re.sub(r"ctx->gpr\[(\d+)\]\._u32\[0\]", r"spu_w0(r\1)", c)
                c = re.sub(r"ctx->gpr\[(\d+)\]", r"r\1", c)
                if "ctx->gpr" in c:
                    res = ("", set(), set(), "handback")
                else:
                    c = self.fp_barrier(c)
                    m = re.match(r"r(\d+) = (.*)$", c, re.S)
                    if m:
                        w = {int(m.group(1))}
                        r = set(int(x) for x in re.findall(r"\br(\d+)\b", m.group(2)))
                    else:
                        w = set()
                        r = set(int(x) for x in re.findall(r"\br(\d+)\b", c))
                    res = (c, r, w, "channel" if mn in CHANNEL else "data")
        self.lines[key] = res
        return res

    def fp_barrier(self, c):
        """A float operation's register operands through spu_opq() (spu_helpers.h): a procedure here is one C
        function where the lift had many, so a literal one lift function made (il, ilhu, ...) can reach a float
        operation of another one in the same C function, and the compiler folds constant float arithmetic in
        round-to-nearest without denormals-are-zero (x * 1.0 stays a denormal x; the SPU flushes it): n_static
        found that fold in its merged lift (w01673300). Behind spu_opq the operation always runs, with the SPU's
        float mode, as the interpreter runs it. No code for run-time values (an empty asm constraint)."""
        m = re.match(r"^(r\d+ = )(spu_\w+)\((.*)\);$", c, re.S)
        if not m or m.group(2) not in FP_HELPERS:
            return c
        args, depth, cur = [], 0, ""
        for ch in m.group(3):
            if ch == "," and depth == 0:
                args.append(cur.strip()); cur = ""
                continue
            depth += ch == "("
            depth -= ch == ")"
            cur += ch
        args.append(cur.strip())
        args = [f"spu_opq({a})" if re.fullmatch(r"r\d+", a) else a for a in args]
        return f"{m.group(1)}{m.group(2)}({', '.join(args)});"

    def regs_of(self, i):
        """(read, written) registers of instruction i (control ones included)."""
        txt, r, w, kind = self.data_line(i)
        if kind != "control":
            return r, w
        mn = i.mn
        r = set()
        if mn in COND_BR:
            r.add(self.reg(i.ops[0]))
        elif mn == "bi":
            r.add(self.reg(i.ops[0]))
        elif mn in BI_FAMILY:
            r |= {self.reg(i.ops[0]), self.reg(i.ops[1])}
        elif mn == "bisl":
            r.add(self.reg(i.ops[-1]))
        elif mn in HALTS:
            r |= set(int(x) for x in re.findall(r"\$r(\d+)", i.d.operands))
        return r, self.writes(i)

    # ---------------------------------------------------------------- contracts
    def call_target(self, i):
        t = self.target(i)
        if t is None or t == i.addr + 4:
            return None
        return t

    def contracts(self):
        for e, p in self.procs.items():
            used, wrote = set(), set()
            rt = dirt = False
            calls = set()
            for a in p["body"]:
                i = self.ins[a]
                r, w = self.regs_of(i)
                used |= r | w
                wrote |= w
                kind = self.data_line(i)[3]
                if kind == "channel":
                    dirt = True
                if i.mn == "bisl":
                    rt = dirt = True
                if (i.mn in BI_FAMILY or i.mn == "bisl") and (i.raw & 0x40000):
                    dirt = True
                if i.mn in BI_FAMILY and p["switches"].get(a, ("",))[0] == "table":
                    rt = dirt = True               # may go on in any procedure (wg_call): every register
                if i.mn in ("brsl", "brasl"):
                    t = self.call_target(i)
                    if t is not None:
                        if t in self.procs:
                            calls.add(t)
                        else:
                            rt = dirt = True
                # tails: branches / fall-through to other procedures
                for t in self.exits_of(p, i):
                    if t in self.procs and t != e:
                        calls.add(t)
                    if t in self.hooked:
                        rt = dirt = True
            p.update(used=used, wrote=wrote, rt=rt, dirt=dirt, calls=calls)
        for p in self.procs.values():
            p["T"], p["W"] = set(p["used"]), set(p["wrote"])
        changed = True
        while changed:
            changed = False
            for p in self.procs.values():
                for c in p["calls"]:
                    q = self.procs[c]
                    for k in ("rt", "dirt"):
                        if q[k] and not p[k]:
                            p[k] = True
                            changed = True
                    if not q["T"] <= p["T"]:
                        p["T"] |= q["T"]
                        changed = True
                    if not q["W"] <= p["W"]:
                        p["W"] |= q["W"]
                        changed = True
        for p in self.procs.values():
            if p["rt"] or DEBUG:
                p["T"], p["W"] = set(ALL), set(ALL)
        self.stats["rt"] = sum(1 for p in self.procs.values() if p["rt"])
        self.stats["pure"] = sum(1 for p in self.procs.values() if not p["dirt"])

    def exits_of(self, p, i):
        """Static targets i can transfer to outside the procedure's body (tail calls)."""
        a, mn = i.addr, i.mn
        out = []
        if mn in ("br", "bra") or mn in COND_BR:
            t = self.target(i)
            if t is not None and t not in p["body"]:
                out.append(t)
        sw = p["switches"].get(a)
        if sw and sw[0] == "const" and sw[1] not in p["body"]:
            out.append(sw[1])
        if self.falls(i) and a + 4 not in p["body"]:
            out.append(a + 4)
        return [t for t in out if t != self.endpc]

    def falls(self, i):
        mn = i.mn
        if mn in ("br", "bra", "bi") or mn in STOPS:
            return False
        return True

    # ---------------------------------------------------------------- dirty (interrupt can be pending) analysis
    def dirty_points(self, p):
        """Addresses in the body where an interrupt test goes (a lift entry the code reaches with something
        done since the last test that can make one pending), and calls / tails reached that way."""
        body = p["body"]
        din = defaultdict(bool)
        order = sorted(body)
        changed = True
        succ = {}
        for a in order:
            i = self.ins[a]
            s = []
            if self.falls(i) and a + 4 in body:
                s.append(a + 4)
            t = self.target(i) if (i.mn in ("br", "bra") or i.mn in COND_BR) else None
            if t is not None and t in body:
                s.append(t)
            sw = p["switches"].get(a)
            if sw:
                if sw[0] == "const":
                    if sw[1] in body:
                        s.append(sw[1])
                else:
                    s += [x for x in sw[1] if x in body]
            succ[a] = s
        while changed:
            changed = False
            for a in order:
                i = self.ins[a]
                d = din[a] and a not in self.lset           # a test at a lift entry clears it
                d = d or self.makes_dirty(i)
                for t in succ[a]:
                    if d and not din[t]:
                        din[t] = True
                        changed = True
        return din

    def makes_dirty(self, i):
        kind = self.data_line(i)[3]
        if kind == "channel":
            return True
        if i.mn == "bisl":
            return True
        if (i.mn in BI_FAMILY or i.mn == "bisl") and (i.raw & 0x40000):
            return True
        if i.mn in ("brsl", "brasl"):
            t = self.call_target(i)
            if t is not None:
                return t not in self.procs or self.procs[t]["dirt"]
        return False

    # ---------------------------------------------------------------- emission
    def name(self, e):
        return f"wg_{e:05X}"

    def emit(self):
        o = [f"/* Generated by wgen_gen.py from this copy's {self.img}: do not edit, do not publish. */",
             f"#define WGEN_PROCS {len(self.procs)}"]
        for e in sorted(self.procs):
            o.append(f"static SPU_SV uint32_t {self.name(e)}(spu_context* ctx, uint32_t _E);")
        o.append("static SPU_SV uint32_t wg_call(spu_context* ctx, uint32_t pc, uint32_t _E);")
        o.append("static int wg_is_entry(uint32_t pc);")
        o.append("")
        for e in sorted(self.procs):
            o.extend(self.emit_proc(self.procs[e]))
            o.append("")
        o.append("static SPU_SV uint32_t wg_call(spu_context* ctx, uint32_t pc, uint32_t _E) {")
        o.append("    switch (pc) {")
        ents = sorted(e for e in self.procs if e != self.endpc)    # (a job-chain job's LS 0 is its end, not a call)
        for e in ents:
            o.append(f"    case 0x{e:X}u: return {self.name(e)}(ctx, _E);")
        o.append("    default: return pc;")
        o.append("    }")
        o.append("}")
        o.append("static int wg_is_entry(uint32_t pc) {")
        o.append("    switch (pc) {")
        for k in range(0, len(ents), 8):
            o.append("    " + " ".join(f"case 0x{e:X}u:" for e in ents[k:k + 8]))
        o.append("        return 1;")
        o.append("    default: return 0;")
        o.append("    }")
        o.append("}")
        o.append(f"#define WGEN_ENTRY_FN {self.name(self.entry)}")
        if self.kind == "chain":     # (native_wgen.h WG_HOOK: how the job ends)
            o.append(f"#define WGEN_END_PC 0x{self.endpc:X}u      /* a job-chain job: a branch to LS 0 ends it too */")
            o.append("#define WGEN_CHAIN 1")
        else:
            o.append("#define WGEN_END_PC 0xFFFFFFFFu      /* (none: the job returns to the address in r0) */")
            o.append("#define WGEN_CHAIN 0")
        return "\n".join(o) + "\n"

    def st(self, regs):
        return " ".join(f"ctx->gpr[{r}] = r{r};" for r in sorted(regs))

    def ld(self, regs):
        return " ".join(f"r{r} = ctx->gpr[{r}];" for r in sorted(regs))

    def emit_proc(self, p):
        e = p["entry"]
        body = sorted(p["body"])
        used = sorted(p["used"])
        self.cur = p
        self.STO = self.st(p["wrote"])
        self.din = self.dirty_points(p)
        o = [f"/* procedure 0x{e:05X}: {len(body)} instructions{', reaches the runtime' if p['rt'] else ''} */",
             f"static SPU_SV uint32_t {self.name(e)}(spu_context* ctx, uint32_t _E) {{",
             "    uint8_t* const _ls = ctx->ls; const int _rd = g_spu_ls_rdiag, _wd = g_spu_ls_wdiag;",
             "    (void)_ls; (void)_rd; (void)_wd; (void)_E;"]
        for k in range(0, len(used), 12):
            o.append("    u128 " + ", ".join(f"r{r}" for r in used[k:k + 12]) + ";")
        if used:
            o.append("    " + self.ld(used))
        if body[0] != e:
            o.append(f"    goto L_{e:05X};      /* the entry is not the lowest address of the body */")
        prev = None
        for idx, a in enumerate(body):
            i = self.ins[a]
            if a in p["labels"] or prev is None or prev != a - 4 or not self.falls(self.ins[prev]):
                o.append(f"L_{a:05X}:;")
            if a in self.lset and self.din[a]:
                o.append("    " + self.check(a))
            lines = self.emit_insn(p, i)
            o.extend("    " + ln for ln in lines if ln)
            if self.falls(i) and (idx + 1 >= len(body) or body[idx + 1] != a + 4):
                o.append("    " + self.jump(p, a + 4, self.dirty_after(i, a)))
            prev = a
        o.append("}")
        return o

    def dirty_after(self, i, a):
        return (self.din[a] and a not in self.lset) or self.makes_dirty(i)

    def handback(self, pc_expr, why):
        """Hand the SPU to the lifted code at pc_expr. A procedure its callers call with every register stored
        runs the lifted code from there in a bracket until it returns to this procedure's return address, and
        goes back to native code there (as a lifted call would); any other unwinds to the job's entry, which
        gives the rest of the job to the lifted code."""
        self.stats["handback"] += 1
        if self.cur["rt"]:
            return (f"{{ {self.STO} WG_HB_IN(ctx, WG_HB_{why}, {pc_expr}); {{ int32_t _si = (int32_t)ctx->image_id; "
                    f"ctx->pc = {pc_expr}; g_spu_trampoline_fn = spu_indirect_branch; ctx->host_depth++; spu_drain_call(ctx, _E); "
                    f"ctx->host_depth--; spu_img_restore(ctx, _si); }} return ctx->pc & SPU_LS_MASK; }}")
        return f"{{ {self.STO} WG_HB(ctx, WG_HB_{why}, {pc_expr}); return {pc_expr}; }}"

    def check(self, a):
        self.stats["checks"] += 1
        return (f"if (__builtin_expect(ctx->int_enable && (ctx->event_status & ctx->event_mask), 0)) "
                + self.handback(f"0x{a:X}u", "IRQ"))

    def jump(self, p, t, dirty):
        """A branch to t from procedure p (dirty: an interrupt may be pending, to be tested where the lift does)."""
        if t == self.endpc:
            return f"{{ {self.STO} return 0x{t:X}u; }}"    # the job's end: the runtime's drain ends it there
        if t in p["body"]:
            return f"goto L_{t:05X};"
        if t in self.procs:
            pre = self.check(t) + " " if dirty and t in self.lset else ""
            dbg = ""
            if DEBUG:
                dbg = (f"if (wg_forced(0x{t:X}u)) {{ int32_t _si = (int32_t)ctx->image_id; ctx->pc = 0x{t:X}u; ctx->host_depth++; "
                       f"spu_indirect_branch(ctx); spu_drain_call(ctx, _E); ctx->host_depth--; spu_img_restore(ctx, _si); "
                       f"return ctx->pc; }} ")
            return f"{{ {pre}{self.STO} {dbg}return {self.name(t)}(ctx, _E); }}"
        return self.handback(f"0x{t:X}u", "JUMP")

    def call(self, p, q, a, ret):
        """A call of procedure q at a (link already set) that returns to ret."""
        Q = self.procs[q]
        tw = Q["T"] | Q["W"]
        pre = (self.check(a) + " ") if self.din[a] and a not in self.lset else ""   # (at a lift entry: tested above)
        before = self.st(p["wrote"] & tw)
        rest = self.st(p["wrote"] - tw)
        after = self.ld(p["used"] & Q["W"])
        callx = (f"{self.name(q)}(ctx, 0x{ret:X}u)" if Q["dirt"] else
                 f"WG_CALL({self.name(q)}, 0x{q:X}u, ctx, 0x{ret:X}u)")
        if DEBUG:      # (every register is in the context here: T = W = ALL)
            callx = f"(wg_forced(0x{q:X}u) ? (ctx->pc = 0x{q:X}u, wg_lifted_call(ctx, 0x{ret:X}u)) : {callx})"
        if p["rt"]:
            follow = (f"uint32_t _f = _r; while (_f != 0x{ret:X}u && wg_is_entry(_f)) _f = wg_call(ctx, _f, 0x{ret:X}u); "
                      f"if (_f != 0x{ret:X}u) return _f; {self.ld(p['used'])} goto WG_ON_{a:05X}; ")
            return [f"{pre}{before} {{ const uint32_t _r = {callx}; "
                    f"if (__builtin_expect(_r != 0x{ret:X}u, 0)) {{ {rest} {follow}}} }} {after} WG_ON_{a:05X}:;"]
        return [f"{pre}{before} {{ const uint32_t _r = {callx}; "
                f"if (__builtin_expect(_r != 0x{ret:X}u, 0)) {{ {rest} return _r; }} }} {after}"]

    def rt_call(self, a, ret, tg):
        """A call into the runtime, as the lift makes it: every register in the context. In a SPURS job image a
        call of a lift entry (a hand-written native's address) calls that entry's function by its name, as the
        lift does (no dispatcher lookup)."""
        self.stats["rtcalls"] += 1
        t = int(tg.rstrip("u"), 16) if tg.startswith("0x") else None
        if self.kind != "wws" and t is not None and t in self.lset:
            return [f"{self.st(self.cur['wrote'])} {{ int32_t _si = (int32_t)ctx->image_id; ctx->host_depth++; "
                    f"WG_RT(ctx); gt5_{self.img}_spu_func_{t:08X}_sv(ctx); spu_drain_call(ctx, 0x{ret:X}u); ctx->host_depth--; "
                    f"spu_img_restore(ctx, _si); }} {self.ld(self.cur['used'])}"]
        return [f"{self.st(self.cur['wrote'])} {{ int32_t _si = (int32_t)ctx->image_id; ctx->pc = {tg}; ctx->host_depth++; "
                f"WG_RT(ctx); spu_indirect_branch(ctx); spu_drain_call(ctx, 0x{ret:X}u); ctx->host_depth--; "
                f"spu_img_restore(ctx, _si); }} {self.ld(self.cur['used'])}"]

    def local_switch(self, p, var):
        """goto the label var names when it is one of this procedure's (a branch through a register)"""
        ts = sorted(t for t in p["labels"] if t in p["body"] and t != self.endpc)
        if not ts:
            return ""
        return f"switch ({var}) {{ " + " ".join(f"case 0x{t:X}u: goto L_{t:05X};" for t in ts) + " default: break; } "

    def ied(self, i):
        return "ctx->int_enable = 1; " if i.raw & 0x40000 else ("ctx->int_enable = 0; " if i.raw & 0x80000 else "")

    def cond(self, mn, reg):
        w = f"spu_w0(r{reg})"
        h = f"(uint16_t)spu_w0(r{reg})"
        return {"brz": f"{w} == 0", "brnz": f"{w} != 0", "brhz": f"{h} == 0", "brhnz": f"{h} != 0",
                "biz": f"{w} == 0", "binz": f"{w} != 0", "bihz": f"{h} == 0", "bihnz": f"{h} != 0"}[mn]

    def emit_insn(self, p, i):
        a, mn = i.addr, i.mn
        txt, r, w, kind = self.data_line(i)
        dirty_here = self.din[a] and a not in self.lset
        if kind == "nop":
            return []
        if kind == "channel" and DEBUG:
            return [self.STO + " " + txt]
        if kind in ("data", "channel"):
            return [txt]
        if kind == "handback":
            return [self.handback(f"0x{a:X}u", "INSN")]
        if mn in ("br", "bra"):
            t = self.target(i)
            if t == a:
                return [self.handback(f"0x{a:X}u", "INSN")]
            return [self.jump(p, t, dirty_here)]
        if mn in COND_BR:
            t = self.target(i)
            return [f"if ({self.cond(mn, self.reg(i.ops[0]))}) {self.jump(p, t, dirty_here)}"]
        if mn in ("brsl", "brasl"):
            t = self.target(i)
            lr = i.raw & 0x7F
            link = f"r{lr} = spu_link(0x{(a + 4) & 0x3FFFC:X});"
            if t == a:
                return [link, self.handback(f"0x{a:X}u", "INSN")]
            if t == a + 4:
                return [link]
            if t in self.procs:
                return [link] + self.call(p, t, a, a + 4)
            return [link] + self.rt_call(a, a + 4, f"0x{t:X}u")
        if mn == "bisl":
            lr = i.raw & 0x7F
            ra = self.reg(i.ops[-1])
            ret = a + 4
            pre = self.check(a) + " " if dirty_here else ""
            return [f"{pre}{{ const uint32_t _tg = spu_w0(r{ra}) & SPU_LS_MASK & ~3u; r{lr} = spu_link(0x{ret & 0x3FFFC:X}); "
                    f"{self.ied(i)}{self.local_switch(p, '_tg')}",
                    f"  if (wg_is_entry(_tg)) {{ {self.STO} const uint32_t _r = wg_call(ctx, _tg, 0x{ret:X}u); "
                    f"if (__builtin_expect(_r != 0x{ret:X}u, 0)) return _r; {self.ld(p['used'])} }}",
                    "  else { " + " ".join(self.rt_call(a, ret, "_tg")) + " }",
                    "}"]
        if mn in BI_FAMILY:
            reg = self.reg(i.ops[0] if mn == "bi" else i.ops[1])
            pre = f"if ({self.cond(mn, self.reg(i.ops[0]))}) " if mn != "bi" else ""
            ied = self.ied(i)
            sw = p["switches"].get(a)
            if sw is not None and sw[0] == "const":
                return [pre + f"{{ {ied}{self.jump(p, sw[1], dirty_here or bool(i.raw & 0x40000))} }}"]
            cases = ""
            if sw is not None:
                self.stats["switch"] += 1
                cases = "switch (_t) { " + " ".join(f"case 0x{t:X}u: goto L_{t:05X};" for t in sw[1] if t in p["body"]) \
                        + " default: break; } "
            irq = ""
            if i.raw & 0x40000:
                irq = (f"if (__builtin_expect(ctx->int_enable && (ctx->event_status & ctx->event_mask), 0) && _t != _E) "
                       f"{self.handback('_t', 'IRQ')} ")
            # a return (_t == _E), a jump to one of the job's procedures (a tail call), or a hand-back
            tail = (self.local_switch(p, "_t") + "if (wg_is_entry(_t)) return wg_call(ctx, _t, _E); ") if p["rt"] else ""
            return [pre + f"{{ const uint32_t _t = spu_w0(r{reg}) & SPU_LS_MASK & ~3u; {ied}{irq}{cases}"
                    f"{self.STO} if (__builtin_expect(_t != _E, 0)) {{ {tail}{self.handback('_t', 'JUMP')} }} return _t; }}"]
        if mn in HALTS:
            func = self.L.LiftedFunction(name="x", start_addr=a, end_addr=a + 4)
            c = self.lifter._translate(i.d, func)
            m = re.match(r"if \((.*?)\) \{", c)
            if not m:
                return [self.handback(f"0x{a:X}u", "INSN")]
            cnd = re.sub(r"ctx->gpr\[(\d+)\]", r"r\1", m.group(1))
            return [f"if ({cnd}) " + self.handback(f"0x{a:X}u", "INSN")]
        return [self.handback(f"0x{a:X}u", "INSN")]


if __name__ == "__main__":
    main()
