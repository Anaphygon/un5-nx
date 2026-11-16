/* libmpeg2 (third_party/libmpeg2, GPL-2.0-or-later) in one unit, for movie.cpp.
   tools/un5_from_iso.sh links it into the generated sources as native_mpeg2.c,
   next to a libmpeg2 folder link. */
#include "libmpeg2/alloc.c"
#include "libmpeg2/cpu_accel.c"
#include "libmpeg2/cpu_state.c"
#include "libmpeg2/decode.c"
#include "libmpeg2/header.c"
#include "libmpeg2/idct.c"
#include "libmpeg2/motion_comp.c"
#include "libmpeg2/slice.c"
