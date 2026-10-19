/* Tables emitted by tools/recomp.py describing the recompiled program. */
#pragma once
#include <stdint.h>

typedef void (*RecompFn)(void);
typedef struct { uint32_t addr; RecompFn fn; const char *name; } RecompFunc;
typedef struct { uint32_t addr; uint32_t len; const unsigned char *data; } ImageChunk;
typedef struct { int sid; uint32_t base; uint32_t len; } SegInfo;

extern const RecompFunc g_recomp_funcs[];
extern const unsigned g_recomp_nfuncs;
extern const ImageChunk g_image_chunks[];
extern const unsigned g_image_nchunks;
extern const uint32_t g_image_a5;
extern const uint32_t g_image_main;
extern const SegInfo g_image_segs[];
extern const unsigned g_image_nsegs;
