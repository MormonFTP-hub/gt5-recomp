/* Native PPU functions: the run-time half (scripts/ppu_native_patch.py, src/native/ppu_natives.txt).
 *
 * Included at the end of each lifted PPU source the build copies with native functions in it (after the
 * lifted code, so with its ppu_recomp.h), and by ppu_native.cpp.
 *   GT5_PPU_NATIVE=1 (default)  the native functions
 *   GT5_PPU_NATIVE=0            the lifted ones, as before
 *   GT5_PPU_NATIVE=2            compare: on every call the native's compare form runs on a copy of the
 *                               registers with its guest stores held in a log, then the lifted function
 *                               runs for real; registers and stored bytes are compared and the first
 *                               differences are logged ("[ppu-native] differ ...")
 * Every 10 s while calls are made: "[ppu-native] ..." with the counts. */
#pragma once
#include "ppu_recomp.h"
#include <stdint.h>
#include <string.h>

#define GT5_PPU_NAT_MAX 256

extern "C" int g_gt5_ppu_native;

/* At start-up, once per native function: its index for the counters. */
int gt5_ppu_native_register(const char* addr, int no_compare_form);

/* Per-thread counters (summed by the log line; no lock and no shared line on the call path). */
struct gt5_ppu_nat_tls {
    uint64_t calls;
    uint64_t native[GT5_PPU_NAT_MAX];
    uint64_t lifted[GT5_PPU_NAT_MAX];
    uint64_t compared[GT5_PPU_NAT_MAX];
    uint64_t differed[GT5_PPU_NAT_MAX];
    uint64_t skipped[GT5_PPU_NAT_MAX];    /* compare mode, not compared: see gt5_ppu_native_compare */
    uint64_t nanquiet[GT5_PPU_NAT_MAX];   /* compared, the same but for a signaling NaN's quiet bit */
};
extern PPU_THREAD_LOCAL gt5_ppu_nat_tls* g_gt5_ppu_nat_t;
gt5_ppu_nat_tls* gt5_ppu_nat_tls_new(void);
void gt5_ppu_nat_tick(void);

static inline void gt5_ppu_nat_count(int k)
{
    gt5_ppu_nat_tls* t = g_gt5_ppu_nat_t;
    if (__builtin_expect(t == nullptr, 0)) t = gt5_ppu_nat_tls_new();
    t->native[k]++;
    if (__builtin_expect((++t->calls & 0xFFFFFu) == 0, 0)) gt5_ppu_nat_tick();
}
void gt5_ppu_nat_lifted(int k);

/* Compare mode. The compare form of a native is its text with each guest access sent here:
 * loads see the bytes it stored before (then guest memory), stores go to the log only. */
struct gt5_ppu_shadow;
typedef void (*gt5_ppu_shadow_fn)(ppu_context*, gt5_ppu_shadow*);
void gt5_ppu_native_compare(ppu_context* ctx, gt5_ppu_shadow_fn shadow, void (*lifted)(ppu_context*), int k);

/* raw: the lifted code's raw access (no range check: vector and byte-reversed forms); otherwise the
 * gated scalar path, and an address that path sends out of line (null page, raw SPU windows, the gcm
 * poll word) makes the call not compared. */
void gt5_sh_read(gt5_ppu_shadow* s, uint32_t ea, void* dst, uint32_t n, int raw);
void gt5_sh_write(gt5_ppu_shadow* s, uint32_t ea, const void* src, uint32_t n, int raw);
const uint8_t* gt5_sh_p16(gt5_ppu_shadow* s, uint32_t ea);
static inline void gt5_sh_zero(gt5_ppu_shadow* s, uint32_t ea, uint32_t n)     /* dcbz (n = 128) */
{
    static const uint8_t z[256] = {0};
    gt5_sh_write(s, ea, z, n <= 256 ? n : 256, 1);
}

static inline uint8_t gt5_sh_r8(gt5_ppu_shadow* s, uint64_t a) { uint8_t v; gt5_sh_read(s, (uint32_t)a, &v, 1, 0); return v; }
static inline uint16_t gt5_sh_r16(gt5_ppu_shadow* s, uint64_t a) { uint16_t v; gt5_sh_read(s, (uint32_t)a, &v, 2, 0); return __builtin_bswap16(v); }
static inline uint32_t gt5_sh_r32(gt5_ppu_shadow* s, uint64_t a) { uint32_t v; gt5_sh_read(s, (uint32_t)a, &v, 4, 0); return __builtin_bswap32(v); }
static inline uint64_t gt5_sh_r64(gt5_ppu_shadow* s, uint64_t a) { uint64_t v; gt5_sh_read(s, (uint32_t)a, &v, 8, 0); return __builtin_bswap64(v); }
static inline void gt5_sh_w8(gt5_ppu_shadow* s, uint64_t a, uint8_t v) { gt5_sh_write(s, (uint32_t)a, &v, 1, 0); }
static inline void gt5_sh_w16(gt5_ppu_shadow* s, uint64_t a, uint16_t v) { v = __builtin_bswap16(v); gt5_sh_write(s, (uint32_t)a, &v, 2, 0); }
static inline void gt5_sh_w32(gt5_ppu_shadow* s, uint64_t a, uint32_t v) { v = __builtin_bswap32(v); gt5_sh_write(s, (uint32_t)a, &v, 4, 0); }
static inline void gt5_sh_w64(gt5_ppu_shadow* s, uint64_t a, uint64_t v) { v = __builtin_bswap64(v); gt5_sh_write(s, (uint32_t)a, &v, 8, 0); }
static inline void gt5_sh_ld16(gt5_ppu_shadow* s, void* dst, uint64_t ea) { gt5_sh_read(s, (uint32_t)ea, dst, 16, 1); }
static inline void gt5_sh_st16(gt5_ppu_shadow* s, uint64_t ea, const void* src) { gt5_sh_write(s, (uint32_t)ea, src, 16, 1); }
/* The generated natives' VMX values in host order (gen_ppu_leaf.py's vector pass): each word as
 * ppu_vmx.h's vmx_ld4 reads it; the operations are ppu_vmx.h's own four-lane ones. */
#if defined(VMX_SIMD_FMA)
static inline __m128 gt5_nat_h(const void* r) { return vmx_ld4(r); }
static inline void gt5_nat_r(void* r, __m128 h) { vmx_st4(r, h); }
static inline __m128 gt5_nat_fl(__m128 v, int nj) { return nj ? vmx_flush4(v) : v; }
static inline __m128i gt5_nat_bsw(__m128 v)
{
    return _mm_shuffle_epi8(_mm_castps_si128(v), _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}
static inline __m128 gt5_nat_unbsw(__m128i v)
{
    return _mm_castsi128_ps(_mm_shuffle_epi8(v, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
}
static inline __m128 gt5_nat_vperm(__m128 a, __m128 b, __m128 c)          /* vmx_vperm */
{
    return gt5_nat_unbsw(vmx_perm16(gt5_nat_bsw(a), gt5_nat_bsw(b), gt5_nat_bsw(c)));
}
static inline __m128 gt5_nat_vsldoi(__m128 a, __m128 b, unsigned sh)        /* vmx_vsldoi */
{
    const __m128i sel = _mm_add_epi8(vmx_iota16(), _mm_set1_epi8((char)(sh & 15u)));
    return gt5_nat_unbsw(vmx_perm16(gt5_nat_bsw(a), gt5_nat_bsw(b), sel));
}
#endif

template <class T> static inline T gt5_sh_rawld(gt5_ppu_shadow* s, uint64_t ea) { T v; gt5_sh_read(s, (uint32_t)ea, &v, sizeof(T), 1); return v; }
template <class T, class V> static inline void gt5_sh_rawst(gt5_ppu_shadow* s, uint64_t ea, V val) { T v = (T)val; gt5_sh_write(s, (uint32_t)ea, &v, sizeof(T), 1); }
