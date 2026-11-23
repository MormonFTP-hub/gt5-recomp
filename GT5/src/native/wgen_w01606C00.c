/* wgen_w01606C00.c -- the WWS job w01606C00 (a geometry job; its hand-written natives stay where they hook) as native code: the code is
 * generated at build time by wgen_gen.py from this copy's own game files (wgen_w01606C00_gen.inc in the build
 * folder); native_wgen.h has the switch (GT5_WGEN_W01606C00 / GT5_WGEN = 0 lifted, 1 native, 2 compared) and the
 * counters ("[wgen] w01606C00 ..."). natives.txt: `w01606C00 00004030 - wgen_w01606C00.c hooked`. */
#define WG_IMG_NAME "w01606C00"
#define WG_ENV "GT5_WGEN_W01606C00"
#include "native_wgen.h"
#if __has_include("wgen_w01606C00_gen.inc")
#  include "wgen_w01606C00_gen.inc"
#endif
#ifdef WGEN_PROCS
WG_HOOK(w01606C00, 00004030)
#else              /* the generator left the job lifted in this copy (its WGEN_NOT_BUILT says why), or did not run */
#  ifndef WGEN_NOT_BUILT
#    define WGEN_NOT_BUILT "not generated in this build"
#  endif
WG_HOOK_LIFTED(w01606C00, 00004030)
#endif
