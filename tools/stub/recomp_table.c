/* Empty game image for BONKHEADS_STUB_GAME builds (CI, portability checks).
 * Stands in for the build/gen/recomp_table.c that tools/recomp.py writes from
 * the original archive; holds nothing derived from the game. */
#include "runtime/image.h"

const RecompFunc g_recomp_funcs[1] = { { 0, 0, 0 } };
const unsigned g_recomp_nfuncs = 0;
const ImageChunk g_image_chunks[1] = { { 0, 0, 0 } };
const unsigned g_image_nchunks = 0;
const uint32_t g_image_a5 = 0;
const uint32_t g_image_main = 0;
const SegInfo g_image_segs[1] = { { 0, 0, 0 } };
const unsigned g_image_nsegs = 0;
