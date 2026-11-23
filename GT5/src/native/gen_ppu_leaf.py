#!/usr/bin/env python3
"""Native PPU leaf functions, made at build time from this copy's own lift (scripts/ppu_native_patch.py
imports this; nothing of the lifted code is shipped).

A leaf is a lifted function that calls no other guest function: no func_XXXXXXXX call, no indirect call,
no syscall, no HLE import, no trampoline. Its lifted text keeps every guest register in the context
structure in memory (ctx->gpr[3], ctx->vr[18] ...), and the compiler has to load and store them around
every guest memory access, because a guest access goes through a byte pointer it cannot tell apart from
the context. The native form is the same statements with each register the function uses held in a C
local: read from the context once at entry, written back at every return. The operations, their order,
every guest load and store (the same gated scalar paths, the same raw vector paths) and every value are
the lifted ones, so the result is the same by construction: guest memory is never the context.

Refused (the function stays lifted, the build says why): anything but a leaf, a register indexed by a
variable, a context field other than gpr/fpr/vr/cr/xer/vscr/lr/ctr, `ctx` passed whole to a helper, or a
call to a helper not in the list below (each one checked to touch no guest register but through the
pointer it is given).

The compare form (GT5_PPU_NATIVE=2) is the same text again with its guest loads and stores sent to a
shadow log (src/native/ppu_native.h), so it can run on a copy of the registers before the lifted
function runs for real; a function with a guest access of another shape has no compare form (counted).
"""
import re

FIELDS = ("gpr", "fpr", "vr", "cr", "xer", "vscr", "lr", "ctr")
CTYPE = {"gpr": "uint64_t", "fpr": "double", "vr": "ppu_vr", "cr": "uint32_t", "xer": "uint32_t",
         "vscr": "uint32_t", "lr": "uint64_t", "ctr": "uint64_t"}
# Helpers of the lifted code's preamble, ppu_recomp.h and ppu_vmx.h that a leaf may call: each works on
# its arguments (values, or pointers to the registers it is given) and guest memory only.
CALLS = {
    "vm_read8", "vm_read16", "vm_read32", "vm_read64", "vm_write8", "vm_write16", "vm_write32", "vm_write64",
    "vm_raw_ld16", "vm_raw_st16", "VM_RAW_LD", "VM_RAW_ST",
    "ppu_fadd", "ppu_fsub", "ppu_fmul", "ppu_fdiv", "ppu_fp_bin", "ppu_fmadd_core", "ppu_fp_single", "ppu_frsp",
    "ppu_fp_quiet", "ppu_fp_default_qnan", "ppu_f2i32", "ppu_f2i64",
    "ppu_vbswap32", "ppu_vldf4", "ppu_vstf4", "ppu_vldu4", "ppu_vstu4",
    "ppc_rlw64", "ppc_rlwinm", "ppc_rlwimi", "ppc_rotl64", "ppc_mask64", "ppc_rldicl", "ppc_rldicr", "ppc_rldic",
    "ppc_rldimi", "ppc_sraw", "ppc_srad", "ppc_mulhd", "ppc_mulhdu",
    "memcpy", "memset", "sqrt", "sqrtf", "fabs", "fabsf", "fma", "fmaf", "nearbyint", "nearbyintf", "trunc",
    "truncf", "floor", "floorf", "ceil", "ceilf", "copysign", "copysignf",
    "ppu_timebase_now", "PPU_FENCE",
}
CALL_PREFIXES = ("vmx_", "__builtin_")
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "do", "else", "goto", "case"}


def find_function(text, name):
    """(start, end) of `void <name>(ppu_context* ctx) {` .. the closing `}` line, or None."""
    m = re.search(r"^void %s\(ppu_context\* ctx\) \{[^\n]*\n" % re.escape(name), text, re.M)
    if not m:
        return None
    e = re.compile(r"^\}[ \t]*\r?$", re.M).search(text, m.end())
    if not e:
        return None
    end = text.find("\n", e.start())
    return m.start(), (end + 1 if end >= 0 else len(text))


def fn_sha1(ftext):
    import hashlib
    return hashlib.sha1(ftext.replace("\r\n", "\n").encode("latin-1")).hexdigest()


def analyse(ftext):
    """(registers used [(field, index or None)], reason refused or None) for one lifted function."""
    body = ftext.split("\n", 1)[1]
    if re.search(r"\bfunc_[0-9A-F]{8}\b", body):
        return None, "calls a guest function"
    for bad in ("g_trampoline_fn", "ps3_indirect_call", "lv2_syscall", "ps3_hle_call", "DRAIN_TRAMPOLINE", "longjmp"):
        if bad in body:
            return None, "not a leaf (%s)" % bad
    if re.search(r"\bctx\b(?!->)", body):
        return None, "passes the whole context to a helper"
    regs = set()
    for f, rest in re.findall(r"\bctx->(\w+)(\[[^\]]*\])?", body):
        if f not in FIELDS:
            return None, "uses ctx->%s" % f
        if f in ("gpr", "fpr", "vr"):
            m = re.fullmatch(r"\[(\d+)\]", rest or "")
            if not m or int(m.group(1)) > 31:
                return None, "ctx->%s%s is not a fixed register" % (f, rest)
            regs.add((f, int(m.group(1))))
        else:
            if rest:
                return None, "ctx->%s%s" % (f, rest)
            regs.add((f, None))
    for c in set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", body)):
        if c in KEYWORDS or c in CALLS or c.startswith(CALL_PREFIXES):
            continue
        if re.fullmatch(r"u?int(8|16|32|64)_t|float|double|char|unsigned|int|long|short|bool", c):
            continue   # a cast written as a call
        return None, "calls %s()" % c
    order = {f: i for i, f in enumerate(FIELDS)}
    regs = [r for r in regs if r[0] != "fpr"]          # kept in the context (see _localize)
    return sorted(regs, key=lambda r: (order[r[0]], r[1] or 0)), None


def _local(f, i):
    return "R_%s%d" % (f[0] if f in ("gpr", "fpr", "vr") else f, i) if i is not None else "R_" + f


def _localize(body):
    body = re.sub(r"\bctx->gpr\[(\d+)\]", r"R_g\1", body)
    # Floating-point registers stay in the context: the compiler folds a float-double-float round trip
    # (lfs then stfs) held in a local, which leaves a signaling NaN unquieted where the lifted code,
    # through the context in memory, quiets it. Through memory the native is compiled as the lift is.
    body = re.sub(r"\bctx->fpr\[(\d+)\]", r"ctx_->fpr[\1]", body)
    body = re.sub(r"\bctx->vr\[(\d+)\]", r"R_v\1", body)
    body = re.sub(r"\bctx->(cr|xer|vscr|lr|ctr)\b", r"R_\1", body)
    return body


def _shadow(body):
    """The guest accesses of a localized body sent to the shadow log, or None (an access of another shape)."""
    for w in ("8", "16", "32", "64"):
        body = re.sub(r"\bvm_read%s\(" % w, "gt5_sh_r%s(_sh, " % w, body)
        body = re.sub(r"\bvm_write%s\(" % w, "gt5_sh_w%s(_sh, " % w, body)
    body = re.sub(r"\bvm_raw_ld16\(", "gt5_sh_ld16(_sh, ", body)
    body = re.sub(r"\bvm_raw_st16\(", "gt5_sh_st16(_sh, ", body)
    body = re.sub(r"\bVM_RAW_LD\(\s*(\w+)\s*,", r"gt5_sh_rawld<\1>(_sh,", body)
    body = re.sub(r"\bVM_RAW_ST\(\s*(\w+)\s*,", r"gt5_sh_rawst<\1>(_sh,", body)
    # lvlx / lvrx read the aligned quadword through a host pointer
    body = re.sub(r"(\bvmx_lv[lr]x\(&R_v\d+, )vm_base \+ \(uint32_t\)\(([^()]*)\)", r"\1gt5_sh_p16(_sh, (uint32_t)(\2))", body)
    # stvlx / stvrx: a byte loop through a host pointer to the aligned quadword
    body = re.sub(r"uint8_t\* m = vm_base \+ \(uint32_t\)\(ea & ~0xFULL\); (const uint8_t\* s = \(const uint8_t\*\)&R_v\d+; "
                  r"for \([^;]*;[^;]*;[^)]*\)) m\[([^\]]+)\] = s\[i\];",
                  r"const uint32_t m_ = (uint32_t)(ea & ~0xFULL); \1 gt5_sh_write(_sh, m_ + (uint32_t)(\2), &s[i], 1, 1);", body)
    # dcbz: a raw 128-byte clear
    body = re.sub(r"memset\(vm_base \+ \(uint32_t\)ea, 0, (\d+)\);", r"gt5_sh_zero(_sh, (uint32_t)ea, \1);", body)
    if re.search(r"\bvm_base\b|\b_mm\b", body):
        return None
    return body


def _written(text):
    """Locals a text may write: assigned, incremented, or with their address taken."""
    w = set(re.findall(r"\b(R_[a-z]+\d*)\s*(?:<<|>>|[-+*/%&|^])?=(?!=)", text))
    w |= set(re.findall(r"&(R_[a-z]+\d*)\b", text))
    w |= set(re.findall(r"(?:\+\+|--)\s*(R_[a-z]+\d*)\b", text))
    w |= set(re.findall(r"\b(R_[a-z]+\d*)\s*(?:\+\+|--)", text))
    return w


# ---- the vector pass: VMX registers as host-order float vectors (ppu_vmx.h's own four-lane operations) ----
# A register's value lives in R_vN (guest byte order, the lifted form) and/or H_vN (__m128, each word in host
# order: what vmx_ld4 gives). The common VMX statements work on H_vN directly, so a value is converted once
# when it comes from memory or the context, not on every operation; any other statement gets its registers
# in R_vN form (converted back first) and runs as lifted. Every guest load and store stays the lifted call.
# A value an operation has already flushed (VSCR[NJ]: denormals to zero) is not flushed again on its next
# use (flushing is idempotent). Labels, gotos and returns are joins: R_vN is made current before them, and
# after a label nothing is assumed about H_vN.
_BIT = {"a^b": "_mm_xor_ps({A}, {B})", "a|b": "_mm_or_ps({A}, {B})", "a&b": "_mm_and_ps({A}, {B})",
        "a&~b": "_mm_andnot_ps({B}, {A})", "~(a|b)": "_mm_xor_ps(_mm_or_ps({A}, {B}), _mm_castsi128_ps(_mm_set1_epi32(-1)))",
        "(a&~c)|(b&c)": "_mm_or_ps(_mm_andnot_ps({C}, {A}), _mm_and_ps({B}, {C}))"}


def _vector_pass(loc):
    """(new body text, set of vector registers written) or None when the body has no vector statement it takes."""
    hv, rv, fl, out = set(), set(range(32)), set(), []
    wrote = set()
    taken = 0

    def need(n):
        if n not in hv:
            out.append("        H_v%d = gt5_nat_h(&R_v%d);" % (n, n))
            hv.add(n); fl.discard(n)
        return "H_v%d" % n

    def fv(n):                                     # a float operand: flushed when NJ is on, unless known flushed
        h = need(n)
        return h if n in fl else "gt5_nat_fl(%s, nj_)" % h

    def define(n, expr, flushed):
        out.append("        H_v%d = %s;" % (n, expr))
        hv.add(n); rv.discard(n); wrote.add(n)
        (fl.add if flushed else fl.discard)(n)

    def sync(n):
        if n in hv and n not in rv:
            out.append("        gt5_nat_r(&R_v%d, H_v%d);" % (n, n))
            rv.add(n)

    def sync_all():
        for n in sorted(hv):
            sync(n)

    for line in loc.split("\n"):
        s = line.strip()
        if re.fullmatch(r"loc_[0-9A-F]+:", s):
            sync_all(); out.append(line); hv.clear(); fl.clear(); continue
        if re.search(r"\bgoto\b|\breturn\b", s):
            sync_all(); out.append(line); continue
        if not re.search(r"\bR_v\d", s) or s.startswith("uint64_t _cs_"):
            out.append(line)
            if re.search(r"\bR_vscr\s*(?:[|&^]?=)(?!=)", s):
                out.append("        nj_ = (R_vscr & 0x00010000u) != 0;")
                fl.clear()
            continue
        z = s.replace(" ", "")
        m = re.fullmatch(r"\{uint64_tea=(.*)&~0xFULL;vm_raw_ld16\(&R_v(\d+),ea\);\}", z)
        if m:                                       # lvx
            d = int(m.group(2))
            out.append(line.replace("vm_raw_ld16(&R_v%d, ea);" % d, "ppu_vr t_; vm_raw_ld16(&t_, ea); H_v%d = gt5_nat_h(&t_);" % d))
            hv.add(d); rv.discard(d); fl.discard(d); wrote.add(d); taken += 1
            continue
        m = re.fullmatch(r"\{uint64_tea=(.*)&~0xFULL;vm_raw_st16\(ea,&R_v(\d+)\);\}", z)
        if m:                                       # stvx: from R_vN, made current
            n = int(m.group(2))
            if n not in hv and n not in rv:
                need(n)
            sync(n); out.append(line); taken += 1
            continue
        m = re.fullmatch(r"\{uint32_t\*d=\(uint32_t\*\)&R_v(\d+);uint32_tval=\(\(uint32_t\*\)&R_v(\d+)\)\[([0-3])\];"
                         r"d\[0\]=d\[1\]=d\[2\]=d\[3\]=val;\}", z)
        if m:                                       # vspltw
            d, a, k = int(m.group(1)), int(m.group(2)), int(m.group(3))
            src = need(a); f = a in fl
            define(d, "_mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(%s), 0x%02X))" % (src, k * 0x55), f)
            taken += 1
            continue
        m = re.fullmatch(r"vmx_v(madd|nmsub)fp\(&R_v(\d+),&R_v(\d+),&R_v(\d+),&R_v(\d+),R_vscr\);", z)
        if m:                                       # vmaddfp / vnmsubfp (= ppu_vmx.h's four-lane form)
            d, a, b, c = (int(x) for x in m.group(2, 3, 4, 5))
            x, y, w = fv(a), fv(b), fv(c)
            core = ("_mm_fmadd_ps(%s, %s, %s)" % (x, w, y) if m.group(1) == "madd" else
                    "_mm_xor_ps(_mm_fmsub_ps(%s, %s, %s), _mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u)))" % (x, w, y))
            out.append("        { __m128 r_ = vmx_fix4(%s); H_v%d = nj_ ? vmx_flush4(r_) : r_; }" % (core, d))
            hv.add(d); rv.discard(d); fl.add(d); wrote.add(d); taken += 1
            continue
        m = re.fullmatch(r"vmx_v(add|sub)fp\(&R_v(\d+),&R_v(\d+),&R_v(\d+),R_vscr\);", z)
        if m:                                       # vaddfp / vsubfp
            d, a, b = (int(x) for x in m.group(2, 3, 4))
            x, y = fv(a), fv(b)
            out.append("        { const __m128 x_ = %s, y_ = %s; __m128 r_ = _mm_%s_ps(x_, y_); if (vmx_anynan4(r_)) r_ = vmx_nan2_4(r_, x_, y_);"
                       " H_v%d = nj_ ? vmx_flush4(r_) : r_; }" % (x, y, m.group(1), d))
            hv.add(d); rv.discard(d); fl.add(d); wrote.add(d); taken += 1
            continue
        m = re.fullmatch(r"\{uint64_t\*d=\(uint64_t\*\)&R_v(\d+);uint64_t\*a=\(uint64_t\*\)&R_v(\d+);uint64_t\*b=\(uint64_t\*\)&R_v(\d+);"
                         r"(?:uint64_t\*c=\(uint64_t\*\)&R_v(\d+);)?(.*)\}", z)
        if m:                                       # vxor vor vand vandc vnor vsel
            body = m.group(5)
            e = None
            mm = re.fullmatch(r"d\[0\]=(.*);d\[1\]=(.*);", body)
            if mm and mm.group(1).replace("[0]", "") == mm.group(2).replace("[1]", "") and "[1]" not in mm.group(1):
                e = mm.group(1).replace("[0]", "")
            mm = re.fullmatch(r"for\(inti=0;i<2;i\+\+\)d\[i\]=(.*);", body)
            if mm:
                e = mm.group(1).replace("[i]", "")
            if e in _BIT and (("c" in e) == (m.group(4) is not None)):
                d, a, b = int(m.group(1)), int(m.group(2)), int(m.group(3))
                c = int(m.group(4)) if m.group(4) else None
                if e == "a^b" and a == b:
                    define(d, "_mm_setzero_ps()", True)
                elif e in ("a|b", "a&b") and a == b:
                    src = need(a); define(d, src, a in fl)
                else:
                    A, B = need(a), need(b); C = need(c) if c is not None else None
                    define(d, _BIT[e].format(A=A, B=B, C=C), False)
                taken += 1
                continue
        m = re.fullmatch(r"\{uint32_t\*d=\(uint32_t\*\)&R_v(\d+);uint32_t\*a=\(uint32_t\*\)&R_v(\d+);uint32_t\*b=\(uint32_t\*\)&R_v(\d+);"
                         r"uint32_tt\[4\];t\[0\]=a\[([02])\];t\[1\]=b\[\4\];t\[2\]=a\[([13])\];t\[3\]=b\[\5\];memcpy\(d,t,16\);\}", z)
        if m and int(m.group(5)) == int(m.group(4)) + 1:     # vmrghw / vmrglw
            d, a, b = int(m.group(1)), int(m.group(2)), int(m.group(3))
            A, B = need(a), need(b)
            define(d, "_mm_unpack%s_ps(%s, %s)" % ("lo" if m.group(4) == "0" else "hi", A, B), a in fl and b in fl)
            taken += 1
            continue
        m = re.fullmatch(r"vmx_vperm\(&R_v(\d+),&R_v(\d+),&R_v(\d+),&R_v(\d+)\);", z)
        if m:
            d, a, b, c = (int(x) for x in m.groups())
            define(d, "gt5_nat_vperm(%s, %s, %s)" % (need(a), need(b), need(c)), False)
            taken += 1
            continue
        m = re.fullmatch(r"vmx_vsldoi\(&R_v(\d+),&R_v(\d+),&R_v(\d+),(\d+)\);", z)
        if m:
            d, a, b, k = (int(x) for x in m.groups())
            define(d, "gt5_nat_vsldoi(%s, %s, %u)" % (need(a), need(b), k), False)
            taken += 1
            continue
        m = re.fullmatch(r"\{uint32_tv\[4\];for\(inti=0;i<4;i\+\+\)v\[i\]=\(uint32_t\)\(int32_t\)(-?\d+);ppu_vstu4\(&R_v(\d+),v\);\}", z)
        if m:                                       # vspltisw
            define(int(m.group(2)), "_mm_castsi128_ps(_mm_set1_epi32((int32_t)(%s)))" % m.group(1), False)
            taken += 1
            continue
        # anything else: as lifted, on the registers' R_vN form
        regs = sorted(set(int(x) for x in re.findall(r"\bR_v(\d+)\b", s)))
        for n in regs:
            sync(n)
        out.append(line)
        for n in regs:
            hv.discard(n); fl.discard(n); rv.add(n)
        wrote |= set(regs)
        # ppu_vmx.h helpers given &vscr only set SAT; an assignment (mtvscr) may change NJ
        if re.search(r"\bR_vscr\s*(?:[|&^]?=)(?!=)", s):
            out.append("        nj_ = (R_vscr & 0x00010000u) != 0;")
            fl.clear()
    sync_all()
    if not taken:
        return None
    return "\n".join(out), wrote


def generate(ftext, name, sym_native, sym_shadow):
    """C++ text of the native (and compare) form of lifted function `name` (its full text `ftext`), or
    (None, reason)."""
    regs, why = analyse(ftext)
    if why:
        return None, why
    head, body = ftext.split("\n", 1)
    body = body.rstrip()
    assert body.endswith("}")
    body = body[:-1]                                   # the function's closing brace
    loc = _localize(body)
    if re.search(r"\bctx\b(?!_)", loc):
        return None, "context left after localizing"
    decl = "\n".join("    %s %s = ctx_->%s;" % (CTYPE[f], _local(f, i), f if i is None else "%s[%d]" % (f, i))
                     for f, i in regs)

    def wb_for(written):
        return " ".join("ctx_->%s = %s;" % (f if i is None else "%s[%d]" % (f, i), _local(f, i))
                        for f, i in regs if _local(f, i) in written)

    def with_wb(b, wb):
        return re.sub(r"\breturn;", "{ " + wb + " return; }", b) + "\n    " + wb + "\n}\n"

    plain_wb = wb_for(_written(loc))
    vec = _vector_pass(loc) if any(f == "vr" for f, i in regs) else None
    vdecl = ""
    if vec:
        vtext, vw = vec
        vwb = wb_for((_written(vtext) - {"R_v%d" % n for n in range(32)}) | {"R_v%d" % n for n in vw})
        vdecl = ("\n    __m128 " + ", ".join("H_v%d" % i for f, i in regs if f == "vr") + ";"
                 + "\n    int nj_ = (R_vscr & 0x00010000u) != 0; (void)nj_;")
        if not any(f == "vscr" for f, i in regs):
            vdecl = "\n    uint32_t R_vscr = ctx_->vscr;" + vdecl

    def emit(sym, args, text_plain, text_vec):
        o = []
        if text_vec is not None:
            o += ["#if VMX_SIMD_FMA", "void %s(%s) {" % (sym, args), decl + vdecl, with_wb(text_vec, vwb), "#else"]
        o += ["void %s(%s) {" % (sym, args), decl, with_wb(text_plain, plain_wb)]
        if text_vec is not None:
            o.append("#endif")
        return o

    out = ["/* %s: native (registers in C locals%s), made from this lift's text by gen_ppu_leaf.py */"
           % (name, ", VMX values in host order" if vec else "")]
    out += emit(sym_native, "ppu_context* ctx_", loc, vec[0] if vec else None)
    sh = _shadow(loc)
    shv = _shadow(vec[0]) if vec else None
    if sh is not None and (shv is not None or not vec):
        out += emit(sym_shadow, "ppu_context* ctx_, gt5_ppu_shadow* _sh", sh, shv)
    else:
        sh = None
    return "\n".join(out), (None if sh is not None else "no compare form")
