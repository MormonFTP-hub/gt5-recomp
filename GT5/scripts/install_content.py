#!/usr/bin/env python3
"""Install your own GT5 game updates (2.xx) and DLC from their PlayStation Store packages.

    python scripts/install_content.py [PATH ...] [--dry-run]

PATH is any mix of files and folders; folders are searched for *.pkg and *.rap. With no PATH,
GT5\\content is used: put the update and DLC .pkg files and your DLC licences (.rap) there, in
any layout. Run it again whenever you add something: what is already installed is skipped.

What it does, all inside GT5\\hdd0 (the port's PS3 hard disk):
  * Game updates for your disc (the title id in GT5\\vfs\\PS3_GAME\\PARAM.SFO) are unpacked in
    version order into hdd0\\game\\<title id>, as the PS3 installs them. Each update brings only
    the files it changes, so they all go in, oldest first. The update's EBOOT.BIN is not
    installed: the newest one is copied to GT5\\input\\EBOOT_<version>.BIN, to be decrypted with
    RPCS3 into GT5\\input\\EBOOT.elf like the disc's (README, Game updates).
  * DLC packages for your title are unpacked into the same folder. GT5's DLC is small: a
    "voucher" (USRDIR\\vou\\vouNN.edat) per item, the cars and tracks themselves are in the
    updates.
  * Each licence (<content id>.rap) becomes the key the runtime decrypts that content with,
    hdd0\\home\\00000001\\exdata\\<content id>.rifkey, and every installed EDAT is checked
    against it. An EDAT without a licence is reported: the game can't read it, so whatever it
    grants stays locked. Nothing is unlocked any other way.

Everything stays on your PC; nothing is sent anywhere. Use it only with content you own.
"""
import argparse
import hashlib
import hmac
import os
import re
import shutil
import struct
import sys
import zlib
from pathlib import Path

try:
    from Crypto.Cipher import AES
    from Crypto.Hash import CMAC
except ImportError:
    sys.exit("install_content.py needs pycryptodome (in the venv setup.ps1 makes: venv\\Scripts\\python.exe)")

GT5 = Path(__file__).resolve().parent.parent
HDD0 = GT5 / "hdd0"
EXDATA = HDD0 / "home" / "00000001" / "exdata"
STAMP_NAME = ".installed_packages.txt"

# Retail PS3 PKG files are AES-128-CTR encrypted under this public platform key.
PKG_KEY = bytes.fromhex("2E7B71D7C9C9A14EA3221F188828B8F8")

# Licence (.rap) -> RIF key, the published NPDRM conversion (RPCS3, make_npdata). It is applied
# only to the .rap files you give it, for content you bought.
RAP_KEY = bytes.fromhex("869F7745C13FD890CCF29188E3CC3EDF")
RAP_PBOX = [0x0C, 0x03, 0x06, 0x04, 0x01, 0x0B, 0x0F, 0x08, 0x02, 0x07, 0x00, 0x05, 0x0A, 0x0E, 0x0D, 0x09]
RAP_E1 = bytes.fromhex("A93E1FD67C55A329B75FDDA62A95C7A5")
RAP_E2 = bytes.fromhex("67D45DA3296D006A4E7C537BF5538C74")
EDAT_KEY = {0: bytes.fromhex("BE959CA8308DEFA2E5E180C63712A9AE"),
            1: bytes.fromhex("4CA9C14B01C95309969BEC68AA0BC081")}


# ---- PARAM.SFO ------------------------------------------------------------------------------------------
def read_sfo(data):
    """{key: value} of a PARAM.SFO (strings and integers)."""
    if data[:4] != b"\0PSF":
        return {}
    key_tab, data_tab, n = struct.unpack("<III", data[8:20])
    out = {}
    for i in range(n):
        ko, fmt, ln, _mx, do = struct.unpack("<HHIII", data[20 + i * 16:36 + i * 16])
        key = data[key_tab + ko:data.index(b"\0", key_tab + ko)].decode()
        raw = data[data_tab + do:data_tab + do + ln]
        out[key] = struct.unpack("<I", raw[:4])[0] if fmt == 0x0404 else raw.rstrip(b"\0").decode("utf-8", "replace")
    return out


# ---- PKG ------------------------------------------------------------------------------------------------
class Pkg:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        h = self.f.read(0xC0)
        if h[:4] != b"\x7fPKG":
            raise ValueError("not a PS3 package")
        rev, typ = struct.unpack(">HH", h[4:8])
        if rev != 0x8000 or typ != 1:
            raise ValueError("not a retail PS3 package (debug and PSP packages aren't supported)")
        self.item_count = struct.unpack(">I", h[0x14:0x18])[0]
        self.data_off = struct.unpack(">Q", h[0x20:0x28])[0]
        self.content_id = h[0x30:0x60].rstrip(b"\0").decode()
        self.digest = h[0x60:0x70].hex()
        self.riv = int.from_bytes(h[0x70:0x80], "big")
        self._items = None

    def close(self):
        self.f.close()

    @property
    def title_id(self):
        return self.content_id[7:16]

    def read(self, rel, size):
        start = rel & ~0xF
        pad = rel - start
        self.f.seek(self.data_off + start)
        enc = self.f.read((pad + size + 15) & ~0xF)
        ctr = AES.new(PKG_KEY, AES.MODE_CTR, nonce=b"",
                      initial_value=(self.riv + start // 16) & ((1 << 128) - 1))
        return ctr.decrypt(enc)[pad:pad + size]

    def items(self):
        if self._items is None:
            tbl = self.read(0, self.item_count * 32)
            self._items = []
            for i in range(self.item_count):
                no, ns, do, ds, flags = struct.unpack(">IIQQI", tbl[i * 32:i * 32 + 28])
                name = self.read(no, ns).decode("utf-8", "replace")
                self._items.append((name, do, ds, flags & 0xFF))
        return self._items

    def file(self, name):
        for n, do, ds, kind in self.items():
            if n == name and kind not in (0x04, 0x12):
                return self.read(do, ds)
        return None

    def extract(self, dest, skip=(), skip_top_files=False):
        for name, do, ds, kind in self.items():
            if name in skip or ".." in Path(name).parts:
                continue
            if skip_top_files and "/" not in name and kind not in (0x04, 0x12):
                continue                 # a DLC's own PARAM.SFO/ICON0.PNG: the game's stay
            out = dest / name
            if kind in (0x04, 0x12):
                out.mkdir(parents=True, exist_ok=True)
                continue
            out.parent.mkdir(parents=True, exist_ok=True)
            with open(out, "wb") as o:
                pos = 0
                while pos < ds:
                    n = min(1 << 22, ds - pos)
                    o.write(self.read(do + pos, n))
                    pos += n


# ---- RAP / EDAT ---------------------------------------------------------------------------------------
def rap_to_rif(rap):
    k = bytearray(AES.new(RAP_KEY, AES.MODE_ECB).decrypt(rap))
    for _ in range(5):
        for i in range(16):
            p = RAP_PBOX[i]
            k[p] ^= RAP_E1[p]
        for i in range(15, 0, -1):
            p, pp = RAP_PBOX[i], RAP_PBOX[i - 1]
            k[p] ^= k[pp]
        o = 0
        for i in range(16):
            p = RAP_PBOX[i]
            kc = (k[p] - o) & 0xFF
            ec2 = RAP_E2[p]
            if o != 1 or kc != 0xFF:
                o = 1 if kc < ec2 else 0
                k[p] = (kc - ec2) & 0xFF
            elif kc == 0xFF:
                k[p] = (kc - ec2) & 0xFF
            else:
                k[p] = kc
    return bytes(k)


def edat_decrypt(data, key):
    """Plaintext of an uncompressed EDAT under `key`, or None when a block hash fails."""
    if data[:4] != b"NPD\0":
        return None
    ver = struct.unpack(">I", data[4:8])[0]
    digest, dev_hash = data[0x40:0x50], data[0x60:0x70]
    flags, bs, size = struct.unpack(">IIQ", data[0x80:0x90])
    if flags & 0x80000001 or bs == 0:
        return None                      # compressed or debug: not used by GT5's vouchers
    nb = (size + bs - 1) // bs
    meta_size = 0x20 if flags & 0x21 else 0x10
    lead = bool(flags & 0x20)
    base = 0x100 + nb * meta_size
    ecb = AES.new(key, AES.MODE_ECB)
    erk = EDAT_KEY[1 if ver == 4 else 0]
    out = bytearray()
    for b in range(nb):
        moff = 0x100 + b * (meta_size + bs) if lead else 0x100 + b * meta_size
        doff = moff + meta_size if lead else base + b * bs
        meta = data[moff:moff + meta_size]
        bh = bytes(x ^ y for x, y in zip(meta[:16], meta[16:32])) if meta_size == 0x20 else meta[:16]
        ln = size % bs if (b == nb - 1 and size % bs) else bs
        enc = data[doff:doff + ((ln + 15) & ~15)]
        kres = ecb.encrypt((dev_hash[:12] if ver > 1 else b"\0" * 12) + struct.pack(">I", b))
        hseed = ecb.encrypt(kres) if flags & 0x10 else kres
        if flags & 0x08:
            ckey = AES.new(erk, AES.MODE_CBC, b"\0" * 16).decrypt(kres)
            hkey = AES.new(erk, AES.MODE_CBC, b"\0" * 16).decrypt(hseed)
        else:
            ckey, hkey = kres, hseed
        if not flags & 0x10:
            mac = CMAC.new(hkey, enc, ciphermod=AES).digest()
        else:
            mac = hmac.new(hkey + (b"\0" * 4 if flags & 0x20 else b""), enc, hashlib.sha1).digest()
        if mac[:16] != bh:
            return None
        out += AES.new(ckey, AES.MODE_CBC, b"\0" * 16 if ver <= 1 else digest).decrypt(enc)[:ln]
    return bytes(out)


def gt5_voucher_grants(plain):
    """What a GT5 voucher grants, for the summary: [(type, key)], type "car" or "dlc"."""
    if plain[:4] != bytes.fromhex("C5EEF7FF"):
        return []
    try:
        xml = zlib.decompress(plain[8:], -15).decode("utf-8", "replace")
    except zlib.error:
        return []
    kind = re.search(r"<type>\s*(\w+)\s*</type>", xml)
    return [(kind.group(1) if kind else "?", k) for k in re.findall(r'<param\s+key="([^"]+)"', xml)]


# ---- install ------------------------------------------------------------------------------------------
def version_key(v):
    return tuple(int(x) for x in re.findall(r"\d+", v or "0")) or (0,)


def find_inputs(paths):
    pkgs, raps = [], []
    for p in paths:
        p = Path(p)
        files = [p] if p.is_file() else sorted(q for q in p.rglob("*") if q.is_file()) if p.is_dir() else []
        for q in files:
            if q.suffix.lower() == ".pkg":
                pkgs.append(q)
            elif q.suffix.lower() == ".rap":
                raps.append(q)
    return pkgs, raps


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("paths", nargs="*", help="files or folders with .pkg and .rap (default: GT5\\content)")
    ap.add_argument("--dry-run", action="store_true", help="say what would be installed, change nothing")
    ap.add_argument("--hdd0", type=Path, help="install into this folder instead of GT5\\hdd0 (for testing)")
    args = ap.parse_args()
    paths = args.paths or [GT5 / "content"]
    global HDD0, EXDATA
    if args.hdd0:
        HDD0 = args.hdd0.resolve()
        EXDATA = HDD0 / "home" / "00000001" / "exdata"

    sfo_path = GT5 / "vfs" / "PS3_GAME" / "PARAM.SFO"
    if not sfo_path.exists():
        sys.exit("GT5\\vfs\\PS3_GAME\\PARAM.SFO not found: copy your disc's PS3_GAME there first (README, Setup).")
    disc = read_sfo(sfo_path.read_bytes())
    title = disc.get("TITLE_ID", "")
    if not re.fullmatch(r"[A-Z]{4}\d{5}", title):
        sys.exit(f"couldn't read the title id from {sfo_path}")
    game_dir = HDD0 / "game" / title
    stamp_path = game_dir / STAMP_NAME
    installed = set(stamp_path.read_text().split()) if stamp_path.exists() else set()
    print(f"Your game: {title}, disc version {disc.get('APP_VER', '?')}")

    pkg_files, rap_files = find_inputs(paths)
    if not pkg_files and not rap_files:
        print(f"No .pkg or .rap files in {', '.join(str(p) for p in paths)}. Nothing to do.")
        return 0

    updates, dlcs = [], []
    for path in pkg_files:
        try:
            pkg = Pkg(path)
        except (OSError, ValueError) as e:
            print(f"  skip  {path.name}: {e}")
            continue
        if pkg.title_id != title:
            print(f"  skip  {path.name}: for {pkg.title_id}, not your {title}")
            pkg.close()
            continue
        sfo = read_sfo(pkg.file("PARAM.SFO") or b"")
        # Both say CATEGORY GD; only a game update has an APP_VER (and usually an EBOOT.BIN).
        if "APP_VER" in sfo or any(n == "USRDIR/EBOOT.BIN" for n, *_ in pkg.items()):
            updates.append((version_key(sfo.get("APP_VER")), sfo.get("APP_VER", "?"), pkg))
        else:
            dlcs.append(pkg)
    updates.sort(key=lambda u: u[0])

    # Updates build on each other: if one older than an installed one is new here, redo the chain.
    newest_done = max((u[0] for u in updates if u[2].digest in installed), default=None)
    redo = any(u[2].digest not in installed and newest_done is not None and u[0] < newest_done
               for u in updates)
    todo_updates = [u for u in updates if redo or u[2].digest not in installed]
    todo_dlcs = [p for p in dlcs if p.digest not in installed]

    print(f"Game updates: {len(updates)} found, {len(todo_updates)} to install"
          + (" (re-applying all in order)" if redo else ""))
    print(f"DLC packages: {len(dlcs)} found, {len(todo_dlcs)} to install")
    print(f"Licences (.rap): {len(rap_files)} found")
    if args.dry_run:
        for _, v, p in todo_updates:
            print(f"  would install update {v}: {p.path.name}")
        for p in todo_dlcs:
            print(f"  would install DLC {p.content_id}: {p.path.name}")
        return 0

    for _, ver, pkg in todo_updates:
        print(f"  update {ver} ({pkg.path.name})...", flush=True)
        pkg.extract(game_dir, skip={"USRDIR/EBOOT.BIN"})
        installed.add(pkg.digest)
        stamp_path.write_text("\n".join(sorted(installed)) + "\n")
    if updates:
        ver, pkg = updates[-1][1], updates[-1][2]
        dest = GT5 / "input" / f"EBOOT_{ver}.BIN"
        if not dest.exists():
            eboot = pkg.file("USRDIR/EBOOT.BIN")
            if eboot:
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(eboot)
        print(f"  newest update {ver}: its executable is GT5\\input\\{dest.name}. Decrypt it with RPCS3 "
              f"(Utilities > Decrypt PS3 Binaries) and save the result as GT5\\input\\EBOOT.elf.")

    for pkg in todo_dlcs:
        print(f"  DLC {pkg.content_id} ({pkg.path.name})", flush=True)
        pkg.extract(game_dir, skip_top_files=True)
        installed.add(pkg.digest)
        stamp_path.write_text("\n".join(sorted(installed)) + "\n")
    for _, _, p in updates:
        p.close()
    for p in dlcs:
        p.close()

    # Licences: one key per content id.
    if rap_files:
        EXDATA.mkdir(parents=True, exist_ok=True)
    for rap in rap_files:
        cid = rap.stem
        data = rap.read_bytes()
        if len(data) != 16 or not re.fullmatch(r"[A-Z]{2}\d{4}-[A-Z]{4}\d{5}_\d{2}-[A-Z0-9]{16}", cid):
            print(f"  skip  {rap.name}: not a licence (expected <content id>.rap, 16 bytes)")
            continue
        (EXDATA / f"{cid}.rifkey").write_bytes(rap_to_rif(data))

    # Check every installed EDAT against its licence.
    grants, ok, unlicensed, bad = [], 0, [], []
    for edat in sorted(game_dir.rglob("*.edat")) if game_dir.exists() else []:
        data = edat.read_bytes()
        if data[:4] != b"NPD\0":
            continue
        cid = data[0x10:0x40].rstrip(b"\0").decode("ascii", "replace")
        if struct.unpack(">I", data[8:12])[0] & 3 == 3:
            continue                     # free content: the game's own key, nothing to check here
        key_file = EXDATA / f"{cid}.rifkey"
        if not key_file.exists():
            unlicensed.append((edat.name, cid))
            continue
        plain = edat_decrypt(data, key_file.read_bytes())
        if plain is None:
            bad.append((edat.name, cid))
            continue
        ok += 1
        grants += gt5_voucher_grants(plain)
    cars = sum(1 for kind, _ in grants if kind == "car")
    packs = sum(1 for kind, _ in grants if kind == "dlc")
    print(f"DLC licences: {ok} verified" + (f" (GT5: {cars} gift cars, {packs} packs)" if grants else ""))
    for name, cid in unlicensed:
        print(f"  no licence for {name} ({cid}): it stays locked until you add {cid}.rap and run this again")
    for name, cid in bad:
        print(f"  {name} ({cid}) doesn't decrypt with its licence: is the .rap for this content?")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
