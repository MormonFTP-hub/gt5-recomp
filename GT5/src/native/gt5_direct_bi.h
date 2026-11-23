/* gt5_direct_bi.h -- direct branches in the lifted SPU job images.
 *
 * scripts/spu_native_patch.py --direct-bi rewrites an image's branches to a register (`bi`, `biz`, `binz`,
 * `bie`/`bid` to the next instruction, loop back edges chosen with selb) and its calls through a register
 * (`bisl`) so that, when the register holds one of the image's own entries, control goes to that entry's
 * lifted function directly instead of through the runtime's dispatcher (spu_indirect_branch). It does so
 * only where the dispatcher would choose that very function and change nothing else on the way
 * (gt5_dbi_ready below, gt5_dbi_confirm in gt5_direct_bi.c); anything else still goes to the dispatcher.
 * GT5_DIRECT_BI=0 (read at start): every image as before; 2: each direct branch checked against the
 * dispatcher's own lookup first ("[direct-bi] <image>: checked N, MISMATCH M"); GT5_DIRECT_BI_OFF=<image,...>
 * leaves the named images with the dispatcher.
 *
 * Cross-image (n_jobmgr, 22 Nov 2026): a target outside the image's own entries goes on to the image the
 * dispatcher would resolve it in, when that image has direct branches too -- a WWS job's call into the job
 * manager's API (no resident slot over the target: the context's image), the job manager's call of a job
 * (the target in a resident slot: that slot's image) -- through that image's own switch and its own
 * confirmation (gt5_dbi_route). GT5_DIRECT_BI_XIMAGE=0: each image's own entries only, as before. Also from
 * n_jobmgr: a branch while an interrupt's registers are saved goes direct too unless it is the return to the
 * interrupted point (the only case the dispatcher's interrupt-return check acts on), and the runtime's
 * interrupt path takes the job manager's handler body directly (g_spu_direct_dispatch, gt5_dbi_ctx_branch). */
#ifndef GT5_DIRECT_BI_H
#define GT5_DIRECT_BI_H
#include <stdint.h>
#include "spu_context.h"

typedef struct { uint32_t pc; void (*fn)(spu_context*); } gt5_dbi_ent;
typedef SPU_SV void (*gt5_dbi_sv)(spu_context*);

typedef struct gt5_dbi_image {
    volatile int mode;            /* 0 the dispatcher, 1 direct, 2 direct and checked (set at registration) */
    const char* name;
    int image_id;                 /* the id the image registers its functions under (src/gen/spu_jobs.c) */
    int kind;                     /* 1: resident code at [lsa, lsa + size) under the job manager (a WWS job's
                                     code); 0: the context's own image (the WWS job manager) */
    uint32_t lsa, size;
    uint32_t lo, hi;              /* the lowest entry, the highest + 4 */
    const gt5_dbi_ent* tab;       /* the entries, by address: the image's own function table, less the
                                     return points (where a waiting host frame takes the branch) and the
                                     runtime's service addresses */
    uint32_t n;
    int cfg;                      /* the mode it was configured with (PS3_AB_WHAT=directbi switches 0 / cfg) */
    volatile int bad;             /* the registry serves another function at one of the entries */
    volatile uint32_t rgen;       /* the registry count the entries were last checked against (0: never) */
    volatile long long checked, mismatch;
    struct gt5_dbi_image* next;
    /* the image's switches (spu_native_patch.py), for branches routed here from another image */
    gt5_dbi_sv branch_sv, branch_chk_sv, call_sv, call_chk_sv;
} gt5_dbi_image;

/* The context state a confirmation holds for: within [lo, hi) the image's entries are what the dispatcher
 * resolves, until the context's resident code, image, overlay or the image changes. One per kind of image
 * (0: the context's image, 1: a resident slot's), so a job and the manager calling each other keep both. */
typedef struct {
    const spu_context* ctx;
    const gt5_dbi_image* img;
    uint32_t id, res;             /* ctx->dcache_id, ctx->dcache_residency (the resident code's signature) */
    int iid, ovl;                 /* ctx->image_id, ctx->resident_ovl */
    uint32_t lo, hi;
} gt5_dbi_memo;
extern SPU_THREAD_LOCAL gt5_dbi_memo t_gt5_dbi[2];
extern gt5_dbi_image* g_gt5_dbi_by_id[256];   /* the registered images by image id */
extern int g_gt5_dbi_xroute;                  /* GT5_DIRECT_BI_XIMAGE: 1 (default) cross-image routes, 0 off */

void gt5_dbi_register(gt5_dbi_image* im);
gt5_dbi_image* gt5_dbi_find(const char* name);
int gt5_dbi_confirm(spu_context* ctx, const gt5_dbi_image* im);
int gt5_dbi_check(spu_context* ctx, gt5_dbi_image* im);
void gt5_dbi_ab_poll(void);
SPU_SV void gt5_dbi_ctx_branch(spu_context* ctx);
SPU_SV void gt5_dbi_iret_branch(spu_context* ctx);   /* n_static: the interrupt return */
/* spu_channels.c: the WWS job manager's register save / restore stub at 0x3FEC0 natively (1 = done, ctx->pc
 * where it returns to; 0 = leave it to the dispatcher) */
int spu_wws_irq_stub_direct(spu_context* ctx);

/* May the branch to ctx->pc (one of im's entries) go there directly? Only when the drain loop has nothing
 * to do for this step (as for a lifted direct branch: SPU_DIRECT) and the dispatcher's fast path would
 * take it plainly: not the return to an interrupted point (an interrupt's registers saved, interrupts on,
 * the pc at its resume point: the dispatcher's interrupt-return check), no task resident, the image's
 * entries confirmed for the context's present resident code, image and overlay. */
static inline int gt5_dbi_step_ok(const spu_context* ctx)
{
    const uint32_t pc = ctx->pc;
    return g_spu_direct > 0 && ctx->steps && !g_spu_stack_reset_hint[(pc >> 2) & 1023u] &&
           !(ctx->int_enable && (ctx->event_status & ctx->event_mask)) &&
           !(ctx->irq_saved && ctx->int_enable && (pc & SPU_LS_MASK) == (ctx->irq_resume_pc & SPU_LS_MASK)) &&
           !ctx->resident_task;
}
static inline int gt5_dbi_ready(spu_context* ctx, const gt5_dbi_image* im)
{
    const uint32_t pc = ctx->pc;
    if (!gt5_dbi_step_ok(ctx))
        return 0;
    const gt5_dbi_memo* m = &t_gt5_dbi[im->kind & 1];
    if (m->ctx == ctx && m->img == im && m->id == ctx->dcache_id && m->res == ctx->dcache_residency &&
        m->iid == ctx->image_id && m->ovl == ctx->resident_ovl && pc - m->lo < m->hi - m->lo)
        return 1;
    return gt5_dbi_confirm(ctx, im);
}

/* A target outside the image's own entries (ctx->pc): the image the dispatcher would resolve it in, if that
 * is the other kind and has direct branches -- from a WWS job's code (kind 1) with no resident slot over the
 * target: the context's image (the job manager); from the context's image (kind 0) with a slot over it: that
 * slot's image. NULL: the dispatcher. (A job's target in another slot, or the manager's outside every slot,
 * is not routed: the two conditions exclude each other, so a route never comes back.) */
static inline const gt5_dbi_image* gt5_dbi_route(const spu_context* ctx, int from_kind)
{
    if (!g_gt5_dbi_xroute) return 0;
    const uint32_t pc = ctx->pc;
    int slot = 0;
    for (unsigned s = 0; s < 4; s++)
        if (ctx->resident_code[s].image_id && pc - ctx->resident_code[s].lsa < ctx->resident_code[s].size) {
            slot = ctx->resident_code[s].image_id;
            break;
        }
    const int to = from_kind == 1 ? (slot ? -1 : ctx->image_id) : (slot ? slot : -1);
    if ((unsigned)to >= 256u) return 0;
    const gt5_dbi_image* im = g_gt5_dbi_by_id[to];
    return im && im->kind == (from_kind ^ 1) && im->branch_sv ? im : 0;
}
#endif
