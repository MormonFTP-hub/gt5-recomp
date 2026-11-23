/* wgen_jFE031AF6.c -- the sound job jFE031AF6 (socetverb: the reverb) as native code: the code is generated
 * at build time by wgen_gen.py from this copy's own job image (wgen_jFE031AF6_gen.inc in the build folder);
 * native_wgen.h has the switch (GT5_WGEN_JFE031AF6 / GT5_WGEN = 0 lifted, 1 native, 2 compared) and the counters
 * ("[wgen] jFE031AF6 ..."). natives.txt: `jFE031AF6 00000000 - wgen_jFE031AF6.c hooked`. */
#define WG_IMG_NAME "jFE031AF6"
#define WG_ENV "GT5_WGEN_JFE031AF6"
#include "native_wgen.h"
#if __has_include("wgen_jFE031AF6_gen.inc")
#  include "wgen_jFE031AF6_gen.inc"
#endif
#ifdef WGEN_PROCS
WG_HOOK(jFE031AF6, 00000000)
#else              /* the generator left the job lifted in this copy (its WGEN_NOT_BUILT says why), or did not run */
#  ifndef WGEN_NOT_BUILT
#    define WGEN_NOT_BUILT "not generated in this build"
#  endif
WG_HOOK_LIFTED(jFE031AF6, 00000000)
#endif
