/* wgen_w018CAD00.c -- the WWS job w018CAD00 (the per-batch geometry and cull job) as native code: the code is
 * generated at build time by wgen_gen.py from this copy's own game files (wgen_w018CAD00_gen.inc in the build
 * folder); native_wgen.h has the switch (GT5_WGEN_W018CAD00 / GT5_WGEN = 0 lifted, 1 native, 2 compared) and the
 * counters ("[wgen] w018CAD00 ..."). natives.txt: `w018CAD00 000107D8 - wgen_w018CAD00.c hooked`. */
#define WG_IMG_NAME "w018CAD00"
#define WG_ENV "GT5_WGEN_W018CAD00"
#include "native_wgen.h"
#if __has_include("wgen_w018CAD00_gen.inc")
#  include "wgen_w018CAD00_gen.inc"
#endif
#ifdef WGEN_PROCS
WG_HOOK(w018CAD00, 000107D8)
#else              /* the generator left the job lifted in this copy (its WGEN_NOT_BUILT says why), or did not run */
#  ifndef WGEN_NOT_BUILT
#    define WGEN_NOT_BUILT "not generated in this build"
#  endif
WG_HOOK_LIFTED(w018CAD00, 000107D8)
#endif
