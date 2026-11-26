// Cockpit view: a wider (or narrower) field of view, and the driver's eye moved in the seat.
//
// PS3_COCKPIT_FOV     degrees added to the cockpit view's field of view, top to bottom (20 = 20 wider)
// PS3_COCKPIT_SEAT    centimetres the eye moves forward (negative: back)
// PS3_COCKPIT_HEIGHT  centimetres the eye moves up (negative: down)
// All three are unset by default (play.bat, the launcher's Display page, the in-game menu), and then
// nothing here touches the game: the wrapper is only put in the camera's way once one of them is set.
// They follow the settings registry, so the menu changes them while a race runs.
//
// GT5 builds each race camera from a chain of small camera objects (namespace GTCamera), owned by
// GTCamera::CameraOnboard. The view from the driver's seat ends with GTCamera::InCarCamera, and its
// apply() is handed the camera state, whose first 64 bytes are the eye's matrix (rows: right, up,
// back, position; metres in the world). apply() adds the driver's head movement (braking, cornering).
// After the chain, CameraOnboard gives every in-car view the same field of view, its own field at
// +0x368: 52.4 degrees top to bottom unless the game picked one of its other two (42, 35).
//
// Here InCarCamera's apply() is wrapped. Its function descriptor, which the vtable points at, is
// pointed at a host function instead: that runs the game's apply(), moves the eye along the camera's
// own axes, and sets the owner's in-car field of view to the game's value plus the extra degrees. The
// class is found by its C++ type name in the executable and the field by the code that reads it, so
// every release that has both works the same way; one that doesn't is logged and left alone. The
// chase, bumper and roof views and the menus don't use either.
//
// PS3_OUTSIDE_FOV     degrees added to the field of view of the other views Select picks (bumper, roof,
//                     chase): the same, for the views from outside the cockpit. Unset = nothing touched.
// Those views come from the same CameraOnboard: it keeps one camera state per mount (16 of 0x330 bytes),
// and once a tick its update (a virtual function, found by the code that copies the chosen state) copies
// the chosen view's state to where the race camera reads it, plus a second copy the change between
// views works from. Each state names its mount (+0x300), and the game's table of mounts says which kind
// each is: kind 1 is in-car, whose field of view is the cockpit's (above). Here that update is wrapped:
// after it, a copy whose mount is any other kind gets the extra degrees on its field of view (+0x70,
// degrees top to bottom). The game builds the copies again every tick, so nothing adds up. The replay's
// TV cameras, the menus' cameras and the free camera are other cameras and never pass through here.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppu_context.h"

extern "C" unsigned char* vm_base;
extern "C" void ps3_on_loaded(void (*fn)(void));
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
// The settings registry (ps3recomp runtime/settings/gt5_settings.h).
extern "C" double gt5_setting_float(const char* key, double if_unset);
extern "C" void gt5_settings_on_change(const char* key,
                                       void (*fn)(const char* key, const char* old_value, const char* new_value, void* user),
                                       void* user);

// The lifted function table (generated/ppu_recomp_table.cpp): guest address -> lifted function.
struct gt5_func_entry {
    uint64_t addr;
    void (*func)(ppu_context*);
    const char* name;
};
extern "C" const gt5_func_entry function_table[];
extern "C" const uint64_t function_table_count;

namespace {

// Where the executable and its data sit in guest memory: every GT5 release fits well inside this.
const uint32_t kImageLo = 0x00010000;
const uint32_t kImageHi = 0x02000000;

const uint32_t kInCarOwner = 0x08;       // InCarCamera: its CameraOnboard
const uint32_t kOwnerInCarFovy = 0x368;  // CameraOnboard: the in-car views' field of view, degrees

// CameraOnboard's own code that hands an in-car view that field of view (if (r5 == 1) state->fovy =
// this->incar_fovy): the same five instructions in every release looked at (Europe 01.00 and 2.17,
// Asia 01.00 and 2.01).
const uint32_t kInCarFovyCode[] = {0x2F850001, 0x4C9E0020, 0xC0030368, 0xD0040070, 0x4E800020};

float s_fov_extra;    // degrees
float s_seat_fwd;     // metres
float s_seat_up;      // metres
void (*s_apply)(ppu_context*);
uint32_t s_opd;       // InCarCamera::apply's function descriptor; 0 = no in-car camera in this executable
uint32_t s_code;      // the game's apply()
bool s_fov_ok;        // the in-car field of view is where it's expected
bool s_wrapped;       // the descriptor points at incar_apply

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

float rdf(uint32_t ea)
{
    const uint32_t u = rd32(ea);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

void wrf(uint32_t ea, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    wr32(ea, u);
}

float setting_number(const char* name, float lo, float hi)
{
    const float v = (float)gt5_setting_float(name, 0.0);   // "2.5" and "2,5" both, whatever the locale
    if (!(v == v)) return 0.0f;
    return v < lo ? lo : (v > hi ? hi : v);
}

// The first aligned word at or after `from` holding `value`, or 0.
uint32_t find_word(uint32_t value, uint32_t from)
{
    const uint32_t be = __builtin_bswap32(value);
    for (uint32_t ea = (from + 3) & ~3u; ea < kImageHi; ea += 4) {
        uint32_t w;
        memcpy(&w, vm_base + ea, 4);
        if (w == be) return ea;
    }
    return 0;
}

bool has_code(const uint32_t* words, int n)
{
    for (uint32_t ea = find_word(words[0], kImageLo); ea; ea = find_word(words[0], ea + 4)) {
        int i = 1;
        while (i < n && rd32(ea + 4 * i) == words[i]) i++;
        if (i == n) return true;
    }
    return false;
}

// InCarCamera's vtable entry for apply(): the function descriptor's guest address, or 0.
uint32_t find_incar_apply_opd()
{
    static const char kName[] = "N8GTCamera11InCarCameraE";   // with its terminating zero
    const unsigned char* lo = vm_base + kImageLo;
    const unsigned char* hi = vm_base + kImageHi;
    for (const unsigned char* p = lo; p + sizeof kName <= hi; p++) {
        p = (const unsigned char*)memchr(p, 'N', (size_t)(hi - p));
        if (!p || p + sizeof kName > hi) break;
        if (memcmp(p, kName, sizeof kName) != 0 || p[-1] != 0) continue;
        const uint32_t name_ea = (uint32_t)(p - vm_base);
        // The type information object: {its own vtable, name, base class}; then the class's vtable:
        // {0, type information, virtual functions...}. apply() is the sixth function.
        for (uint32_t h = find_word(name_ea, kImageLo); h; h = find_word(name_ea, h + 4)) {
            const uint32_t ti = h - 4;
            if (!rd32(ti)) continue;
            for (uint32_t v = find_word(ti, kImageLo); v; v = find_word(ti, v + 4)) {
                const uint32_t vt = v - 4;
                if (rd32(vt) != 0) continue;
                const uint32_t opd = rd32(vt + 8 + 5 * 4);
                if (opd >= kImageLo && opd < kImageHi && (opd & 3) == 0) return opd;
            }
        }
    }
    return 0;
}

// A camera apply() as GT5 compiles it: opens a stack frame, makes calls, returns, and is short.
bool looks_like_apply(uint32_t code)
{
    if (code < kImageLo || code >= kImageHi || (code & 3)) return false;
    bool frame = false;
    for (uint32_t ea = code; ea < code + 16; ea += 4)
        if ((rd32(ea) & 0xFFFF0000u) == 0xF8210000u) frame = true;   // stdu r1,-N(r1)
    if (!frame) return false;
    int calls = 0;
    for (uint32_t ea = code; ea < code + 0x200; ea += 4) {
        const uint32_t w = rd32(ea);
        if ((w & 0xFC000003u) == 0x48000001u) calls++;             // bl
        if (w == 0x4E800020u) return calls >= 3;                    // blr
    }
    return false;
}

void incar_apply(ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3];   // the InCarCamera
    const uint32_t st = (uint32_t)ctx->gpr[4];     // the camera state
    s_apply(ctx);
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        tf(ctx);
    }

    if (st && (s_seat_fwd != 0.0f || s_seat_up != 0.0f)) {
        // Rows of the eye's matrix: 0 right, 1 up, 2 back, 3 position. Forward is minus "back".
        for (int i = 0; i < 3; i++) {
            const float up = rdf(st + 0x10 + 4 * i), back = rdf(st + 0x20 + 4 * i);
            const float pos = rdf(st + 0x30 + 4 * i);
            wrf(st + 0x30 + 4 * i, pos + s_seat_up * up - s_seat_fwd * back);
        }
    }

    const uint32_t owner = self ? rd32(self + kInCarOwner) : 0;
    // The owner keeps the value from frame to frame, so the game's own number is what's there
    // unless it's the one written here last time (the game can switch between its three).
    static uint32_t last_owner;
    static float last_written, game_fov;
    static int logged;
    const float fov_extra = s_fov_extra;   // the menu can change it on another thread
    if (fov_extra == 0.0f && owner && owner == last_owner && last_written != 0.0f) {
        // Switched off while playing: the game's own number goes back, once.
        if (rdf(owner + kOwnerInCarFovy) == last_written) wrf(owner + kOwnerInCarFovy, game_fov);
        last_written = 0.0f;
        last_owner = 0;
    }
    if (fov_extra != 0.0f && owner) {
        const float now = rdf(owner + kOwnerInCarFovy);
        if (owner != last_owner || now != last_written) {
            if (now != game_fov && logged < 8) {
                logged++;
                fprintf(stderr, "[cockpit] the game's cockpit view: %.1f degrees, now %.1f\n", now,
                        fminf(fmaxf(now + fov_extra, 10.0f), 140.0f));
            }
            game_fov = now;
            last_owner = owner;
        }
        if (game_fov > 1.0f && game_fov < 179.0f) {
            last_written = fminf(fmaxf(game_fov + fov_extra, 10.0f), 140.0f);
            wrf(owner + kOwnerInCarFovy, last_written);
        }
    }
}

// Reads the three settings and, the first time one of them is set, points the descriptor at the wrapper
// (one aligned word of guest memory: a camera update on another thread sees the old or the new one).
// The wrapper stays once it is in: with all three back at zero it only runs the game's apply().
void cockpit_refresh()
{
    float fov = setting_number("PS3_COCKPIT_FOV", -30.0f, 60.0f);
    const float fwd = setting_number("PS3_COCKPIT_SEAT", -100.0f, 100.0f) / 100.0f;
    const float up = setting_number("PS3_COCKPIT_HEIGHT", -50.0f, 50.0f) / 100.0f;
    static bool said_none, said_fov;
    if (fov == 0.0f && fwd == 0.0f && up == 0.0f && !s_wrapped) return;   // the game as it is
    if (!s_opd || !s_apply) {
        if (!said_none) {
            said_none = true;
            if (!s_opd)
                fprintf(stderr, "[cockpit] no in-car camera found in this executable (0x%08X): cockpit settings"
                                " ignored\n", s_code);
            else
                fprintf(stderr, "[cockpit] the in-car camera (0x%08X) isn't in the lifted code: cockpit settings"
                                " ignored\n", s_code);
        }
        return;
    }
    if (fov != 0.0f && !s_fov_ok) {
        if (!said_fov) {
            said_fov = true;
            fprintf(stderr, "[cockpit] the in-car field of view isn't where it's expected in this executable:"
                            " PS3_COCKPIT_FOV ignored\n");
        }
        fov = 0.0f;
        if (fwd == 0.0f && up == 0.0f && !s_wrapped) return;
    }
    const bool changed = fov != s_fov_extra || fwd != s_seat_fwd || up != s_seat_up;
    s_fov_extra = fov; s_seat_fwd = fwd; s_seat_up = up;
    if (!s_wrapped) {
        // The descriptor's own address stands in for the function: the vtable call lands here.
        wr32(s_opd, s_opd);
        s_wrapped = true;
    }
    if (changed)
        fprintf(stderr, "[cockpit] field of view %+.0f degrees, seat %+.0f cm forward, %+.0f cm up"
                        " (in-car camera 0x%08X)\n", s_fov_extra, s_seat_fwd * 100.0f, s_seat_up * 100.0f, s_code);
}

void cockpit_setting_changed(const char*, const char*, const char*, void*) { cockpit_refresh(); }

// ---- The outside views (PS3_OUTSIDE_FOV) --------------------------------------------------------------

// A camera state (0x330 bytes):
const uint32_t kStateFovy = 0x70;       // its field of view, degrees top to bottom
const uint32_t kStateMount = 0x300;     // its mount (the game's table below)
const int kMounts = 16;
const int kKindInCar = 1;

float s_out_extra;         // degrees
uint32_t s_ob_opd;         // CameraOnboard's update: its function descriptor, 0 = not found
uint32_t s_ob_code;
void (*s_ob_update)(ppu_context*);
uint32_t s_ob_active;      // CameraOnboard: the chosen view's index (only logged)
uint32_t s_ob_out[2];      // CameraOnboard: the two copies of the chosen view's state
bool s_ob_wrapped;
bool s_ob_probe;           // GT5_OUTFOV_PROBE=1: wrapped from the start, and every change of view logged
int32_t s_mount_id[kMounts], s_mount_kind[kMounts];

// The kind of a mount by the game's table, or -1 when it isn't there.
int mount_kind(int32_t mount)
{
    if (mount < 0) return -1;
    for (int i = 0; i < kMounts; i++)
        if (s_mount_id[i] == mount) return s_mount_kind[i];
    return -1;
}

// The game's table of camera mounts, {mount, kind} x 16, starting {0,0} {26,1} {1,3} {6,5} in every
// release looked at (Europe 1.00, 2.11 and 2.17, Asia 1.00 and 2.01). False when it isn't found once.
bool find_mount_table()
{
    static const uint32_t kStart[] = {0, 0, 26, 1, 1, 3, 6, 5};
    uint32_t found = 0;
    for (uint32_t ea = find_word(26, kImageLo + 8); ea; ea = find_word(26, ea + 4)) {
        int i = 0;
        while (i < 8 && rd32(ea - 8 + 4 * i) == kStart[i]) i++;
        if (i < 8) continue;
        if (found) return false;   // twice: don't guess
        found = ea - 8;
    }
    if (!found) return false;
    for (int i = 0; i < kMounts; i++) {
        s_mount_id[i] = (int32_t)rd32(found + 8 * i);
        s_mount_kind[i] = (int32_t)rd32(found + 8 * i + 4);
    }
    return true;
}

// Is this CameraOnboard's per-tick update? It copies the chosen state twice (memcpy, r5 = 0x330); next
// to the first: lwz r4,ACTIVE(rX); mulli r4,r4,0x330; addi r4,r4,STATES; addi r0,rY,OUT; and an
// addi rX,rY,OUT2 just before the second.
bool parse_onboard_update(uint32_t code)
{
    if (code < kImageLo || code + 0x600 >= kImageHi || (code & 3)) return false;
    bool frame = false;
    for (uint32_t k = 0; k < 4; k++)
        if ((rd32(code + 4 * k) & 0xFFFF0000u) == 0xF8210000u) frame = true;   // stdu r1,-N(r1)
    if (!frame) return false;
    const uint32_t n = 0x600 / 4;
    uint32_t li5[2], nli = 0;
    for (uint32_t k = 0; k < n && nli < 2; k++)
        if (rd32(code + 4 * k) == 0x38A00330u) li5[nli++] = k;              // li r5,0x330
    if (nli < 2) return false;
    int64_t act = -1, states = -1, out = -1, out2 = -1;
    bool mul = false;
    for (uint32_t j = li5[0] >= 4 ? li5[0] - 4 : 0; j < li5[0] + 6 && j < n; j++) {
        const uint32_t w = rd32(code + 4 * j);
        if ((w & 0xFFE00000u) == 0x80800000u && act < 0) act = w & 0xFFFF;              // lwz r4,D(rX)
        if ((w & 0xFFFF0000u) == 0x38840000u) states = w & 0xFFFF;                      // addi r4,r4,S
        if ((w & 0xFFE00000u) == 0x38000000u && ((w >> 16) & 31)) out = w & 0xFFFF;      // addi r0,rY,O
        if (w == 0x1C840330u) mul = true;                                               // mulli r4,r4,0x330
    }
    for (uint32_t j = li5[1] >= 6 ? li5[1] - 6 : 0; j < li5[1]; j++) {
        const uint32_t w = rd32(code + 4 * j);
        if ((w >> 26) == 14 && ((w >> 16) & 31) && (w & 0xFFFF) >= 0x1000 && !(w & 0x8000))
            out2 = w & 0xFFFF;                                                          // addi rX,rY,O2
    }
    if (!mul || act < 0 || states < 0 || out < 0 || out2 < 0) return false;
    s_ob_active = (uint32_t)act;
    s_ob_out[0] = (uint32_t)out;
    s_ob_out[1] = (uint32_t)out2;
    return true;
}

// CameraOnboard's vtable entry for the update: the function descriptor's guest address, or 0.
uint32_t find_onboard_update_opd()
{
    static const char kName[] = "N8GTCamera13CameraOnboardE";
    const unsigned char* lo = vm_base + kImageLo;
    const unsigned char* hi = vm_base + kImageHi;
    uint32_t found = 0;
    for (const unsigned char* p = lo; p + sizeof kName <= hi; p++) {
        p = (const unsigned char*)memchr(p, 'N', (size_t)(hi - p));
        if (!p || p + sizeof kName > hi) break;
        if (memcmp(p, kName, sizeof kName) != 0 || p[-1] != 0) continue;
        const uint32_t name_ea = (uint32_t)(p - vm_base);
        for (uint32_t h = find_word(name_ea, kImageLo); h; h = find_word(name_ea, h + 4)) {
            const uint32_t ti = h - 4;
            if (!rd32(ti)) continue;
            for (uint32_t v = find_word(ti, kImageLo); v; v = find_word(ti, v + 4)) {
                const uint32_t vt = v - 4;
                if (rd32(vt) != 0) continue;
                for (int i = 0; i < 48; i++) {
                    const uint32_t opd = rd32(vt + 8 + 4 * i);
                    if (opd < kImageLo || opd >= kImageHi || (opd & 3)) break;
                    const uint32_t code = rd32(opd);
                    if (code < kImageLo || code >= kImageHi || (code & 3)) break;
                    if (!parse_onboard_update(code)) continue;
                    if (found && found != opd) return 0;   // two of them: don't guess
                    found = opd;
                }
            }
        }
    }
    if (found) parse_onboard_update(rd32(found));   // its offsets again (a later candidate may have set them)
    return found;
}

// One copy of the chosen view's state: an outside view gets the extra degrees. A copy the game didn't
// build again since the last tick (it still holds what was written here) is left as it is.
void outside_fix(uint32_t st, float extra)
{
    static struct { uint32_t st; float written; } s_last[8];
    static int s_next;
    int e = -1;
    for (int i = 0; i < 8; i++) if (s_last[i].st == st) e = i;
    if (e < 0) { e = s_next; s_next = (s_next + 1) & 7; s_last[e].st = st; s_last[e].written = 0.0f; }
    const float now = rdf(st + kStateFovy);
    if (s_last[e].written != 0.0f && now == s_last[e].written) return;
    s_last[e].written = 0.0f;
    if (extra == 0.0f) return;
    const int kind = mount_kind((int32_t)rd32(st + kStateMount));
    if (kind < 0 || kind == kKindInCar) return;
    if (!(now > 1.0f && now < 179.0f)) return;
    const float v = fminf(fmaxf(now + extra, 10.0f), 140.0f);
    wrf(st + kStateFovy, v);
    s_last[e].written = v;
}

// GT5_OUTFOV_PROBE: a line each time a CameraOnboard's chosen view changes (at most 200).
void outside_probe(uint32_t self)
{
    static struct { uint32_t self; int32_t key; } s_seen[8];
    static int s_lines;
    if (s_lines >= 200) return;
    const uint32_t a = self + s_ob_out[0], b = self + s_ob_out[1];
    const int32_t idx = (int32_t)rd32(self + s_ob_active);
    const int32_t ma = (int32_t)rd32(a + kStateMount), mb = (int32_t)rd32(b + kStateMount);
    const int32_t key = (idx & 0xFF) | ((ma & 0xFF) << 8) | ((mb & 0xFF) << 16);
    int e = -1, free_e = -1;
    for (int i = 0; i < 8; i++) {
        if (s_seen[i].self == self) e = i;
        if (!s_seen[i].self && free_e < 0) free_e = i;
    }
    if (e >= 0 && s_seen[e].key == key) return;
    if (e < 0) { if (free_e < 0) return; e = free_e; s_seen[e].self = self; }
    s_seen[e].key = key;
    s_lines++;
    fprintf(stderr, "[outfov-probe] onboard 0x%08X view %d: out mount %d kind %d fov %.2f | out2 mount %d kind %d fov %.2f\n",
            self, idx, ma, mount_kind(ma), rdf(a + kStateFovy), mb, mount_kind(mb), rdf(b + kStateFovy));
}

void onboard_update(ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3];   // the CameraOnboard
    s_ob_update(ctx);
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        tf(ctx);
    }
    if (!self) return;
    const float extra = s_out_extra;   // the menu can change it on another thread
    outside_fix(self + s_ob_out[0], extra);
    outside_fix(self + s_ob_out[1], extra);
    if (s_ob_probe) outside_probe(self);
}

// Reads PS3_OUTSIDE_FOV and, the first time it is set, points the descriptor at the wrapper (which then
// stays; at zero it only runs the game's update).
void outside_refresh()
{
    const float fov = setting_number("PS3_OUTSIDE_FOV", -30.0f, 60.0f);
    static bool said_none;
    if (fov == 0.0f && !s_ob_wrapped && !s_ob_probe) return;   // the game as it is
    if (!s_ob_opd || !s_ob_update) {
        if (!said_none) {
            said_none = true;
            fprintf(stderr, "[cockpit] the race camera's update %s: PS3_OUTSIDE_FOV ignored\n",
                    !s_ob_opd ? "isn't found in this executable" : "isn't in the lifted code");
        }
        return;
    }
    const bool changed = fov != s_out_extra;
    s_out_extra = fov;
    if (!s_ob_wrapped) {
        wr32(s_ob_opd, s_ob_opd);
        s_ob_wrapped = true;
    }
    if (changed)
        fprintf(stderr, "[cockpit] outside views: field of view %+.0f degrees (race camera 0x%08X)\n", s_out_extra, s_ob_code);
}

void outside_setting_changed(const char*, const char*, const char*, void*) { outside_refresh(); }

void outside_find()
{
    const char* probe = getenv("GT5_OUTFOV_PROBE");
    s_ob_probe = probe && probe[0] == '1';
    const uint32_t opd = find_onboard_update_opd();
    if (!opd || !find_mount_table()) return;
    s_ob_code = rd32(opd);
    for (uint64_t i = 0; i < function_table_count; i++)
        if (function_table[i].addr == s_ob_code) s_ob_update = function_table[i].func;
    s_ob_opd = opd;
    if (s_ob_update) ppu_register_function(opd, onboard_update);
    if (s_ob_probe)
        fprintf(stderr, "[outfov-probe] update 0x%08X (descriptor 0x%08X): view +0x%X, copies +0x%X +0x%X\n",
                s_ob_code, opd, s_ob_active, s_ob_out[0], s_ob_out[1]);
}

void gt5_cockpit_apply()
{
    // Found once, when the game is loaded; the wrapper is registered under the descriptor's address
    // (which nothing calls until cockpit_refresh points the descriptor at itself).
    const uint32_t opd = find_incar_apply_opd();
    s_code = opd ? rd32(opd) : 0;
    if (opd && looks_like_apply(s_code)) {
        s_opd = opd;
        s_fov_ok = has_code(kInCarFovyCode, 5);
        for (uint64_t i = 0; i < function_table_count; i++)
            if (function_table[i].addr == s_code) s_apply = function_table[i].func;
        if (s_apply) ppu_register_function(opd, incar_apply);
    }
    cockpit_refresh();
    gt5_settings_on_change("PS3_COCKPIT_FOV", cockpit_setting_changed, nullptr);
    gt5_settings_on_change("PS3_COCKPIT_SEAT", cockpit_setting_changed, nullptr);
    gt5_settings_on_change("PS3_COCKPIT_HEIGHT", cockpit_setting_changed, nullptr);

    outside_find();
    outside_refresh();
    gt5_settings_on_change("PS3_OUTSIDE_FOV", outside_setting_changed, nullptr);
}

const int s_registered = (ps3_on_loaded(gt5_cockpit_apply), 0);

}  // namespace

// For the in-game menu: whether the outside views' field of view can be changed in this executable.
extern "C" int gt5_outside_fov_found(void) { return s_ob_opd && s_ob_update ? 1 : 0; }
