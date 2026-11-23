/*
 * native_cad00.c -- w018CAD00 (GT5's per-batch geometry job, "GPUSyncArray") as native code: n_cad00.
 *
 * What runs natively here: the job's routine 0xF6C0, the batch cull for batches with mirror records (opcode
 * 44 of the job's program, through 0x7C98 / 0x7B10; about one call a job, 18 % of the job's own time on the
 * job captures of a race). The routine is one call, run from its lifted statements (cut from this build's own
 * lift by gen_cad00_f6c0.py into cad00_f6c0_lift.inc, natives.txt's generate line) with its three hot loops
 * (85 % of its time) taken natively:
 *   0xF970  the vertex transform: batch matrix, 1/w (frest, fi, one Newton step), the screen position
 *   0xFA78  the bounding box of the transformed vertices (min and max, lane by lane)
 *   0xFD60  per record (a quad on screen: four corners and a plane) and per vertex: inside the four edges, on
 *           the plane's side and z >= 0? Then the record's bit goes into the vertex's fourth word.
 * Natively eight vertices a step in AVX2 with the SPU's single-precision arithmetic exactly (round toward zero,
 * denormals as zero, a zero result +0, the same operations in the same order; frest / fi the helpers' integer
 * rules; any value the x86 operations do not reproduce -- exponent 255, an overflow -- and the lifted loop runs
 * instead). The SPU loops are software-pipelined (a vertex group is loaded, worked on and stored over several
 * passes; the first passes store leftovers that later passes overwrite) and some keep running values across
 * passes (ORs, min / max). So: every vertex group natively and the running values exactly; then the loop's
 * LAST passes run as the lifted loop itself, entered with its counters (and running values) as they are there,
 * which leaves every register exactly as the whole lifted loop does; the vertex groups its first of those
 * passes store leftovers into are put back after it, and the ORs set to their exact values. Under six vertex
 * groups (three for the box): the lifted loop. Its fourth loop (0xFF00, the kept triangles, ~20 %) and the
 * rest stay the lifted statements.
 * Speed (bin\ftest, the 200 real cull_F6C0 entry states of cull1): the routine 12.5 -> 6.0 us a call.
 *
 * Exactness: the routine's local-store result (records and vertices transformed, the bounding box, the vertex
 * bits, the kept triangles, the count) and every register equal the lifted routine's: jharn ftest on the 200
 * real entry states and 2,000,000 mutated ones (zeros of both signs, denormals, FLT_MAX, exponent 255, NaN,
 * smaller counts, random bits: both native and handed-back paths); the 24 job captures of a race replayed
 * (every channel operation, DMA and PUT byte, all registers and local store at the job's end); cull1's 600 job
 * tails (bin\ctest); in the game GT5_CAD00_NATIVE=2.
 *
 * GT5_CAD00_NATIVE (read once): 0 the lifted routine; 1 native (the default); 2 both, compared: the native
 * on the side first, then the lifted function from the same state (its result stays); local store and all
 * registers compared, "[cad00] MISMATCH ..." for the first ones. GT5_CAD00_STAT=1 (or mode 2): "[cad00] ..."
 * every 5 s: calls, native, handed back and why, per loop, compared / mismatches. Always, once: "[cad00] 0xF6C0
 * runs as native code" at the first native call.
 * The lifted routine runs instead when: GT5_CAD00_NATIVE=0, an interrupt is pending, an LS diagnostic is on,
 * MXCSR is not the SPU mode (round toward zero, FTZ, DAZ), the image's code at 0xF6C0-0x10060 or its shuffle
 * constants are not the ones this was written for. Per loop the lifted loop runs when: too few vertex groups,
 * the loop's registers are not the shape expected, a value out of the exact range, inputs overlapping outputs.
 */
#include "spu_recomp.h"
#include "spu_helpers.h"
#include "spu_context.h"
#include "native_geo.h"
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <windows.h>
#endif

#ifdef NATIVE_HOOKED
SPU_SV void gt5_w018CAD00_spu_func_0000F6C0_orig_sv(spu_context* ctx);
#  define CAD_LIFTED_F6C0 gt5_w018CAD00_spu_func_0000F6C0_orig_sv
#else
SPU_SV void gt5_w018CAD00_spu_func_0000F6C0_sv(spu_context* ctx);
#  define CAD_LIFTED_F6C0 gt5_w018CAD00_spu_func_0000F6C0_sv
#endif

#define CAD_LSM (SPU_LS_SIZE - 1u)
#define CAD_LIKELY(x)   __builtin_expect(!!(x), 1)
#define CAD_UNLIKELY(x) __builtin_expect(!!(x), 0)

/* ---- switch and counters ------------------------------------------------------------------------- */
int g_cad_mode = -1;
static int g_cad_stat;                 /* GT5_CAD00_STAT=1 or mode 2: the line every 5 s */
enum {
    CAD_CALLS, CAD_NATIVE, CAD_L_MODE, CAD_L_GUARD, CAD_L_IMAGE,
    CAD_REC, CAD_REC_NATIVE, CAD_REC_SMALL, CAD_REC_SHAPE, CAD_REC_RANGE, CAD_VERTS,
    CAD_L2, CAD_L2_NATIVE, CAD_L2_SMALL, CAD_L2_SHAPE, CAD_L2_RANGE, CAD_L2_VERTS,
    CAD_L3, CAD_L3_NATIVE, CAD_L3_SMALL, CAD_L3_SHAPE, CAD_L3_RANGE,
    CAD_CMP, CAD_CMP_BAD, CAD_N
};
static volatile long long g_cad[CAD_N];
static SPU_THREAD_LOCAL long long t_cad[CAD_N];
static SPU_THREAD_LOCAL unsigned t_cad_n;
#define CAD_ADD(k, n) (t_cad[k] += (n))

static __attribute__((noinline)) int cad_init(void)
{
    static volatile long said;
    const char* e = getenv("GT5_CAD00_NATIVE");
    int m = 1;
    if (e && (e[0] == '0' || e[0] == '1' || e[0] == '2') && e[1] == 0) m = e[0] - '0';
    const char* ls = getenv("SPU_LOCKSTEP");
    if (m && ls && *ls && *ls != '0') m = 0;
    const char* st = getenv("GT5_CAD00_STAT");
    g_cad_stat = (st && st[0] == '1') || m == 2;
    if (m != 1 && __atomic_exchange_n(&said, 1, __ATOMIC_SEQ_CST) == 0) {
        fprintf(stderr, "[cad00] GT5_CAD00_NATIVE=%d%s\n", m, m == 2 ? " (native and lifted, compared)" : m ? "" : " (lifted)");
        fflush(stderr);
    }
    g_cad_mode = m;
    return m;
}
static inline int cad_mode(void) { const int m = g_cad_mode; return CAD_LIKELY(m >= 0) ? m : cad_init(); }

void cad_report(FILE* f)
{
    long long v[CAD_N];
    for (int k = 0; k < CAD_N; k++) v[k] = __atomic_load_n(&g_cad[k], __ATOMIC_RELAXED) + t_cad[k];
    fprintf(f, "[cad00] 0xF6C0 calls %lld: native %lld, handed back %lld (GT5_CAD00_NATIVE=0 %lld, interrupt/diagnostic/MXCSR %lld, "
               "other code %lld) | transform %lld: native %lld, lifted %lld (small %lld, shape %lld, range %lld), vertices %lld | "
               "box %lld: native %lld, lifted %lld (small %lld, shape %lld, range %lld) | records %lld: native %lld, lifted %lld "
               "(small %lld, shape %lld, range %lld), vertices %lld | compared %lld, MISMATCH %lld\n",
            v[CAD_CALLS], v[CAD_NATIVE], v[CAD_L_MODE] + v[CAD_L_GUARD] + v[CAD_L_IMAGE], v[CAD_L_MODE], v[CAD_L_GUARD], v[CAD_L_IMAGE],
            v[CAD_L2], v[CAD_L2_NATIVE], v[CAD_L2_SMALL] + v[CAD_L2_SHAPE] + v[CAD_L2_RANGE], v[CAD_L2_SMALL], v[CAD_L2_SHAPE],
            v[CAD_L2_RANGE], v[CAD_L2_VERTS],
            v[CAD_L3], v[CAD_L3_NATIVE], v[CAD_L3_SMALL] + v[CAD_L3_SHAPE] + v[CAD_L3_RANGE], v[CAD_L3_SMALL], v[CAD_L3_SHAPE], v[CAD_L3_RANGE],
            v[CAD_REC], v[CAD_REC_NATIVE], v[CAD_REC_SMALL] + v[CAD_REC_SHAPE] + v[CAD_REC_RANGE], v[CAD_REC_SMALL], v[CAD_REC_SHAPE],
            v[CAD_REC_RANGE], v[CAD_VERTS], v[CAD_CMP], v[CAD_CMP_BAD]);
    fflush(f);
}
void cad_reset(void)
{
    for (int k = 0; k < CAD_N; k++) { __atomic_store_n(&g_cad[k], 0, __ATOMIC_RELAXED); t_cad[k] = 0; }
}
/* every 64 calls of a thread: its counts into the totals; the line every 5 s */
static __attribute__((noinline)) void cad_flush(void)
{
    for (int k = 0; k < CAD_N; k++) if (t_cad[k]) { __atomic_fetch_add(&g_cad[k], t_cad[k], __ATOMIC_RELAXED); t_cad[k] = 0; }
    t_cad_n = 0;
#ifdef _WIN32
    static volatile LONG64 next;
    const LONG64 now = (LONG64)GetTickCount64();
    LONG64 t = next;
    if (!t) { InterlockedCompareExchange64(&next, now + 5000, 0); return; }
    if (now < t || InterlockedCompareExchange64(&next, now + 5000, t) != t) return;
    if (g_cad_stat) cad_report(stderr);
#endif
}

/* ---- the image this was written for ---------------------------------------------------------------- */
static uint64_t cad_fnv(const uint8_t* p, uint32_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static volatile int g_cad_img;          /* 0 not checked, 1 the same, -1 other code */
static __attribute__((noinline)) int cad_image_check(const uint8_t* ls)
{
    static const uint32_t k[] = { 0x11380, 0x114D0, 0x112F0, 0x11670, 0x11520, 0x114A0, 0x11650 };
    uint8_t c[16 * 7];
    for (int i = 0; i < 7; i++) memcpy(c + 16 * i, ls + k[i], 16);
    const int ok = cad_fnv(ls + 0xF6C0, 0x10060 - 0xF6C0) == 0xf1039fd0882d30b9ull && cad_fnv(c, sizeof c) == 0xc3a4f901d718ca35ull;
    if (!ok) {
        static volatile long said;
        if (__atomic_exchange_n(&said, 1, __ATOMIC_SEQ_CST) == 0) {
            fprintf(stderr, "[cad00] the job's code at 0xF6C0 is not the code this was written for: lifted\n"); fflush(stderr);
        }
    }
    g_cad_img = ok ? 1 : -1;
    return ok;
}
static inline int cad_image_ok(const uint8_t* ls) { const int v = g_cad_img; return CAD_LIKELY(v > 0) || (v == 0 && cad_image_check(ls)); }

/* ---- the vertex-in-record loop 0xFD60 ---------------------------------------------------------------- */
typedef struct {
    u128 r2, r8, r10, r14, r20, r21, r22, r30, r36, r41, r43, r45, r49, r50, r51, r52, r54, r62, r63, r64;
    u128 c0x, c0y, c0z, c1x, c1y, c2x, c2y, c3x, c3y;          /* r23 r31 r44 r24 r29 r6 r27 r25 r28 */
    u128 e01x, e01y, e12x, e12y, e23x, e23y, e30x, e30y;       /* r39 r32 r38 r34 r37 r35 r40 r33 */
    u128 nx, ny, nz, nzs;                                      /* r55 r57 r42 r58 */
    u128 X, Y, Z;                                              /* r46 r53 r48: the group the pipeline starts with */
} cad_l4_in;
typedef struct {
    int on;                       /* the loop's last six passes are running as the lifted loop */
    uint32_t keep_lsa;            /* three vertex groups to put back after them */
    uint8_t keep[192];
    u128 r62, r8;                 /* the two ORs, exact */
} cad_l4_state;
static SPU_THREAD_LOCAL cad_l4_state t_l4;

static inline u128 cad_u(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { u128 r; r._u32[0] = a; r._u32[1] = b; r._u32[2] = c; r._u32[3] = d; return r; }
static inline int cad_eq(u128 a, u128 b) { return memcmp(&a, &b, 16) == 0; }
static inline int cad_splat(u128 a) { return a._u32[0] == a._u32[1] && a._u32[0] == a._u32[2] && a._u32[0] == a._u32[3]; }
static inline __m256 cad_b(u128 a) { const __m128 v = _mm_loadu_ps((const float*)a._u32); return _mm256_set_m128(v, v); }   /* the four lanes, twice */
static inline __m256i cad_bi(u128 a) { const __m128i v = _mm_loadu_si128((const __m128i*)a._u32); return _mm256_set_m128i(v, v); }

/* SPU single precision under the SPU's MXCSR mode (round toward zero, FTZ, DAZ): the host operation gives the
 * SPU's result when it is in the host's range, except that a zero result may be -0 where the SPU's is +0. A
 * -0 operand changes another result only in the sign of a zero (it is the same real number), so the zero rule
 * is applied only to results that are stored or whose sign is read (CAD_FIN), as the helpers' range rule is to
 * every result: *bad keeps the largest magnitude seen (as bits); one at or above FLT_MAX (in round toward
 * zero the host saturates where the SPU's extended range goes on), Inf or NaN: the lifted code runs instead. */
static inline __m256 cad_rng(__m256 r, __m256i* bad)
{
    *bad = _mm256_max_epu32(*bad, _mm256_and_si256(_mm256_castps_si256(r), _mm256_set1_epi32(0x7FFFFFFF)));
    return r;
}
static inline __m256 cad_fin(__m256 r)               /* a zero is +0 */
{
    const __m256i ri = _mm256_castps_si256(r), sg = _mm256_set1_epi32((int)0x80000000u);
    return _mm256_castsi256_ps(_mm256_andnot_si256(_mm256_and_si256(_mm256_cmpeq_epi32(ri, sg), sg), ri));
}
static inline int cad_bad(__m256i bad) { const __m256i b = _mm256_cmpgt_epi32(bad, _mm256_set1_epi32(0x7F7FFFFE)); return !_mm256_testz_si256(b, b); }
#define CAD_FS(a, b)     cad_rng(_mm256_sub_ps((a), (b)), &bad)
#define CAD_FM(a, b)     cad_rng(_mm256_mul_ps((a), (b)), &bad)
#define CAD_FMA(a, b, c) cad_rng(_mm256_fmadd_ps((a), (b), (c)), &bad)      /* a*b + c */
#define CAD_FNMS(a, b, c) cad_rng(_mm256_fnmadd_ps((a), (b), (c)), &bad)    /* c - a*b */

typedef struct {
    __m256 c0x, c0y, c0z, c1x, c1y, c2x, c2y, c3x, c3y, e01x, e01y, e12x, e12y, e23x, e23y, e30x, e30y, nx, ny, nz;
    __m256i nzs, bit;
} cad_l4_k;
/* the bit of each lane (the record's bit where the vertex is inside, else 0), as 0xFD60's r22 */
static inline __m256i cad_l4_flags(const cad_l4_k* k, __m256 X, __m256 Y, __m256 Z, __m256i* bad_out)
{
    __m256i bad = *bad_out;
    const __m256 dx0 = CAD_FS(X, k->c0x), dy0 = CAD_FS(Y, k->c0y);
    const __m256 e0 = cad_fin(CAD_FNMS(dy0, k->e01x, CAD_FM(dx0, k->e01y)));
    const __m256 e1 = cad_fin(CAD_FNMS(CAD_FS(Y, k->c1y), k->e12x, CAD_FM(CAD_FS(X, k->c1x), k->e12y)));
    const __m256 e2 = cad_fin(CAD_FNMS(CAD_FS(Y, k->c2y), k->e23x, CAD_FM(CAD_FS(X, k->c2x), k->e23y)));
    const __m256 e3 = cad_fin(CAD_FNMS(CAD_FS(Y, k->c3y), k->e30x, CAD_FM(CAD_FS(X, k->c3x), k->e30y)));
    const __m256 pl = cad_fin(CAD_FMA(CAD_FS(Z, k->c0z), k->nz, CAD_FMA(k->ny, dy0, CAD_FM(dx0, k->nx))));
    const __m256i i0 = _mm256_castps_si256(e0), i1 = _mm256_castps_si256(e1), i2 = _mm256_castps_si256(e2), i3 = _mm256_castps_si256(e3);
    __m256i t = _mm256_andnot_si256(_mm256_xor_si256(i0, i1), _mm256_andnot_si256(_mm256_xor_si256(i2, i1), _mm256_set1_epi32(-1)));
    t = _mm256_andnot_si256(_mm256_xor_si256(i3, i2), t);
    t = _mm256_andnot_si256(_mm256_xor_si256(_mm256_castps_si256(pl), k->nzs), t);
    t = _mm256_andnot_si256(_mm256_castps_si256(Z), t);
    *bad_out = bad;
    return _mm256_and_si256(_mm256_srai_epi32(t, 31), k->bit);
}
/* the 16-byte big-endian vertex lines at p (a group of four) and p + d -> X, Y, Z (host order, eight lanes) */
static inline void cad_l4_load(const uint8_t* p, uint32_t d, __m256* X, __m256* Y, __m256* Z)
{
    const __m256i bsw = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const __m256i r0 = _mm256_shuffle_epi8(_mm256_setr_m128i(_mm_loadu_si128((const __m128i*)(p + 0)), _mm_loadu_si128((const __m128i*)(p + d + 0))), bsw);
    const __m256i r1 = _mm256_shuffle_epi8(_mm256_setr_m128i(_mm_loadu_si128((const __m128i*)(p + 16)), _mm_loadu_si128((const __m128i*)(p + d + 16))), bsw);
    const __m256i r2 = _mm256_shuffle_epi8(_mm256_setr_m128i(_mm_loadu_si128((const __m128i*)(p + 32)), _mm_loadu_si128((const __m128i*)(p + d + 32))), bsw);
    const __m256i r3 = _mm256_shuffle_epi8(_mm256_setr_m128i(_mm_loadu_si128((const __m128i*)(p + 48)), _mm_loadu_si128((const __m128i*)(p + d + 48))), bsw);
    const __m256i t0 = _mm256_unpacklo_epi32(r0, r1), t1 = _mm256_unpackhi_epi32(r0, r1);
    const __m256i t2 = _mm256_unpacklo_epi32(r2, r3), t3 = _mm256_unpackhi_epi32(r2, r3);
    *X = _mm256_castsi256_ps(_mm256_unpacklo_epi64(t0, t2)); *Y = _mm256_castsi256_ps(_mm256_unpackhi_epi64(t0, t2));
    *Z = _mm256_castsi256_ps(_mm256_unpacklo_epi64(t1, t3));
}
static inline uint32_t cad_or8(__m256i v)          /* OR of the four lanes of each half, both halves */
{
    __m128i x = _mm_or_si128(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
    x = _mm_or_si128(x, _mm_shuffle_epi32(x, 0x4E));
    x = _mm_or_si128(x, _mm_shuffle_epi32(x, 0xB1));
    return (uint32_t)_mm_cvtsi128_si32(x);
}
static inline uint32_t cad_or4(u128 a) { return a._u32[0] | a._u32[1] | a._u32[2] | a._u32[3]; }
static inline u128 cad_orx_w(uint32_t w) { return cad_u(w, 0, 0, 0); }

/* 0xFD60 for one record: 1 = done natively up to its last six passes (r21 r45 r50 set for them, t_l4 armed),
 * 0 = nothing changed (the lifted loop runs). */
static int cad_l4_run(spu_context* ctx, const cad_l4_in* in, u128* r21o, u128* r45o, u128* r50o)
{
    uint8_t* const ls = ctx->ls;
    CAD_ADD(CAD_REC, 1);
    /* the shape: r21 = -16 * vertices (rounded to 4) in every word, the store FIFO r45 primed with it, the vertex
     * lines at r41 + r21 + 64 g (r43 r49 r51 the next three lines), the shuffle controls as the routine sets them */
    const uint32_t R0 = in->r21._u32[0];
    const int32_t r0s = (int32_t)R0;
    if (!cad_splat(in->r21) || !cad_eq(in->r45, in->r21) || r0s >= 0 || (R0 & 63u) ||
        in->r43._u32[0] != in->r41._u32[0] + 16u || in->r49._u32[0] != in->r41._u32[0] + 32u || in->r51._u32[0] != in->r41._u32[0] + 48u ||
        !cad_eq(in->r10, cad_u(0x04050607u, 0x14151617u, 0x00010203u, 0x10111213u)) ||
        !cad_eq(in->r14, cad_u(0x0C0D0E0Fu, 0x1C1D1E1Fu, 0x08090A0Bu, 0x18191A1Bu)) ||
        !cad_eq(in->r20, cad_u(0x08090A0Bu, 0x18191A1Bu, 0x0C0D0E0Fu, 0x1C1D1E1Fu)) ||
        !cad_eq(in->r30, cad_u(0x18191A1Bu, 0x1C1D1E1Fu, 0x00010203u, 0x04050607u)) ||
        !cad_eq(in->r36, cad_u(0x00010203u, 0x04050607u, 0x18191A1Bu, 0x1C1D1E1Fu)) ||
        !cad_eq(in->r54, cad_u(0x04050607u, 0x08090A0Bu, 0x0C0D0E0Fu, 0x10111213u))) {
        CAD_ADD(CAD_REC_SHAPE, 1);
#ifdef CAD_HARNESS
        if (getenv("CAD_DEBUG")) fprintf(stderr, "[cad00] shape: r21 %08X %08X r45 %08X %08X r41 %08X r43 %08X r10 %08X r14 %08X r20 %08X r30 %08X r36 %08X r54 %08X\n",
                                         in->r21._u32[0], in->r21._u32[3], in->r45._u32[0], in->r45._u32[3], in->r41._u32[0], in->r43._u32[0],
                                         in->r10._u32[0], in->r14._u32[0], in->r20._u32[0], in->r30._u32[0], in->r36._u32[0], in->r54._u32[0]);
#endif
        return 0;
    }
    const uint32_t G = (uint32_t)(-r0s) / 64u;
    const uint32_t base = in->r41._u32[0] + R0;
    if (G < 6u) { CAD_ADD(CAD_REC_SMALL, 1); return 0; }
    if ((base & 15u) || base > SPU_LS_SIZE || (uint64_t)base + 64ull * (G + 3u) > SPU_LS_SIZE) { CAD_ADD(CAD_REC_SHAPE, 1); return 0; }

    cad_l4_k k;
    k.c0x = cad_b(in->c0x); k.c0y = cad_b(in->c0y); k.c0z = cad_b(in->c0z); k.c1x = cad_b(in->c1x); k.c1y = cad_b(in->c1y);
    k.c2x = cad_b(in->c2x); k.c2y = cad_b(in->c2y); k.c3x = cad_b(in->c3x); k.c3y = cad_b(in->c3y);
    k.e01x = cad_b(in->e01x); k.e01y = cad_b(in->e01y); k.e12x = cad_b(in->e12x); k.e12y = cad_b(in->e12y);
    k.e23x = cad_b(in->e23x); k.e23y = cad_b(in->e23y); k.e30x = cad_b(in->e30x); k.e30y = cad_b(in->e30y);
    k.nx = cad_b(in->nx); k.ny = cad_b(in->ny); k.nz = cad_b(in->nz); k.nzs = cad_bi(in->nzs); k.bit = cad_bi(in->r2);

    /* every group's bits (a group's four lanes; the last odd group computed twice) */
    static SPU_THREAD_LOCAL uint32_t* flags; static SPU_THREAD_LOCAL uint32_t cap;
    if (4u * G + 8u > cap) { free(flags); cap = 4u * G + 64u; flags = (uint32_t*)malloc(4u * cap); if (!flags) { cap = 0; CAD_ADD(CAD_REC_SHAPE, 1); return 0; } }
    __m256i bad = _mm256_setzero_si256(), any_in = _mm256_setzero_si256(), any_out = _mm256_setzero_si256();
    const uint8_t* const vb = ls + base;
    for (uint32_t g = 0; g < G; g += 2) {
        const uint32_t d = g + 1u < G ? 64u : 0u;
        __m256 X, Y, Z;
        cad_l4_load(vb + 64u * g, d, &X, &Y, &Z);
        const __m256i f = cad_l4_flags(&k, X, Y, Z, &bad);
        _mm256_storeu_si256((__m256i*)(flags + 4u * g), f);
        any_in = _mm256_or_si256(any_in, f);
        any_out = _mm256_or_si256(any_out, _mm256_xor_si256(f, _mm256_set1_epi32(-1)));
    }
    /* the group the pipeline starts with (the routine loads vertex 0 into all four lanes before the loop) */
    __m256i f_1;
    {
        const __m256 X = cad_b(in->X), Y = cad_b(in->Y), Z = cad_b(in->Z);
        f_1 = cad_l4_flags(&k, X, Y, Z, &bad);
    }
    if (cad_bad(bad)) { CAD_ADD(CAD_REC_RANGE, 1); return 0; }

    /* the two ORs as the whole lifted loop leaves them. Pass t ORs ~r22(t-1) into r62 (orx), r22(t) being the
     * bits of group t-2 (r22 at entry for t = 0; the record's bit masked by r63 at entry for group -2; the
     * first group for -1); and r8 |= r64(t-1), r64(s) = shufb(orx(r22(s)), the same, r50 after s shifts). */
    u128 m2;
    for (int i = 0; i < 4; i++) m2._u32[i] = (in->r63._u32[i] & 0x80000000u ? 0xFFFFFFFFu : 0u) & in->r2._u32[i];   /* group -2 */
    uint32_t o_1;                                                                                            /* group -1 */
    {
        uint32_t w[8]; _mm256_storeu_si256((__m256i*)w, f_1);
        o_1 = w[0] | w[1] | w[2] | w[3];
        const uint32_t n_1 = ~w[0] | ~w[1] | ~w[2] | ~w[3];
        const uint32_t ng = cad_or8(any_out);
        const uint32_t r62w = cad_or4(in->r62) | ~in->r22._u32[0] | ~in->r22._u32[1] | ~in->r22._u32[2] | ~in->r22._u32[3] |
                              ~m2._u32[0] | ~m2._u32[1] | ~m2._u32[2] | ~m2._u32[3] | n_1 | ng;
        t_l4.r62 = cad_u(r62w, 0, 0, 0);
    }
    /* r8: the placement control r50 shifts r52's first word in each pass; after the passes where it changes,
     * one placement of the OR of all the rest (a byte selection, so it distributes over OR) */
    {
        /* contribution s (0 .. G + 1) is the OR of group s - 2's bits (group -2, -1, then 0 .. G - 1) */
        u128 r8 = in->r8, ctrl = in->r50;
        for (int i = 0; i < 4; i++) r8._u32[i] |= in->r64._u32[i];
        uint32_t s = 0;
        for (; s <= G + 1u; s++) {
            const uint32_t o = s == 0 ? cad_or4(m2) : s == 1 ? o_1 :
                               flags[4u * (s - 2u)] | flags[4u * (s - 2u) + 1] | flags[4u * (s - 2u) + 2] | flags[4u * (s - 2u) + 3];
            const u128 v = cad_orx_w(o), p = spu_shufb(v, v, ctrl);
            for (int i = 0; i < 4; i++) r8._u32[i] |= p._u32[i];
            const u128 nc = spu_shufb(ctrl, in->r52, in->r54);
            if (cad_eq(nc, ctrl)) break;                   /* a fixed control: the rest in one placement */
            ctrl = nc;
        }
        if (s < G + 1u) {                                  /* contributions s + 1 .. G + 1 */
            uint32_t o = s + 1u <= 1u ? o_1 : 0u;
            for (uint32_t g = s + 1u >= 2u ? s - 1u : 0u; g < G; g++) o |= flags[4u * g] | flags[4u * g + 1] | flags[4u * g + 2] | flags[4u * g + 3];
            const u128 v = cad_orx_w(o), p = spu_shufb(v, v, ctrl);
            for (int i = 0; i < 4; i++) r8._u32[i] |= p._u32[i];
        }
        t_l4.r8 = r8;
    }
    /* the vertices' fourth words */
    for (uint32_t v = 0; v < 4u * G; v++) {
        const uint32_t f = flags[v];
        if (f) { uint8_t* w = ls + base + 16u * v + 12u; uint32_t x; memcpy(&x, w, 4); x |= __builtin_bswap32(f); memcpy(w, &x, 4); }
    }
    /* the last six passes as the lifted loop: counters as they are at pass G - 3 */
    t_l4.keep_lsa = base + 64u * (G - 6u);
    memcpy(t_l4.keep, ls + t_l4.keep_lsa, 192);
    const uint32_t Rg3 = R0 + 64u * (G - 3u);
    *r21o = cad_u(Rg3, Rg3, Rg3, Rg3);
    *r45o = cad_u(R0 + 64u * (G - 6u), R0 + 64u * (G - 5u), R0 + 64u * (G - 4u), Rg3);
    {
        u128 ctrl = in->r50;
        for (uint32_t s = 0; s < G - 3u; s++) { const u128 nc = spu_shufb(ctrl, in->r52, in->r54); if (cad_eq(nc, ctrl)) break; ctrl = nc; }
        *r50o = ctrl;
    }
    t_l4.on = 1;
    CAD_ADD(CAD_REC_NATIVE, 1); CAD_ADD(CAD_VERTS, 4u * G);
    return 1;
}
/* after the loop: the three groups its first native-entered passes stored leftovers into, and the ORs */
static inline void cad_l4_done(spu_context* ctx, u128* r62, u128* r8)
{
    memcpy(ctx->ls + t_l4.keep_lsa, t_l4.keep, 192);
    *r62 = t_l4.r62; *r8 = t_l4.r8;
    t_l4.on = 0;
}

/* ---- the vertex transform 0xF970 -------------------------------------------------------------------
 * Per vertex (x, y, z from its 16-byte line at r8 + 16 v): with M the batch's matrix in splatted registers and
 * zf = z with the sign flipped in r45's lanes,
 *   w = fma(z, r37, fma(y, r28, fma(x, r34, r32)))
 *   a = fma(zf, r6, fma(y, r21, fma(x, r22, r38)));  b = fma(zf, r29, fma(r35, y, fma(x, r39, r23)));
 *   c = fma(zf, r27, fma(y, r42, fma(r2, x, r33)))
 *   e = frest(w); f = fi(w, e); s = |fma(fnms(f, w, r13), f, f)|      (1/w refined once)
 *   out line v at r7 + 16 v = { fms(c, s, r11), fms(b, s, r11), fms(a, s, r11), r16 }
 * The SPU loop is pipelined five passes deep (its first five passes store leftovers into vertex group 0, which
 * the sixth overwrites); its counters are r31 (the input offset) and r47 (the store offsets, a FIFO of
 * halfwords). Natively every group; then its last six passes as the lifted loop (the first five of them store
 * leftovers into groups G-6 .. G-2, put back after). Inputs and outputs must not overlap. */
typedef struct {
    u128 r2, r6, r8, r10, r11, r13, r14, r15, r16, r20, r21, r22, r23, r25, r27, r28, r29, r30, r31, r32, r33, r34, r35, r36,
         r37, r38, r39, r40, r41, r42, r43, r45, r46, r47, r49, r51, r53, r54, r55;
} cad_l2_in;
static SPU_THREAD_LOCAL struct { int on; uint32_t keep_lsa; uint8_t keep[320]; } t_l2;
static inline __m256i cad_bsw(__m256i v)
{
    return _mm256_shuffle_epi8(v, _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}
/* frest and fi, eight lanes: spu_helpers.h's spu_frest / spu_fi (integer work on the bit fields, the hardware's
 * fraction table) at twice the width; checked against them lane by lane (bin\ftest's transform runs and
 * jharn ftest -e). */
static inline __m256 cad_frest8(__m256 w)
{
    const __m256i x = _mm256_castps_si256(w);
    const __m256i exp = _mm256_and_si256(_mm256_srli_epi32(x, 23), _mm256_set1_epi32(0xFF));
    const __m256i idx = _mm256_and_si256(_mm256_srli_epi32(x, 18), _mm256_set1_epi32(0x1F));
    const __m256i row = _mm256_srli_epi32(idx, 3);
    __m256i fr = _mm256_permutevar8x32_epi32(_mm256_loadu_si256((const __m256i*)spu_frest_fraction_lut), idx);
    for (int k = 1; k < 4; k++)
        fr = _mm256_blendv_epi8(fr, _mm256_permutevar8x32_epi32(_mm256_loadu_si256((const __m256i*)(spu_frest_fraction_lut + 8 * k)), idx),
                                _mm256_cmpeq_epi32(row, _mm256_set1_epi32(k)));
    fr = _mm256_and_si256(fr, _mm256_set1_epi32(0x7FFFFF));
    __m256i rexp = _mm256_sub_epi32(_mm256_set1_epi32(253), exp);
    rexp = _mm256_andnot_si256(_mm256_cmpgt_epi32(exp, _mm256_set1_epi32(252)), rexp);
    rexp = _mm256_blendv_epi8(rexp, _mm256_set1_epi32(255), _mm256_cmpeq_epi32(exp, _mm256_setzero_si256()));
    return _mm256_castsi256_ps(_mm256_or_si256(_mm256_or_si256(_mm256_and_si256(x, _mm256_set1_epi32((int)0x80000000u)),
                                                               _mm256_slli_epi32(rexp, 23)), fr));
}
static inline __m256 cad_fi8(__m256 w, __m256 e)      /* fi(a = w, b = e) */
{
    const __m256i est = _mm256_castps_si256(e), y = _mm256_and_si256(_mm256_castps_si256(w), _mm256_set1_epi32(0x7FFFF));
    const __m256i exp = _mm256_and_si256(_mm256_srli_epi32(est, 23), _mm256_set1_epi32(0xFF));
    const __m256i b19 = _mm256_slli_epi32(_mm256_and_si256(_mm256_srli_epi32(est, 10), _mm256_set1_epi32(0x1FFF)), 19);
    const __m256i sy = _mm256_mullo_epi32(_mm256_and_si256(est, _mm256_set1_epi32(0x3FF)), y);
    const __m256i big = _mm256_cmpeq_epi32(_mm256_max_epu32(b19, sy), b19);
    const __m256i d = _mm256_sub_epi32(b19, sy);
    const __m256i frac = _mm256_and_si256(_mm256_blendv_epi8(_mm256_srli_epi32(d, 8), _mm256_srli_epi32(d, 9), big), _mm256_set1_epi32(0x7FFFFF));
    const __m256i e2 = _mm256_sub_epi32(exp, _mm256_andnot_si256(big, _mm256_set1_epi32(1)));
    const __m256i r = _mm256_or_si256(_mm256_or_si256(_mm256_and_si256(est, _mm256_set1_epi32((int)0x80000000u)), _mm256_slli_epi32(e2, 23)), frac);
    const __m256i zero = _mm256_or_si256(_mm256_cmpeq_epi32(exp, _mm256_setzero_si256()), _mm256_cmpeq_epi32(e2, _mm256_setzero_si256()));
    return _mm256_castsi256_ps(_mm256_andnot_si256(zero, r));
}
#define CAD_FMS(a, b, c) cad_rng(_mm256_fmsub_ps((a), (b), (c)), &bad)      /* a*b - c */
static int cad_l2_run(spu_context* ctx, const cad_l2_in* in, u128* r31o, u128* r47o)
{
    uint8_t* const ls = ctx->ls;
    CAD_ADD(CAD_L2, 1);
    const uint32_t R0 = in->r31._u32[0];
    const int32_t r0s = (int32_t)R0;
    const uint32_t lo0 = R0 & 0xFFFFu;
    if (!cad_splat(in->r31) || r0s >= 0 || r0s <= -65536 || (R0 & 63u) ||
        !cad_eq(in->r47, cad_u(0xFFFF0000u | lo0, lo0 << 16 | lo0, lo0 << 16 | lo0, lo0 << 16 | lo0)) ||
        in->r43._u32[0] != in->r41._u32[0] + 16u || in->r49._u32[0] != in->r41._u32[0] + 32u || in->r51._u32[0] != in->r41._u32[0] + 48u ||
        in->r53._u32[0] != in->r8._u32[0] + 16u || in->r54._u32[0] != in->r8._u32[0] + 32u || in->r55._u32[0] != in->r8._u32[0] + 48u ||
        !cad_eq(in->r10, cad_u(0x04050607u, 0x14151617u, 0x00010203u, 0x10111213u)) ||
        !cad_eq(in->r14, cad_u(0x0C0D0E0Fu, 0x1C1D1E1Fu, 0x08090A0Bu, 0x18191A1Bu)) ||
        !cad_eq(in->r15, cad_u(0x00010203u, 0x10111213u, 0x04050607u, 0x14151617u)) ||
        !cad_eq(in->r20, cad_u(0x08090A0Bu, 0x18191A1Bu, 0x0C0D0E0Fu, 0x1C1D1E1Fu)) ||
        !cad_eq(in->r30, cad_u(0x18191A1Bu, 0x1C1D1E1Fu, 0x00010203u, 0x04050607u)) ||
        !cad_eq(in->r36, cad_u(0x00010203u, 0x04050607u, 0x18191A1Bu, 0x1C1D1E1Fu)) ||
        !cad_eq(in->r40, cad_u(0xC0C00405u, 0x06070809u, 0x0A0B1213u, 0x80808080u)) ||
        !cad_eq(in->r46, cad_u(0, 0, 0xFFFFFFFFu, 0xFFFFFFFFu)) || !cad_eq(in->r25, cad_u(0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu, 0x7FFFFFFFu))) {
        CAD_ADD(CAD_L2_SHAPE, 1); return 0;
    }
    const uint32_t G = (uint32_t)(-r0s) / 64u;
    const uint32_t ib = in->r8._u32[0] + R0, ob = in->r41._u32[0] + R0;
    if (G < 6u) { CAD_ADD(CAD_L2_SMALL, 1); return 0; }
    if ((ib & 15u) || (ob & 15u) || (uint64_t)ib + 64ull * (G + 5u) > SPU_LS_SIZE || (uint64_t)ob + 64ull * G > SPU_LS_SIZE ||
        !(ib + 64u * (G + 5u) <= ob || ob + 64u * G <= ib)) { CAD_ADD(CAD_L2_SHAPE, 1); return 0; }
    const __m256 m2 = cad_b(in->r2), m6 = cad_b(in->r6), m21 = cad_b(in->r21), m22 = cad_b(in->r22), m23 = cad_b(in->r23);
    const __m256 m27 = cad_b(in->r27), m28 = cad_b(in->r28), m29 = cad_b(in->r29), m32 = cad_b(in->r32), m33 = cad_b(in->r33);
    const __m256 m34 = cad_b(in->r34), m35 = cad_b(in->r35), m37 = cad_b(in->r37), m38 = cad_b(in->r38), m39 = cad_b(in->r39);
    const __m256 m42 = cad_b(in->r42), one = cad_b(in->r13), eps = cad_b(in->r11);
    const __m256i flip = cad_bi(in->r45), w4 = cad_bi(in->r16), absm = cad_bi(in->r25);
    __m256i bad = _mm256_setzero_si256();
    for (uint32_t g = 0; g < G; g += 2) {
        const int two = g + 1u < G;
        __m256 x, y, z;
        cad_l4_load(ls + ib + 64u * g, two ? 64u : 0u, &x, &y, &z);
        const __m256 w = CAD_FMA(z, m37, CAD_FMA(y, m28, CAD_FMA(x, m34, m32)));
        const __m256 zf = _mm256_castsi256_ps(_mm256_xor_si256(_mm256_castps_si256(z), flip));
        const __m256 a = CAD_FMA(zf, m6, CAD_FMA(y, m21, CAD_FMA(x, m22, m38)));
        const __m256 b = CAD_FMA(zf, m29, CAD_FMA(m35, y, CAD_FMA(x, m39, m23)));
        const __m256 c = CAD_FMA(zf, m27, CAD_FMA(y, m42, CAD_FMA(m2, x, m33)));
        const __m256 e = cad_frest8(w), f = cad_fi8(w, e);
        const __m256 s = _mm256_and_ps(CAD_FMA(CAD_FNMS(f, w, one), f, f), _mm256_castsi256_ps(absm));
        const __m256i o1 = _mm256_castps_si256(cad_fin(CAD_FMS(c, s, eps))), o2 = _mm256_castps_si256(cad_fin(CAD_FMS(b, s, eps)));
        const __m256i o3 = _mm256_castps_si256(cad_fin(CAD_FMS(a, s, eps)));
        const __m256i t0 = _mm256_unpacklo_epi32(o1, o2), t1 = _mm256_unpackhi_epi32(o1, o2);
        const __m256i t2 = _mm256_unpacklo_epi32(o3, w4), t3 = _mm256_unpackhi_epi32(o3, w4);
        const __m256i v0 = cad_bsw(_mm256_unpacklo_epi64(t0, t2)), v1 = cad_bsw(_mm256_unpackhi_epi64(t0, t2));
        const __m256i v2 = cad_bsw(_mm256_unpacklo_epi64(t1, t3)), v3 = cad_bsw(_mm256_unpackhi_epi64(t1, t3));
        uint8_t* const o = ls + ob + 64u * g;
        _mm_storeu_si128((__m128i*)(o + 0), _mm256_castsi256_si128(v0)); _mm_storeu_si128((__m128i*)(o + 16), _mm256_castsi256_si128(v1));
        _mm_storeu_si128((__m128i*)(o + 32), _mm256_castsi256_si128(v2)); _mm_storeu_si128((__m128i*)(o + 48), _mm256_castsi256_si128(v3));
        if (two) {
            _mm_storeu_si128((__m128i*)(o + 64), _mm256_extracti128_si256(v0, 1)); _mm_storeu_si128((__m128i*)(o + 80), _mm256_extracti128_si256(v1, 1));
            _mm_storeu_si128((__m128i*)(o + 96), _mm256_extracti128_si256(v2, 1)); _mm_storeu_si128((__m128i*)(o + 112), _mm256_extracti128_si256(v3, 1));
        }
    }
    if (cad_bad(bad)) { CAD_ADD(CAD_L2_RANGE, 1); return 0; }   /* the lifted loop rewrites every output line */
    t_l2.keep_lsa = ob + 64u * (G - 6u);
    memcpy(t_l2.keep, ls + t_l2.keep_lsa, 320);
    const uint32_t R = R0 + 64u * (G - 1u);
#define CAD_LO(k) ((R0 + 64u * (uint32_t)(k)) & 0xFFFFu)
    *r31o = cad_u(R, R, R, R);
    *r47o = cad_u(0xFFFF0000u | CAD_LO(G - 6u), CAD_LO(G - 5u) << 16 | CAD_LO(G - 4u), CAD_LO(G - 3u) << 16 | CAD_LO(G - 2u), 0u);
#undef CAD_LO
    t_l2.on = 1;
    CAD_ADD(CAD_L2_NATIVE, 1); CAD_ADD(CAD_L2_VERTS, 4u * G);
    return 1;
}

/* ---- the bounding box 0xFA78 -----------------------------------------------------------------------
 * Per pass: the four vertex lines loaded the pass before (the routine loads vertex 0 four times before the
 * loop) reduced lane by lane, min and max, by the SPU's tree (fcgt + selb: (a,b), (c,d), then the two), then
 * folded into r27 (min) and r28 (max); a tie keeps the operand selb keeps. Natively the same tree and the same
 * fold order for every group but the last two; the last two passes run as the lifted loop, entered with the
 * folds, its counter and the lines it would hold there (no stores: nothing to put back). */
typedef struct { u128 r6, r21, r22, r23, r24, r27, r28, r41, r43, r49, r51; } cad_l3_in;
static inline __m256i cad_fcgt8(__m256i a, __m256i b, __m256i* bad)
{
    const __m256 x = _mm256_castsi256_ps(a), y = _mm256_castsi256_ps(b);
    *bad = _mm256_or_si256(*bad, _mm256_castps_si256(_mm256_cmp_ps(x, y, _CMP_UNORD_Q)));
    return _mm256_castps_si256(_mm256_cmp_ps(x, y, _CMP_GT_OQ));
}
static inline __m256i cad_sel8(__m256i a, __m256i b, __m256i m) { return _mm256_blendv_epi8(a, b, m); }   /* selb a, b, m */
static inline __m128i cad_fcgt4(__m128i a, __m128i b, __m128i* bad)
{
    const __m128 x = _mm_castsi128_ps(a), y = _mm_castsi128_ps(b);
    *bad = _mm_or_si128(*bad, _mm_castps_si128(_mm_cmp_ps(x, y, _CMP_UNORD_Q)));
    return _mm_castps_si128(_mm_cmp_ps(x, y, _CMP_GT_OQ));
}
static inline __m128i cad_l(u128 a) { return _mm_loadu_si128((const __m128i*)a._u32); }
static inline u128 cad_s(__m128i v) { u128 r; _mm_storeu_si128((__m128i*)r._u32, v); return r; }
static inline __m128i cad_lq(const uint8_t* p) { return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)p), _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)); }
static int cad_l3_run(spu_context* ctx, const cad_l3_in* in, u128* r21o, u128* r22o, u128* r23o, u128* r24o, u128* r6o, u128* r27o, u128* r28o)
{
    const uint8_t* const ls = ctx->ls;
    CAD_ADD(CAD_L3, 1);
    const uint32_t R0 = in->r21._u32[0];
    const int32_t r0s = (int32_t)R0;
    if (!cad_splat(in->r21) || r0s >= 0 || (R0 & 63u) ||
        in->r43._u32[0] != in->r41._u32[0] + 16u || in->r49._u32[0] != in->r41._u32[0] + 32u || in->r51._u32[0] != in->r41._u32[0] + 48u) {
        CAD_ADD(CAD_L3_SHAPE, 1); return 0;
    }
    const uint32_t G = (uint32_t)(-r0s) / 64u, vb = in->r41._u32[0] + R0;
    if (G < 3u) { CAD_ADD(CAD_L3_SMALL, 1); return 0; }
    if ((vb & 15u) || (uint64_t)vb + 64ull * G > SPU_LS_SIZE) { CAD_ADD(CAD_L3_SHAPE, 1); return 0; }
    __m128i bad4 = _mm_setzero_si128();
    __m256i bad = _mm256_setzero_si256();
    __m128i mn_acc = cad_l(in->r27), mx_acc = cad_l(in->r28);
    /* group -1: the lines in r22 r23 r24 r6 */
    {
        const __m128i a = cad_l(in->r22), b = cad_l(in->r23), c = cad_l(in->r24), d = cad_l(in->r6);
        const __m128i gab = cad_fcgt4(a, b, &bad4), gcd = cad_fcgt4(c, d, &bad4);
        const __m128i mnab = _mm_blendv_epi8(a, b, gab), mxab = _mm_blendv_epi8(b, a, gab);
        const __m128i mncd = _mm_blendv_epi8(c, d, gcd), mxcd = _mm_blendv_epi8(d, c, gcd);
        const __m128i g1 = cad_fcgt4(mnab, mncd, &bad4), g2 = cad_fcgt4(mxab, mxcd, &bad4);
        const __m128i mn = _mm_blendv_epi8(mnab, mncd, g1), mx = _mm_blendv_epi8(mxcd, mxab, g2);
        mn_acc = _mm_blendv_epi8(mn, mn_acc, cad_fcgt4(mn, mn_acc, &bad4));
        mx_acc = _mm_blendv_epi8(mx_acc, mx, cad_fcgt4(mx, mx_acc, &bad4));
    }
    /* groups 0 .. G-4, two at a time through the tree, folded in order */
    const uint32_t gn = G - 3u;
    for (uint32_t g = 0; g < gn; g += 2) {
        const uint8_t* p = ls + vb + 64u * g;
        const uint32_t d2 = g + 1u < gn ? 64u : 0u;
        const __m256i a = _mm256_setr_m128i(cad_lq(p), cad_lq(p + d2)), b = _mm256_setr_m128i(cad_lq(p + 16), cad_lq(p + d2 + 16));
        const __m256i c = _mm256_setr_m128i(cad_lq(p + 32), cad_lq(p + d2 + 32)), d = _mm256_setr_m128i(cad_lq(p + 48), cad_lq(p + d2 + 48));
        const __m256i gab = cad_fcgt8(a, b, &bad), gcd = cad_fcgt8(c, d, &bad);
        const __m256i mnab = cad_sel8(a, b, gab), mxab = cad_sel8(b, a, gab);
        const __m256i mncd = cad_sel8(c, d, gcd), mxcd = cad_sel8(d, c, gcd);
        const __m256i mn = cad_sel8(mnab, mncd, cad_fcgt8(mnab, mncd, &bad)), mx = cad_sel8(mxcd, mxab, cad_fcgt8(mxab, mxcd, &bad));
        __m128i m = _mm256_castsi256_si128(mn), x = _mm256_castsi256_si128(mx);
        mn_acc = _mm_blendv_epi8(m, mn_acc, cad_fcgt4(m, mn_acc, &bad4));
        mx_acc = _mm_blendv_epi8(mx_acc, x, cad_fcgt4(x, mx_acc, &bad4));
        if (d2) {
            m = _mm256_extracti128_si256(mn, 1); x = _mm256_extracti128_si256(mx, 1);
            mn_acc = _mm_blendv_epi8(m, mn_acc, cad_fcgt4(m, mn_acc, &bad4));
            mx_acc = _mm_blendv_epi8(mx_acc, x, cad_fcgt4(x, mx_acc, &bad4));
        }
    }
    if (!_mm256_testz_si256(bad, bad) || !_mm_testz_si128(bad4, bad4)) { CAD_ADD(CAD_L3_RANGE, 1); return 0; }
    /* pass G-2: the lines of group G-3 in r22 r23 r24 r6, the folds through group G-4 */
    const uint8_t* q = ls + vb + 64u * (G - 3u);
    *r22o = cad_s(cad_lq(q)); *r23o = cad_s(cad_lq(q + 16)); *r24o = cad_s(cad_lq(q + 32)); *r6o = cad_s(cad_lq(q + 48));
    *r27o = cad_s(mn_acc); *r28o = cad_s(mx_acc);
    const uint32_t R = R0 + 64u * (G - 2u);
    *r21o = cad_u(R, R, R, R);
    CAD_ADD(CAD_L3_NATIVE, 1);
    return 1;
}

/* the lifted statements (gen_cad00_f6c0.py), with the loops' native entries */
static SPU_THREAD_LOCAL int t_l4_enable;
#ifdef CAD_HARNESS              /* the harness: CAD_NO_LOOPS=<mask> leaves loops lifted (1 transform, 2 box, 4 records) */
static int cad_no(int bit) { static int m = -1; if (m < 0) m = getenv("CAD_NO_LOOPS") ? atoi(getenv("CAD_NO_LOOPS")) : 0; return (m & bit) != 0; }
#else
#  define cad_no(bit) 0
#endif
#define CAD_F6_LOOP2 do { \
        if (t_l4_enable && !cad_no(1)) { \
            cad_l2_in _in = { r2, r6, r8, r10, r11, r13, r14, r15, r16, r20, r21, r22, r23, r25, r27, r28, r29, r30, r31, r32, r33, r34, \
                              r35, r36, r37, r38, r39, r40, r41, r42, r43, r45, r46, r47, r49, r51, r53, r54, r55 }; \
            if (cad_l2_run(ctx, &_in, &r31, &r47)) { _u31 = r31; _u47 = r47; } \
        } \
    } while (0)
#define CAD_F6_LOOP2_DONE do { if (t_l2.on) { memcpy(ctx->ls + t_l2.keep_lsa, t_l2.keep, 320); t_l2.on = 0; } } while (0)
#define CAD_F6_LOOP3 do { \
        if (t_l4_enable && !cad_no(2)) { \
            cad_l3_in _in = { r6, r21, r22, r23, r24, r27, r28, r41, r43, r49, r51 }; \
            if (cad_l3_run(ctx, &_in, &r21, &r22, &r23, &r24, &r6, &r27, &r28)) { \
                _u21 = r21; _u22 = r22; _u23 = r23; _u24 = r24; _u6 = r6; _u27 = r27; _u28 = r28; } \
        } \
    } while (0)
#define CAD_F6_LOOP4 do { \
        if (t_l4_enable && !cad_no(4)) { \
            cad_l4_in _in = { r2, r8, r10, r14, r20, r21, r22, r30, r36, r41, r43, r45, r49, r50, r51, r52, r54, r62, r63, r64, \
                              r23, r31, r44, r24, r29, r6, r27, r25, r28, r39, r32, r38, r34, r37, r35, r40, r33, \
                              r55, r57, r42, r58, r46, r53, r48 }; \
            if (cad_l4_run(ctx, &_in, &r21, &r45, &r50)) { _u21 = r21; _u45 = r45; _u50 = r50; } \
        } \
    } while (0)
#define CAD_F6_LOOP4_DONE do { if (t_l4.on) { cad_l4_done(ctx, &r62, &r8); _u62 = r62; _u8 = r8; } } while (0)
#ifdef CAD_PROF                  /* harness: time per region of the routine (rdtsc at its labels) */
unsigned long long g_cad_prof[64][2]; static SPU_THREAD_LOCAL unsigned long long t_cad_t0; static SPU_THREAD_LOCAL int t_cad_reg = -1;
static const uint32_t k_cad_prof_at[] = { 0xF6C0, 0xF828, 0xF970, 0xFA78, 0xFB40, 0xFD60, 0xFE78, 0xFF00, 0xFFFC, 0x10050 };
static inline int cad_prof_slot(uint32_t a) { for (int i = 0; i < 10; i++) if (k_cad_prof_at[i] == a) return i; return 10; }
void cad_prof_begin(void) { t_cad_reg = 0; t_cad_t0 = __rdtsc(); }
void cad_prof_end(void) { if (t_cad_reg >= 0) { g_cad_prof[t_cad_reg][0] += __rdtsc() - t_cad_t0; g_cad_prof[t_cad_reg][1]++; } t_cad_reg = -1; }
void cad_prof_print(void)
{
    unsigned long long tot = 0;
    for (int i = 0; i < 11; i++) tot += g_cad_prof[i][0];
    for (int i = 0; i < 11; i++) if (g_cad_prof[i][1])
        printf("  from 0x%05X: %5.1f %% (%llu entries)\n", i < 10 ? k_cad_prof_at[i] : 0, 100.0 * (double)g_cad_prof[i][0] / (double)(tot ? tot : 1), g_cad_prof[i][1]);
}
#  define CAD_AT(a) do { const unsigned long long _t = __rdtsc(); if (t_cad_reg >= 0) { g_cad_prof[t_cad_reg][0] += _t - t_cad_t0; g_cad_prof[t_cad_reg][1]++; } t_cad_reg = cad_prof_slot(a); t_cad_t0 = _t; } while (0)
#else
#  define CAD_AT(a) ((void)0)
#endif
#include "cad00_f6c0_lift.inc"

/* ---- the entry ------------------------------------------------------------------------------------ */
static inline int cad_guard(const spu_context* ctx)
{
    return !NATIVE_GEO_IRQ_PENDING(ctx) && (g_spu_ls_rdiag | g_spu_ls_wdiag) == 0 && (_mm_getcsr() & 0xE040u) == 0xE040u;
}
/* GT5_CAD00_NATIVE=2: the native routine on the side, then the lifted function itself from the same state (its
 * exit stays: SPU_RET, or the return to the dispatcher it makes for an interrupt, then nothing is compared);
 * local store and all 128 registers compared */
static __attribute__((noinline)) void cad_f6c0_compare(spu_context* ctx)
{
    static SPU_THREAD_LOCAL u128 g0[128], g1[128];
    uint8_t* save = (uint8_t*)malloc(2u * SPU_LS_SIZE);
    if (!save) { CAD_LIFTED_F6C0(ctx); return; }
    memcpy(g0, ctx->gpr, sizeof g0); memcpy(save, ctx->ls, SPU_LS_SIZE);
    const uint32_t pc0 = ctx->pc, link = ctx->gpr[0]._u32[0];
    t_l4_enable = 1;
    cad_f6c0_lift(ctx);                                   /* native */
    memcpy(g1, ctx->gpr, sizeof g1); memcpy(save + SPU_LS_SIZE, ctx->ls, SPU_LS_SIZE);
    memcpy(ctx->gpr, g0, sizeof g0); memcpy(ctx->ls, save, SPU_LS_SIZE); ctx->pc = pc0;
    g_spu_trampoline_fn = 0;
    CAD_LIFTED_F6C0(ctx);                                 /* the lifted function: this result stays */
    if (ctx->pc != link || g_spu_trampoline_fn) { free(save); return; }
    CAD_ADD(CAD_CMP, 1);
    if (memcmp(save + SPU_LS_SIZE, ctx->ls, SPU_LS_SIZE) != 0 || memcmp(g1, ctx->gpr, sizeof g1) != 0) {
        CAD_ADD(CAD_CMP_BAD, 1);
        static volatile long n;
        if (__atomic_add_fetch(&n, 1, __ATOMIC_RELAXED) <= 8) {
            uint32_t off = 0xFFFFFFFFu; int reg = -1;
            for (uint32_t i = 0; i < SPU_LS_SIZE && off == 0xFFFFFFFFu; i++) if (save[SPU_LS_SIZE + i] != ctx->ls[i]) off = i;
            for (int r = 0; r < 128 && reg < 0; r++) if (memcmp(&g1[r], &ctx->gpr[r], 16)) reg = r;
            fprintf(stderr, "[cad00] MISMATCH 0xF6C0 (list %05X, line %05X, records %05X): first LS byte %05X, first register r%d\n",
                    g0[3]._u32[0], g0[4]._u32[0], g0[9]._u32[0], off, reg);
            fflush(stderr);
        }
    }
    free(save);
}
#ifdef CAD_HARNESS
void (*g_cad_probe)(spu_context*, int);   /* the harness: called at the entry (0) and after the call (1) */
#endif
SPU_SV void cad_f6c0(spu_context* ctx)
{
#ifdef CAD_HARNESS
    static int scr = -1;
    if (scr < 0) scr = getenv("CAD_SCRAMBLE") != NULL;
    if (scr) {      /* the lifted routine, then every register it may change (r2-r79) overwritten: does anyone read them? */
        const uint32_t link = ctx->gpr[0]._u32[0];
        { int32_t _si = (int32_t)ctx->image_id; ctx->host_depth++; CAD_LIFTED_F6C0(ctx);
          spu_drain_call(ctx, link); ctx->host_depth--; spu_img_restore(ctx, _si); }
        for (int g = 2; g < 80; g++) for (int k = 0; k < 4; k++) ctx->gpr[g]._u32[k] = 0xDEAD0000u + (uint32_t)(g << 4) + (uint32_t)k;
        SPU_RET(ctx);
    }
    if (g_cad_probe) g_cad_probe(ctx, 0);
#endif
    CAD_ADD(CAD_CALLS, 1);
    if (CAD_UNLIKELY(++t_cad_n >= 64u)) cad_flush();
    const int m = cad_mode();
    if (CAD_UNLIKELY(m == 0)) { CAD_ADD(CAD_L_MODE, 1); __attribute__((musttail)) return CAD_LIFTED_F6C0(ctx); }
    if (CAD_UNLIKELY(!cad_guard(ctx))) { CAD_ADD(CAD_L_GUARD, 1); __attribute__((musttail)) return CAD_LIFTED_F6C0(ctx); }
    if (CAD_UNLIKELY(!cad_image_ok(ctx->ls))) { CAD_ADD(CAD_L_IMAGE, 1); __attribute__((musttail)) return CAD_LIFTED_F6C0(ctx); }
    CAD_ADD(CAD_NATIVE, 1);
    {
        static volatile long said;
        if (CAD_UNLIKELY(!said) && __atomic_exchange_n(&said, 1, __ATOMIC_SEQ_CST) == 0) {
            fprintf(stderr, "[cad00] 0xF6C0 runs as native code%s\n", m == 2 ? " (compared with the lifted code)" : ""); fflush(stderr);
        }
    }
    if (CAD_UNLIKELY(m == 2)) { cad_f6c0_compare(ctx); return; }      /* the lifted routine ran last: its exit stays */
    t_l4_enable = 1;
    __attribute__((musttail)) return cad_f6c0_lift(ctx);
}

#ifdef NATIVE_HOOKED
/* the routine's hook (natives.txt): native_w018CAD00.c's counters and cull tags first (nc_f6c0_entry), then
 * cad_f6c0 above */
SPU_SV void nc_f6c0_entry(spu_context* ctx);
SPU_SV void gt5_w018CAD00_spu_func_0000F6C0_sv(spu_context* ctx)
{
    __attribute__((musttail)) return nc_f6c0_entry(ctx);
}
#endif

#ifdef CAD_HARNESS
/* the harness: one run of the routine from ctx's state, lifted (0) or native (1); returns after it */
void cad_harness_run(spu_context* ctx, int native)
{
    static int copy_only = -1;
    if (copy_only < 0) copy_only = getenv("CAD_COPY_ONLY") != NULL;     /* the lifted statements' copy, no native loops */
    if (!native) { CAD_LIFTED_F6C0(ctx); return; }
    t_l4_enable = !copy_only;
#ifdef CAD_PROF
    cad_prof_begin(); cad_f6c0_lift(ctx); cad_prof_end();
#else
    cad_f6c0_lift(ctx);
#endif
}
#ifndef CAD_PROF
void cad_prof_print(void) {}
#endif
#endif
