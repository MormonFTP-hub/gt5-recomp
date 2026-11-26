/* setjmp/longjmp for the recompiled build. Adapted from the GT5 Prologue port.
 *
 * func_00D00D74 and func_00D00E98 are the PS3 libc setjmp and longjmp: the first
 * saves r1, r2, r13-r31, LR, CR and the callee-saved FPRs into a 16-byte-aligned
 * buffer, the second loads them all back. On hardware longjmp ends in `blr`
 * *after* restoring LR from the buffer, so the branch goes to the setjmp call
 * site, not to longjmp's own caller.
 *
 * A static recompiler cannot express that. ppu_lifter emits longjmp's `blr` as a
 * plain `return;`, so the lifted longjmp restores every guest register and then
 * returns to whoever called it, now running with the register values that were
 * live at the setjmp. Prologue's LZ decoder aborts a decompression this way, and
 * this binary has the same call shape (2 setjmp sites, 3 longjmp sites).
 *
 * The fix is to put a real host setjmp in the lifted frame that calls the guest
 * setjmp, and to have the guest longjmp reach it with a host longjmp. The guest
 * register file is still restored by the original lifted longjmp body; only the
 * transfer of control is host-side.
 *
 * scripts/setjmp_patch.py installs the call-site half; re-run it after every
 * re-lift, since generated/ is rebuilt wholesale.
 *
 * The runtime's indirect-call count (#59). ps3_indirect_call (runtime/ppu/ppu_loader.cpp)
 * counts the indirect calls a thread is inside (g_dispatch_depth, and g_vcall_sp beside it)
 * and refuses any call past 4000 ("[ppu] recursion cap ... -- skipping": the call is not
 * made at all). It counts down when a call returns, and a host longjmp skips those returns:
 * every indirect call between the setjmp and the longjmp stayed counted for good. GT5's
 * streaming decompressor thread ends its streams this way: setjmp in 0x00A7F8F4 (Europe
 * 2.11; the same code in 1.00 calls setjmp at 0x00BC8A08 / 0x00BC9580), the end-of-stream
 * callbacks (2.11: 0x00A7EA8C / 0x00A7EC90; 1.00: 0x00BC8590 / 0x00BC87B0, reached through
 * a function pointer) longjmp back. That call stayed counted, one per stream, so after
 * about 4,000 streams in one session the thread's calls stopped being made: the callback
 * that wakes the thread waiting for the data was one of them, and that thread waited for
 * good. Seen on Europe 2.11 as the data install stopping at about 18 %
 * (the 4001st call came at the 2,224th installed file, the volume's worker thread then
 * waited at 0x00A7EA28 for good, Cancel did nothing), and in two player logs from the US disc:
 * a black screen at start (1.00) and no file loaded any more mid-session (2.11). The count at the
 * setjmp is now kept with the buffer and put back when the longjmp lands there, as
 * returning normally would have. GT5_LONGJMP_DEPTH=0: the old behaviour, for comparing.
 */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <atomic>

#define GT5_JMP_MAX 16

/* runtime/ppu/ppu_loader.cpp: this thread's indirect-call count and the stack of
 * call targets beside it (the same hooks a coroutine layer uses). */
extern "C" int* ps3_ppu_dispatch_depth_slot(void);
extern "C" int* ps3_ppu_vcall_sp_slot(void);

struct gt5_jmp {
    jmp_buf  host;
    uint32_t buf_ea;          /* the guest jmp_buf, aligned the way the guest aligns it */
    int      dispatch_depth;  /* the runtime's indirect-call count when this buffer was armed */
    int      vcall_sp;
};

/* A stack, not a map: setjmp buffers nest, and a buffer is only a valid target
 * while the frame that armed it is still on the host stack. The wrapper around
 * each setjmp-calling function restores the depth on the way out, so a buffer
 * whose frame has returned can never be fired into. */
static thread_local gt5_jmp g_jmp[GT5_JMP_MAX];
static thread_local int     g_depth;

/* The guest rounds the buffer pointer up to 16 in both setjmp and longjmp
 * (addi r3,r3,15 / rldicr r3,r3,0,59), so key on the same value. */
static uint32_t gt5_jmp_key(uint32_t ea) { return (ea + 15u) & ~15u; }

/* GT5_LONGJMP_DEPTH=0: do not put the indirect-call count back (the old behaviour). */
static bool gt5_jmp_restore_depth(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GT5_LONGJMP_DEPTH"); on = !(e && e[0] == '0'); }
    return on != 0;
}

extern "C" gt5_jmp* gt5_jmp_push(uint32_t buf_ea)
{
    if (g_depth >= GT5_JMP_MAX) {
        fprintf(stderr, "[setjmp] depth overflow, buffer 0x%08X not armed\n", buf_ea);
        return &g_jmp[GT5_JMP_MAX - 1];
    }
    gt5_jmp* e = &g_jmp[g_depth++];
    e->buf_ea = gt5_jmp_key(buf_ea);
    e->dispatch_depth = *ps3_ppu_dispatch_depth_slot();
    e->vcall_sp = *ps3_ppu_vcall_sp_slot();
    return e;
}

extern "C" int  gt5_jmp_depth(void)       { return g_depth; }
extern "C" void gt5_jmp_unwind(int depth) { if (depth < g_depth) g_depth = depth; }

/* A few lines for the log: the first longjmps, then one every 2,000, with how many
 * indirect calls each one leaves behind and the thread's count afterwards. */
static void gt5_jmp_note(const gt5_jmp& e, int depth_now, bool restore)
{
    static std::atomic<unsigned> fires{0};
    const unsigned n = ++fires;
    if (n > 3 && n % 2000 != 0) return;
    const int left = depth_now - e.dispatch_depth;
    if (restore)
        fprintf(stderr, "[setjmp] longjmp %u to 0x%08X: %d indirect call(s) unwound, the thread's call count "
                        "put back to %d\n", n, e.buf_ea, left, e.dispatch_depth);
    else
        fprintf(stderr, "[setjmp] longjmp %u to 0x%08X: %d indirect call(s) unwound and still counted, the "
                        "thread's call count stays %d (GT5_LONGJMP_DEPTH=0; calls past 4000 are skipped)\n",
                n, e.buf_ea, left, depth_now);
}

/* Returns only if no live buffer matches, in which case the caller falls back
 * to the old behaviour and returns normally. */
extern "C" void gt5_jmp_fire(uint32_t buf_ea, int value)
{
    uint32_t key = gt5_jmp_key(buf_ea);
    for (int i = g_depth - 1; i >= 0; i--) {
        if (g_jmp[i].buf_ea != key) continue;
        g_depth = i;                       /* the target frame is the new top */
        /* The setjmp's frame is where this lands: every indirect call made since it
         * was armed is unwound by the longjmp, so the runtime's count goes back to
         * what it was there (see the top). */
        int* depth = ps3_ppu_dispatch_depth_slot();
        const bool restore = gt5_jmp_restore_depth();
        gt5_jmp_note(g_jmp[i], *depth, restore);
        if (restore && *depth >= g_jmp[i].dispatch_depth) {
            *depth = g_jmp[i].dispatch_depth;
            *ps3_ppu_vcall_sp_slot() = g_jmp[i].vcall_sp;
        }
        longjmp(g_jmp[i].host, value ? value : 1);
    }
    static int warned = 0;
    if (warned++ < 4)
        fprintf(stderr, "[setjmp] longjmp to 0x%08X with no live buffer\n", buf_ea);
}

/* The call-site patch cannot see the layout of gt5_jmp, so hand it the jmp_buf.
 * setjmp still expands lexically at the call site, which is the part that
 * matters -- this only locates the storage. */
extern "C" jmp_buf* gt5_jmp_host(gt5_jmp* e) { return &e->host; }
