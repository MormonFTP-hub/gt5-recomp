/* wgen_jD58063CD.c -- the sound job jD58063CD (socisynth: the mixer, its filters and resampler) as native code: the code is generated
 * at build time by wgen_gen.py from this copy's own job image (wgen_jD58063CD_gen.inc in the build folder);
 * native_wgen.h has the switch (GT5_WGEN_JD58063CD / GT5_WGEN = 0 lifted, 1 native, 2 compared) and the counters
 * ("[wgen] jD58063CD ..."). natives.txt: `jD58063CD 00000000 - wgen_jD58063CD.c hooked`. */
#define WG_IMG_NAME "jD58063CD"
#define WG_ENV "GT5_WGEN_JD58063CD"
#include "native_wgen.h"
#if __has_include("wgen_jD58063CD_gen.inc")
#  include "wgen_jD58063CD_gen.inc"
#endif
#ifdef WGEN_PROCS
WG_HOOK(jD58063CD, 00000000)
#else              /* the generator left the job lifted in this copy (its WGEN_NOT_BUILT says why), or did not run */
#  ifndef WGEN_NOT_BUILT
#    define WGEN_NOT_BUILT "not generated in this build"
#  endif
WG_HOOK_LIFTED(jD58063CD, 00000000)
#endif
