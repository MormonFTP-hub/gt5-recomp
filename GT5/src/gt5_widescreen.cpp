// Ultrawide (wider than 16:9): GT5's own view setup, given the wider shape.
//
// GT5 draws for the shape of the TV. At start-up it reads the PS3's TV setting and hands its view
// setup a shape constant (16:9, or 4:3 for an old TV). The 3D cameras widen their view to that
// shape, keeping the height (Hor+), and the 2D layer (HUD, menus) keeps its 16:9 layout centred
// in it. The picture stays 1280x720, squeezed sideways, ready to be stretched by the TV. So a wider
// screen needs only those constants changed; the renderer then shows the picture across the wider
// window (display_aspect.c, rsx_live_draw.c).
//
// The constants are FOUND by the code that reads them, on every release (Europe / Asia 1.00, Asia 2.01,
// Europe 2.11 / 2.17 checked offline, n_shaders round 5); the value 16/9 alone occurs a dozen times. Right after
// the call of the TV-shape getter G (1 = 4:3, 2 = 16:9) the game picks a float in two places:
//  - the screen setup, a three-way pick: cmpwi r3,1 / cmpwi r3,2 / beq / bne, then the 16:9, 1.0 and 4:3
//    constants (in that order: r3 == 2, any other, r3 == 1);
//  - the 3D view setup, a two-way pick into f1: cmpwi r3,1 / bne, then the 4:3 and the 16:9 constant, then a call
//    of the view function S1. The same function also calls a second view function S2 with f1 = that constant.
//    Every float load into f1 from a 16:9 word that is passed to S1 or S2 is the 3D view's shape: Europe 2.11 /
//    2.17 keep it in two words (one of the S2 calls reads its own copy), the other releases in one.
// A load is lfs fX,d(r2) (the TOC: the one whose words at those places hold exactly those values; both picks
// must lead to the same TOC for G through their call's TOC change) or lis rY,h ... lfs fX,l(rY). Exactly one of
// each pick, both calling the same G, or nothing is changed. Europe 1.00's: screen 0x0186E2CC (read at
// 0x008CE594), 3D view 0x0186F910 (0x0092D358, 0x0092D3BC, 0x0092E388).
//
// PS3_ASPECT or PS3_WINDOW_SIZE (play.bat) choose the shape. At 16:9, the default, nothing here
// touches the game.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <initializer_list>
#include <vector>

#include "ppu_context.h"

extern "C" unsigned char* vm_base;
extern "C" float ps3_display_aspect(void);
extern "C" int ps3_display_wide(void);
extern "C" void ps3_display_title_widens(void);
extern "C" void ps3_on_loaded(void (*fn)(void));

struct gt5_func_entry {   // the lifted functions (as gt5_volread.cpp has them; only their addresses are read here)
    uint64_t addr;
    void (*func)(ppu_context*);
    const char* name;
};
extern "C" const gt5_func_entry function_table[];
extern "C" const uint64_t function_table_count;

namespace {

const uint32_t kShape16x9 = 0x3FE38E39;  // 16/9 as a big-endian float
const uint32_t kShape4x3 = 0x3FAAAAAB;   // 4/3
const uint32_t kOne = 0x3F800000;        // 1.0
const uint32_t kImageLo = 0x00010000, kImageHi = 0x02000000;

uint32_t rd32(uint32_t ea)
{
    uint32_t v;
    memcpy(&v, vm_base + ea, 4);
    return __builtin_bswap32(v);
}

void wr32(uint32_t ea, uint32_t v)
{
    v = __builtin_bswap32(v);
    memcpy(vm_base + ea, &v, 4);
}

int32_t simm(uint32_t w) { return (int32_t)(int16_t)(w & 0xFFFFu); }

// The function a bl at ea calls (through a cross-TOC stub: std r2,40(r1); addis; addi; b), and the TOC change.
uint32_t bl_target(uint32_t ea, uint32_t* delta)
{
    const uint32_t w = rd32(ea);
    if (delta) *delta = 0;
    if ((w >> 26) != 18 || (w & 3) != 1) return 0;
    int32_t off = (int32_t)(w & 0x03FFFFFCu);
    if (off & 0x02000000) off -= 0x04000000;
    uint32_t t = ea + (uint32_t)off;
    if (t >= kImageLo && t < kImageHi - 16 && rd32(t) == 0xF8410028u && (rd32(t + 12) >> 26) == 18) {
        if (delta) *delta = (uint32_t)(simm(rd32(t + 4)) << 16) + (uint32_t)simm(rd32(t + 8));
        int32_t o2 = (int32_t)(rd32(t + 12) & 0x03FFFFFCu);
        if (o2 & 0x02000000) o2 -= 0x04000000;
        t = t + 12 + (uint32_t)o2;
    }
    return t >= kImageLo && t < kImageHi ? t : 0;
}

// lfs fX,d(rB) at ea: rB = r2 (toc: value = d) or set by lis rB,h within the 8 instructions before it, none of them
// naming rB or branching (abs: value = the address).
struct Load { bool ok, toc; uint32_t value, freg, words; };
Load lfs_at(uint32_t ea)
{
    Load l = {};
    const uint32_t w = rd32(ea);
    if ((w >> 26) != 48) return l;
    const uint32_t rb = (w >> 16) & 31;
    l.freg = (w >> 21) & 31;
    l.words = 1;
    if (rb == 2) { l.ok = l.toc = true; l.value = (uint32_t)simm(w); return l; }
    if (rb == 0) return l;
    for (uint32_t k = 1; k <= 8; k++) {
        const uint32_t p = rd32(ea - 4 * k), op = p >> 26;
        if (op == 15 && ((p >> 16) & 31) == 0 && ((p >> 21) & 31) == rb) {
            l.ok = true;
            l.value = ((p & 0xFFFFu) << 16) + (uint32_t)simm(w);
            return l;
        }
        if (rb == ((p >> 21) & 31) || rb == ((p >> 16) & 31) || rb == ((p >> 11) & 31) || op == 16 || op == 18 || op == 19)
            return l;
    }
    return l;
}
// A pick's slot: [lis rY,h;] lfs fX,...
Load slot_at(uint32_t ea)
{
    Load l = lfs_at(ea);
    if (l.ok) return l;
    if ((rd32(ea) >> 26) != 15) return l;
    l = lfs_at(ea + 4);
    if (l.ok && !l.toc) { l.words = 2; return l; }
    l.ok = false;
    return l;
}
uint32_t bc_target(uint32_t ea, uint32_t bo, uint32_t bi)
{
    const uint32_t w = rd32(ea);
    if ((w >> 26) != 16 || ((w >> 21) & 31) != bo || ((w >> 16) & 31) != bi || (w & 3)) return 0;
    return ea + (uint32_t)simm(w & 0xFFFCu);
}
// b forward (no link): its target, else 0
uint32_t b_target(uint32_t ea)
{
    const uint32_t w = rd32(ea);
    if ((w >> 26) != 18 || (w & 3)) return 0;
    int32_t off = (int32_t)(w & 0x03FFFFFCu);
    if (off & 0x02000000) off -= 0x04000000;
    return ea + (uint32_t)off;
}

// The places a pick's loads can mean: all TOC-relative (every TOC whose words there hold `want`, found from the first
// value's words) or all absolute. Each result: the TOC (0 for absolute) and the addresses.
struct Res { uint32_t toc; uint32_t addr[3]; };
std::vector<Res> resolve(const Load* l, const uint32_t* want, int n)
{
    std::vector<Res> out;
    bool all_toc = true, all_abs = true;
    for (int i = 0; i < n; i++) { all_toc &= l[i].toc; all_abs &= !l[i].toc; }
    if (all_abs) {
        Res r = {0, {0, 0, 0}};
        for (int i = 0; i < n; i++) { r.addr[i] = l[i].value; if (rd32(r.addr[i]) != want[i]) return out; }
        out.push_back(r);
        return out;
    }
    if (!all_toc) return out;
    for (uint32_t a = 0x01000000u; a + 4 <= kImageHi; a += 4) {
        if (rd32(a) != want[0]) continue;
        Res r = {a - l[0].value, {0, 0, 0}};
        bool ok = (r.toc & 3) == 0 && r.toc >= 0x01000000u && r.toc < kImageHi;
        for (int i = 0; ok && i < n; i++) {
            r.addr[i] = r.toc + l[i].value;
            ok = r.addr[i] >= kImageLo && r.addr[i] + 4 <= kImageHi && rd32(r.addr[i]) == want[i];
        }
        if (ok) out.push_back(r);
    }
    return out;
}

struct Pick { uint32_t at, getter, delta; std::vector<Res> res; uint32_t end; };   // end: after the 2-way's last slot

// What was found: the screen constant, the 3D view's (one or two), and why not.
struct Found {
    uint32_t screen = 0, view[4] = {0, 0, 0, 0}, n_view = 0;
    uint32_t pick3 = 0, pick2 = 0, s1 = 0, s2 = 0, readers = 0, unresolved = 0;
    const char* why = nullptr;
};

Found find_shape_constants()
{
    Found f;
    // the code: the lifted functions' range
    uint32_t lo = kImageHi, hi = 0;
    for (uint64_t j = 0; j < function_table_count; j++) {
        const uint64_t a = function_table[j].addr;
        if (a < kImageLo || a >= kImageHi) continue;
        if (a < lo) lo = (uint32_t)a;
        if (a > hi) hi = (uint32_t)a;
    }
    if (lo >= hi) { f.why = "no code"; return f; }
    hi = hi + 0x10000u < kImageHi ? hi + 0x10000u : kImageHi;
    std::vector<Pick> three, two;
    for (uint32_t ea = lo + 16; ea + 32 <= hi; ea += 4) {
        const uint32_t w = rd32(ea);
        if ((w & 0xFC7FFFFFu) != 0x2C030001u) continue;   // cmpwi crA, r3, 1
        const uint32_t cra = (w >> 23) & 7;
        uint32_t g = 0, delta = 0;
        for (uint32_t k = 1; k <= 3 && !g; k++) {         // bl G just before (a nop / ld r2,40(r1) between)
            const uint32_t p = rd32(ea - 4 * k);
            if ((p >> 26) == 18 && (p & 3) == 1) { g = bl_target(ea - 4 * k, &delta); break; }
            if (p != 0x60000000u && p != 0xE8410028u) break;
        }
        if (!g) continue;
        const uint32_t w2 = rd32(ea + 4);
        if ((w2 & 0xFC7FFFFFu) == 0x2C030002u) {           // the screen's three-way pick
            const uint32_t crb = (w2 >> 23) & 7;
            const uint32_t l1 = bc_target(ea + 8, 12, 4 * cra + 2), l2 = bc_target(ea + 12, 4, 4 * crb + 2);
            if (!l1 || !l2) continue;
            Load s[3];
            s[0] = slot_at(ea + 16);
            if (!s[0].ok) continue;
            const uint32_t b1 = ea + 16 + 4 * s[0].words;
            if (!b_target(b1) || l2 != b1 + 4) continue;
            s[1] = slot_at(l2);
            s[2] = slot_at(l1);
            if (!s[1].ok || !s[2].ok) continue;
            const uint32_t want[3] = {kShape16x9, kOne, kShape4x3};
            three.push_back({ea, g, delta, resolve(s, want, 3), 0});
        } else {                                           // the 3D view's two-way pick into f1
            const uint32_t l = bc_target(ea + 4, 4, 4 * cra + 2);
            if (!l) continue;
            Load s[2];
            s[0] = slot_at(ea + 8);
            if (!s[0].ok || s[0].freg != 1) continue;
            const uint32_t b1 = ea + 8 + 4 * s[0].words;
            if (!b_target(b1) || l != b1 + 4) continue;
            s[1] = slot_at(l);
            if (!s[1].ok || s[1].freg != 1) continue;
            const uint32_t want[2] = {kShape4x3, kShape16x9};
            two.push_back({ea, g, delta, resolve(s, want, 2), l + 4 * s[1].words});
        }
    }
    // the getter both picks call, exactly one pick of each, and one TOC for G through both
    std::vector<uint32_t> gs;
    for (const Pick& p : three)
        if (!p.res.empty())
            for (const Pick& q : two)
                if (!q.res.empty() && q.getter == p.getter) gs.push_back(p.getter);
    std::sort(gs.begin(), gs.end());
    gs.erase(std::unique(gs.begin(), gs.end()), gs.end());
    if (gs.size() != 1) { f.why = gs.empty() ? "no pick after the TV-shape getter" : "more than one getter"; return f; }
    const Pick* p3 = nullptr;
    const Pick* p2 = nullptr;
    int n3 = 0, n2 = 0;
    for (const Pick& p : three) if (p.getter == gs[0] && !p.res.empty()) { p3 = &p; n3++; }
    for (const Pick& p : two) if (p.getter == gs[0] && !p.res.empty()) { p2 = &p; n2++; }
    if (n3 != 1 || n2 != 1) { f.why = "more than one pick"; return f; }
    // one TOC each, and G's the same through both calls (absolute picks need none)
    const Res* r3 = nullptr;
    const Res* r2 = nullptr;
    int pairs = 0;
    for (const Res& a : p3->res)
        for (const Res& b : p2->res) {
            if (a.toc && b.toc && a.toc + p3->delta != b.toc + p2->delta) continue;
            r3 = &a; r2 = &b; pairs++;
        }
    if (pairs != 1) { f.why = pairs ? "the constants' TOC is not clear" : "the picks' TOCs do not agree"; return f; }
    f.screen = r3->addr[0];
    f.view[f.n_view++] = r2->addr[1];
    f.pick3 = p3->at; f.pick2 = p2->at;
    // S1: the first call after the two-way pick; S2: the other view function its function calls with that constant
    for (uint32_t k = 0; k < 12 && !f.s1; k++) {
        const uint32_t w = rd32(p2->end + 4 * k);
        if ((w >> 26) == 18 && (w & 3) == 1) f.s1 = bl_target(p2->end + 4 * k, nullptr);
    }
    if (!f.s1) { f.why = "no view call after the pick"; return f; }
    struct User { uint32_t at; Load l; uint32_t callee; };
    std::vector<User> users;
    for (uint32_t ea = lo + 32; ea + 32 <= hi; ea += 4) {
        const Load l = lfs_at(ea);
        if (!l.ok || l.freg != 1) continue;
        uint32_t callee = 0;
        for (uint32_t k = 1; k <= 6; k++) {   // the call it feeds: nothing in between writes f1 or branches
            const uint32_t w = rd32(ea + 4 * k), op = w >> 26;
            if (op == 18 && (w & 3) == 1) { callee = bl_target(ea + 4 * k, nullptr); break; }
            if ((op == 48 || op == 50 || op == 59 || op == 63) && ((w >> 21) & 31) == 1) break;
            if (op == 16 || op == 18 || op == 19) break;
        }
        if (callee) users.push_back({ea, l, callee});
    }
    auto addr_of = [&](const Load& l, uint32_t* out) -> int {   // the 16:9 words the load can mean
        if (!l.toc) { *out = l.value; return rd32(l.value) == kShape16x9 ? 1 : 0; }
        int n = 0;
        uint32_t seen = 0;
        for (uint32_t t : {r2->toc, r3->toc}) {
            if (!t) continue;
            const uint32_t a = t + l.value;
            if (a >= kImageLo && a + 4 <= kImageHi && rd32(a) == kShape16x9 && a != seen) { seen = *out = a; n++; }
        }
        return n;
    };
    std::vector<uint32_t> s2;
    for (const User& u : users) {
        uint32_t a = 0;
        if (u.at + 0x400u < p2->at || u.at > p2->at + 0x400u || u.callee == f.s1) continue;
        if (addr_of(u.l, &a) == 1 && a == f.view[0]) s2.push_back(u.callee);
    }
    std::sort(s2.begin(), s2.end());
    s2.erase(std::unique(s2.begin(), s2.end()), s2.end());
    if (s2.size() != 1) { f.why = s2.empty() ? "no second view call" : "more than one second view call"; return f; }
    f.s2 = s2[0];
    for (const User& u : users) {
        if (u.callee != f.s1 && u.callee != f.s2) continue;
        uint32_t a = 0;
        const int n = addr_of(u.l, &a);   // 0: another value (S1 / S2 are called with other shapes too)
        if (n > 1) { f.unresolved++; continue; }
        if (n != 1) continue;
        f.readers++;
        bool have = false;
        for (uint32_t i = 0; i < f.n_view; i++) have |= f.view[i] == a;
        if (!have) {
            if (f.n_view == 4) { f.why = "too many 3D view constants"; return f; }
            f.view[f.n_view++] = a;
        }
    }
    if (f.unresolved) { f.why = "a 3D view load could mean two words"; return f; }
    for (uint32_t i = 0; i < f.n_view; i++)
        if (f.view[i] == f.screen) { f.why = "the screen and the 3D view share a word"; return f; }
    std::sort(f.view, f.view + f.n_view);
    return f;
}

void gt5_widescreen_apply()
{
    const Found f = find_shape_constants();
    if (f.why) {
        // Another release this finder does not know: the runtime keeps the picture 16:9 rather than show a
        // stretched one.
        fprintf(stderr, "[widescreen] the shape constants were not found in this executable (%s): 16:9 only\n", f.why);
        return;
    }
    char views[64] = "";
    for (uint32_t i = 0; i < f.n_view; i++)
        snprintf(views + strlen(views), sizeof views - strlen(views), "%s0x%08X", i ? ", " : "", f.view[i]);
    fprintf(stderr, "[widescreen] shape constants: screen 0x%08X (pick 0x%08X), 3D view %s (pick 0x%08X, %u loads "
                    "for its view calls 0x%08X / 0x%08X)\n", f.screen, f.pick3, views, f.pick2, f.readers, f.s1, f.s2);
    ps3_display_title_widens();
    if (!ps3_display_wide()) return;   // 16:9 asked (the default): the game is left as it is
    const float aspect = ps3_display_aspect();
    uint32_t bits;
    memcpy(&bits, &aspect, 4);
    wr32(f.screen, bits);
    for (uint32_t i = 0; i < f.n_view; i++) wr32(f.view[i], bits);
    fprintf(stderr, "[widescreen] game shape %.4f (16:9 is 1.7778): %u constants set\n", aspect, 1u + f.n_view);
}

const int s_registered = (ps3_on_loaded(gt5_widescreen_apply), 0);

}  // namespace
