/* native_wgen.h -- the run-time half of a generated native WWS job (scripts: src/native/wgen_gen.py, which writes
 * wgen_<image>_gen.inc at build time from this copy's own game files). Included by src/native/wgen_<image>.c,
 * which defines WG_IMG / WG_IMG_NAME / WG_ENV and then WG_HOOK(...) for the job's entry.
 *
 * Switch, per image, read once: GT5_WGEN_<IMAGE> (else GT5_WGEN, else 1)
 *   0  the lifted job (as before)
 *   1  the generated native job (default)
 *   2  native, and every 64th call (GT5_WGEN_CMP_EVERY) of a procedure that never touches the runtime is run
 *      again by the runtime's SPU interpreter from a copy of the state before it: registers and local store
 *      must be the same (counted; the first mismatches are logged with the procedure and the address)
 * Log: "[wgen] <image> runs as native code (...)" once; then every 30 s while it runs:
 *   "[wgen] <image>: jobs run as native code N, of them handed back N (interrupt N, jump N, instruction N), lifted
 *    stretches inside native jobs N (...), runtime calls N[, compared N, MISMATCH N]"
 * Handed back: the native code could not finish the job; the lifted code ran the rest of it. A lifted stretch: the
 * lifted code ran part of a procedure (from an address the native code has no label for) and the job went on natively.
 * GT5_WGEN_STAT=1 prints the line every 5 s and the first addresses handed back. */
#ifndef NATIVE_WGEN_H
#define NATIVE_WGEN_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "spu_recomp.h"          /* the image's own lifted header (its folder is on the include path) */
#include "spu_helpers.h"

#ifndef SPU_THREAD_LOCAL
#  define SPU_THREAD_LOCAL _Thread_local
#endif

enum { WG_HB_IRQ = 0, WG_HB_JUMP = 1, WG_HB_INSN = 2, WG_HB_N = 3 };
static int wg_mode = -1;             /* -1 unread */
static int wg_cmp = 0;               /* mode 2 */
static int wg_stat = 0;
static unsigned wg_cmp_every = 64;    /* GT5_WGEN_CMP_EVERY: compare every n-th call of a procedure */
static long long wg_jobs, wg_hb[WG_HB_N], wg_rtcalls, wg_cmp_n, wg_cmp_bad, wg_cmp_skip;
static long long wg_printed_jobs = -1;
static uint32_t wg_hb_pc[8];
static unsigned wg_hb_pcn;
static SPU_THREAD_LOCAL int t_wg_why;
static SPU_THREAD_LOCAL uint32_t t_wg_pc;
static SPU_THREAD_LOCAL long long t_wg_rt;

#define WG_HB(ctx, why, pc) do { t_wg_why = (why); t_wg_pc = (pc); } while (0)
/* a hand-back inside the job: the lifted code runs from pc until the procedure's return, then native again */
static long long wg_hb_in[WG_HB_N];
#define WG_HB_IN(ctx, why, pc) do { __atomic_add_fetch(&wg_hb_in[(why)], 1, __ATOMIC_RELAXED); WG_HB_TRACE(pc); \
        if (wg_hb_pcn < 8) wg_hb_pc[__atomic_fetch_add(&wg_hb_pcn, 1, __ATOMIC_RELAXED) & 7] = (pc) | 0x80000000u; } while (0)
#ifdef WGEN_HARNESS
static int wg_trace = -1;
#define WG_HB_TRACE(pc) ((wg_trace < 0 ? (wg_trace = getenv("WGEN_TRACE") != NULL) : wg_trace) ? fprintf(stderr, "[wgen-hb] 0x%05X\n", (unsigned)(pc)) : 0)
#define WG_RT(ctx) (t_wg_rt++, (wg_trace < 0 ? (wg_trace = getenv("WGEN_TRACE") != NULL) : wg_trace) ? fprintf(stderr, "[wgen-rt] call 0x%05X lr 0x%05X\n", ctx->pc & SPU_LS_MASK, ctx->gpr[0]._u32[0] & SPU_LS_MASK) : 0)
#else
#define WG_RT(ctx) (t_wg_rt++)
#define WG_HB_TRACE(pc) ((void)0)
#endif

static void wg_read_mode(void)
{
    const char* e = getenv(WG_ENV);
    if (!e || !*e) e = getenv("GT5_WGEN");
    int m = (e && *e) ? atoi(e) : 1;
    if (m < 0 || m > 2) m = 1;
    const char* s = getenv("GT5_WGEN_STAT");
    wg_stat = s && *s && *s != '0';
    { const char* c = getenv("GT5_WGEN_CMP_EVERY"); if (c && atoi(c) > 0) wg_cmp_every = (unsigned)atoi(c); }
    wg_cmp = m == 2;
    __atomic_store_n(&wg_mode, m, __ATOMIC_RELEASE);
}

static double wg_now(void) { struct timespec t; timespec_get(&t, TIME_UTC); return (double)t.tv_sec + t.tv_nsec / 1e9; }

static void wg_print(FILE* f)
{
    fprintf(f, "[wgen] %s: jobs run as native code %lld, of them handed back %lld (interrupt %lld, jump %lld, instruction %lld), "
            "lifted stretches inside native jobs %lld (interrupt %lld, jump %lld, instruction %lld), runtime calls %lld",
            WG_IMG_NAME, wg_jobs, wg_hb[0] + wg_hb[1] + wg_hb[2], wg_hb[0], wg_hb[1], wg_hb[2],
            wg_hb_in[0] + wg_hb_in[1] + wg_hb_in[2], wg_hb_in[0], wg_hb_in[1], wg_hb_in[2], wg_rtcalls);
    if (wg_cmp || wg_cmp_n) fprintf(f, ", compared %lld, MISMATCH %lld (not comparable %lld)", wg_cmp_n, wg_cmp_bad, wg_cmp_skip);
    if (wg_stat && wg_hb_pcn) {
        fprintf(f, " | first hand-backs at");
        for (unsigned k = 0; k < wg_hb_pcn && k < 8; k++) fprintf(f, " 0x%05X", wg_hb_pc[k]);
    }
    fprintf(f, "\n");
    fflush(f);
}

static void wg_tick(void)
{
    static double next;
    static volatile int lock;
    const double now = wg_now();
    if (now < next || __atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) return;
    if (next == 0) next = now;
    if (now >= next) {
        next = now + (wg_stat ? 5.0 : 30.0);
        if (wg_jobs != wg_printed_jobs) { wg_printed_jobs = wg_jobs; wg_print(stderr); }
    }
    __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
}

/* mode 2: a call of a procedure that never touches the runtime (no channel, no DMA, no call into the job
 * manager or lifted code) runs natively on the real state, then the runtime's SPU interpreter runs the same
 * call from a copy of the state as it was before; registers, local store and where it went are compared.
 * The copy runs as no image at all (image id -1, no resident code, no policy services): the interpreter then
 * never looks up or enters lifted code, so nothing on the copy reaches the real run's dispatcher, drain or
 * driver (in the game the dispatch caches answered for the job's own code: v1 stopped at the first lifted
 * entry, every call "not comparable"). A `stop` planted at the return address in the COPY's local store ends
 * the interpreted run there (and is taken out again before the compare). Only calls the native code returned
 * from normally are compared: the interpreter then follows the same path, inside the procedure. */
uint32_t spu_interp_run_until(spu_context* ctx, uint32_t start_lsa, uint32_t stop_lsa);   /* spu_interp.c */
static SPU_THREAD_LOCAL spu_context* t_wg_scratch;
static SPU_THREAD_LOCAL unsigned t_wg_tick;
static SPU_THREAD_LOCAL int t_wg_incmp;
typedef SPU_SV uint32_t wg_fn_t(spu_context*, uint32_t);
#define WG_CMP_STOP 0x00003FFEu          /* stop 0x3FFE */
static __attribute__((noinline)) uint32_t wg_compare(spu_context* ctx, wg_fn_t* fn, uint32_t entry, uint32_t ret)
{
    if (t_wg_incmp || ++t_wg_tick < wg_cmp_every) return fn(ctx, ret);
    t_wg_tick = 0;
    spu_context* s = t_wg_scratch;
    if (!s) { s = (spu_context*)malloc(sizeof *s); if (!s) return fn(ctx, ret); t_wg_scratch = s; }
    memcpy(s, ctx, sizeof *s);
    s->ls = s->ls_store;
    if (ctx->ls != ctx->ls_store) memcpy(s->ls_store, ctx->ls, SPU_LS_SIZE);
    t_wg_incmp = 1;                         /* the procedures it calls are not compared on their own meanwhile */
    const uint32_t r = fn(ctx, ret);
    t_wg_incmp = 0;
    if (((r ^ ret) & SPU_LS_MASK) || (ret & 3u) || ret + 4u > SPU_LS_SIZE) {
        __atomic_add_fetch(&wg_cmp_skip, 1, __ATOMIC_RELAXED);
        return r;
    }
    s->int_enable = 0; s->irq_frame = 0; s->irq_saved = 0;
    memset(s->resident_code, 0, sizeof s->resident_code); s->resident_ovl = 0; s->resident_task = 0;
    s->image_id = -1; s->policy_mode = 0; s->status = SPU_STATUS_RUNNING; s->stop_code = 0;
    uint8_t keep[4];
    memcpy(keep, s->ls + ret, 4);
    s->ls[ret] = (uint8_t)(WG_CMP_STOP >> 24); s->ls[ret + 1] = (uint8_t)(WG_CMP_STOP >> 16);
    s->ls[ret + 2] = (uint8_t)(WG_CMP_STOP >> 8); s->ls[ret + 3] = (uint8_t)WG_CMP_STOP;
    spu_interp_run_until(s, entry & SPU_LS_MASK, 0);
    memcpy(s->ls + ret, keep, 4);
    const int reached = (s->status & SPU_STATUS_STOPPED_BY_STOP) && s->stop_code == (WG_CMP_STOP & 0x3FFFu) &&
                        (s->pc & SPU_LS_MASK) == ((ret + 4u) & SPU_LS_MASK);
    if (!reached) {                         /* the interpreted run ended elsewhere: a stop of its own, a halt */
        __atomic_add_fetch(&wg_cmp_skip, 1, __ATOMIC_RELAXED);
        return r;
    }
    __atomic_add_fetch(&wg_cmp_n, 1, __ATOMIC_RELAXED);
    int reg = -1;
    for (int g = 0; g < 128 && reg < 0; g++) if (memcmp(&s->gpr[g], &ctx->gpr[g], 16)) reg = g;
    long ls = -1;
    if (memcmp(s->ls, ctx->ls, SPU_LS_SIZE))
        for (long x = 0; x < SPU_LS_SIZE && ls < 0; x++) if (s->ls[x] != ctx->ls[x]) ls = x;
    if (reg >= 0 || ls >= 0) {
        const long long n = __atomic_add_fetch(&wg_cmp_bad, 1, __ATOMIC_RELAXED);
        if (n <= 8) {
            fprintf(stderr, "[wgen] MISMATCH %s 0x%05X (return 0x%05X): first register r%d, first local-store byte 0x%05lX%s\n",
                    WG_IMG_NAME, entry, ret, reg, ls, reg >= 0 ? "" : "");
            if (reg >= 0)
                fprintf(stderr, "[wgen]   r%d native %08X %08X %08X %08X, interpreter %08X %08X %08X %08X\n", reg,
                        ctx->gpr[reg]._u32[0], ctx->gpr[reg]._u32[1], ctx->gpr[reg]._u32[2], ctx->gpr[reg]._u32[3],
                        s->gpr[reg]._u32[0], s->gpr[reg]._u32[1], s->gpr[reg]._u32[2], s->gpr[reg]._u32[3]);
            fflush(stderr);
        }
    }
    return r;
}
/* --debug builds of the generated file: WGEN_LIFT=<lo>-<hi>[,...] runs the procedures with entries in those
 * ranges as lifted code (to find a procedure that differs, by halving). */
static uint32_t wg_force[64][2]; static int wg_nforce = -1;
static int wg_forced(uint32_t pc)
{
    if (wg_nforce < 0) {
        wg_nforce = 0;
        const char* e = getenv("WGEN_LIFT");
        while (e && *e && wg_nforce < 64) {
            char* q; const unsigned long lo = strtoul(e, &q, 16), hi = *q == '-' ? strtoul(q + 1, &q, 16) : lo;
            wg_force[wg_nforce][0] = (uint32_t)lo; wg_force[wg_nforce][1] = (uint32_t)hi; wg_nforce++;
            e = *q == ',' ? q + 1 : NULL;
        }
    }
    for (int k = 0; k < wg_nforce; k++) if (pc >= wg_force[k][0] && pc <= wg_force[k][1]) return 1;
    return 0;
}
static uint32_t wg_lifted_call(spu_context* ctx, uint32_t ret)
{
    int32_t si = (int32_t)ctx->image_id;
    ctx->host_depth++; spu_indirect_branch(ctx); spu_drain_call(ctx, ret); ctx->host_depth--; spu_img_restore(ctx, si);
    return ret;
}
#define WG_CALL(fn, entry, ctx, ret) (__builtin_expect(wg_cmp, 0) ? wg_compare((ctx), (fn), (entry), (ret)) : fn((ctx), (ret)))

/* The job's entry: WG_HOOK(w01695680, 00004030) defines gt5_w01695680_spu_func_00004030_sv (natives.txt
 * `hooked`): the native job, or the lifted one (..._orig_sv). WG_HOOK_LIFTED: the lifted one only (no generated
 * code in this copy). wgen_<image>.c picks one AFTER it has included the generated file (WGEN_PROCS defined there):
 * a choice made here, when this header is read, could not see what that file says.
 * The generated file also says how the job ends (WGEN_CHAIN, WGEN_END_PC): a WWS job returns to the address the
 * manager's call left in r0; a job-chain job (src/native/wgen_gen.py JIMAGES, entered at LS 0 by the runtime's
 * driver with 0x3FF00 in r0) returns there or branches to LS 0 (WGEN_END_PC), and the runtime's drain ends the run
 * at either, as for the lifted job. The lifted job-chain job's first transfer is the run's step 0 (spu_drain.c: a
 * branch to LS 0 ends a job only after it): the native job has taken it (WG_ENTER). */
#define WG_CAT4(a, b, c, d) a##b##c##d
#define WG_HOOK_LIFTED(img, addr) \
    SPU_SV void WG_CAT4(gt5_, img, _spu_func_, addr##_orig_sv)(spu_context* ctx); \
    SPU_SV void WG_CAT4(gt5_, img, _spu_func_, addr##_sv)(spu_context* ctx) { \
        if (__builtin_expect(wg_mode < 0, 0)) { wg_read_mode(); \
            fprintf(stderr, "[wgen] %s stays lifted code (%s)\n", WG_IMG_NAME, WGEN_NOT_BUILT); fflush(stderr); } \
        WG_CAT4(gt5_, img, _spu_func_, addr##_orig_sv)(ctx); }
#define WG_IS_END(R, E) ((R) == (E) || (R) == WGEN_END_PC)
#define WG_ENTER(ctx) do { if (WGEN_CHAIN && !(ctx)->steps) (ctx)->steps = 1; } while (0)
/* (a WWS job) back to the manager, running: a stop-and-signal in a manager call left STOPPED_BY_STOP behind (the
 * lifted job's interpreted stretches set RUNNING again; a lingering STOPPED would end the manager's run at its next
 * unwind) */
#define WG_BACK_RUNNING(ctx) do { if (!WGEN_CHAIN && (ctx)->status == SPU_STATUS_STOPPED_BY_STOP) (ctx)->status = SPU_STATUS_RUNNING; } while (0)
#define WG_HOOK(img, addr) \
    SPU_SV void WG_CAT4(gt5_, img, _spu_func_, addr##_orig_sv)(spu_context* ctx); \
    SPU_SV void WG_CAT4(gt5_, img, _spu_func_, addr##_sv)(spu_context* ctx) { \
        if (__builtin_expect(wg_mode < 0, 0)) { wg_read_mode(); \
            fprintf(stderr, "[wgen] %s runs as %s (generated: %d procedures)\n", WG_IMG_NAME, \
                    wg_mode == 0 ? "lifted code (GT5_WGEN 0)" : wg_mode == 2 ? "native code, compared with the lifted code" \
                    : "native code", WGEN_PROCS); fflush(stderr); } \
        if (wg_mode == 0) { WG_CAT4(gt5_, img, _spu_func_, addr##_orig_sv)(ctx); return; } \
        const uint32_t E = ctx->gpr[0]._u32[0] & SPU_LS_MASK & ~3u; \
        t_wg_rt = 0; \
        WG_ENTER(ctx); \
        const long long nj = __atomic_add_fetch(&wg_jobs, 1, __ATOMIC_RELAXED); \
        uint32_t R = WGEN_ENTRY_FN(ctx, E); \
        for (int _k = 0; !WG_IS_END(R, E) && wg_is_entry(R) && _k < 64; _k++) R = wg_call(ctx, R, E);   /* went on in another procedure */ \
        __atomic_add_fetch(&wg_rtcalls, t_wg_rt, __ATOMIC_RELAXED); \
        ctx->pc = R; \
        if (__builtin_expect(WG_IS_END(R, E), 1)) { \
            WG_BACK_RUNNING(ctx); \
            if (ctx->host_depth == 0) g_spu_trampoline_fn = spu_indirect_branch; \
        } else { \
            const long long n = __atomic_add_fetch(&wg_hb[t_wg_why < WG_HB_N ? t_wg_why : WG_HB_INSN], 1, __ATOMIC_RELAXED); \
            if (wg_hb_pcn < 8) wg_hb_pc[__atomic_fetch_add(&wg_hb_pcn, 1, __ATOMIC_RELAXED) & 7] = t_wg_pc; \
            (void)n; g_spu_trampoline_fn = spu_indirect_branch; \
        } \
        if ((nj & 1023) == 0 || wg_stat) wg_tick(); \
    }

/* The stand-alone harness (WGEN_HARNESS): set the mode, print the counters. */
#ifdef WGEN_HARNESS
int wgen_harness_mode(int m) { wg_read_mode(); wg_mode = m; wg_cmp = m == 2; return m; }
void wgen_harness_report(FILE* f) { wg_print(f); }
#endif
#endif
