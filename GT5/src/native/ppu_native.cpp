/* Native PPU functions: switch, counters and compare mode (see ppu_native.h). */
#include "ppu_native.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>

extern "C" int g_gt5_ppu_native = 1;
PPU_THREAD_LOCAL gt5_ppu_nat_tls* g_gt5_ppu_nat_t;

namespace {
struct nat_fn { char addr[12]; int no_compare_form; };
nat_fn s_fn[GT5_PPU_NAT_MAX];
int s_nfn;                                       /* written at start-up only */
std::mutex s_mu;
gt5_ppu_nat_tls* s_tls[256];
std::atomic<int> s_ntls{0};
std::atomic<int64_t> s_last_print{0};
std::atomic<int> s_differ_logged{0};

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

int gt5_ppu_native_register(const char* addr, int no_compare_form)
{
    std::lock_guard<std::mutex> g(s_mu);
    if (s_nfn == 0) {
        const char* e = getenv("GT5_PPU_NATIVE");
        g_gt5_ppu_native = (e && e[0] == '0') ? 0 : (e && e[0] == '2') ? 2 : 1;
    }
    if (s_nfn >= GT5_PPU_NAT_MAX) return GT5_PPU_NAT_MAX - 1;   /* shares the last counter */
    snprintf(s_fn[s_nfn].addr, sizeof s_fn[s_nfn].addr, "%s", addr);
    s_fn[s_nfn].no_compare_form = no_compare_form;
    return s_nfn++;
}

gt5_ppu_nat_tls* gt5_ppu_nat_tls_new(void)
{
    gt5_ppu_nat_tls* t = (gt5_ppu_nat_tls*)calloc(1, sizeof(gt5_ppu_nat_tls));
    std::lock_guard<std::mutex> g(s_mu);
    const int i = s_ntls.load();
    if (i < 256) { s_tls[i] = t; s_ntls.store(i + 1); }          /* past 256 threads: not in the log line */
    g_gt5_ppu_nat_t = t;
    return t;
}

void gt5_ppu_nat_tick(void)
{
    const int64_t t = now_ms(), last = s_last_print.load(std::memory_order_relaxed);
    if (last != 0 && t - last < 10000) return;
    int64_t expect = last;
    if (!s_last_print.compare_exchange_strong(expect, t)) return;
    if (last == 0) return;                                          /* the first 10 s start now */
    uint64_t sum[6][GT5_PPU_NAT_MAX] = {};
    const int nt = s_ntls.load(), nf = s_nfn;
    for (int i = 0; i < nt; i++) {
        const gt5_ppu_nat_tls* c = s_tls[i];
        for (int k = 0; k < nf; k++) {
            sum[0][k] += c->native[k]; sum[1][k] += c->lifted[k]; sum[2][k] += c->compared[k];
            sum[3][k] += c->differed[k]; sum[4][k] += c->skipped[k]; sum[5][k] += c->nanquiet[k];
        }
    }
    uint64_t tot[6] = {};
    for (int j = 0; j < 6; j++) for (int k = 0; k < nf; k++) tot[j] += sum[j][k];
    char top[1024]; int n = 0; top[0] = 0;
    for (int shown = 0; shown < 10; shown++) {                     /* the ten most called */
        int best = -1;
        for (int k = 0; k < nf; k++) {
            const uint64_t v = sum[0][k] + sum[1][k] + sum[2][k] + sum[4][k];
            if (v && (best < 0 || v > sum[0][best] + sum[1][best] + sum[2][best] + sum[4][best])) best = k;
        }
        if (best < 0) break;
        n += snprintf(top + n, sizeof top - n, " %s %llu", s_fn[best].addr,
                      (unsigned long long)(sum[0][best] + sum[1][best] + sum[2][best] + sum[4][best]));
        sum[0][best] = sum[1][best] = sum[2][best] = sum[4][best] = 0;
        if (n >= (int)sizeof top - 40) break;
    }
    fprintf(stderr, "[ppu-native] mode %d, %d functions: native %llu calls, lifted %llu, compared %llu, differed %llu,"
                    " not compared %llu, same but a signaling NaN quieted %llu |%s\n", g_gt5_ppu_native, nf, (unsigned long long)tot[0],
            (unsigned long long)tot[1], (unsigned long long)tot[2], (unsigned long long)tot[3], (unsigned long long)tot[4],
            (unsigned long long)tot[5], top);
    fflush(stderr);
}

void gt5_ppu_nat_lifted(int k)
{
    gt5_ppu_nat_tls* t = g_gt5_ppu_nat_t;
    if (!t) t = gt5_ppu_nat_tls_new();
    t->lifted[k]++;
    if ((++t->calls & 0xFFFFu) == 0) gt5_ppu_nat_tick();
}

/* ---- compare mode ---- */
struct gt5_ppu_shadow {
    enum { HBITS = 18, HSIZE = 1 << HBITS, MAXB = 1 << 16 };
    uint32_t gen;
    int special, overflow, nlist;
    uint32_t list[MAXB];              /* each byte address stored to, once */
    uint32_t hkey[HSIZE], hgen[HSIZE];
    uint8_t hval[HSIZE];
    alignas(16) uint8_t p16[16];
};
static PPU_THREAD_LOCAL gt5_ppu_shadow* t_shadow;

static inline uint32_t sh_slot(const gt5_ppu_shadow* s, uint32_t a, int* found)
{
    uint32_t h = (a * 0x9E3779B1u) >> (32 - gt5_ppu_shadow::HBITS);
    for (;;) {
        if (s->hgen[h] != s->gen) { *found = 0; return h; }
        if (s->hkey[h] == a) { *found = 1; return h; }
        h = (h + 1) & (gt5_ppu_shadow::HSIZE - 1);
    }
}

void gt5_sh_read(gt5_ppu_shadow* s, uint32_t ea, void* dst, uint32_t n, int raw)
{
    if (!raw) {
        const ppu_mm m = ppu_mm_take();
        if (!vm_mm_in(m.ld_span, ea, n) || (n == 4 && ea == m.poll)) { s->special = 1; memset(dst, 0, n); return; }
    }
    uint8_t* d = (uint8_t*)dst;
    for (uint32_t i = 0; i < n; i++) {
        int f; const uint32_t a = ea + i, h = sh_slot(s, a, &f);
        d[i] = f ? s->hval[h] : vm_base[a];
    }
}

void gt5_sh_write(gt5_ppu_shadow* s, uint32_t ea, const void* src, uint32_t n, int raw)
{
    if (!raw) {
        const ppu_mm m = ppu_mm_take();
        if (!vm_mm_in(m.st_span, ea, n)) { s->special = 1; return; }
    }
    const uint8_t* p = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) {
        int f; const uint32_t a = ea + i, h = sh_slot(s, a, &f);
        if (!f) {
            if (s->nlist >= gt5_ppu_shadow::MAXB) { s->overflow = 1; return; }
            s->hgen[h] = s->gen; s->hkey[h] = a; s->list[s->nlist++] = a;
        }
        s->hval[h] = p[i];
    }
}

const uint8_t* gt5_sh_p16(gt5_ppu_shadow* s, uint32_t ea)
{
    gt5_sh_read(s, ea, s->p16, 16, 1);
    return s->p16;
}

static void reg_name(size_t off, char* out, size_t cap, const ppu_context* a, const ppu_context* b)
{
    const size_t g = offsetof(ppu_context, gpr), f = offsetof(ppu_context, fpr), v = offsetof(ppu_context, vr);
    if (off >= g && off < g + sizeof a->gpr) {
        const int i = (int)((off - g) / 8);
        snprintf(out, cap, "r%d native=%016llX lifted=%016llX", i, (unsigned long long)a->gpr[i], (unsigned long long)b->gpr[i]);
    } else if (off >= f && off < f + sizeof a->fpr) {
        const int i = (int)((off - f) / 8); uint64_t x, y; memcpy(&x, &a->fpr[i], 8); memcpy(&y, &b->fpr[i], 8);
        snprintf(out, cap, "f%d native=%016llX lifted=%016llX", i, (unsigned long long)x, (unsigned long long)y);
    } else if (off >= v && off < v + sizeof a->vr) {
        const int i = (int)((off - v) / 16);
        snprintf(out, cap, "v%d native=%08X%08X%08X%08X lifted=%08X%08X%08X%08X", i,
                 __builtin_bswap32(a->vr[i].w[0]), __builtin_bswap32(a->vr[i].w[1]), __builtin_bswap32(a->vr[i].w[2]), __builtin_bswap32(a->vr[i].w[3]),
                 __builtin_bswap32(b->vr[i].w[0]), __builtin_bswap32(b->vr[i].w[1]), __builtin_bswap32(b->vr[i].w[2]), __builtin_bswap32(b->vr[i].w[3]));
    } else {
        snprintf(out, cap, "context byte %u (cr %08X/%08X xer %08X/%08X vscr %08X/%08X lr %llX/%llX ctr %llX/%llX)", (unsigned)off,
                 a->cr, b->cr, a->xer, b->xer, a->vscr, b->vscr, (unsigned long long)a->lr, (unsigned long long)b->lr,
                 (unsigned long long)a->ctr, (unsigned long long)b->ctr);
    }
}

/* A signaling NaN that only one side quieted (its quiet bit the one difference): the lifted code itself quiets
 * one on a float-double-float round trip or not, depending on whether the compiler kept the value in a
 * register, so the compare form (guest accesses as calls) can differ there alone. Counted, not a difference. */
static int nan_q32(uint32_t x, uint32_t y)
{
    return (x ^ y) == 0x00400000u && (x & 0x7F800000u) == 0x7F800000u && (x & 0x003FFFFFu) != 0;
}
static int nan_q64(uint64_t x, uint64_t y)
{
    return (x ^ y) == 0x0008000000000000ull && (x & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
           (x & 0x0007FFFFFFFFFFFFull) != 0;
}
static int ctx_nan_quiet_only(size_t off, const ppu_context* a, const ppu_context* b)
{
    const size_t f = offsetof(ppu_context, fpr), v = offsetof(ppu_context, vr);
    if (off >= f && off < f + sizeof a->fpr) {
        const int i = (int)((off - f) / 8); uint64_t x, y; memcpy(&x, &a->fpr[i], 8); memcpy(&y, &b->fpr[i], 8);
        return nan_q64(x, y);
    }
    if (off >= v && off < v + sizeof a->vr) {
        const int i = (int)((off - v) / 16), l = (int)((off - v) % 16) / 4;
        return nan_q32(__builtin_bswap32(a->vr[i].w[l]), __builtin_bswap32(b->vr[i].w[l]));
    }
    return 0;
}
static int mem_nan_quiet_only(gt5_ppu_shadow* s, uint32_t ad)
{
    for (uint32_t n = 4; n <= 8; n += 4) {             /* the aligned float or double holding the byte */
        const uint32_t w = ad & ~(n - 1);
        uint8_t mine[8], real[8]; int all = 1;
        for (uint32_t j = 0; j < n; j++) {
            int f; const uint32_t h = sh_slot(s, w + j, &f);
            all &= f; mine[j] = f ? s->hval[h] : 0; real[j] = vm_base[w + j];
        }
        if (!all) continue;
        if (n == 4) { uint32_t x, y; memcpy(&x, mine, 4); memcpy(&y, real, 4); if (nan_q32(__builtin_bswap32(x), __builtin_bswap32(y))) return 1; }
        else { uint64_t x, y; memcpy(&x, mine, 8); memcpy(&y, real, 8); if (nan_q64(__builtin_bswap64(x), __builtin_bswap64(y))) return 1; }
    }
    return 0;
}

void gt5_ppu_native_compare(ppu_context* ctx, gt5_ppu_shadow_fn shadow, void (*lifted)(ppu_context*), int k)
{
    gt5_ppu_nat_tls* t = g_gt5_ppu_nat_t;
    if (!t) t = gt5_ppu_nat_tls_new();
    if ((++t->calls & 0xFFFFu) == 0) gt5_ppu_nat_tick();
    gt5_ppu_shadow* s = t_shadow;
    if (!s) { s = (gt5_ppu_shadow*)calloc(1, sizeof(gt5_ppu_shadow)); t_shadow = s; }
    if (!shadow || !s) { t->skipped[k]++; lifted(ctx); return; }
    if (++s->gen == 0) { memset(s->hgen, 0, sizeof s->hgen); s->gen = 1; }
    s->special = s->overflow = 0; s->nlist = 0;
    ppu_context in, mine;
    memcpy(&in, ctx, sizeof in);
    memcpy(&mine, ctx, sizeof mine);
    shadow(&mine, s);
    lifted(ctx);
    if (s->special || s->overflow) { t->skipped[k]++; return; }
    char what[256]; what[0] = 0;
    int nanq = 0;
    /* The registers only: gpr fpr vr cr lr ctr xer fpscr vscr (everything before cia). A native is made only for
     * a function that names no other field, so neither side writes cia, thread_id, the reservation or vrsave;
     * the reservation is host bookkeeping that other threads clear (ppu_resv_break_*) at any moment. */
    const uint8_t* a = (const uint8_t*)&mine; const uint8_t* b = (const uint8_t*)ctx;
    for (size_t i = 0; i < offsetof(ppu_context, cia); i++)
        if (a[i] != b[i]) {
            if (ctx_nan_quiet_only(i, &mine, ctx)) { nanq = 1; continue; }
            reg_name(i, what, sizeof what, &mine, ctx); break;
        }
    if (!what[0]) {
        for (int i = 0; i < s->nlist; i++) {
            int f; const uint32_t ad = s->list[i], h = sh_slot(s, ad, &f);
            if (vm_base[ad] != s->hval[h]) {
                if (mem_nan_quiet_only(s, ad)) { nanq = 1; continue; }
                snprintf(what, sizeof what, "guest byte %08X native=%02X lifted=%02X (%d bytes stored)", ad, s->hval[h], vm_base[ad], s->nlist);
                break;
            }
        }
    }
    t->compared[k]++;
    if (!what[0]) { if (nanq) t->nanquiet[k]++; return; }
    t->differed[k]++;
    if (s_differ_logged.fetch_add(1) < 16) {
        fprintf(stderr, "[ppu-native] differ %s: %s | in r1=%llX r2=%llX r3=%llX r4=%llX r5=%llX r6=%llX r7=%llX r8=%llX"
                        " lr=%llX vscr=%08X f1=%a\n", s_fn[k].addr, what,
                (unsigned long long)in.gpr[1], (unsigned long long)in.gpr[2], (unsigned long long)in.gpr[3],
                (unsigned long long)in.gpr[4], (unsigned long long)in.gpr[5], (unsigned long long)in.gpr[6],
                (unsigned long long)in.gpr[7], (unsigned long long)in.gpr[8], (unsigned long long)in.lr, in.vscr, in.fpr[1]);
        fflush(stderr);
    }
}
