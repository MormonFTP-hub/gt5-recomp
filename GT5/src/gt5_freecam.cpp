// Free camera: in a race or a replay, the view leaves the game's camera and flies where the player steers it
// (asked for on 24 Nov 2026: GT5's Photo Mode isn't offered in Arcade).
//
// Off until the player switches it on (GT5_FREECAM_KEY, F6 unless set; or the in-game menu's Game page), and
// until then nothing here touches the game. Switched on, it starts exactly where the game's camera is, and the
// game gets no input while it is on (gt5_menu.cpp holds the controller at rest, as for the menu), so the car
// doesn't drive off. It works while GT5 is paused too, so a race or a replay can be paused and looked around.
// The input (keys, mouse, controller) comes from the in-game menu's window code (gt5_menu.cpp), which calls
// the functions at the bottom of this file; a build without the menu has no way to switch it on.
//
// Where the view comes from. GT5 draws a race through its scene library, PDIScene, whose camera for the race
// is a GTRender::RaceCameraStatus2SceneCameraAdapter: a PDIScene::SceneCamera made from the race camera's
// status (GTEngine::IRaceCameraStatus: position, rotation, field of view...). Every time the scene wants the
// view it asks the adapter, and the adapter builds it from the status, in four functions of its vtable:
//     [2]  projection, returned           (r3 the result, r4 this)
//     [3]  view, returned                 (r3 the result, r4 this)
//     [13] projection, into r5            (r3 this)
//     [14] view, into r5                  (r3 this)
// A view is the inverse of the eye's matrix: four rows of four floats, rows right / up / back / position (the
// world's metres), translation in the last row; a projection has the field of view in its first two
// diagonal entries. Those four are wrapped here, the way gt5_cockpit.cpp wraps InCarCamera::apply: each one's
// function descriptor, which the vtable points at, is pointed at a host function that runs the game's own and
// then, while the free camera is on and this is the race view's adapter, writes the free camera's view (and
// its field of view into the projection) over what the game made. Whatever the game's camera is (chase,
// cockpit, a replay's TV cameras) and whatever else the race is doing (paused, a replay stopped), the scene
// draws from here. (Europe 1.00, measured: each frame the scene asks [2] and [3] of two adapters that carry the
// same camera, and nothing asks [13] / [14]; both adapters are flown. GT5's race pause and a paused replay keep
// asking every frame.) The descriptors are pointed at the wrappers the first time the free camera is switched on;
// switched off again, the wrappers only run the game's functions. The class is found by its C++ type name
// in the executable and each function checked by the calls it makes, so every release that has it works the
// same way (Europe 1.00, 2.11, 2.17, Asia 1.00, 2.01 checked); one that doesn't says so in the log once.
//
// GT5_FREECAM_PROBE=1 (tests): the wrappers from the start, and a "[freecam-probe]" line a second for each
// adapter the game asks: how often each of the four ran, where its camera is, and the status behind it.
#include <atomic>
#include <chrono>
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
extern "C" int cellPad_race_pause_state(void);   // cellPad.c: 1 paused, 0 running, -1 not known (the probe's lines)

// The lifted function table (generated/ppu_recomp_table.cpp): guest address -> lifted function.
struct gt5_func_entry {
    uint64_t addr;
    void (*func)(ppu_context*);
    const char* name;
};
extern "C" const gt5_func_entry function_table[];
extern "C" const uint64_t function_table_count;

namespace {

const uint32_t kImageLo = 0x00010000;
const uint32_t kImageHi = 0x02000000;

enum Slot { PROJ = 0, VIEW, PROJ_OUT, VIEW_OUT, NSLOT };
const int kSlotIndex[NSLOT] = {2, 3, 13, 14};
const char* const kSlotName[NSLOT] = {"projection", "view", "projection out", "view out"};
// The status functions each one calls, by their vtable offsets, in order: what tells them apart and says the
// adapter is the one this was written for.
const uint32_t kProjCalls[] = {0x28, 0x2C, 0x34, 0x38, 0x30};
const uint32_t kViewCalls[] = {0x10, 0x14};

// Flying.
const double kSpeed = 8.0;            // metres a second at full stick / a key held
const double kFast = 4.0, kSlow = 0.25;
const double kMouseRad = 0.0022;      // radians a pixel of mouse movement, at 60 degrees' field of view
const double kStickRad = 2.0;         // radians a second at full right stick, at 60 degrees
const double kPitchMax = 1.55;        // radians, up or down (89 degrees)
const double kFovMin = 5.0, kFovMax = 120.0;
const double kPi = 3.14159265358979323846;
const double kStaleS = 1.5;          // the race view gone this long: the free camera lets go
const double kStepS = 0.008;         // a new camera position at most this often (a frame's passes share one)

uint32_t s_opd[NSLOT], s_code[NSLOT];
void (*s_fn[NSLOT])(ppu_context*);
bool s_found;                          // the adapter and its four functions, as expected and lifted
bool s_probe;
std::atomic<bool> s_wrapped{false};

// ---- guest memory --------------------------------------------------------------------------------------

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
void rd_mat(uint32_t ea, float m[16])
{
    for (int i = 0; i < 16; i++) {
        const uint32_t u = rd32(ea + 4 * i);
        memcpy(&m[i], &u, 4);
    }
}
void wr_mat(uint32_t ea, const float m[16])
{
    for (int i = 0; i < 16; i++) {
        uint32_t u;
        memcpy(&u, &m[i], 4);
        wr32(ea + 4 * i, u);
    }
}
bool guest_ok(uint32_t ea, uint32_t len) { return ea >= kImageLo && ea + len > ea && ea + len <= 0xF0000000u; }

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

// The adapter's vtable ({0, type information, virtual functions...}), or 0.
uint32_t find_vtable()
{
    static const char kName[] = "N8GTRender35RaceCameraStatus2SceneCameraAdapterE";   // with its terminating zero
    const unsigned char* lo = vm_base + kImageLo;
    const unsigned char* hi = vm_base + kImageHi;
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
                if (rd32(vt) == 0) return vt;
            }
        }
    }
    return 0;
}

// The vtable offsets of the virtual calls a function makes, in order ("lwz rD, off(rB)" and, within three
// instructions, "lwz r0, 0(rD)": the descriptor's code word), whatever the registers. n = how many found.
int virtual_calls(uint32_t code, uint32_t* out, int cap)
{
    int n = 0;
    for (uint32_t ea = code; ea < code + 0x200; ea += 4) {
        const uint32_t w = rd32(ea);
        if (w == 0x4E800020u) break;                                       // blr
        if ((w >> 26) != 32 || (w & 0xFFFF) == 0 || (w & 0xFFFF) >= 0x100) continue;   // lwz with a small offset
        const uint32_t d = (w >> 21) & 31;
        for (int k = 1; k <= 3; k++)
            if (rd32(ea + 4 * k) == (0x80000000u | (d << 16))) {
                if (n < cap) out[n] = w & 0xFFFF;
                n++;
                break;
            }
    }
    return n;
}

bool calls_are(uint32_t code, const uint32_t* want, int n)
{
    uint32_t got[16];
    const int k = virtual_calls(code, got, 16);
    if (k != n) return false;
    for (int i = 0; i < n; i++) if (got[i] != want[i]) return false;
    return true;
}

// ---- time and a lock ------------------------------------------------------------------------------------

double now_s()
{
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

std::atomic_flag s_lock = ATOMIC_FLAG_INIT;
struct Lock {
    Lock() { while (s_lock.test_and_set(std::memory_order_acquire)) {} }
    ~Lock() { s_lock.clear(std::memory_order_release); }
};

// ---- what the game asks ----------------------------------------------------------------------------------

struct Adapter {
    uint32_t self = 0;
    double last_view = -1.0;           // now_s() of its last view
    float view[16] = {}, proj[16] = {};   // the game's own, last made
    bool have_view = false, have_proj = false;
    unsigned calls[NSLOT] = {};        // this second
    unsigned views_last = 0;           // views in the last whole second
    unsigned views_arming = 0;         // views since the free camera was asked for
    bool follower = false;             // its view is the race view's camera too: flown with it
};
const int kAdapters = 8;
Adapter s_ad[kAdapters];
double s_sec_t = 0.0;

Adapter* adapter_of(uint32_t self)   // under the lock
{
    Adapter* free_slot = nullptr;
    Adapter* oldest = &s_ad[0];
    for (Adapter& a : s_ad) {
        if (a.self == self) return &a;
        if (!a.self && !free_slot) free_slot = &a;
        if (a.last_view < oldest->last_view) oldest = &a;
    }
    Adapter* a = free_slot ? free_slot : oldest;
    *a = Adapter();
    a->self = self;
    return a;
}

// ---- the free camera ------------------------------------------------------------------------------------

enum State { OFF = 0, ARMING, ON };
std::atomic<int> s_state{OFF};
double s_arm_t = 0.0;                   // when it was asked for
uint32_t s_main = 0;                    // the adapter it draws for
std::atomic<int64_t> s_main_seen_us{0}; // now_s() of its last view, in microseconds (read without the lock)
// [raceview] now_s() of the last view any race view adapter was asked for, in microseconds (0 = none yet), for
// gt5_race_view_age_ms: the controller's trigger layout (gt5_padmap.cpp) follows it. s_race_view_hook: the
// wrappers are in from the start for it (PAD_RACE_VIEW=0: not, as before 27 Nov 2026).
std::atomic<int64_t> s_race_view_us{0};
bool s_race_view_hook;
double s_px, s_py, s_pz;                // position, world metres
double s_yaw, s_pitch, s_roll;          // radians
double s_fov = 60.0, s_fov0 = 60.0;     // degrees, top to bottom; the game's when it started
double s_last_step = -1.0;
// Input, from the menu's window code and its controller feed (any thread).
std::atomic<unsigned> s_keys{0};
std::atomic<int> s_mouse_dx{0}, s_mouse_dy{0}, s_wheel{0};
std::atomic<int> s_reset{0};
std::atomic<uint64_t> s_pad{0};         // connected | buttons << 8 | lx << 24 | ly << 32 | rx << 40 | ry << 48 (l2, r2 apart)
std::atomic<unsigned> s_pad_trig{0};    // l2 | r2 << 8
unsigned s_pad_prev;
int s_said_missing;
int s_follow_said;

enum Keys : unsigned { K_FWD = 1, K_BACK = 2, K_LEFT = 4, K_RIGHT = 8, K_UP = 16, K_DOWN = 32, K_FAST = 64, K_SLOW = 128 };

// The eye's rows from yaw, pitch and roll: forward f, right r, up u, back = -f. Yaw 0 looks along -z, yaw
// turns left about +y, pitch looks up, roll turns the picture about the forward axis.
void axes(double yaw, double pitch, double roll, double r[3], double u[3], double b[3])
{
    const double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch);
    const double f[3] = {-sy * cp, sp, -cy * cp};
    const double r0[3] = {cy, 0.0, -sy};
    const double u0[3] = {r0[1] * f[2] - r0[2] * f[1], r0[2] * f[0] - r0[0] * f[2], r0[0] * f[1] - r0[1] * f[0]};
    const double cr = cos(roll), sr = sin(roll);
    for (int i = 0; i < 3; i++) {
        r[i] = cr * r0[i] + sr * u0[i];
        u[i] = -sr * r0[i] + cr * u0[i];
        b[i] = -f[i];
    }
}

// The game's view taken apart: where the eye is and how it is turned. false: not an eye's view.
bool pose_from_view(const float v[16])
{
    // v[i*4+j]: row i, column j. The eye's rows R_j[i] = v[i][j]; position = -sum_j v[3][j] R_j.
    double R[3][3];
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) R[j][i] = v[i * 4 + j];
    double p[3] = {0, 0, 0};
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) p[i] -= v[12 + j] * R[j][i];
    for (int j = 0; j < 3; j++) {
        const double len = sqrt(R[j][0] * R[j][0] + R[j][1] * R[j][1] + R[j][2] * R[j][2]);
        if (!(len > 0.9 && len < 1.1)) return false;
    }
    const double f[3] = {-R[2][0], -R[2][1], -R[2][2]};
    s_pitch = asin(f[1] < -1.0 ? -1.0 : f[1] > 1.0 ? 1.0 : f[1]);
    s_yaw = atan2(-f[0], -f[2]);
    double r0[3], u0[3], b0[3];
    axes(s_yaw, s_pitch, 0.0, r0, u0, b0);
    s_roll = atan2(R[0][0] * u0[0] + R[0][1] * u0[1] + R[0][2] * u0[2], R[0][0] * r0[0] + R[0][1] * r0[1] + R[0][2] * r0[2]);
    s_px = p[0]; s_py = p[1]; s_pz = p[2];
    return true;
}

void view_from_pose(float v[16])
{
    double r[3], u[3], b[3];
    axes(s_yaw, s_pitch, s_roll, r, u, b);
    const double* R[3] = {r, u, b};
    const double p[3] = {s_px, s_py, s_pz};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) v[i * 4 + j] = (float)R[j][i];
        v[i * 4 + 3] = 0.0f;
    }
    for (int j = 0; j < 3; j++) v[12 + j] = (float)-(p[0] * R[j][0] + p[1] * R[j][1] + p[2] * R[j][2]);
    v[15] = 1.0f;
}

// The eye's position in the world from a view (row-vector convention: translation in the last row).
void eye_of(const float v[16], double p[3])
{
    for (int i = 0; i < 3; i++) p[i] = 0.0;
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) p[i] -= (double)v[12 + j] * v[i * 4 + j];
}

// Two views of one camera: eyes within 3 m (a frame apart at any speed GT5 has) and looking the same way.
bool same_camera(const float a[16], const float b[16])
{
    double pa[3], pb[3];
    eye_of(a, pa);
    eye_of(b, pb);
    const double dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
    const double back_dot = (double)a[2] * b[2] + (double)a[6] * b[6] + (double)a[10] * b[10];
    return dx * dx + dy * dy + dz * dz < 9.0 && back_dot > 0.98;
}

double fov_of(const float p[16])
{
    const double y = fabs((double)p[5]);
    return y > 1e-6 ? 2.0 * atan(1.0 / y) * 180.0 / kPi : 60.0;
}

double stick(int v, double dead)   // 0..255 -> -1..1 past the dead zone
{
    double s = ((double)v - 128.0) / 127.0;
    if (s > 1.0) s = 1.0;
    if (s < -1.0) s = -1.0;
    const double a = fabs(s);
    if (a <= dead) return 0.0;
    return (s > 0 ? 1.0 : -1.0) * (a - dead) / (1.0 - dead);
}

double clamp1(double v) { return v > 1.0 ? 1.0 : v < -1.0 ? -1.0 : v; }

double trigger(int v)   // 0..255 -> 0..1 past a small dead zone (a resting trigger reads a little)
{
    const double t = (double)v / 255.0;
    return t <= 0.06 ? 0.0 : (t - 0.06) / 0.94;
}

// One step of flying, up to now (under the lock).
void step(double now)
{
    double dt = s_last_step < 0.0 ? 0.0 : now - s_last_step;
    s_last_step = now;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;                                  // a hitch doesn't throw the camera across the track
    const unsigned k = s_keys.load(std::memory_order_relaxed);
    const uint64_t pad = s_pad.load(std::memory_order_relaxed);
    const unsigned trig = s_pad_trig.load(std::memory_order_relaxed);
    const bool pc = (pad & 1) != 0;
    const unsigned buttons = pc ? (unsigned)((pad >> 8) & 0xFFFF) : 0;
    const double lx = pc ? stick((int)((pad >> 24) & 0xFF), 0.15) : 0.0, ly = pc ? stick((int)((pad >> 32) & 0xFF), 0.15) : 0.0;
    const double rx = pc ? stick((int)((pad >> 40) & 0xFF), 0.12) : 0.0, ry = pc ? stick((int)((pad >> 48) & 0xFF), 0.12) : 0.0;
    const double l2 = pc ? trigger((int)(trig & 0xFF)) : 0.0, r2 = pc ? trigger((int)((trig >> 8) & 0xFF)) : 0.0;
    // CELL_PAD_CTRL_*: l3 1<<1, r3 1<<2, up 1<<4, down 1<<6, l1 1<<10, r1 1<<11
    const unsigned pressed = buttons & ~s_pad_prev;
    s_pad_prev = buttons;

    const double fov_scale = s_fov / 60.0;
    // Looking: the mouse, and the right stick (squared, for small corrections).
    const int mdx = s_mouse_dx.exchange(0), mdy = s_mouse_dy.exchange(0);
    s_yaw -= (double)mdx * kMouseRad * fov_scale;
    s_pitch -= (double)mdy * kMouseRad * fov_scale;
    s_yaw -= rx * fabs(rx) * kStickRad * fov_scale * dt;
    s_pitch -= ry * fabs(ry) * kStickRad * fov_scale * dt;
    if (s_pitch > kPitchMax) s_pitch = kPitchMax;
    if (s_pitch < -kPitchMax) s_pitch = -kPitchMax;
    if (s_yaw > kPi) s_yaw -= 2.0 * kPi;
    if (s_yaw < -kPi) s_yaw += 2.0 * kPi;

    // Zoom: the mouse wheel (a notch is 10 %), the d-pad's up and down (half or double in a second held).
    const int wheel = s_wheel.exchange(0);
    if (wheel) s_fov *= pow(0.9, (double)wheel);
    if (buttons & (1u << 4)) s_fov *= exp(-0.69 * dt);
    if (buttons & (1u << 6)) s_fov *= exp(0.69 * dt);
    if (s_reset.exchange(0) || (pressed & (1u << 2))) { s_fov = s_fov0; s_roll = 0.0; }
    if (s_fov < kFovMin) s_fov = kFovMin;
    if (s_fov > kFovMax) s_fov = kFovMax;

    // Moving: along where the camera looks, sideways level, up and down straight.
    double fwd = ((k & K_FWD) ? 1.0 : 0.0) - ((k & K_BACK) ? 1.0 : 0.0) - ly;
    double side = ((k & K_RIGHT) ? 1.0 : 0.0) - ((k & K_LEFT) ? 1.0 : 0.0) + lx;
    double vert = ((k & K_UP) ? 1.0 : 0.0) - ((k & K_DOWN) ? 1.0 : 0.0) + r2 - l2;
    fwd = clamp1(fwd);
    side = clamp1(side);
    vert = clamp1(vert);
    double speed = kSpeed;
    if ((k & K_FAST) || (buttons & (1u << 11))) speed *= kFast;
    if ((k & K_SLOW) || (buttons & (1u << 10))) speed *= kSlow;
    const double cy = cos(s_yaw), sy = sin(s_yaw), cp = cos(s_pitch), sp = sin(s_pitch);
    const double f[3] = {-sy * cp, sp, -cy * cp}, r[3] = {cy, 0.0, -sy};
    const double d = speed * dt;
    s_px += (f[0] * fwd + r[0] * side) * d;
    s_py += (f[1] * fwd + vert) * d;
    s_pz += (f[2] * fwd + r[2] * side) * d;
}

void wrap()
{
    if (s_wrapped.exchange(true)) return;
    // The descriptor's own address stands in for the function: the vtable call lands in the wrapper.
    // One aligned word each: a camera on another thread sees the old or the new one.
    for (int i = 0; i < NSLOT; i++) wr32(s_opd[i], s_opd[i]);
}

const char* status_class(uint32_t self)   // the status behind an adapter, by its type name (the probe)
{
    static char buf[96];
    snprintf(buf, sizeof buf, "?");
    const uint32_t st = guest_ok(self + 4, 4) ? rd32(self + 4) : 0;
    if (!guest_ok(st, 4)) return "none";
    const uint32_t vp = rd32(st);
    if (!guest_ok(vp - 4, 8)) return buf;
    const uint32_t ti = rd32(vp - 4);
    if (!guest_ok(ti + 4, 4)) return buf;
    const uint32_t name = rd32(ti + 4);
    if (!guest_ok(name, 64)) return buf;
    snprintf(buf, sizeof buf, "%.80s", (const char*)(vm_base + name));
    return buf;
}

// Once a second (under the lock): the counts roll over, the probe's lines, and the free camera lets go when
// the race view it draws for has gone (the race or replay ended).
void second_tick(double now)
{
    if (now - s_sec_t < 1.0) return;
    s_sec_t = now;
    for (Adapter& a : s_ad) {
        if (!a.self) continue;
        if (s_probe && (a.calls[0] || a.calls[1] || a.calls[2] || a.calls[3])) {
            double p[3] = {0, 0, 0}, fy = 0;
            if (a.have_view) eye_of(a.view, p);
            if (a.have_proj) fy = fov_of(a.proj);
            fprintf(stderr, "[freecam-probe] t=%.1f adapter 0x%08X (status 0x%08X %s): proj %u view %u proj-out %u view-out %u;"
                            " eye %.1f %.1f %.1f, back %.2f %.2f %.2f, up %.2f %.2f %.2f, fov %.1f; race pause %d%s\n",
                    now, a.self, guest_ok(a.self + 4, 4) ? rd32(a.self + 4) : 0, status_class(a.self), a.calls[0],
                    a.calls[1], a.calls[2], a.calls[3], p[0], p[1], p[2],
                    a.view[2], a.view[6], a.view[10], a.view[1], a.view[5], a.view[9], fy, cellPad_race_pause_state(),
                    s_state.load() == ON ? (a.self == s_main ? " [free camera]" : a.follower ? " [follows]" : "") : "");
        }
        a.views_last = a.calls[VIEW] + a.calls[VIEW_OUT];
        for (unsigned& c : a.calls) c = 0;
    }
}

// The free camera was asked for: it takes the race view's adapter that the game asked most since then, and
// starts from that adapter's last view and field of view (under the lock).
void arming_check(double now)
{
    Adapter* best = nullptr;
    for (Adapter& a : s_ad)
        if (a.self && a.views_arming && a.have_view && a.have_proj && (!best || a.views_arming > best->views_arming)) best = &a;
    if (best && (best->views_arming >= 3 || now - s_arm_t > 0.25)) {
        if (!pose_from_view(best->view)) {
            fprintf(stderr, "[freecam] the game's view doesn't look like a camera's: not switched on\n");
            s_state.store(OFF);
            return;
        }
        s_main = best->self;
        s_main_seen_us.store((int64_t)(now * 1e6), std::memory_order_relaxed);
        s_fov = s_fov0 = fov_of(best->proj);
        s_last_step = -1.0;
        s_mouse_dx.store(0); s_mouse_dy.store(0); s_wheel.store(0); s_reset.store(0);
        s_state.store(ON);
        fprintf(stderr, "[freecam] on: from %.1f %.1f %.1f, %.1f degrees (adapter 0x%08X)\n", s_px, s_py, s_pz, s_fov, s_main);
    }
}

// ---- the wrappers ---------------------------------------------------------------------------------------

template <int SLOT>
void wrapper(ppu_context* ctx)
{
    const bool by_value = SLOT == PROJ || SLOT == VIEW;
    const uint32_t self = (uint32_t)(by_value ? ctx->gpr[4] : ctx->gpr[3]);
    const uint32_t out = (uint32_t)(by_value ? ctx->gpr[3] : ctx->gpr[5]);
    s_fn[SLOT](ctx);
    while (g_trampoline_fn) {
        void (*tf)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        tf(ctx);
    }
    if (SLOT == VIEW || SLOT == VIEW_OUT) s_race_view_us.store((int64_t)(now_s() * 1e6), std::memory_order_relaxed);
    const int state = s_state.load(std::memory_order_acquire);
    if (state == OFF && !s_probe) return;          // the game's own, untouched
    if (!guest_ok(out, 64)) return;
    const bool view = SLOT == VIEW || SLOT == VIEW_OUT;
    const double now = now_s();
    Lock lock;
    Adapter* a = adapter_of(self);
    a->calls[SLOT]++;
    if (view) {
        rd_mat(out, a->view);
        a->have_view = true;
        a->last_view = now;
        if (self == s_main) s_main_seen_us.store((int64_t)(now * 1e6), std::memory_order_relaxed);
        if (state == ARMING) a->views_arming++;
    } else {
        rd_mat(out, a->proj);
        a->have_proj = true;
    }
    second_tick(now);
    if (state == ARMING) {
        arming_check(now);
        if (s_state.load() != ON) {
            if (now - s_arm_t > kStaleS) {
                s_state.store(OFF);
                fprintf(stderr, "[freecam] no race view to fly in: not switched on\n");
            }
            return;
        }
    }
    if (s_state.load() != ON) return;
    if (self != s_main) {
        // The view it drew for went away (back to the menus): it lets go once that has lasted a second.
        Adapter* m = nullptr;
        for (Adapter& x : s_ad) if (x.self == s_main) m = &x;
        if (!m || now - m->last_view > kStaleS) {
            s_state.store(OFF);
            fprintf(stderr, "[freecam] off: the race view ended\n");
            return;
        }
        // GT5 asks two adapters for each frame's race view (both with the race camera's status): every one
        // whose own view is the same camera as the race view's is flown too, or part of the scene would
        // stay behind. A view of another camera (a mirror, a second player) is left alone.
        if (view) {
            const bool was = a->follower;
            a->follower = same_camera(a->view, m->view);
            if (a->follower != was && s_follow_said++ < 8)
                fprintf(stderr, "[freecam] adapter 0x%08X %s the race view\n", self, a->follower ? "follows" : "no longer follows");
        }
        if (!a->follower) return;
    }
    float mat[16];
    if (view) {
        // The scene asks for the view many times a frame (each pass, the culling): one step a frame, so all
        // of a frame's passes see the same camera.
        if (s_last_step < 0.0 || now - s_last_step >= kStepS) step(now);
        view_from_pose(mat);
    } else {
        memcpy(mat, a->proj, sizeof mat);
        // The field of view: both scales by the same factor, so the picture's shape stays the game's.
        const double y = fabs((double)mat[5]);
        if (y > 1e-6) {
            const double want = 1.0 / tan(s_fov * kPi / 360.0), k = want / y;
            mat[0] = (float)(mat[0] * k);
            mat[5] = (float)(mat[5] * k);
        }
    }
    wr_mat(out, mat);
}

void (*const kWrappers[NSLOT])(ppu_context*) = {wrapper<PROJ>, wrapper<VIEW>, wrapper<PROJ_OUT>, wrapper<VIEW_OUT>};

void freecam_loaded()
{
    const uint32_t vt = find_vtable();
    if (!vt) { fprintf(stderr, "[freecam] the race camera isn't in this executable: no free camera\n"); return; }
    for (int i = 0; i < NSLOT; i++) {
        s_opd[i] = rd32(vt + 8 + 4 * kSlotIndex[i]);
        s_code[i] = (s_opd[i] >= kImageLo && s_opd[i] < kImageHi && !(s_opd[i] & 3)) ? rd32(s_opd[i]) : 0;
        const bool proj = i == PROJ || i == PROJ_OUT;
        if (!s_code[i] || s_code[i] < kImageLo || s_code[i] >= kImageHi || (s_code[i] & 3) ||
            !(proj ? calls_are(s_code[i], kProjCalls, 5) : calls_are(s_code[i], kViewCalls, 2))) {
            fprintf(stderr, "[freecam] the race camera's %s (0x%08X) isn't what it should be: no free camera\n",
                    kSlotName[i], s_code[i]);
            return;
        }
        for (uint64_t k = 0; k < function_table_count; k++)
            if (function_table[k].addr == s_code[i]) s_fn[i] = function_table[k].func;
        if (!s_fn[i]) {
            fprintf(stderr, "[freecam] the race camera's %s (0x%08X) isn't in the lifted code: no free camera\n",
                    kSlotName[i], s_code[i]);
            return;
        }
    }
    for (int i = 0; i < NSLOT; i++) ppu_register_function(s_opd[i], kWrappers[i]);
    s_found = true;
    const char* e = getenv("GT5_FREECAM_PROBE");
    s_probe = e && *e && *e != '0';
    // [raceview] When the race view is drawn is what tells a race from GT5's menus for the controller's trigger
    // layout (gt5_padmap.cpp): the wrappers go in now, and only note the time until the free camera is on.
    e = getenv("PAD_RACE_VIEW");
    s_race_view_hook = !(e && e[0] == '0');
    if (s_probe || s_race_view_hook) wrap();
    fprintf(stderr, "[freecam] free camera ready (race camera 0x%08X: view 0x%08X, projection 0x%08X)%s\n", vt,
            s_code[VIEW], s_code[PROJ], s_probe ? "; probe on" : "");
}

const int s_registered = (ps3_on_loaded(freecam_loaded), 0);

}  // namespace

// ---- for the menu's window code (gt5_menu.cpp); any thread ----------------------------------------------

// 1 on (or about to be: it starts at the next picture), 0 off. Also where it lets go when no race view has
// been asked for in a while (asked for away from a race, or the race ended with nothing else drawn since).
extern "C" int gt5_freecam_on(void)
{
    const int st = s_state.load();
    if (st == OFF) return 0;
    const double now = now_s();
    if (st == ARMING && now - s_arm_t > kStaleS + 0.5) {
        if (s_state.exchange(OFF) == ARMING) fprintf(stderr, "[freecam] no race view to fly in: not switched on\n");
        return 0;
    }
    if (st == ON && now - (double)s_main_seen_us.load(std::memory_order_relaxed) * 1e-6 > kStaleS + 0.5) {
        if (s_state.exchange(OFF) == ON) fprintf(stderr, "[freecam] off: the race view ended\n");
        return 0;
    }
    return 1;
}

// [raceview] Milliseconds since GT5 last asked for a race view (a race, a replay, the 0-400 m mode, their pause
// and the screen before a race start: everything drawn with the race camera); a large number when it hasn't
// since the start; -1 when that isn't known (no race camera found, or PAD_RACE_VIEW=0). Any thread.
extern "C" long long gt5_race_view_age_ms(void)
{
    if (!s_found || !s_race_view_hook) return -1;
    const int64_t t = s_race_view_us.load(std::memory_order_relaxed);
    if (!t) return 1LL << 40;
    const long long ms = (long long)((now_s() * 1e6 - (double)t) / 1000.0);
    return ms < 0 ? 0 : ms;
}

// Switch on or off. Returns 1 on, 0 off, -1 there is no free camera in this executable.
extern "C" int gt5_freecam_set(int on)
{
    if (!s_found) {
        if (on && !s_said_missing++) fprintf(stderr, "[freecam] asked for, but there is none in this executable\n");
        return on ? -1 : 0;
    }
    if (!on) {
        if (s_state.exchange(OFF) != OFF) fprintf(stderr, "[freecam] off\n");
        return 0;
    }
    if (s_state.load() != OFF) return 1;
    {
        Lock lock;
        for (Adapter& a : s_ad) { a.views_arming = 0; a.follower = false; }
        s_arm_t = now_s();
        s_state.store(ARMING);
    }
    wrap();
    return 1;
}

extern "C" int gt5_freecam_toggle(void) { return gt5_freecam_set(!gt5_freecam_on()); }

// 0 off, 1 starting (it takes the game's camera at the next picture of a race view), 2 on.
extern "C" int gt5_freecam_state(void) { return gt5_freecam_on() ? (s_state.load() == ON ? 2 : 1) : 0; }

// There is a free camera in this executable (the menu shows its rows only then).
extern "C" int gt5_freecam_found(void) { return s_found ? 1 : 0; }

// Held keys: 1 forward, 2 back, 4 left, 8 right, 16 up, 32 down, 64 faster, 128 slower.
extern "C" void gt5_freecam_keys(unsigned keys) { s_keys.store(keys, std::memory_order_relaxed); }
// Mouse movement in pixels (right, down), mouse wheel notches (away from the player = zoom in).
extern "C" void gt5_freecam_mouse(int dx, int dy) { s_mouse_dx.fetch_add(dx); s_mouse_dy.fetch_add(dy); }
extern "C" void gt5_freecam_wheel(int notches) { s_wheel.fetch_add(notches); }
// The field of view back to the game's, the horizon level.
extern "C" void gt5_freecam_reset(void) { s_reset.store(1); }
// The controller, as the menu's feed has it (CELL_PAD_CTRL_* buttons, sticks 0..255 with 128 at rest).
extern "C" void gt5_freecam_pad(int connected, unsigned buttons, int lx, int ly, int rx, int ry, int l2, int r2)
{
    const uint64_t v = (connected ? 1ull : 0ull) | (uint64_t)(buttons & 0xFFFF) << 8 | (uint64_t)(lx & 0xFF) << 24 |
                       (uint64_t)(ly & 0xFF) << 32 | (uint64_t)(rx & 0xFF) << 40 | (uint64_t)(ry & 0xFF) << 48;
    s_pad.store(v, std::memory_order_relaxed);
    s_pad_trig.store((unsigned)(l2 & 0xFF) | (unsigned)(r2 & 0xFF) << 8, std::memory_order_relaxed);
}
