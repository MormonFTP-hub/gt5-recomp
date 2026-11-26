// Volume reader: finds the shader programs in the game's files before the game draws them, by reading the game's
// model files with the game's OWN file code. Nothing of the volume's format, key or index is known here: the game's
// code lists, opens, decrypts and unpacks; this only asks it to, and hands the unpacked bytes to the shader side
// (libs/video/rsx_shader_prep.inc):
//  - [found] every program of every shader container in them, by the containers' own structure, with no list
//    (rsx_shader_prep_take_shds, libs/video/rsx_shader_found.inc): kept in <prep dir>\found.bin, made by
//    gt5.exe --prepare-shaders before the next start;
//  - the listed programs (a shipped list), when the lists name programs not known yet (GT5_VOLREAD=1 only).
//
// When: in the collect pass, never during play. `gt5.exe --collect-shaders` (play.bat with GT5_COLLECT=1; the
// launcher, update.ps1 and setup.ps1 run it before the prepare step) starts the game with no window, no sound and
// no input (boot_main.cpp); the game only boots as far as its first screen, its saves go to a throwaway folder,
// every file it would copy to the hard disk is read from the volume instead, and the reader reads every carrier
// file at full speed, prints "GT5_FILES <done> <total>" on stdout and ends the process (exit code 0). It runs
// again only when the game's files changed: <prep dir>\collected.txt holds what they were (files_line: the
// executable, the disc's volume, each game folder's mod marker; GT5Launcher.cs computes the same line) and the
// reading rules; the same again = the pass ends at once. A cut-off pass carries on next time: a file goes to the
// done-list only after its programs are in found.bin.
// In play the reader does nothing (one line); GT5_VOLREAD=1 reads during play as v0.1.109-0.1.113 did (tests).
//
// Where programs are (probe runs 1-2, 19 Nov, Europe 1.00): only in model containers ("MDL3" / "PACB" once
// unpacked): /car/hq, /car/race, /car/interior, /wheel/hq, /wheel/race, /tire/hq, /tire/race, /character,
// /crowd, and in /crs the course models <course>x and the skies <course>.sky / .envsky. 18,813 files, 6.0 GB
// stored / 15.2 GB unpacked.
//
// The game's functions are FOUND by what their code does, on every release (Europe / Asia 1.00, Asia 2.01,
// Europe 2.11 / 2.17 checked offline, n_shaders round 5): no address and no instruction schedule is assumed.
//  - FileObject's dtor and its TOC: slot 0 of the vtable found by the class's type name (N6PDISTD10FileObjectE)
//    and that slot's descriptor;
//  - stat: the one place that calls open, stores request code 4 at +0x84 of the request open returned, then calls
//    run, close and the dtor (consecutive calls); the call before open is the ctor, which must store FileObject's
//    vtable at 0(r3); the call before that is the result's init;
//  - load: the one place that calls the same open, stores request code 0, calls the allocator setter, stores the
//    range at +0x74 / +0x78 / +0x7C / +0x80 of the request, then calls the same run and close; its first call must
//    be the same result init as stat's (so its start is right);
//  - unpack / its end: every place that tests the packed magic (lis 0xFFF7 / ori 0xEEC5 against a byte-reversed
//    load), then calls unpack(state, dst, src, 1) and right after it the end; all must agree;
//  - TOCs: stat and load reach every one of their functions by direct calls (same TOC as the dtor's descriptor);
//    unpack's end has a descriptor of its own, and every tester reaches unpack and its end with the same TOC change.
// Not found on an executable: one line, the volume is not read, nothing else changes. Europe 1.00's (TOC
// 0x018765B0): 0x00BB539C FileObject ctor, 0x00BB61F0 open(obj, path), 0x00BB4D34 run, 0x00BB6408 close,
// 0x00BB522C dtor, 0x00BB6708 stat, 0x00BB6A90 load(result, path, allocator, {buffer, size}, range[4], flag),
// 0x00BE27A4 unpack(state, dst, src, 1), 0x00BE271C its end; request code at obj+0x84, 3 = list a directory
// with sizes (entries at obj+0xA0, room at +0xA4; 0x40 bytes each: +0 type 3 directory / 1 file, +4 size as
// stored, +8 unpacked, +0xC name), result at obj+0xB0 (status, +0x10 the number of entries).
// The requests run on the volume device's own thread, as the game's own do.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ppu_context.h"

#ifdef _WIN32
#include <windows.h>

extern "C" unsigned char* vm_base;
extern "C" void ps3_on_loaded(void (*fn)(void));
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern "C" int64_t sys_ppu_thread_create(ppu_context* ctx);
extern "C" void* sys_heap_malloc(uint32_t heap, uint32_t size);
extern "C" int32_t sys_heap_free(uint32_t heap, void* ptr);
extern "C" int cellPad_race_pause_state(void);   // libs/input/cellPad.c: -1 = no race engine driving a car
extern "C" unsigned long long cellPad_last_activity_ms(void);   // the last poll with a pad or the keys in use
extern "C" unsigned long long cellPad_now_ms(void);
// libs/video/rsx_shader_prep.inc
extern "C" int rsx_shader_prep_scan_guest(uint32_t ea, uint32_t n, uint32_t* found, uint32_t* known,
                                          uint32_t* vertex);
extern "C" uint32_t rsx_shader_prep_scan_unknown(uint64_t* list_hash, const char** dir);
extern "C" int rsx_shader_prep_lists_ready(void);
// libs/video/rsx_shader_found.inc
extern "C" int rsx_shader_prep_take_shds(uint32_t ea, uint32_t n, uint32_t* containers, uint32_t* added,
                                         uint32_t* vertex, uint32_t* skipped);
extern "C" const char* rsx_shader_prep_found_dir(uint32_t* programs, uint32_t* rules);
extern "C" const char* rsx_shader_prep_store_dir(uint32_t* rules);
extern "C" int rsx_shader_prep_found_flush(void);   // 1: every program found so far is in found.bin
extern "C" void rsx_shader_prep_found_coverage(void);

struct gt5_func_entry {
    uint64_t addr;
    void (*func)(ppu_context*);
    const char* name;
};
extern "C" const gt5_func_entry function_table[];
extern "C" const uint64_t function_table_count;

namespace {

// Found by find_functions().
uint32_t kToc, kFoCtor, kFoDtor, kFoOpen, kFoRun, kFoClose, kLoad, kStat, kUnpack, kUnpackEnd, kUnpackToc;

// The carrier groups, in the order a player meets them; an ending of "" means every file of the directory.
struct Group { const char* dir; const char* ending; };
const Group kGroups[] = {
    {"/car/hq", ""}, {"/wheel/hq", ""}, {"/tire/hq", ""}, {"/crs", "x"}, {"/crs", ".sky"}, {"/crs", ".envsky"},
    {"/car/race", ""}, {"/wheel/race", ""}, {"/tire/race", ""}, {"/character", ""}, {"/crowd", ""},
    {"/car/interior", ""},
};
bool s_found_mode, s_list_mode;   // what this session reads for (see the top)
bool s_collect;                   // gt5.exe --collect-shaders (GT5_COLLECT=1)

// The collect pass: what it writes besides the store, and why it ends (exit codes; the launcher only reads 0).
const int kCollectVersion = 1;    // the finder's / the pass's own rules: another value reads every file again
enum { kExitDone = 0, kExitNoCode = 3, kExitNoVolume = 4, kExitNoStore = 5, kExitTooLong = 6 };
char s_collected[MAX_PATH];       // <prep dir>\collected.txt
std::string s_files;              // files_line() at the start of the pass
uint32_t s_rules;                 // the container reading rules (RSX_SHDS_VERSION)

const uint32_t kBufCap = 30u << 20, kUbufCap = 62u << 20;

uint32_t rd32(uint32_t ea) { uint32_t v; memcpy(&v, vm_base + ea, 4); return __builtin_bswap32(v); }
void wr32(uint32_t ea, uint32_t v) { v = __builtin_bswap32(v); memcpy(vm_base + ea, &v, 4); }
uint64_t rd64(uint32_t ea) { return ((uint64_t)rd32(ea) << 32) | rd32(ea + 4); }
uint32_t galloc(uint32_t n) { return (uint32_t)(uintptr_t)sys_heap_malloc(0, n); }
void gfree(uint32_t ea) { if (ea) sys_heap_free(0, (void*)(uintptr_t)ea); }

double now_s()
{
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

struct Fn { uint32_t code, toc; void (*fn)(ppu_context*); };
Fn s_fn[8];
ppu_context* s_self;   // the reader thread's own context (its TLS, thread id and stack)

void (*lifted(uint32_t code))(ppu_context*)
{
    for (uint64_t j = 0; j < function_table_count; j++)
        if (function_table[j].addr == code) return function_table[j].func;
    return nullptr;
}

// ---- the collect pass: ending it ---------------------------------------------------------------------------------
// The whole process ends here (the game's threads with it): stderr and stdout are unbuffered, the store and the
// done-list are written as they go.
[[noreturn]] void collect_exit(int code)
{
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    for (;;) Sleep(1000);
}

// ---- finding the game's functions (by what the code is, on every release) -----------------------------------------
const uint32_t kImageLo = 0x00010000, kImageHi = 0x02000000;   // every GT5 release's executable fits

int32_t simm(uint32_t w) { return (int32_t)(int16_t)(w & 0xFFFFu); }

// The function a bl at ea calls, through a cross-TOC stub (std r2,40(r1); addis; addi; b) if it is one.
uint32_t bl_target(uint32_t ea)
{
    const uint32_t w = rd32(ea);
    if ((w >> 26) != 18 || (w & 3) != 1) return 0;
    int32_t off = (int32_t)(w & 0x03FFFFFCu);
    if (off & 0x02000000) off -= 0x04000000;
    uint32_t t = ea + (uint32_t)off;
    if (t >= kImageLo && t < kImageHi - 16 && rd32(t) == 0xF8410028u && (rd32(t + 12) >> 26) == 18) {
        int32_t o2 = (int32_t)(rd32(t + 12) & 0x03FFFFFCu);
        if (o2 & 0x02000000) o2 -= 0x04000000;
        t = t + 12 + (uint32_t)o2;
    }
    return t >= kImageLo && t < kImageHi ? t : 0;
}
// The branch's own target (the stub itself when it goes through one).
uint32_t bl_raw(uint32_t ea)
{
    int32_t off = (int32_t)(rd32(ea) & 0x03FFFFFCu);
    if (off & 0x02000000) off -= 0x04000000;
    return ea + (uint32_t)off;
}
// The TOC change of a call: 0 for a direct one, the stub's addis/addi sum for one through a cross-TOC stub.
uint32_t bl_toc_delta(uint32_t ea)
{
    const uint32_t s = bl_raw(ea);
    if (s < kImageLo || s >= kImageHi - 16 || rd32(s) != 0xF8410028u) return 0;
    return (uint32_t)(simm(rd32(s + 4)) << 16) + (uint32_t)simm(rd32(s + 8));
}

struct Site { uint32_t at, to; bool direct; };
std::vector<Site> s_sites;            // every bl of the code, in address order
std::vector<uint32_t> s_starts;       // the lifted functions, sorted
uint32_t s_code_lo, s_code_hi;

void collect_sites()
{
    if (!s_sites.empty()) return;
    s_starts.reserve((size_t)function_table_count);
    for (uint64_t j = 0; j < function_table_count; j++) {
        const uint64_t a = function_table[j].addr;
        if (a >= kImageLo && a < kImageHi) s_starts.push_back((uint32_t)a);
    }
    std::sort(s_starts.begin(), s_starts.end());
    s_starts.erase(std::unique(s_starts.begin(), s_starts.end()), s_starts.end());
    if (s_starts.empty()) return;
    s_code_lo = s_starts.front();
    s_code_hi = s_starts.back() + 0x10000u < kImageHi ? s_starts.back() + 0x10000u : kImageHi;
    for (uint32_t ea = s_code_lo; ea + 4 <= s_code_hi; ea += 4) {
        const uint32_t w = rd32(ea);
        if ((w >> 26) != 18 || (w & 3) != 1) continue;
        const uint32_t to = bl_target(ea);
        if (to) s_sites.push_back({ea, to, bl_raw(ea) == to});
    }
}
// The lifted function holding ea (0 if none).
uint32_t func_of(uint32_t ea)
{
    auto it = std::upper_bound(s_starts.begin(), s_starts.end(), ea);
    return it == s_starts.begin() ? 0 : *(it - 1);
}

// Exactly one 'li rK,code; stw rK,0x84(r3)' in [a, b) and no other store to +0x84 of r3.
bool stores_code(uint32_t a, uint32_t b, int32_t code)
{
    int n = 0;
    bool ok = false;
    for (uint32_t e = a; e < b; e += 4) {
        const uint32_t w = rd32(e);
        if ((w & 0xFC1FFFFFu) != 0x90030084u) continue;   // stw rK, 0x84(r3)
        n++;
        const uint32_t k = (w >> 21) & 31;
        for (uint32_t j = 1; j < 12 && e - 4 * j >= a; j++) {
            const uint32_t p = rd32(e - 4 * j), op = p >> 26;
            if (op == 14 && ((p >> 16) & 31) == 0 && ((p >> 21) & 31) == k) { ok = simm(p) == code; break; }   // li
            if (((p >> 21) & 31) == k && op != 36 && op != 37 && op != 38 && op != 44 && op != 62) break;     // rK written
        }
    }
    return n == 1 && ok;
}
bool cmp_r3_zero(uint32_t a, uint32_t b)
{
    for (uint32_t e = a; e < b; e += 4)
        if ((rd32(e) & 0xFC7FFFFFu) == 0x2C030000u) return true;   // cmpwi crN, r3, 0
    return false;
}
// The first 64 instructions of fn store `value` at 0(r3): lwz rT,d(r2) (toc), lis / addi / ori tracked.
bool stores_at_r3(uint32_t fn, uint32_t toc, uint32_t value)
{
    uint32_t reg[32];
    bool known[32] = {};
    for (uint32_t i = 0; i < 64; i++) {
        const uint32_t w = rd32(fn + 4 * i), op = w >> 26, rt = (w >> 21) & 31, ra = (w >> 16) & 31;
        if (w == 0x4E800020u) break;                                                         // blr
        if (op == 32 && ra == 2) { reg[rt] = rd32(toc + (uint32_t)simm(w)); known[rt] = true; }   // lwz rT, d(r2)
        else if (op == 15 && ra == 0) { reg[rt] = (w & 0xFFFFu) << 16; known[rt] = true; }       // lis
        else if (op == 14 && ra && known[ra]) { reg[rt] = reg[ra] + (uint32_t)simm(w); known[rt] = true; }   // addi
        else if (op == 24 && known[rt]) { reg[ra] = reg[rt] | (w & 0xFFFFu); known[ra] = true; }  // ori rA, rS
        else if (op == 36 && ra == 3 && (w & 0xFFFFu) == 0) { if (known[rt] && reg[rt] == value) return true; }
        else if (op != 36 && op != 37 && op != 38 && op != 44 && op != 62 && op != 18 && op != 16) {
            known[rt] = false;                                     // what it wrote is not followed
            if (op == 31 || (op >= 24 && op <= 30)) known[ra] = false;   // logical / rotate forms write rA
        }
    }
    return false;
}
// The descriptors {code, toc} of a function in the data (toc 4-aligned in the image's upper half): 0 if none or
// they do not agree.
uint32_t descriptor_toc(uint32_t code)
{
    uint32_t toc = 0;
    for (uint32_t ea = 0x01000000u; ea + 8 <= kImageHi; ea += 4) {
        if (rd32(ea) != code) continue;
        const uint32_t t = rd32(ea + 4);
        if ((t & 3) || t < 0x01000000u || t >= kImageHi) continue;
        if (toc && toc != t) return 0;
        toc = t;
    }
    return toc;
}

// The vtable (the word holding the type information; its slots follow) of the class with this type name; 0 unless
// exactly one.
uint32_t find_word(uint32_t value, uint32_t from)
{
    for (uint32_t ea = (from + 3) & ~3u; ea + 4 <= kImageHi; ea += 4)
        if (rd32(ea) == value) return ea;
    return 0;
}
uint32_t find_vtable(const char* name, int* count)
{
    const size_t len = strlen(name) + 1;   // with its terminating zero
    const unsigned char* lo = vm_base + kImageLo;
    const unsigned char* hi = vm_base + kImageHi;
    uint32_t found = 0;
    *count = 0;
    for (const unsigned char* p = lo; p + len <= hi; p++) {
        p = (const unsigned char*)memchr(p, name[0], (size_t)(hi - p));
        if (!p || p + len > hi) break;
        if (memcmp(p, name, len) != 0 || p[-1] != 0) continue;
        const uint32_t name_ea = (uint32_t)(p - vm_base);
        for (uint32_t h = find_word(name_ea, kImageLo); h; h = find_word(name_ea, h + 4)) {
            const uint32_t ti = h - 4;   // type information: {its own vtable, name, ...}
            for (uint32_t v = find_word(ti, kImageLo); v; v = find_word(ti, v + 4)) {
                if (rd32(v - 4) != 0) continue;   // the primary vtable: {0, type information, slots...}
                (*count)++;
                found = v;
            }
        }
    }
    return *count == 1 ? found : 0;
}

bool find_fail(const char* what)
{
    fprintf(stderr, "[shader-prep] volume: the game's file code was not found in this executable (%s): the volume is "
                    "not read\n", what);
    return false;
}

bool find_functions()
{
    collect_sites();
    if (s_sites.empty()) return find_fail("no code");
    // FileObject: its dtor and TOC from its vtable's first slot
    int nv = 0;
    const uint32_t v = find_vtable("N6PDISTD10FileObjectE", &nv);
    if (!v) return find_fail(nv ? "FileObject has more than one vtable" : "no FileObject vtable");
    const uint32_t opd0 = rd32(v + 4);
    if (opd0 < kImageLo || opd0 >= kImageHi) return find_fail("FileObject's first slot");
    const uint32_t D = rd32(opd0), T = rd32(opd0 + 4);
    const uint32_t vptr = v + 4;   // the address point the ctor stores
    // stat: open, request code 4, run, close, dtor (consecutive calls)
    size_t is = 0;
    int ns = 0;
    for (size_t i = 2; i + 3 < s_sites.size(); i++) {
        const Site &o = s_sites[i], &r = s_sites[i + 1], &d = s_sites[i + 3];
        if (d.to != D || r.at - o.at > 0x80u) continue;
        if (!stores_code(o.at + 4, r.at, 4) || !cmp_r3_zero(o.at + 4, o.at + 16)) continue;
        ns++;
        is = i;
    }
    if (ns != 1) {
        char b[64];
        snprintf(b, sizeof b, "stat: %d places", ns);
        return find_fail(b);
    }
    const Site &sx = s_sites[is - 2], &sc = s_sites[is - 1], &so = s_sites[is], &sr = s_sites[is + 1],
               &scl = s_sites[is + 2], &sd = s_sites[is + 3];
    if (!sc.direct || !so.direct || !sr.direct || !scl.direct || !sd.direct || so.at - sc.at > 0x40u)
        return find_fail("stat's calls");
    const uint32_t C = sc.to, O = so.to, R = sr.to, Cl = scl.to, X = sx.to;
    if (!stores_at_r3(C, T, vptr)) return find_fail("the ctor does not set FileObject's vtable");
    // load: the same open, request code 0, the allocator, the range at +0x74..+0x80, the same run and close
    size_t il = 0;
    int nl = 0;
    for (size_t i = 2; i + 3 < s_sites.size(); i++) {
        const Site &o = s_sites[i], &a = s_sites[i + 1], &r = s_sites[i + 2], &c = s_sites[i + 3];
        if (o.to != O || r.to != R || c.to != Cl || r.at - o.at > 0x100u) continue;
        if (!stores_code(o.at + 4, a.at, 0)) continue;
        unsigned range = 0;
        for (uint32_t e = a.at + 4; e < r.at; e += 4) {
            const uint32_t w = rd32(e), ra = (w >> 16) & 31, d = w & 0xFFFFu;
            if ((w >> 26) != 36 || ra == 1 || ra == 2) continue;   // stw, not to the stack or the TOC
            if (d == 0x74) range |= 1; else if (d == 0x78) range |= 2; else if (d == 0x7C) range |= 4; else if (d == 0x80) range |= 8;
        }
        if (range != 15) continue;
        nl++;
        il = i;
    }
    if (nl != 1) {
        char b[64];
        snprintf(b, sizeof b, "load: %d places", nl);
        return find_fail(b);
    }
    if (!s_sites[il].direct) return find_fail("load's calls");
    // the functions' starts: stat's and load's first call is the result's init X, nothing before it
    const uint32_t S = func_of(sx.at), L = func_of(s_sites[il - 2].at);
    auto first_call_is = [](uint32_t f, uint32_t site) {
        auto it = std::lower_bound(s_sites.begin(), s_sites.end(), f, [](const Site& s, uint32_t a) { return s.at < a; });
        bool frame = false;
        for (uint32_t e = f; e < site && e < f + 0x80u; e += 4) if ((rd32(e) & 0xFFFF0000u) == 0xF8210000u) frame = true;
        return f && it != s_sites.end() && it->at == site && frame;   // stdu r1,-N(r1) before it
    };
    if (s_sites[il - 2].to != X || !first_call_is(S, sx.at) || !first_call_is(L, s_sites[il - 2].at))
        return find_fail("the starts of stat and load");
    // unpack and its end: every tester of the packed magic agrees
    uint32_t U = 0, UE = 0;
    int testers = 0;
    bool same_toc_change = true;
    for (uint32_t e = s_code_lo; e + 4 <= s_code_hi; e += 4) {
        const uint32_t w = rd32(e);
        if ((w >> 26) != 15 || ((w >> 16) & 31) != 0 || (w & 0xFFFFu) != 0xFFF7u) continue;   // lis rA, 0xFFF7
        const uint32_t ra = (w >> 21) & 31;
        bool ori = false, lwbrx = false;
        for (uint32_t j = 1; j <= 5; j++) if (rd32(e + 4 * j) == (0x60000000u | (ra << 21) | (ra << 16) | 0xEEC5u)) ori = true;
        for (int j = -6; j < 12; j++) if ((rd32(e + 4 * j) & 0xFC0007FEu) == 0x7C00042Cu) lwbrx = true;
        if (!ori || !lwbrx) continue;
        auto it = std::lower_bound(s_sites.begin(), s_sites.end(), e, [](const Site& s, uint32_t a) { return s.at < a; });
        for (; it != s_sites.end() && it + 1 != s_sites.end() && it->at < e + 0x180u; ++it) {
            bool six = false;   // li r6, 1 just before the call
            for (uint32_t j = 1; j <= 5; j++) if (rd32(it->at - 4 * j) == 0x38C00001u) six = true;
            if (!six) continue;
            if ((U && (U != it->to || UE != (it + 1)->to))) return find_fail("unpack: the testers disagree");
            U = it->to; UE = (it + 1)->to;
            if (bl_toc_delta(it->at) != bl_toc_delta((it + 1)->at)) same_toc_change = false;
            testers++;
            break;
        }
    }
    if (!U) return find_fail("no unpack");
    const uint32_t tu = descriptor_toc(UE);
    if (!tu || !same_toc_change) return find_fail("unpack's TOC");
    kToc = T; kUnpackToc = tu;
    kFoCtor = C; kFoDtor = D; kFoOpen = O; kFoRun = R; kFoClose = Cl; kLoad = L; kStat = S; kUnpack = U; kUnpackEnd = UE;
    const uint32_t want[8] = {kFoCtor, kFoDtor, kFoOpen, kFoRun, kFoClose, kLoad, kUnpack, kUnpackEnd};
    static const char* const names[8] = {"FileObject ctor", "FileObject dtor", "open", "run", "close", "load",
                                         "unpack", "unpack end"};
    for (int i = 0; i < 8; i++) {
        s_fn[i].code = want[i];
        s_fn[i].toc = i >= 6 ? kUnpackToc : kToc;
        s_fn[i].fn = lifted(want[i]);
        if (!s_fn[i].fn) {
            char b[96];
            snprintf(b, sizeof b, "its %s 0x%08X is not in this build", names[i], want[i]);
            return find_fail(b);
        }
    }
    fprintf(stderr, "[shader-prep] volume: the game's file code: load 0x%08X, stat 0x%08X, open 0x%08X, run 0x%08X, "
                    "close 0x%08X, FileObject 0x%08X / 0x%08X, unpack 0x%08X / 0x%08X (%d places), TOC 0x%08X / 0x%08X\n",
            kLoad, kStat, kFoOpen, kFoRun, kFoClose, kFoCtor, kFoDtor, kUnpack, kUnpackEnd, testers, kToc, kUnpackToc);
    return true;
}

// The game's request entry (FileObject's third virtual function, "submit") is wrapped so the reader knows
// when the game itself uses its file layer (play only): found by the class's type name, its vtable's first function
// checked against the FileObject dtor found above. The descriptor's own address stands in for the function
// (as GT5\src\gt5_cockpit.cpp does).
void (*s_submit)(ppu_context*);
uint32_t s_submit_opd;
volatile LONG s_inflight;              // the reader has a request of its own running
volatile ULONGLONG s_game_submit_ms;   // GetTickCount64 of the game's last request
uint64_t s_reader_tid = ~0ull;

void submit_wrap(ppu_context* ctx)
{
    if (ctx->thread_id != s_reader_tid && !InterlockedCompareExchange(&s_inflight, 0, 0))
        s_game_submit_ms = GetTickCount64();
    s_submit(ctx);
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        tf(ctx);
    }
}
void wrap_submit()
{
    int n = 0;
    const uint32_t v = find_vtable("N6PDISTD10FileObjectE", &n);
    if (!v || rd32(rd32(v + 4)) != kFoDtor) return;
    const uint32_t opd2 = rd32(v + 12);
    if (opd2 < kImageLo || opd2 >= kImageHi || (opd2 & 3)) return;
    s_submit = lifted(rd32(opd2));
    if (!s_submit) return;
    s_submit_opd = opd2;
    ppu_register_function(s_submit_opd, submit_wrap);
    wr32(s_submit_opd, s_submit_opd);   // the descriptor now leads to submit_wrap
    fprintf(stderr, "[shader-prep] volume: the game's requests seen at 0x%08X\n", s_submit_opd);
}

uint64_t call(uint32_t code, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0,
              uint64_t a4 = 0, uint64_t a5 = 0)
{
    const Fn* f = nullptr;
    for (const Fn& x : s_fn)
        if (x.code == code) f = &x;
    if (!f || !f->fn) return 0;
    InterlockedExchange(&s_inflight, 1);
    ppu_context c = *s_self;
    c.gpr[1] = (s_self->gpr[1] - 0x2000) & ~0xFull;   // below the thread's own frame
    c.gpr[2] = f->toc;
    c.gpr[3] = a0; c.gpr[4] = a1; c.gpr[5] = a2; c.gpr[6] = a3; c.gpr[7] = a4; c.gpr[8] = a5;
    c.lr = 0;
    c.cia = code;
    ppu_context* saved = g_active_ctx;
    g_active_ctx = &c;
    f->fn(&c);
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        tf(&c);
    }
    g_active_ctx = saved;
    InterlockedExchange(&s_inflight, 0);
    return c.gpr[3];
}

uint32_t s_path, s_obj, s_result, s_bufdesc, s_range, s_state, s_ent, s_ent_cap, s_buf, s_ubuf;

// ---- reads that do not install ----------------------------------------------------------------------------------
// GT5 copies a volume file to the hard disk the first time it opens it for reading ("Installing..." at the bottom
// right; the PDIPFS folder of its game data): its volume device (FileDevicePFSGameData) opens a handle in slot 34
// (Europe 1.00 0x00BC0918), which looks the file up and calls slot 46 (0x00BBC54C); slot 46 asks slot 60
// (0x00BBC3DC: not under one of the device's excluded folders, not above its size limit, room left on the disk)
// and on yes copies the whole file, counted in the "installing" count the indicator shows (0x00BB59F4), and
// opens the copy; on no it reads the file from the volume, as it does for any file it does not install. Slot 34
// is the only caller of slot 46, and slot 46 the only one that installs. So the reader's opens get "no": slot
// 34's wrapper marks this thread while it serves one of the reader's own FileObjects (its stack: load's, or
// s_obj), slot 60's wrapper answers "no" while marked and hands every other question to the game's own code.
// In the collect pass every question gets "no" (the game's own opens too): it installs nothing.
// The device's vtable is found by its type name, its slots checked by the calls between them (34 calls slot 46,
// 46 calls slot 60); anything else: nothing is wrapped and the volume is not read.
void (*s_open_fn)(ppu_context*);
void (*s_install_fn)(ppu_context*);
uint32_t s_open_opd, s_install_opd;
uint32_t s_stack_lo, s_stack_hi;           // the reader's call stack (call()'s frames)
volatile LONG s_reader_opens, s_installs_declined;
thread_local int t_reader_open;

bool reader_object(uint32_t obj)
{
    return obj && (obj == s_obj || (obj >= s_stack_lo && obj < s_stack_hi));
}
void open_wrap(ppu_context* ctx)
{
    const int before = t_reader_open;
    const uint32_t h = (uint32_t)ctx->gpr[4];
    t_reader_open = h >= 0x10000u && reader_object(rd32(h + 8));   // the handle's FileObject
    if (t_reader_open) InterlockedIncrement(&s_reader_opens);
    s_open_fn(ctx);
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        tf(ctx);
    }
    t_reader_open = before;
}
void install_wrap(ppu_context* ctx)
{
    if (t_reader_open || s_collect) {   // one of the reader's files, or the collect pass: read from the volume
        InterlockedIncrement(&s_installs_declined);
        ctx->gpr[3] = 0;
        return;
    }
    s_install_fn(ctx);
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        tf(ctx);
    }
}
// A virtual call through vtable offset off (lwz rX, off(rY); ... bctrl) in the first 2 KB of code.
bool has_vcall(uint32_t code, uint32_t off)
{
    for (uint32_t i = 0; i < 0x200; i++) {
        const uint32_t w = rd32(code + 4 * i), ra = (w >> 16) & 31;
        if ((w >> 26) != 32 || (w & 0xFFFFu) != off || ra == 1 || ra == 2) continue;
        for (uint32_t j = 1; j <= 8; j++)
            if (rd32(code + 4 * (i + j)) == 0x4E800421u) return true;
    }
    return false;
}
bool find_install_hooks()
{
    static const char kName[] = "N6PDIPS321FileDevicePFSGameDataE";
    const unsigned char* lo = vm_base + kImageLo;
    const unsigned char* hi = vm_base + kImageHi;
    auto slot = [](uint32_t v, uint32_t k) { return rd32(v + 4 + 4 * k); };
    auto in_image = [](uint32_t a) { return a >= kImageLo && a < kImageHi && !(a & 3); };
    for (const unsigned char* p = lo; p + sizeof kName <= hi; p++) {
        p = (const unsigned char*)memchr(p, 'N', (size_t)(hi - p));
        if (!p || p + sizeof kName > hi) break;
        if (memcmp(p, kName, sizeof kName) != 0 || p[-1] != 0) continue;
        const uint32_t name_ea = (uint32_t)(p - vm_base);
        for (uint32_t h = find_word(name_ea, kImageLo); h; h = find_word(name_ea, h + 4)) {
            const uint32_t ti = h - 4;
            for (uint32_t v = find_word(ti, kImageLo); v; v = find_word(ti, v + 4)) {
                if (rd32(v - 4) != 0) continue;   // the primary vtable: {0, type information, slots...}
                const uint32_t o34 = slot(v, 34), o46 = slot(v, 46), o60 = slot(v, 60);
                if (!in_image(o34) || !in_image(o46) || !in_image(o60)) continue;
                const uint32_t c34 = rd32(o34), c46 = rd32(o46), c60 = rd32(o60);
                if (!in_image(c34) || !in_image(c46) || !in_image(c60)) continue;
                if (!has_vcall(c34, 46 * 4) || !has_vcall(c46, 60 * 4)) continue;
                void (*f34)(ppu_context*) = lifted(c34);
                void (*f60)(ppu_context*) = lifted(c60);
                if (!f34 || !f60) continue;
                s_open_opd = o34; s_open_fn = f34;
                s_install_opd = o60; s_install_fn = f60;
                fprintf(stderr, "[shader-prep] volume: reads without install: the volume device's open 0x%08X and install "
                                "check 0x%08X (its slots 34 / 60, vtable 0x%08X) wrapped for %s\n",
                        c34, c60, v + 4, s_collect ? "every file (the collect pass)" : "the reader's own files");
                return true;
            }
        }
    }
    return false;
}
void wrap_install_hooks()
{
    ppu_register_function(s_open_opd, open_wrap);
    ppu_register_function(s_install_opd, install_wrap);
    wr32(s_open_opd, s_open_opd);         // the descriptors now lead to the wrappers
    wr32(s_install_opd, s_install_opd);
}

void put_path(const std::string& p)
{
    const size_t n = p.size() < 1000 ? p.size() : 1000;
    memcpy(vm_base + s_path, p.data(), n);
    vm_base[s_path + n] = 0;
}

struct Ent { std::string name; uint32_t type, stored, unpacked; };

// One directory as the game lists it: 0 = listed, else the request's status (4: the volume's index is not
// read yet), -1 = not opened.
int list_dir(const std::string& dir, std::vector<Ent>& out)
{
    out.clear();
    for (int pass = 0; pass < 2; pass++) {
        put_path(dir);
        memset(vm_base + s_obj, 0, 0x200);
        call(kFoCtor, s_obj);
        const uint32_t o = (uint32_t)call(kFoOpen, s_obj, s_path);
        int status = -1;
        uint64_t count = 0;
        if (o) {
            wr32(o + 0x84, 3);
            wr32(o + 0xA0, pass ? s_ent : 0);
            wr32(o + 0xA4, pass ? s_ent_cap : 0);
            call(kFoRun, o);
            status = (int)rd32(o + 0xB0);
            count = rd64(o + 0xC0);
            call(kFoClose, o);
        }
        call(kFoDtor, s_obj);
        if (status != 0) return status;
        if (pass == 0) {
            if (count > s_ent_cap) {
                const uint32_t want = (uint32_t)(count + 64);
                const uint32_t e = galloc(want * 0x40);
                if (!e) return -2;
                gfree(s_ent);
                s_ent = e; s_ent_cap = want;
            }
            continue;
        }
        if (count > s_ent_cap) count = s_ent_cap;
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t e = s_ent + i * 0x40;
            char name[64];
            memcpy(name, vm_base + e + 0xC, 52);
            name[52] = 0;
            out.push_back({name, rd32(e), rd32(e + 4), rd32(e + 8)});
        }
    }
    return 0;
}

bool ends_with(const std::string& s, const char* e)
{
    const size_t n = strlen(e);
    return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}

// The game using its file layer right now (its own loads go first; play only). With the request wrapper: a request
// of the game's (not the reader's, not made while the reader's own request runs: the device's work for it) in the
// last 500 ms. Without it: process reads over 100 ms while this thread reads nothing, 1 MB in 100 ms.
bool game_reading()
{
    if (s_submit) {
        if (GetTickCount64() - s_game_submit_ms >= 500) return false;
        Sleep(100);
        return true;
    }
    IO_COUNTERS a, b;
    if (!GetProcessIoCounters(GetCurrentProcess(), &a)) return false;
    Sleep(100);
    if (!GetProcessIoCounters(GetCurrentProcess(), &b)) return false;
    return b.ReadTransferCount - a.ReadTransferCount >= (1u << 20);
}

struct Stats {
    uint64_t files = 0, stored = 0, unpacked = 0, found = 0, found_v = 0, too_big = 0, failed = 0;
    uint64_t containers = 0, added = 0, added_v = 0;   // [found] shader containers met, programs new to the store
    uint64_t skipped = 0, files_skipped = 0;           // [found] containers not read, files with one
    double wait_load = 0, wait_drive = 0;
} s_st;
uint32_t s_gfound[sizeof kGroups / sizeof kGroups[0]];          // programs found per group
std::string s_glast[sizeof kGroups / sizeof kGroups[0]];        // the group's file of its last find

// One file: load as stored, unpack if packed, search. false: not read (too big / failed). s_last_skipped: shader
// containers in it this build could not read (then it is not written to the done-list: a later build reads it).
uint32_t s_last_skipped;
bool read_file(const std::string& path, const Ent& e)
{
    s_last_skipped = 0;
    if (e.stored > kBufCap || e.unpacked > kUbufCap) { s_st.too_big++; return true; }   // never fits: done
    put_path(path);
    memset(vm_base + s_result, 0, 0x80);
    wr32(s_bufdesc, s_buf);
    wr32(s_bufdesc + 4, kBufCap);
    memset(vm_base + s_range, 0, 16);
    wr32(s_buf, 0);
    call(kLoad, s_result, s_path, 0, s_bufdesc, s_range, 0);
    if (rd32(s_result) != 0) { s_st.failed++; return false; }
    uint32_t data = s_buf, size = e.stored;
    if (rd32(s_buf) == 0xC5EEF7FFu) {   // packed: C5 EE F7 FF, then minus the unpacked size (little-endian)
        const uint32_t usize = 0u - __builtin_bswap32(rd32(s_buf + 4));
        if (usize == 0 || usize > kUbufCap) { s_st.too_big++; return true; }
        memset(vm_base + s_state, 0, 0x2000);
        call(kUnpack, s_state, s_ubuf, s_buf, 1);
        call(kUnpackEnd, s_state);
        data = s_ubuf;
        size = usize;
    }
    uint32_t found = 0, known = 0, vert = 0;
    if (s_list_mode) rsx_shader_prep_scan_guest(data, size, &found, &known, &vert);
    s_st.files++; s_st.stored += e.stored; s_st.unpacked += size; s_st.found += found; s_st.found_v += vert;
    if (s_found_mode) {   // [found] every container in it, every program of them
        uint32_t c = 0, a = 0, v = 0, sk = 0;
        rsx_shader_prep_take_shds(data, size, &c, &a, &v, &sk);
        s_last_skipped = sk;
        s_st.containers += c; s_st.added += a; s_st.added_v += v; s_st.skipped += sk;
        if (sk) s_st.files_skipped++;
        // LD_FOUND_DUMP=<folder>: the first 6 files with a container not read, whole, unpacked (to work out why;
        // the game's data: for this PC only)
        static int dumped;
        const char* dd = getenv("LD_FOUND_DUMP");
        if (sk && dd && dd[0] && dumped < 6) {
            std::string fn = std::string(dd) + "\\";
            for (char ch : path) fn += (ch == '/' || ch == '\\' || ch == ':') ? '_' : ch;
            fn += ".bin";
            CreateDirectoryA(dd, nullptr);
            if (FILE* f = fopen(fn.c_str(), "wb")) {
                fwrite(vm_base + data, 1, size, f);
                fclose(f);
                dumped++;
                fprintf(stderr, "[shader-prep] volume: dumped %s (%u bytes, %u containers not read) to %s\n", path.c_str(),
                        size, sk, fn.c_str());
            }
        }
    }
    return true;
}

// A player driving: a race engine drives a car and is not paused (cellPad_race_pause_state() == 0) AND the
// controller or the keys were used within the last 3 s (play only, GT5_VOLREAD=1).
bool player_driving()
{
    if (cellPad_race_pause_state() != 0) return false;
    const unsigned long long now = cellPad_now_ms(), act = cellPad_last_activity_ms();
    return act && now >= act && now - act < 3000;
}

// ---- the collect pass: what the game's files are, the done marker, the throwaway save --------------------------
uint64_t fnv64(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}
bool file_stat(const char* p, unsigned long long* len, unsigned long long* ft)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    *len = *ft = 0;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    *len = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    *ft = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    return true;
}
// "files <EBOOT.elf size> <its time> <GT.VOL size> <its time> <mods>", in the GT5 folder (the current one): mods =
// "<game folder>:<size>:<time>" of each hdd0\game\<id>\USRDIR\mod_installed.txt (install_mod.ps1), ordinal order,
// joined by ","; "-" when none. Times are FILETIME (UTC, 100 ns). GT5Launcher.cs CollectLine() makes the same.
std::string files_line()
{
    unsigned long long el, et, vl, vt;
    file_stat("input\\EBOOT.elf", &el, &et);
    file_stat("vfs\\PS3_GAME\\USRDIR\\GT.VOL", &vl, &vt);
    std::vector<std::string> games;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("hdd0\\game\\*", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
                games.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(games.begin(), games.end());
    std::string mods;
    for (const std::string& g : games) {
        unsigned long long ml, mt;
        const std::string p = "hdd0\\game\\" + g + "\\USRDIR\\mod_installed.txt";
        if (!file_stat(p.c_str(), &ml, &mt)) continue;
        char one[320];
        snprintf(one, sizeof one, "%s%s:%llu:%llu", mods.empty() ? "" : ",", g.c_str(), ml, mt);
        mods += one;
    }
    char line[512];
    snprintf(line, sizeof line, "files %llu %llu %llu %llu %s", el, et, vl, vt, mods.empty() ? "-" : mods.c_str());
    return line;
}
std::string collect_head()
{
    char b[64];
    snprintf(b, sizeof b, "collect %d rules %u", kCollectVersion, s_rules);
    return b;
}
// collected.txt: the head, the files line, and what the pass did (for the log reader). Written last.
void write_collected(const char* what)
{
    const std::string tmp = std::string(s_collected) + ".new";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    fprintf(f, "%s\n%s\n%s\n", collect_head().c_str(), s_files.c_str(), what);
    const bool ok = fclose(f) == 0;
    if (ok) MoveFileExA(tmp.c_str(), s_collected, MOVEFILE_REPLACE_EXISTING);
}
void delete_tree(const std::string& dir)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            const std::string p = dir + "\\" + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                delete_tree(p);
            else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryA(p.c_str());
                else DeleteFileA(p.c_str());
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
}
std::string s_save_dir;   // <prep dir>\collect_save: the game's saves (and trophies) during the pass
// "GT5_FILES <done> <total>" on stdout: the launcher's / installer's / update window's progress bar.
void collect_progress(size_t done, size_t total, bool force)
{
    static double last;
    const double t = now_s();
    if (!force && t - last < 0.25) return;
    last = t;
    printf("GT5_FILES %zu %zu\n", done, total);
    fflush(stdout);
}

void reader_thread(ppu_context* ctx)
{
    s_self = ctx;
    s_reader_tid = ctx->thread_id;
    const double t0 = now_s();
    // The lists first (shader-prep reads them a few seconds in, also when there are none); then what to read
    // for: every container (the found store), the listed programs not known yet, or nothing.
    uint64_t hash = 0;
    const char* dir = nullptr;
    for (int i = 0; i < 2400 && !rsx_shader_prep_lists_ready(); i++) Sleep(250);
    const uint32_t unknown0 = s_collect ? 0 : rsx_shader_prep_scan_unknown(&hash, &dir);
    uint32_t known0 = 0, rules = 0;
    // every container: in the collect pass, or in play when LD_FOUND=1 asks for it (tests)
    const char* lf = getenv("LD_FOUND");
    const char* fdir = (s_collect || (lf && atoi(lf) != 0)) ? rsx_shader_prep_found_dir(&known0, &rules) : nullptr;
    s_found_mode = fdir != nullptr;
    s_list_mode = unknown0 && dir;
    if (!s_found_mode && !s_list_mode) {
        fprintf(stderr, "[shader-prep] volume: %s: nothing to read\n",
                s_collect ? "the store is off (LD_FOUND=0)" :
                rsx_shader_prep_lists_ready() ? "every listed program is known" : "no shader list");
        if (s_collect) collect_exit(kExitNoStore);
        return;
    }
    if (!dir) dir = fdir;
    // Reads that do not install (see above), or no reading at all (the collect pass wrapped them at load).
    if (!s_collect && !find_install_hooks()) {
        fprintf(stderr, "[shader-prep] volume: the game's install check was not found in this executable: the volume is "
                        "not read (each file read would be copied to the hard disk)\n");
        return;
    }
    s_stack_hi = (uint32_t)((s_self->gpr[1] - 0x2000) & ~0xFull);   // call()'s frames are below this
    s_stack_lo = s_stack_hi - 0x40000u;
    s_path = galloc(1024); s_obj = galloc(0x200); s_result = galloc(0x80); s_bufdesc = galloc(16);
    s_range = galloc(16); s_state = galloc(0x2000); s_ent_cap = 4096; s_ent = galloc(s_ent_cap * 0x40);
    s_buf = galloc(kBufCap); s_ubuf = galloc(kUbufCap);
    if (!s_path || !s_obj || !s_result || !s_bufdesc || !s_range || !s_state || !s_ent || !s_buf || !s_ubuf) {
        fprintf(stderr, "[shader-prep] volume: no guest memory for the reader: not read this session\n");
        if (s_collect) collect_exit(kExitNoStore);
        return;
    }
    // The done-list: its head, then one path a line; another head starts it over. The found store's: "found
    // <rules> load <addr> toc <addr> files <hash>" (this release, these reading rules, these game files); the listed
    // search's: "list <hash>" (another shipped list).
    const std::string done_path = std::string(dir) + (s_found_mode ? "\\volume_found.txt" : "\\volume_read.txt");
    std::unordered_set<std::string> done;
    char head[128];
    if (s_found_mode)
        snprintf(head, sizeof head, "found %u load %08X toc %08X files %016llX", rules, kLoad, kToc,
                 (unsigned long long)fnv64(s_files.empty() ? files_line() : s_files));
    else snprintf(head, sizeof head, "list %016llX", (unsigned long long)hash);
    bool torn = false;   // a cut-off pass left half a line: the next one starts on a line of its own
    if (FILE* f = fopen(done_path.c_str(), "r")) {
        char line[1100];
        bool same = fgets(line, sizeof line, f) && strncmp(line, head, strlen(head)) == 0 &&
                    (line[strlen(head)] == '\n' || line[strlen(head)] == '\r');
        while (same && fgets(line, sizeof line, f)) {
            const size_t l = strlen(line);
            torn = !l || line[l - 1] != '\n';
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] && !torn) done.insert(line);
        }
        fclose(f);
        if (!same) done.clear();
    }
    FILE* dl = fopen(done_path.c_str(), done.empty() ? "w" : "a");
    if (dl && done.empty()) fprintf(dl, "%s\n", head);
    else if (dl && torn) fputc('\n', dl);
    // The volume's index: listing answers 4 until the game has read it.
    std::vector<std::vector<std::pair<std::string, Ent>>> todo(sizeof kGroups / sizeof kGroups[0]);
    std::vector<Ent> ents;
    int st = -1;
    for (int i = 0; i < 1200; i++) {
        st = list_dir(kGroups[0].dir, ents);
        if (st != 4 && st != -1) break;
        Sleep(250);
    }
    if (st != 0) {
        fprintf(stderr, "[shader-prep] volume: the game's volume did not answer (status %d): not read this session\n", st);
        if (dl) fclose(dl);
        if (s_collect) collect_exit(kExitNoVolume);
        return;
    }
    size_t n_files = 0, n_before = 0;
    uint64_t n_bytes = 0;
    for (size_t g = 0; g < todo.size(); g++) {
        if (list_dir(kGroups[g].dir, ents) != 0) continue;
        for (const Ent& e : ents) {
            if (e.type != 1 || (kGroups[g].ending[0] && !ends_with(e.name, kGroups[g].ending))) continue;
            std::string p = std::string(kGroups[g].dir) + "/" + e.name;
            if (done.count(p)) { n_before++; continue; }
            n_files++; n_bytes += e.stored;
            todo[g].push_back({p, e});
        }
    }
    if (s_found_mode)
        fprintf(stderr, "[shader-prep] volume: reading the game's model files for every shader program (%u known so "
                        "far)%s%s: %zu files (%.0f MB) in %zu groups, %zu read in earlier sessions\n", known0,
                s_list_mode ? " and the listed ones not seen yet" : "", s_collect ? ", the collect pass" : "", n_files,
                n_bytes / 1048576.0, todo.size(), n_before);
    else
        fprintf(stderr, "[shader-prep] volume: reading the game's model files for %u listed programs not seen yet: %zu "
                        "files (%.0f MB) in %zu groups, %zu read in earlier sessions\n",
                unknown0, n_files, n_bytes / 1048576.0, todo.size(), n_before);
    if (s_collect) collect_progress(n_before, n_before + n_files, true);
    if (!n_files) {
        if (dl) fclose(dl);
        gfree(s_buf); gfree(s_ubuf); gfree(s_ent); gfree(s_state);
        if (s_collect) {
            rsx_shader_prep_found_flush();
            write_collected("every file read in earlier passes");
            delete_tree(s_save_dir);
            collect_exit(kExitDone);
        }
        return;
    }
    if (!s_collect) wrap_install_hooks();   // from here the reader's own opens do not install
    // In turns, one file of each group; the found store reads every one, the listed search alone stops when
    // nothing listed is unknown any more.
    size_t left = n_files, idx = 0;
    double last_line = 0, last_check = 0;
    bool said20 = false;
    std::vector<size_t> pos(todo.size(), 0);
    const char* why = "every file read";
    // The progress line: 20 s after the start, then once a minute, and at the end.
    auto progress = [&](uint32_t unknown, bool force) {
        const double t = now_s();
        if (!force && !(!said20 && t - t0 >= 20.0) && !(last_line && t - last_line >= 60.0)) return;
        said20 = true;
        last_line = t;
        fprintf(stderr, "[shader-prep] volume t=%.0f s: %llu files read (%.0f MB, %.0f MB unpacked), %llu shader "
                        "containers, %llu programs new to the store (%llu vertex), %llu listed programs found (%llu "
                        "vertex), %u listed still unknown, %zu files left; waited %.0f s for the game's loads, %.0f s "
                        "while a player drove; %llu containers not read (in %llu files); reads without install: %ld "
                        "opens of the reader's, %ld installs declined\n",
                t - t0, (unsigned long long)s_st.files, s_st.stored / 1048576.0, s_st.unpacked / 1048576.0,
                (unsigned long long)s_st.containers, (unsigned long long)s_st.added, (unsigned long long)s_st.added_v,
                (unsigned long long)s_st.found, (unsigned long long)s_st.found_v, unknown, left, s_st.wait_load,
                s_st.wait_drive, (unsigned long long)s_st.skipped, (unsigned long long)s_st.files_skipped,
                (long)s_reader_opens, (long)s_installs_declined);
        // Per group: files read / programs found, and the file of the last find (which groups the last
        // programs come from, to order the groups better).
        std::string g;
        char one[160];
        for (size_t i = 0; i < todo.size(); i++) {
            snprintf(one, sizeof one, "%s%s%s %zu/%u%s%s", i ? ", " : "", kGroups[i].dir,
                     kGroups[i].ending[0] ? kGroups[i].ending : "", pos[i], s_gfound[i],
                     s_glast[i].empty() ? "" : " last ", s_glast[i].c_str());
            g += one;
        }
        fprintf(stderr, "[shader-prep] volume groups (files read / programs found): %s\n", g.c_str());
    };
    uint32_t unknown = unknown0;
    bool unreadable = false;
    while (left) {
        const size_t g = idx++ % todo.size();
        if (pos[g] >= todo[g].size()) continue;
        // The game first (play only): no turn while it reads or while a player drives. The collect pass reads at
        // full speed: the game sits at its first screen.
        if (!s_collect && now_s() - last_check >= 1.0) {
            for (;;) {
                progress(unknown, false);
                if (player_driving()) { Sleep(250); s_st.wait_drive += 0.25; continue; }
                const double w0 = now_s();
                if (game_reading()) { s_st.wait_load += now_s() - w0; continue; }
                break;
            }
            last_check = now_s();
        }
        const auto& item = todo[g][pos[g]++];
        left--;
        const uint64_t found_before = s_st.found + s_st.added, added_before = s_st.added;
        const bool read = read_file(item.first, item.second);
        // The file's programs are in found.bin before the file is in the done-list: a pass cut off between the
        // two reads the file again, never loses its programs.
        const bool stored = !s_found_mode || s_st.added == added_before || rsx_shader_prep_found_flush();
        if (read && dl && !s_last_skipped && stored) {
            fprintf(dl, "%s\n", item.first.c_str());
            fflush(dl);
        }
        if (s_st.found + s_st.added > found_before) {
            s_gfound[g] += (uint32_t)(s_st.found + s_st.added - found_before);
            s_glast[g] = item.first.substr(item.first.rfind('/') + 1);
        }
        if (s_list_mode) unknown = rsx_shader_prep_scan_unknown(nullptr, nullptr);
        if (s_collect) collect_progress(n_before + (n_files - left), n_before + n_files, !left);
        progress(unknown, !left || (s_list_mode && !unknown && !s_found_mode));
        if (s_list_mode && !unknown && !s_found_mode) { why = "every listed program is known"; break; }
        // Containers met and none of them readable by this build: stop (a later build reads these files again).
        if (s_found_mode && !s_st.containers && s_st.files_skipped >= 40) {
            why = "the shader containers in these files are not in a form this build reads";
            unreadable = true;
            progress(unknown, true);
            break;
        }
    }
    if (dl) fclose(dl);
    gfree(s_buf); gfree(s_ubuf); gfree(s_ent); gfree(s_state);
    fprintf(stderr, "[shader-prep] volume: done (%s) after %.0f s: %llu files, %.0f MB read, %llu shader containers, "
                    "%llu programs new to the store (%llu vertex), %llu listed programs found, %llu too big, %llu not "
                    "read, %llu containers not read; reads without install: %ld opens of the reader's, %ld installs "
                    "declined\n", why, now_s() - t0, (unsigned long long)s_st.files, s_st.stored / 1048576.0,
            (unsigned long long)s_st.containers, (unsigned long long)s_st.added, (unsigned long long)s_st.added_v,
            (unsigned long long)s_st.found, (unsigned long long)s_st.too_big, (unsigned long long)s_st.failed,
            (unsigned long long)s_st.skipped, (long)s_reader_opens, (long)s_installs_declined);
    if (s_found_mode) {   // [found] everything to found.bin now, and what the files gave against what this PC drew
        rsx_shader_prep_found_flush();
        rsx_shader_prep_found_coverage();
    }
    if (s_collect) {
        char what[256];
        snprintf(what, sizeof what, "%s: %llu files read now, %zu before, %llu programs new, %llu not read, %llu too big",
                 unreadable ? "unreadable containers" : "every file read", (unsigned long long)s_st.files, n_before,
                 (unsigned long long)s_st.added, (unsigned long long)s_st.failed, (unsigned long long)s_st.too_big);
        // A file the game could not load stays out of the done-list (the log counts it); the next change of the
        // game's files or of these rules reads it again. The pass is not repeated for it at every start.
        write_collected(what);
        delete_tree(s_save_dir);
        fprintf(stderr, "[collect] %s\n", what);
        collect_exit(kExitDone);
    }
}

// The guest reader thread, once the runtime runs guest threads (the shader-prep thread has read its lists by then).
DWORD WINAPI starter(LPVOID)
{
    if (!s_collect) {
        // Off the game's start: the search reads the 32 MB image a few times (well under a second).
        if (!find_functions()) return 0;
        wrap_submit();
    }
    for (int i = 0; i < 2400 && !rsx_shader_prep_lists_ready(); i++) Sleep(250);
    if (!rsx_shader_prep_lists_ready()) {
        if (s_collect) {
            fprintf(stderr, "[collect] the game did not get far enough to read its files (600 s)\n");
            collect_exit(kExitNoVolume);
        }
        return 0;
    }
    const uint32_t opd = galloc(16), name = galloc(16), tid = galloc(8);
    if (!opd || !name || !tid) { if (s_collect) collect_exit(kExitNoStore); return 0; }
    wr32(opd, opd);   // the descriptor's own address stands in for the function (registered below)
    wr32(opd + 4, kToc);
    wr32(opd + 8, 0);
    memcpy(vm_base + name, "volread", 8);
    ppu_register_function(opd, reader_thread);
    ppu_context c;
    memset(&c, 0, sizeof c);
    c.gpr[2] = kToc;
    // the collect pass: an ordinary game thread's priority (the game sits at its first screen); play: the lowest
    c.gpr[3] = tid; c.gpr[4] = opd; c.gpr[5] = 0; c.gpr[6] = s_collect ? 1500 : 3071; c.gpr[7] = 0x100000; c.gpr[8] = 0;
    c.gpr[9] = name;
    if (sys_ppu_thread_create(&c) != 0) {
        fprintf(stderr, "[shader-prep] volume: the reader thread was not created\n");
        if (s_collect) collect_exit(kExitNoStore);
    }
    return 0;
}

// The collect pass's clock: whatever happens in the game, the pass ends.
DWORD WINAPI collect_watchdog(LPVOID)
{
    Sleep(45u * 60u * 1000u);
    fprintf(stderr, "[collect] still not done after 45 minutes: ended (the next pass carries on)\n");
    collect_exit(kExitTooLong);
}

// The collect pass, at load (the game's code has not run yet).
void collect_loaded()
{
    const char* dir = rsx_shader_prep_store_dir(&s_rules);
    if (!dir) {
        fprintf(stderr, "[collect] the shader store is off (LD_FOUND=0 or LD_PREP=0) or its folder can't be made\n");
        collect_exit(kExitNoStore);
    }
    snprintf(s_collected, sizeof s_collected, "%s\\collected.txt", dir);
    s_files = files_line();
    // Nothing changed since the last full pass: done at once.
    if (FILE* f = fopen(s_collected, "r")) {
        char l1[256] = "", l2[600] = "";
        const bool two = fgets(l1, sizeof l1, f) && fgets(l2, sizeof l2, f);
        fclose(f);
        l1[strcspn(l1, "\r\n")] = 0;
        l2[strcspn(l2, "\r\n")] = 0;
        if (two && collect_head() == l1 && s_files == l2) {
            fprintf(stderr, "[collect] the game's files are as at the last pass: nothing to read\n");
            collect_exit(kExitDone);
        }
    }
    DeleteFileA(s_collected);   // a pass is under way
    fprintf(stderr, "[collect] the game's files: %s\n", s_files.c_str());
    // The game's saves and trophies go to a throwaway folder: the player's are never opened. Before the game runs:
    // the runtime reads PS3_GAMEDATA_ROOT at its first save (cellSaveData.c, sceNpTrophy.c).
    s_save_dir = std::string(dir) + "\\collect_save";
    delete_tree(s_save_dir);
    CreateDirectoryA(s_save_dir.c_str(), nullptr);
    char full[MAX_PATH];
    if (GetFullPathNameA(s_save_dir.c_str(), sizeof full, full, nullptr)) _putenv_s("PS3_GAMEDATA_ROOT", full);
    // The file code and the install check, wrapped before the game opens anything (it installs nothing).
    const bool code = find_functions();   // says why when not
    const bool hooks = code && find_install_hooks();
    if (!hooks) {
        if (code)
            fprintf(stderr, "[shader-prep] volume: the game's install check was not found in this executable: the "
                            "volume is not read\n");
        // This release's code is not known to this build: nothing to read until a build that knows it.
        write_collected("this executable's file code was not found");
        delete_tree(s_save_dir);
        collect_exit(kExitNoCode);
    }
    wrap_install_hooks();
    HANDLE h = CreateThread(nullptr, 64u << 10, starter, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    h = CreateThread(nullptr, 64u << 10, collect_watchdog, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

void volread_loaded()
{
    const char* c = getenv("GT5_COLLECT");
    s_collect = c && atoi(c) != 0;
    if (s_collect) { collect_loaded(); return; }
    // In play the volume is read by the collect pass before the game starts, never while it runs. GT5_VOLREAD=1:
    // read during play as before (tests).
    const char* e = getenv("GT5_VOLREAD");
    if (!(e && atoi(e) != 0)) {
        fprintf(stderr, "[shader-prep] volume: read before the game starts (the collect pass), not during play\n");
        return;
    }
    HANDLE h = CreateThread(nullptr, 64u << 10, starter, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

const int s_registered = (ps3_on_loaded(volread_loaded), 0);

}  // namespace

#endif  // _WIN32
