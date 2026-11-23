/* wgen_j75BFE68A.c -- the WWS job manager j75BFE68A (the policy module every WWS job runs under) as native code: the code is generated
 * at build time by wgen_gen.py from this copy's own job image (wgen_j75BFE68A_gen.inc in the build folder);
 * native_wgen.h has the switch (GT5_WGEN_J75BFE68A / GT5_WGEN = 0 lifted, 1 native, 2 compared) and the counters
 * ("[wgen] j75BFE68A ..."). natives.txt: `j75BFE68A 00000A00 - wgen_j75BFE68A.c hooked`. */
#define WG_IMG_NAME "j75BFE68A"
#define WG_ENV "GT5_WGEN_J75BFE68A"
#include "native_wgen.h"
#if __has_include("wgen_j75BFE68A_gen.inc")
#  include "wgen_j75BFE68A_gen.inc"
#endif
#ifdef WGEN_PROCS
WG_HOOK(j75BFE68A, 00000A00)
#else              /* the generator left the job lifted in this copy (its WGEN_NOT_BUILT says why), or did not run */
#  ifndef WGEN_NOT_BUILT
#    define WGEN_NOT_BUILT "not generated in this build"
#  endif
WG_HOOK_LIFTED(j75BFE68A, 00000A00)
#endif
