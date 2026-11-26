#!/usr/bin/env python3
"""Find GT5's SPU job images in this copy's EBOOT, for lift_spu_jobs.py to compile.

GT5 hands most of its SPU work (physics, sound, graphics set-up) to SPURS as raw job images
stored inside EBOOT.elf. Each one is written to analysis/spu_miss under the name the runtime would
have dumped it with, the fingerprint the runtime registers it by (FNV-1a of its bytes). lift_spu_jobs.py
then lifts them to C, and the build compiles them in.

How they are found (static2_spu, 24 Nov 2026: by what they are, not by a list). A SPURS job binary and a
policy module start with a 16-byte identifier made of four `ila $r2` instructions; a job binary has its
entry header right after it (job_entry_header below); a Wws job (0xC0DEC0DE at +0x20) is lift_spu_jobs.py's and
is left out here. Every such start in the EBOOT's loaded segments is a candidate. Its size is the one the
game keeps next to its address (a word with the address, then a word with the size, as the game's job
table holds them), rounded up as the runtime's DMA takes it: 16 bytes for a job binary, 128 for a policy
module. For the few the EBOOT gives no size next to their address, the size the known list below has
for those bytes (no game data is stored in this script: hashes and sizes only). The list also checks
that nothing it names is lost: every job of the European release (BCES00569, disc 01.00), which the
other discs share, in one of its versions (the game updates' 2.xx versions are listed too).

A job binary the game points at that cannot be sized, two sizes for one image, or a listed job found only
by its hash (the scan missed it) fail this step with a plain line: setup stops there, instead of the game
stalling at a black screen later. A candidate the game never points at, and policy-module-like code with no
size stated for it (no game run has ever started one), are reported and left out; a listed job that is not
in this EBOOT at all is reported as before (the 2.xx EBOOTs carry two of the disc's sound jobs elsewhere).

    python scripts/spu_jobs_from_eboot.py [input/EBOOT.elf]
"""
import hashlib
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "analysis" / "spu_miss"
PREFIX = 64   # bytes hashed at each candidate offset
STEP = 16     # SPU images are at least quadword aligned

# (runtime fingerprint, size, SHA-1 of the first PREFIX bytes, SHA-1 of the whole image)
JOBS = [
    ("1B6BDAC375C5438D", 13840, "1dcbe5569e3ff030af74a4ee4a6bc68dc34b5916", "6428d06556207eb480f5f6f53d2b4637bc47a959"),
    ("48AF2BC79A72CB50", 25232, "d059dbdc8f7bc5bb07edd875d17cd28d34d172a4", "a33efe337060b6984fbe0d5c4b8ffcd83be6ae4f"),
    ("49C1D6C47E80AF43", 24720, "a5ca7ca714dfd3d42f6d2791c52e0aabe91d9fe5", "bab6bc8bd1c240db96c648e01ab549958fb5998e"),
    ("5DA195169669A5FF", 72464, "912583737f277c4b255face079ae6ce444b5d295", "724e11c2b86807bc4e7858f39a86dc010271c2f5"),
    ("650EE1A07AEBB151", 22032, "9a05ed1196c433d422230bbe2c81492c5d014ce8", "600792fb187ca897f81879dea87b6fbec6aa8163"),
    ("75BFE68AE96D7577", 12032, "3e6a0b4551e424a7315f2dca03bb8a202ff47486", "3cad156b9d0d985e4788140db9728da0bf8cd734"),
    ("80435D36E623FAA0", 19488, "7928e1212b01f0881edfa032020cca4068960679", "8c77e02b4b9bbb84e492f37ac2c332ab1985d485"),
    ("82D0BE145EC28AEE", 17424, "49b0dff67aa20024f04415f899cb6c82b3ad8dc1", "2e1a55c099adba1ce3f2ac1bb8dd7e815bec546b"),
    ("9804ABA84566F431", 20624, "7b875f4adf8082ec22cc4cf0ed3b4a23430da201", "ec458520f805b66025656eda6431d0fdff7bc144"),
    ("ABD49FC58F53F368", 9488, "8ebad749fab419a62a8db9bd90d817e3a4a758b4", "1ba431ca767a4016eab4c06218a0bcca0e60bf30"),
    ("D58063CD2A0236B5", 53408, "078fd4721b87c3870605f474cbab013c6ae0154e", "e934703d61aca2e7f025503e3326dd8d57c21f01"),
    ("EB79FDFCD7112702", 22288, "7276512751ce9dfae150210e4029aa03592499de", "38b144965d2b3a89bc29cc8a6e1970ea26424120"),
    ("F523F26F69764538", 64912, "f767a6c12afd282eb9a9997df10760a79138ba7e", "eb871a28c52e2f5d430fc93145ca3e51291e2fae"),
    ("FDFFAAC1BCD5352E", 34704, "6282b2e494736583faf042793e20804657c901b0", "61d01493b972923284dad53ece6c80516b0af794"),
    ("FE031AF67ED048DE", 46928, "6872f373fae3b3578750a555b2dbf54d3ad2a5da", "65c1baa23a23c6a15eed04ae15137270fcc4d86c"),
]

# Versions changed by game updates: (fingerprint, size, prefix SHA-1, whole SHA-1, the job above it replaces).
UPDATED = [
    ("C2A031BC164B4A45", 21952, "c7f26544a13fbe51db2e9f71e56d8c08ea06f0e3", "7ea77243d007ad477175d54ceae94d25297741dc", "650EE1A07AEBB151"),  # 2.01
    ("E3FDDA10963A6130", 22544, "2a759a61e4fa122c5344b18265352c9ac4836e40", "fe409a4e6c7f4e72c21021aaadd47cd0d7a0eefb", "EB79FDFCD7112702"),  # 2.01
    ("CB9BD0241BB0DE67", 53280, "a3a0364d94ae6bc5a96fc5fd54b803ac72850c11", "7276beb17d4929a04954b77c87f24f07534be190", "D58063CD2A0236B5"),  # 2.17 (US)
    # New in 2.17, no 01.00 counterpart (the second job table, with the two below it).
    ("0B07CA3DA8C030C2", 29920, "73d23dc69f344dfc689b779d8217e0bc9f55db21", "d7bc0a42a7418b6f40478a794d40968473901382", None),  # 2.17 (US)
    ("18DE371921B1A751", 13200, "a4e9776a48878b1eab752497422034a251f27860", "3c24724fb099b4cefc0878905982adf1f8aa3f0b", None),  # 2.17 (US)
    ("CF6352633FE351E3", 24000, "2ecaeef90de6ee0e0ecba183e65005f3e0c44dd9", "26b9d37ca05af1e4cc50fe4d325e2ecccce445be", None),  # 2.17 (US)
]


def job_entry_header(b):
    """The 32 bytes after the identifier of a SPURS job binary: two entry stubs (the same instruction word,
    each followed by a relative branch), then 16 zero bytes. A policy module has its own code there."""
    w = struct.unpack(">8I", b)
    return w[0] != 0 and w[0] == w[2] and (w[1] >> 23) == 0x64 and (w[3] >> 23) == 0x64 and not any(w[4:])


def fingerprint(blob):
    """The runtime's fingerprint of an image (spu_workload.c spu_workload_fingerprint: FNV-1a 64)."""
    h = 1469598103934665603
    for b in blob:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016X}"


def loaded_segments(data):
    """(file offset, address, size) of each loaded segment with bytes in the file."""
    phoff = struct.unpack(">Q", data[0x20:0x28])[0]
    phnum = struct.unpack(">H", data[0x38:0x3A])[0]
    segs = []
    for i in range(phnum):
        kind, _, off, va, _, fsz, _, _ = struct.unpack(">IIQQQQQQ", data[phoff + i * 56:phoff + i * 56 + 56])
        if kind == 1 and fsz:
            segs.append((off, va, fsz))
    return segs


def candidates(data, segs):
    """Every SPURS job binary / policy module start: {address: (file offset, "job" | "pm")}."""
    out = {}
    for off, va, fsz in segs:
        for o in range(off, off + fsz - 0x40, STEP):
            w = struct.unpack(">8I", data[o:o + 32])
            if not all((x >> 25) == 0x21 and (x & 0x7F) == 2 for x in w[:4]):
                continue
            if struct.unpack(">I", data[o + 0x20:o + 0x24])[0] == 0xC0DEC0DE:
                continue                      # a Wws job: lift_spu_jobs.py finds and lifts those
            out[va + o - off] = (o, "job" if job_entry_header(data[o + 0x10:o + 0x30]) else "pm")
    return out


def references(data, segs, addrs):
    """{address: (sizes stated next to it, times it is pointed at at all)} over the loaded segments."""
    sizes = {a: set() for a in addrs}
    seen = {a: 0 for a in addrs}
    for off, va, fsz in segs:
        blk = data[off:off + fsz]
        for i in range(0, len(blk) - 8, 4):
            a = int.from_bytes(blk[i:i + 4], "big")
            if a in seen:
                seen[a] += 1
                s = int.from_bytes(blk[i + 4:i + 8], "big")
                if 0x30 < s <= 0x40000:
                    sizes[a].add(s)
    # an address loaded in two halves by the code (lis/ori, lis/addi) or kept unaligned counts as pointed at
    for a in addrs:
        if not seen[a] and data.find(a.to_bytes(4, "big")) >= 0:
            seen[a] = 1
    return {a: (sizes[a], seen[a]) for a in addrs}


def main():
    eboot = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "input" / "EBOOT.elf"
    data = eboot.read_bytes()
    segs = loaded_segments(data)
    sha1 = hashlib.sha1
    known = {}                                   # 64-byte prefix SHA-1 -> [(fingerprint, size, whole SHA-1)]
    for job in JOBS + UPDATED:
        known.setdefault(job[2], []).append(job[:4])
    cands = candidates(data, segs)
    refs = references(data, segs, cands)
    found, failed, left_out, unsized, by_hash_only = {}, [], [], [], []
    for ea, (o, kind) in sorted(cands.items()):
        stated, pointed = refs[ea]
        align = 16 if kind == "job" else 128
        sizes = sorted({(s + align - 1) & ~(align - 1) for s in stated if o + ((s + align - 1) & ~(align - 1)) <= len(data)})
        listed = [(fp, size) for fp, size, _, full in known.get(sha1(data[o:o + PREFIX]).hexdigest(), [])
                  if sha1(data[o:o + size]).hexdigest() == full]
        if len(sizes) == 1:
            blob = data[o:o + sizes[0]]
            found[fingerprint(blob)] = (o, blob, "stated")
        elif len(sizes) > 1 and listed:
            # two different words follow its address somewhere: the known list says which one is its size
            fp, size = listed[0]
            found[fp] = (o, data[o:o + size], "known")
        elif len(sizes) > 1:
            failed.append(f"the SPU {kind} at 0x{ea:08X} has {len(sizes)} sizes stated for it ({', '.join(hex(s) for s in sizes)})")
        elif listed:
            fp, size = listed[0]
            found[fp] = (o, data[o:o + size], "known")
        elif pointed and kind == "job":
            failed.append(f"the SPU job at 0x{ea:08X} is used by the game, but its size is nowhere next to its address")
        elif pointed:
            unsized.append(ea)
        else:
            left_out.append(ea)
    # what the list names must all be there (its hashes, wherever they are: a safety net for the scan)
    for off in range(0, len(data) - PREFIX, STEP):
        for fp, size, _, full in known.get(sha1(data[off:off + PREFIX]).hexdigest(), []):
            if fp not in found and sha1(data[off:off + size]).hexdigest() == full:
                found[fp] = (off, data[off:off + size], "list only")
                by_hash_only.append(fp)       # used as before; said below, so the scan can be put right
    OUT.mkdir(parents=True, exist_ok=True)
    for fp, (off, blob, how) in sorted(found.items()):
        (OUT / f"spujob_{fp}_{len(blob)}.bin").write_bytes(blob)
    have = set(found) | {old for fp, *_, old in UPDATED if fp in found}
    missing = [fp for fp, *_ in JOBS if fp not in have]
    new = [fp for fp in found if fp not in {j[0] for j in JOBS + UPDATED}]
    upd = [fp for fp, *_, old in UPDATED if old is None and fp in found]
    print(f"SPU jobs found in {eboot.name}: {len(found)} ({sum(1 for v in found.values() if v[2] == 'stated')} sized by the game's "
          f"own table, {sum(1 for v in found.values() if v[2] == 'known')} by the known list); {len(JOBS) - len(missing)} of the "
          f"{len(JOBS)} the list names" + (f", plus {len(upd)} that only game updates have" if upd else "")
          + (f", plus {len(new)} the list does not know" if new else ""))
    if left_out:
        print(f"  left out, the game never points at them: {len(left_out)} piece(s) of SPU code at "
              + ", ".join(f"0x{a:08X}" for a in left_out))
    if unsized:
        print(f"  left out, no size stated for them (no game run has started one): {len(unsized)} policy-module-like piece(s) "
              f"of SPU code at " + ", ".join(f"0x{a:08X}" for a in unsized))
    if by_hash_only:
        print(f"  found by the known list only, not by their header (please report this line): {', '.join(by_hash_only)}")
    for fp in missing:   # (as before: the 2.xx EBOOTs carry two of the disc's sound jobs elsewhere)
        print(f"  not in this EBOOT: {fp} (or a version of it this script knows)")
    if failed:
        print("A part of the game's SPU code could not be prepared, so the game would stop when it needs it:")
        for f in failed:
            print(f"  - {f}")
        sys.exit(1)


if __name__ == "__main__":
    main()
