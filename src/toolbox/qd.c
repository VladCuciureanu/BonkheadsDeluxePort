/* QuickDraw core: ports, rectangles, regions (rectangular), pens, colours,
 * primitive drawing and CopyBits on 8-bit indexed pixels. */
#include "qd.h"
#include "res.h"
#include <stdlib.h>

uint32_t g_screen_fb;
uint32_t g_main_device;
uint32_t g_cur_device;
uint8_t g_hw_pal[256][3];
bool g_hw_pal_dirty;
static uint32_t qd_globals;   /* address of thePort field */

/* ------------------------------------------------------------ rects */
QRect rect_sect(QRect a, QRect b)
{
    QRect r = { a.top > b.top ? a.top : b.top, a.left > b.left ? a.left : b.left,
                a.bottom < b.bottom ? a.bottom : b.bottom, a.right < b.right ? a.right : b.right };
    if (rect_empty(r)) { QRect z = { 0, 0, 0, 0 }; return z; }
    return r;
}

QRect rect_union(QRect a, QRect b)
{
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    QRect r = { a.top < b.top ? a.top : b.top, a.left < b.left ? a.left : b.left,
                a.bottom > b.bottom ? a.bottom : b.bottom, a.right > b.right ? a.right : b.right };
    return r;
}

/* ------------------------------------------------------------ regions (bbox only) */
uint32_t qd_new_rgn(QRect r)
{
    uint32_t h = mm_new_handle(10, true);
    qd_set_rgn(h, r);
    return h;
}

void qd_set_rgn(uint32_t rgn, QRect r)
{
    if (!rgn || !rdl(rgn)) return;
    if (rect_empty(r)) { QRect z = { 0, 0, 0, 0 }; r = z; }
    wrw(rdl(rgn), 10);
    wr_rect(rdl(rgn) + 2, r);
}

QRect qd_rgn_box(uint32_t rgn)
{
    QRect z = { 0, 0, 0, 0 };
    if (!rgn || !rdl(rgn)) return z;
    return rd_rect(rdl(rgn) + 2);
}

/* ------------------------------------------------------------ colour tables */
static void std_clut_rgb(int i, uint8_t *r, uint8_t *g, uint8_t *b)
{
    static const uint8_t ramp[10] = { 0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11 };
    if (i < 215) {
        int ri = i / 36, gi = (i / 6) % 6, bi = i % 6;
        *r = (uint8_t)(0xFF - ri * 0x33); *g = (uint8_t)(0xFF - gi * 0x33); *b = (uint8_t)(0xFF - bi * 0x33);
    } else if (i < 225) { *r = ramp[i - 215]; *g = *b = 0; }
    else if (i < 235) { *g = ramp[i - 225]; *r = *b = 0; }
    else if (i < 245) { *b = ramp[i - 235]; *r = *g = 0; }
    else if (i < 255) { *r = *g = *b = ramp[i - 245]; }
    else { *r = *g = *b = 0; }
}

uint32_t qd_new_ctab_from(const uint8_t *clut, uint32_t len)
{
    if (clut && len >= 8) {
        uint32_t h = mm_handle_from_bytes(clut, len);
        return h;
    }
    uint32_t h = mm_new_handle(8 + 256 * 8, true);
    uint32_t p = rdl(h);
    wrl(p, 8); wrw(p + 4, 0x8000); wrw(p + 6, 255);
    for (int i = 0; i < 256; i++) {
        uint8_t r, g, b; std_clut_rgb(i, &r, &g, &b);
        uint32_t e = p + 8 + i * 8;
        wrw(e, (uint16_t)i); wrw(e + 2, r * 0x101); wrw(e + 4, g * 0x101); wrw(e + 6, b * 0x101);
    }
    return h;
}

uint32_t qd_device_ctab(void)
{
    uint32_t pm = rdl(rdl(g_main_device) + GD_PMap);
    return rdl(rdl(pm) + PM_pmTable);
}

/* nearest-colour cache keyed by ctab contents seed */
static uint32_t cache_ctab, cache_seed;
static uint8_t cache_lut[32768];
static bool cache_valid[32768];

static int ctab_entries(uint32_t ctab, uint32_t *p)
{
    if (!ctab || !rdl(ctab)) return 0;
    *p = rdl(ctab);
    return (int16_t)rdw(*p + 6) + 1;
}

void qd_index_to_rgb(uint32_t ctab, uint8_t idx, uint16_t *r, uint16_t *g, uint16_t *b)
{
    uint32_t p;
    int n = ctab_entries(ctab, &p);
    if (n <= 0) { uint8_t r8, g8, b8; std_clut_rgb(idx, &r8, &g8, &b8); *r = r8 * 0x101; *g = g8 * 0x101; *b = b8 * 0x101; return; }
    bool device = (rdw(p + 4) & 0x8000) != 0;
    for (int i = 0; i < n; i++) {
        uint32_t e = p + 8 + i * 8;
        int v = device ? i : rdw(e);
        if (v == idx) { *r = (uint16_t)rdw(e + 2); *g = (uint16_t)rdw(e + 4); *b = (uint16_t)rdw(e + 6); return; }
    }
    *r = *g = *b = 0;
}

uint8_t qd_rgb_to_index(uint32_t ctab, uint16_t r, uint16_t g, uint16_t b)
{
    uint32_t p;
    int n = ctab_entries(ctab, &p);
    if (n <= 0) { ctab = qd_device_ctab(); n = ctab_entries(ctab, &p); }
    uint32_t seed = rdl(p);
    if (ctab != cache_ctab || seed != cache_seed) {
        cache_ctab = ctab; cache_seed = seed;
        memset(cache_valid, 0, sizeof cache_valid);
    }
    int key = ((r >> 11) << 10) | ((g >> 11) << 5) | (b >> 11);
    if (cache_valid[key]) {
        /* exact colours bypass the cache's 5-bit quantisation below */
    }
    bool device = (rdw(p + 4) & 0x8000) != 0;
    long best = 0x7FFFFFFF; int bi = 0;
    for (int i = 0; i < n; i++) {
        uint32_t e = p + 8 + i * 8;
        long dr = ((long)rdw(e + 2) - r) >> 4, dg = ((long)rdw(e + 4) - g) >> 4, db = ((long)rdw(e + 6) - b) >> 4;
        long d = dr * dr + dg * dg + db * db;
        if (d < best) { best = d; bi = device ? i : rdw(e); if (!d) break; }
    }
    return (uint8_t)bi;
}

/* fast 15-bit lookup used for bulk conversions (PICT direct pixels) */
uint8_t qd_rgb15_to_index(uint32_t ctab, int r5, int g5, int b5)
{
    uint32_t p;
    int n = ctab_entries(ctab, &p);
    if (n <= 0) ctab = qd_device_ctab();
    if (n > 0 && (ctab != cache_ctab || rdl(p) != cache_seed)) {
        cache_ctab = ctab; cache_seed = rdl(p);
        memset(cache_valid, 0, sizeof cache_valid);
    }
    int key = (r5 << 10) | (g5 << 5) | b5;
    if (!cache_valid[key]) {
        cache_lut[key] = qd_rgb_to_index(ctab, (uint16_t)(r5 * 0x0842), (uint16_t)(g5 * 0x0842), (uint16_t)(b5 * 0x0842));
        cache_ctab = ctab;
        cache_valid[key] = true;
    }
    return cache_lut[key];
}

/* ------------------------------------------------------------ ports */
uint32_t qd_the_port(void) { return qd_globals ? rdl(qd_globals) : 0; }
void qd_set_port(uint32_t port) { if (qd_globals) wrl(qd_globals, port); }

static bool is_color_port(uint32_t port) { return (rdw(port + P_portVersion) & 0xC000) == 0xC000; }

bool qd_bitmap_pixels(uint32_t bm, PixDesc *pd)
{
    if (!bm) return false;
    uint16_t rb = (uint16_t)rdw(bm + 4);
    uint32_t pm;
    if ((rb & 0xC000) == 0xC000) {          /* portBits of a CGrafPort -> PixMapHandle */
        uint32_t h = rdl(bm);
        if (!h || !rdl(h)) return false;
        pm = rdl(h);
    } else if (rb & 0x8000) {
        pm = bm;                             /* a PixMap */
    } else {
        pd->base = rdl(bm); pd->rowbytes = rb & 0x3FFF; pd->bounds = rd_rect(bm + 6);
        pd->depth = 1; pd->ctab = 0;
        return pd->base != 0;
    }
    pd->base = rdl(pm + PM_baseAddr);
    pd->rowbytes = rdw(pm + PM_rowBytes) & 0x3FFF;
    pd->bounds = rd_rect(pm + PM_bounds);
    pd->depth = (int16_t)rdw(pm + PM_pixelSize);
    pd->ctab = rdl(pm + PM_pmTable);
    return pd->base != 0;
}

bool qd_port_pixels(uint32_t port, PixDesc *pd)
{
    if (!port) return false;
    return qd_bitmap_pixels(port + P_portBits, pd);
}

QRect qd_port_clip(uint32_t port)
{
    QRect c = qd_rgn_box(rdl(port + P_clipRgn));
    QRect v = qd_rgn_box(rdl(port + P_visRgn));
    if (!rdl(port + P_clipRgn)) c = rd_rect(port + P_portRect);
    if (!rdl(port + P_visRgn)) v = rd_rect(port + P_portRect);
    return rect_sect(c, v);
}

/* per-port pattern storage (kept host-side) */
typedef struct { uint32_t port; uint8_t pen[8], bk[8], fill[8]; } PortPats;
static PortPats pats[128];
static PortPats *port_pats(uint32_t port)
{
    PortPats *free_slot = NULL;
    for (int i = 0; i < 128; i++) {
        if (pats[i].port == port) return &pats[i];
        if (!pats[i].port && !free_slot) free_slot = &pats[i];
    }
    if (!free_slot) free_slot = &pats[port % 128];
    free_slot->port = port;
    memset(free_slot->pen, 0xFF, 8); memset(free_slot->bk, 0, 8); memset(free_slot->fill, 0xFF, 8);
    return free_slot;
}

/* Classic QuickDraw colours are bit sets: blackBit 5, yellowBit 6, magentaBit 7,
 * cyanBit 8 (subtractive). Color QuickDraw derives the RGB from those bits,
 * which also covers non-standard values such as the 139 the game uses. */
static void classic_rgb(uint32_t c, uint16_t *r, uint16_t *g, uint16_t *b)
{
    if (c & (1u << 5)) { *r = *g = *b = 0; return; }
    *r = (c & (1u << 8)) ? 0 : 0xFFFF;
    *g = (c & (1u << 7)) ? 0 : 0xFFFF;
    *b = (c & (1u << 6)) ? 0 : 0xFFFF;
    /* inverseBit (1) inverts an ink colour: 139 = magenta|inverse -> green.
     * whiteColor (30) also carries the bit but has no ink bits, so stays white. */
    if ((c & (1u << 1)) && (c & 0x1C0)) { *r = (uint16_t)~*r; *g = (uint16_t)~*g; *b = (uint16_t)~*b; }
}

static uint8_t port_index_for(uint32_t port, uint32_t rgbaddr)
{
    PixDesc pd;
    uint32_t ctab = qd_port_pixels(port, &pd) ? pd.ctab : 0;
    return qd_rgb_to_index(ctab, (uint16_t)rdw(rgbaddr), (uint16_t)rdw(rgbaddr + 2), (uint16_t)rdw(rgbaddr + 4));
}

uint8_t qd_fg_index(uint32_t port) { return port_index_for(port, port + P_rgbFg); }
uint8_t qd_bk_index(uint32_t port) { return port_index_for(port, port + P_rgbBk); }

void qd_init_port(uint32_t port, bool color)
{
    QRect scr = { 0, 0, SCREEN_H, SCREEN_W };
    wrw(port, 0);
    if (color) {
        uint32_t pmh = mm_new_handle(PIXMAP_SIZE, true);
        uint32_t spm = rdl(rdl(g_cur_device ? g_cur_device : g_main_device) + GD_PMap);
        memcpy(HP(rdl(pmh)), HP(rdl(spm)), PIXMAP_SIZE);
        wrl(port + P_portBits, pmh);
        wrw(port + P_portVersion, 0xC000);
        wrl(port + 8, 0);
    } else {
        wrl(port + 2, g_screen_fb);
        wrw(port + 6, SCREEN_W);
        wr_rect(port + 8, scr);
    }
    wr_rect(port + P_portRect, scr);
    wrl(port + P_visRgn, qd_new_rgn(scr));
    QRect wide = { -32767, -32767, 32767, 32767 };
    wrl(port + P_clipRgn, qd_new_rgn(wide));
    wrw(port + P_rgbFg, 0); wrw(port + P_rgbFg + 2, 0); wrw(port + P_rgbFg + 4, 0);
    wrw(port + P_rgbBk, 0xFFFF); wrw(port + P_rgbBk + 2, 0xFFFF); wrw(port + P_rgbBk + 4, 0xFFFF);
    wrl(port + P_pnLoc, 0);
    wrw(port + P_pnSize, 1); wrw(port + P_pnSize + 2, 1);
    wrw(port + P_pnMode, 8);
    wrw(port + P_pnVis, 0);
    wrw(port + P_txFont, 0); wrw(port + P_txFace, 0); wrw(port + P_txMode, 1); wrw(port + P_txSize, 0);
    wrl(port + P_fgColor, 33); wrl(port + P_bkColor, 30);
    PortPats *pp = port_pats(port);
    memset(pp->pen, 0xFF, 8); memset(pp->bk, 0, 8); memset(pp->fill, 0xFF, 8);
}

/* ------------------------------------------------------------ drawing core */
void qd_mark_dirty(void) { }

void qd_fill_rect(uint32_t port, QRect r, const uint8_t pat[8], int mode, uint8_t fg, uint8_t bk)
{
    PixDesc pd;
    if (!qd_port_pixels(port, &pd) || pd.depth != 8) return;
    r = rect_sect(r, qd_port_clip(port));
    r = rect_sect(r, pd.bounds);
    if (rect_empty(r)) return;
    if (pd.base == g_screen_fb) {
        QRect sr = { (int16_t)(r.top - pd.bounds.top), (int16_t)(r.left - pd.bounds.left),
                     (int16_t)(r.bottom - pd.bounds.top), (int16_t)(r.right - pd.bounds.left) };
        hitext_damage_screen(sr);
    }
    mode &= 0x3F;
    for (int y = r.top; y < r.bottom; y++) {
        uint8_t prow = pat[y & 7];
        uint8_t *row = HP(pd.base + (uint32_t)(y - pd.bounds.top) * pd.rowbytes);
        for (int x = r.left; x < r.right; x++) {
            uint8_t *px = row + (x - pd.bounds.left);
            bool bit = (prow >> (7 - (x & 7))) & 1;
            switch (mode) {
            case 0: case 8: *px = bit ? fg : bk; break;            /* (pat)Copy */
            case 1: case 9: if (bit) *px = fg; break;              /* (pat)Or */
            case 2: case 10: if (bit) *px ^= 0xFF; break;          /* (pat)Xor */
            case 3: case 11: if (bit) *px = bk; break;             /* (pat)Bic */
            case 4: case 12: *px = bit ? bk : fg; break;           /* notPatCopy */
            case 5: case 13: if (!bit) *px = fg; break;
            case 6: case 14: if (!bit) *px ^= 0xFF; break;
            case 7: case 15: if (!bit) *px = bk; break;
            case 36: if (bit) *px = fg; break;                     /* transparent */
            default: *px = bit ? fg : bk; break;
            }
        }
    }
}

static const uint8_t PAT_BLACK[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t PAT_WHITE[8] = { 0 };

static void paint_rect(QRect r)
{
    uint32_t port = qd_the_port();
    PortPats *pp = port_pats(port);
    qd_fill_rect(port, r, pp->pen, (int16_t)rdw(port + P_pnMode), qd_fg_index(port), qd_bk_index(port));
}

static void erase_rect(QRect r)
{
    uint32_t port = qd_the_port();
    PortPats *pp = port_pats(port);
    qd_fill_rect(port, r, pp->bk, 8, qd_bk_index(port), qd_bk_index(port));
}

static void invert_rect(QRect r) { qd_fill_rect(qd_the_port(), r, PAT_BLACK, 10, 0, 0); }

static void fill_rect_pat(QRect r, uint32_t patp)
{
    uint8_t pat[8];
    memcpy(pat, HP(patp), 8);
    uint32_t port = qd_the_port();
    qd_fill_rect(port, r, pat, 8, qd_fg_index(port), qd_bk_index(port));
}

static void frame_rect(QRect r)
{
    uint32_t port = qd_the_port();
    int pw = (int16_t)rdw(port + P_pnSize + 2), ph = (int16_t)rdw(port + P_pnSize);
    if (rect_empty(r)) return;
    if (r.right - r.left <= 2 * pw || r.bottom - r.top <= 2 * ph) { paint_rect(r); return; }
    QRect t = { r.top, r.left, (int16_t)(r.top + ph), r.right };
    QRect b = { (int16_t)(r.bottom - ph), r.left, r.bottom, r.right };
    QRect l = { (int16_t)(r.top + ph), r.left, (int16_t)(r.bottom - ph), (int16_t)(r.left + pw) };
    QRect rr = { (int16_t)(r.top + ph), (int16_t)(r.right - pw), (int16_t)(r.bottom - ph), r.right };
    paint_rect(t); paint_rect(b); paint_rect(l); paint_rect(rr);
}

static void draw_line_to(int16_t h, int16_t v)
{
    uint32_t port = qd_the_port();
    int x0 = (int16_t)rdw(port + P_pnLoc + 2), y0 = (int16_t)rdw(port + P_pnLoc);
    int pw = (int16_t)rdw(port + P_pnSize + 2), ph = (int16_t)rdw(port + P_pnSize);
    wrw(port + P_pnLoc, (uint16_t)v); wrw(port + P_pnLoc + 2, (uint16_t)h);
    if ((int16_t)rdw(port + P_pnVis) < 0) return;
    int dx = abs(h - x0), dy = -abs(v - y0), sx = x0 < h ? 1 : -1, sy = y0 < v ? 1 : -1, err = dx + dy;
    for (;;) {
        QRect r = { (int16_t)y0, (int16_t)x0, (int16_t)(y0 + ph), (int16_t)(x0 + pw) };
        paint_rect(r);
        if (x0 == h && y0 == v) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ------------------------------------------------------------ CopyBits */
static void copy_bits(uint32_t srcbm, uint32_t dstbm, QRect sr, QRect dr, int mode, uint32_t mask)
{
    PixDesc s, d;
    if (!qd_bitmap_pixels(srcbm, &s) || !qd_bitmap_pixels(dstbm, &d)) return;
    if (rect_empty(sr) || rect_empty(dr)) return;
    QRect clip = d.bounds;
    uint32_t port = qd_the_port();
    if (port && dstbm == port + P_portBits) clip = rect_sect(clip, qd_port_clip(port));
    if (mask) clip = rect_sect(clip, qd_rgn_box(mask));
    QRect dc = rect_sect(dr, clip);
    if (rect_empty(dc)) return;
    if (d.base == g_screen_fb) {
        QRect sdc = { (int16_t)(dc.top - d.bounds.top), (int16_t)(dc.left - d.bounds.left),
                      (int16_t)(dc.bottom - d.bounds.top), (int16_t)(dc.right - d.bounds.left) };
        hitext_damage_screen(sdc);
    }
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top, dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    bool same = sw == dw && sh == dh;
    uint8_t bkidx = port ? qd_bk_index(port) : 0;
    mode &= 0x7F;
    if (s.depth == 8 && d.depth == 8) {
        for (int y = dc.top; y < dc.bottom; y++) {
            int sy = same ? sr.top + (y - dr.top) : sr.top + (int)((long)(y - dr.top) * sh / dh);
            if (sy < s.bounds.top || sy >= s.bounds.bottom) continue;
            uint8_t *srow = HP(s.base + (uint32_t)(sy - s.bounds.top) * s.rowbytes);
            uint8_t *drow = HP(d.base + (uint32_t)(y - d.bounds.top) * d.rowbytes);
            if (same && mode == 0) {
                int x0 = dc.left, x1 = dc.right;
                int sx0 = sr.left + (x0 - dr.left);
                if (sx0 < s.bounds.left) { x0 += s.bounds.left - sx0; sx0 = s.bounds.left; }
                if (sx0 + (x1 - x0) > s.bounds.right) x1 = x0 + (s.bounds.right - sx0);
                if (x1 > x0) memmove(drow + (x0 - d.bounds.left), srow + (sx0 - s.bounds.left), (size_t)(x1 - x0));
                continue;
            }
            for (int x = dc.left; x < dc.right; x++) {
                int sx = same ? sr.left + (x - dr.left) : sr.left + (int)((long)(x - dr.left) * sw / dw);
                if (sx < s.bounds.left || sx >= s.bounds.right) continue;
                uint8_t v = srow[sx - s.bounds.left];
                uint8_t *px = drow + (x - d.bounds.left);
                switch (mode) {
                case 1: if (v != 0) *px = v; break;          /* srcOr: white is transparent */
                case 2: *px ^= v; break;                      /* srcXor */
                case 3: if (v != 0) *px = 0; break;           /* srcBic */
                case 36: if (v != bkidx) *px = v; break;      /* transparent */
                default: *px = v; break;
                }
            }
        }
    } else if (s.depth == 1 && d.depth == 8) {
        uint8_t fg = port ? qd_fg_index(port) : 255, bk = bkidx;
        for (int y = dc.top; y < dc.bottom; y++) {
            int sy = sr.top + (int)((long)(y - dr.top) * sh / dh);
            if (sy < s.bounds.top || sy >= s.bounds.bottom) continue;
            uint8_t *srow = HP(s.base + (uint32_t)(sy - s.bounds.top) * s.rowbytes);
            uint8_t *drow = HP(d.base + (uint32_t)(y - d.bounds.top) * d.rowbytes);
            for (int x = dc.left; x < dc.right; x++) {
                int sx = sr.left + (int)((long)(x - dr.left) * sw / dw) - s.bounds.left;
                if (sx < 0) continue;
                bool bit = (srow[sx >> 3] >> (7 - (sx & 7))) & 1;
                uint8_t *px = drow + (x - d.bounds.left);
                if (mode == 1 || mode == 36) { if (bit) *px = fg; }
                else *px = bit ? fg : bk;
            }
        }
    } else {
        log_warn("CopyBits depth %d -> %d not supported", s.depth, d.depth);
    }
}

/* ============================================================ traps */
static void T_InitGraf(uint16_t op)
{
    (void)op;
    uint32_t p = ARG32();
    qd_globals = p;
    wrl(A[5], p);
    wrl(p, 0);
    /* patterns: white -8, black -16, gray -24, ltGray -32, dkGray -40 */
    memset(HP(p - 8), 0x00, 8);
    memset(HP(p - 16), 0xFF, 8);
    for (int i = 0; i < 8; i++) {
        wrb(p - 24 + i, (i & 1) ? 0x55 : 0xAA);
        wrb(p - 32 + i, (i & 1) ? 0x22 : 0x88);
        wrb(p - 40 + i, (i & 1) ? 0xDD : 0x77);
    }
    /* screenBits at -122 */
    wrl(p - 122, g_screen_fb);
    wrw(p - 118, SCREEN_W);
    QRect scr = { 0, 0, SCREEN_H, SCREEN_W };
    wr_rect(p - 116, scr);
    /* arrow cursor -108 (68 bytes) left blank; randSeed -126 */
    wrl(p - 126, 1);
}

static void T_OpenPort(uint16_t op) { (void)op; uint32_t port = ARG32(); qd_init_port(port, false); qd_set_port(port); }
static void T_InitPort(uint16_t op) { (void)op; uint32_t port = ARG32(); qd_init_port(port, false); qd_set_port(port); }
static void T_OpenCPort(uint16_t op) { (void)op; uint32_t port = ARG32(); qd_init_port(port, true); qd_set_port(port); }
static void T_ClosePort(uint16_t op)
{
    (void)op;
    uint32_t port = ARG32();
    if (rdl(port + P_visRgn)) mm_dispose_handle(rdl(port + P_visRgn));
    if (rdl(port + P_clipRgn)) mm_dispose_handle(rdl(port + P_clipRgn));
    wrl(port + P_visRgn, 0); wrl(port + P_clipRgn, 0);
}
static void T_SetPort(uint16_t op) { (void)op; qd_set_port(ARG32()); }
static void T_GetPort(uint16_t op) { (void)op; uint32_t p = ARG32(); wrl(p, qd_the_port()); }

static void T_SetOrigin(uint16_t op)
{
    (void)op;
    int16_t v = ARGS16(), h = ARGS16();
    uint32_t port = qd_the_port();
    QRect pr = rd_rect(port + P_portRect);
    int dh = h - pr.left, dv = v - pr.top;
    if (!dh && !dv) return;
    pr.left += dh; pr.right += dh; pr.top += dv; pr.bottom += dv;
    wr_rect(port + P_portRect, pr);
    if (is_color_port(port)) {
        uint32_t pm = rdl(rdl(port + P_portBits));
        QRect b = rd_rect(pm + PM_bounds);
        b.left += dh; b.right += dh; b.top += dv; b.bottom += dv;
        wr_rect(pm + PM_bounds, b);
    } else {
        QRect b = rd_rect(port + 8);
        b.left += dh; b.right += dh; b.top += dv; b.bottom += dv;
        wr_rect(port + 8, b);
    }
    QRect vr = qd_rgn_box(rdl(port + P_visRgn));
    vr.left += dh; vr.right += dh; vr.top += dv; vr.bottom += dv;
    qd_set_rgn(rdl(port + P_visRgn), vr);
}

static void T_PortSize(uint16_t op)
{
    (void)op;
    int16_t h = ARGS16(), w = ARGS16();
    uint32_t port = qd_the_port();
    QRect pr = rd_rect(port + P_portRect);
    pr.right = pr.left + w; pr.bottom = pr.top + h;
    wr_rect(port + P_portRect, pr);
}

static void T_MovePortTo(uint16_t op) { (void)op; (void)ARG16(); (void)ARG16(); }
static void T_ClipRect(uint16_t op) { (void)op; uint32_t r = ARG32(); qd_set_rgn(rdl(qd_the_port() + P_clipRgn), rd_rect(r)); }
static void T_SetClip(uint16_t op) { (void)op; uint32_t rgn = ARG32(); qd_set_rgn(rdl(qd_the_port() + P_clipRgn), qd_rgn_box(rgn)); }
static void T_GetClip(uint16_t op) { (void)op; uint32_t rgn = ARG32(); qd_set_rgn(rgn, qd_rgn_box(rdl(qd_the_port() + P_clipRgn))); }

static void T_BackPat(uint16_t op) { (void)op; uint32_t p = ARG32(); memcpy(port_pats(qd_the_port())->bk, HP(p), 8); }
static void T_PenPat(uint16_t op) { (void)op; uint32_t p = ARG32(); memcpy(port_pats(qd_the_port())->pen, HP(p), 8); }
static void T_PenSize(uint16_t op) { (void)op; int16_t h = ARGS16(), w = ARGS16(); uint32_t port = qd_the_port(); wrw(port + P_pnSize, (uint16_t)h); wrw(port + P_pnSize + 2, (uint16_t)w); }
static void T_PenMode(uint16_t op) { (void)op; wrw(qd_the_port() + P_pnMode, ARG16()); }
static void T_PenNormal(uint16_t op)
{
    (void)op;
    uint32_t port = qd_the_port();
    wrw(port + P_pnSize, 1); wrw(port + P_pnSize + 2, 1); wrw(port + P_pnMode, 8);
    memset(port_pats(port)->pen, 0xFF, 8);
}
static void T_HidePen(uint16_t op) { (void)op; uint32_t port = qd_the_port(); wrw(port + P_pnVis, (uint16_t)((int16_t)rdw(port + P_pnVis) - 1)); }
static void T_ShowPen(uint16_t op) { (void)op; uint32_t port = qd_the_port(); wrw(port + P_pnVis, (uint16_t)((int16_t)rdw(port + P_pnVis) + 1)); }
static void T_GetPen(uint16_t op) { (void)op; uint32_t p = ARG32(); wrl(p, rdl(qd_the_port() + P_pnLoc)); }
static void T_GetPenState(uint16_t op)
{
    (void)op;
    uint32_t p = ARG32(), port = qd_the_port();
    wrl(p, rdl(port + P_pnLoc)); wrl(p + 4, rdl(port + P_pnSize)); wrw(p + 8, rdw(port + P_pnMode));
    memcpy(HP(p + 10), port_pats(port)->pen, 8);
}
static void T_SetPenState(uint16_t op)
{
    (void)op;
    uint32_t p = ARG32(), port = qd_the_port();
    wrl(port + P_pnLoc, rdl(p)); wrl(port + P_pnSize, rdl(p + 4)); wrw(port + P_pnMode, rdw(p + 8));
    memcpy(port_pats(port)->pen, HP(p + 10), 8);
}
static void T_MoveTo(uint16_t op) { (void)op; int16_t v = ARGS16(), h = ARGS16(); uint32_t port = qd_the_port(); wrw(port + P_pnLoc, (uint16_t)v); wrw(port + P_pnLoc + 2, (uint16_t)h); }
static void T_Move(uint16_t op)
{
    (void)op;
    int16_t dv = ARGS16(), dh = ARGS16(); uint32_t port = qd_the_port();
    wrw(port + P_pnLoc, (uint16_t)((int16_t)rdw(port + P_pnLoc) + dv));
    wrw(port + P_pnLoc + 2, (uint16_t)((int16_t)rdw(port + P_pnLoc + 2) + dh));
}
static void T_LineTo(uint16_t op) { (void)op; int16_t v = ARGS16(), h = ARGS16(); draw_line_to(h, v); }
static void T_Line(uint16_t op)
{
    (void)op;
    int16_t dv = ARGS16(), dh = ARGS16(); uint32_t port = qd_the_port();
    draw_line_to((int16_t)((int16_t)rdw(port + P_pnLoc + 2) + dh), (int16_t)((int16_t)rdw(port + P_pnLoc) + dv));
}

/* rect utilities */
static void T_SetRect(uint16_t op)
{
    (void)op;
    int16_t b = ARGS16(), r = ARGS16(), t = ARGS16(), l = ARGS16(); uint32_t p = ARG32();
    QRect q = { t, l, b, r }; wr_rect(p, q);
}
static void T_OffsetRect(uint16_t op)
{
    (void)op;
    int16_t dv = ARGS16(), dh = ARGS16(); uint32_t p = ARG32();
    QRect q = rd_rect(p); q.top += dv; q.bottom += dv; q.left += dh; q.right += dh; wr_rect(p, q);
}
static void T_InsetRect(uint16_t op)
{
    (void)op;
    int16_t dv = ARGS16(), dh = ARGS16(); uint32_t p = ARG32();
    QRect q = rd_rect(p); q.top += dv; q.bottom -= dv; q.left += dh; q.right -= dh;
    if (rect_empty(q)) { QRect z = { 0, 0, 0, 0 }; q = z; }
    wr_rect(p, q);
}
static void T_SectRect(uint16_t op)
{
    (void)op;
    uint32_t d = ARG32(), b = ARG32(), a = ARG32();
    QRect r = rect_sect(rd_rect(a), rd_rect(b));
    wr_rect(d, r);
    RET8(!rect_empty(r));
}
static void T_UnionRect(uint16_t op) { (void)op; uint32_t d = ARG32(), b = ARG32(), a = ARG32(); wr_rect(d, rect_union(rd_rect(a), rd_rect(b))); }
static void T_PtInRect(uint16_t op)
{
    (void)op;
    uint32_t r = ARG32(); uint32_t pt = ARG32();
    int16_t v = (int16_t)(pt >> 16), h = (int16_t)pt;
    QRect q = rd_rect(r);
    RET8(v >= q.top && v < q.bottom && h >= q.left && h < q.right);
}
static void T_EqualRect(uint16_t op) { (void)op; uint32_t b = ARG32(), a = ARG32(); RET8(memcmp(HP(a), HP(b), 8) == 0); }
static void T_EmptyRect(uint16_t op) { (void)op; uint32_t a = ARG32(); RET8(rect_empty(rd_rect(a))); }
static void T_Pt2Rect(uint16_t op)
{
    (void)op;
    uint32_t d = ARG32(), p2 = ARG32(), p1 = ARG32();
    int16_t v1 = (int16_t)(p1 >> 16), h1 = (int16_t)p1, v2 = (int16_t)(p2 >> 16), h2 = (int16_t)p2;
    QRect r = { v1 < v2 ? v1 : v2, h1 < h2 ? h1 : h2, v1 > v2 ? v1 : v2, h1 > h2 ? h1 : h2 };
    wr_rect(d, r);
}
static void T_SetPt(uint16_t op) { (void)op; int16_t v = ARGS16(), h = ARGS16(); uint32_t p = ARG32(); wrw(p, (uint16_t)v); wrw(p + 2, (uint16_t)h); }
static void T_AddPt(uint16_t op)
{
    (void)op;
    uint32_t dst = ARG32(), src = ARG32();
    wrw(dst, (uint16_t)((int16_t)rdw(dst) + (int16_t)(src >> 16)));
    wrw(dst + 2, (uint16_t)((int16_t)rdw(dst + 2) + (int16_t)src));
}
static void T_SubPt(uint16_t op)
{
    (void)op;
    uint32_t dst = ARG32(), src = ARG32();
    wrw(dst, (uint16_t)((int16_t)rdw(dst) - (int16_t)(src >> 16)));
    wrw(dst + 2, (uint16_t)((int16_t)rdw(dst + 2) - (int16_t)src));
}
static void T_EqualPt(uint16_t op) { (void)op; uint32_t b = ARG32(), a = ARG32(); RET8(a == b); }

/* shape drawing (ovals/round rects approximated by rects) */
static void T_PaintRect(uint16_t op) { (void)op; paint_rect(rd_rect(ARG32())); }
static void T_FrameRect(uint16_t op) { (void)op; frame_rect(rd_rect(ARG32())); }
static void T_EraseRect(uint16_t op) { (void)op; erase_rect(rd_rect(ARG32())); }
static void T_InverRect(uint16_t op) { (void)op; invert_rect(rd_rect(ARG32())); }
static void T_FillRect(uint16_t op) { (void)op; uint32_t pat = ARG32(); fill_rect_pat(rd_rect(ARG32()), pat); }
static void T_FrameRoundRect(uint16_t op) { (void)op; (void)ARG16(); (void)ARG16(); frame_rect(rd_rect(ARG32())); }
static void T_PaintRoundRect(uint16_t op) { (void)op; (void)ARG16(); (void)ARG16(); paint_rect(rd_rect(ARG32())); }
static void T_EraseRoundRect(uint16_t op) { (void)op; (void)ARG16(); (void)ARG16(); erase_rect(rd_rect(ARG32())); }
static void T_InverRoundRect(uint16_t op) { (void)op; (void)ARG16(); (void)ARG16(); invert_rect(rd_rect(ARG32())); }
static void T_FillRoundRect(uint16_t op) { (void)op; uint32_t pat = ARG32(); (void)ARG16(); (void)ARG16(); fill_rect_pat(rd_rect(ARG32()), pat); }
static void T_FrameOval(uint16_t op) { (void)op; frame_rect(rd_rect(ARG32())); }
static void T_PaintOval(uint16_t op) { (void)op; paint_rect(rd_rect(ARG32())); }
static void T_EraseOval(uint16_t op) { (void)op; erase_rect(rd_rect(ARG32())); }

/* regions */
static void T_NewRgn(uint16_t op) { (void)op; QRect z = { 0, 0, 0, 0 }; RET32(qd_new_rgn(z)); }
static void T_DisposeRgn(uint16_t op) { (void)op; uint32_t h = ARG32(); if (h) mm_dispose_handle(h); }
static void T_CopyRgn(uint16_t op) { (void)op; uint32_t d = ARG32(), s = ARG32(); qd_set_rgn(d, qd_rgn_box(s)); }
static void T_SetEmptyRgn(uint16_t op) { (void)op; QRect z = { 0, 0, 0, 0 }; qd_set_rgn(ARG32(), z); }
static void T_SetRectRgn(uint16_t op)
{
    (void)op;
    int16_t b = ARGS16(), r = ARGS16(), t = ARGS16(), l = ARGS16(); uint32_t h = ARG32();
    QRect q = { t, l, b, r }; qd_set_rgn(h, q);
}
static void T_RectRgn(uint16_t op) { (void)op; uint32_t r = ARG32(), h = ARG32(); qd_set_rgn(h, rd_rect(r)); }
static void T_OffsetRgn(uint16_t op)
{
    (void)op;
    int16_t dv = ARGS16(), dh = ARGS16(); uint32_t h = ARG32();
    QRect q = qd_rgn_box(h);
    if (!rect_empty(q)) { q.top += dv; q.bottom += dv; q.left += dh; q.right += dh; }
    qd_set_rgn(h, q);
}
static void T_InsetRgn(uint16_t op)
{
    (void)op;
    int16_t dv = ARGS16(), dh = ARGS16(); uint32_t h = ARG32();
    QRect q = qd_rgn_box(h); q.top += dv; q.bottom -= dv; q.left += dh; q.right -= dh; qd_set_rgn(h, q);
}
static void T_SectRgn(uint16_t op) { (void)op; uint32_t d = ARG32(), b = ARG32(), a = ARG32(); qd_set_rgn(d, rect_sect(qd_rgn_box(a), qd_rgn_box(b))); }
static void T_UnionRgn(uint16_t op) { (void)op; uint32_t d = ARG32(), b = ARG32(), a = ARG32(); qd_set_rgn(d, rect_union(qd_rgn_box(a), qd_rgn_box(b))); }
static void T_DiffRgn(uint16_t op)
{
    (void)op;
    uint32_t d = ARG32(), b = ARG32(), a = ARG32();
    QRect ra = qd_rgn_box(a), rb = qd_rgn_box(b);
    QRect s = rect_sect(ra, rb);
    if (!rect_empty(s) && s.top == ra.top && s.left == ra.left && s.bottom == ra.bottom && s.right == ra.right) {
        QRect z = { 0, 0, 0, 0 }; ra = z;
    }
    qd_set_rgn(d, ra);
}
static void T_XorRgn(uint16_t op) { (void)op; uint32_t d = ARG32(), b = ARG32(), a = ARG32(); qd_set_rgn(d, rect_union(qd_rgn_box(a), qd_rgn_box(b))); }
static void T_EmptyRgn(uint16_t op) { (void)op; RET8(rect_empty(qd_rgn_box(ARG32()))); }
static void T_EqualRgn(uint16_t op) { (void)op; uint32_t b = ARG32(), a = ARG32(); QRect x = qd_rgn_box(a), y = qd_rgn_box(b); RET8(memcmp(&x, &y, sizeof x) == 0); }
static void T_PtInRgn(uint16_t op)
{
    (void)op;
    uint32_t h = ARG32(), pt = ARG32();
    int16_t v = (int16_t)(pt >> 16), x = (int16_t)pt; QRect q = qd_rgn_box(h);
    RET8(v >= q.top && v < q.bottom && x >= q.left && x < q.right);
}
static void T_RectInRgn(uint16_t op) { (void)op; uint32_t h = ARG32(), r = ARG32(); RET8(!rect_empty(rect_sect(rd_rect(r), qd_rgn_box(h)))); }
static uint32_t open_rgn_target; static QRect open_rgn_box; static bool open_rgn_active;
static void T_OpenRgn(uint16_t op) { (void)op; open_rgn_active = true; QRect z = { 0, 0, 0, 0 }; open_rgn_box = z; open_rgn_target = 0; }
static void T_CloseRgn(uint16_t op) { (void)op; uint32_t h = ARG32(); qd_set_rgn(h, open_rgn_box); open_rgn_active = false; (void)open_rgn_target; }
static void T_PaintRgn(uint16_t op) { (void)op; paint_rect(qd_rgn_box(ARG32())); }
static void T_EraseRgn(uint16_t op) { (void)op; erase_rect(qd_rgn_box(ARG32())); }
static void T_FrameRgn(uint16_t op) { (void)op; frame_rect(qd_rgn_box(ARG32())); }
static void T_InverRgn(uint16_t op) { (void)op; invert_rect(qd_rgn_box(ARG32())); }
static void T_FillRgn(uint16_t op) { (void)op; uint32_t pat = ARG32(); fill_rect_pat(qd_rgn_box(ARG32()), pat); }

/* colours */
static void set_rgb(uint32_t a, uint16_t r, uint16_t g, uint16_t b) { wrw(a, r); wrw(a + 2, g); wrw(a + 4, b); }
static void T_ForeColor(uint16_t op)
{
    (void)op;
    uint32_t c = ARG32(); uint32_t port = qd_the_port();
    uint16_t r, g, b; classic_rgb(c, &r, &g, &b);
    wrl(port + P_fgColor, c); set_rgb(port + P_rgbFg, r, g, b);
}
static void T_BackColor(uint16_t op)
{
    (void)op;
    uint32_t c = ARG32(); uint32_t port = qd_the_port();
    uint16_t r, g, b; classic_rgb(c, &r, &g, &b);
    wrl(port + P_bkColor, c); set_rgb(port + P_rgbBk, r, g, b);
}
static void T_RGBForeColor(uint16_t op) { (void)op; uint32_t p = ARG32(); memcpy(HP(qd_the_port() + P_rgbFg), HP(p), 6); }
static void T_RGBBackColor(uint16_t op) { (void)op; uint32_t p = ARG32(); memcpy(HP(qd_the_port() + P_rgbBk), HP(p), 6); }
static void T_GetForeColor(uint16_t op) { (void)op; uint32_t p = ARG32(); memcpy(HP(p), HP(qd_the_port() + P_rgbFg), 6); }
static void T_GetBackColor(uint16_t op) { (void)op; uint32_t p = ARG32(); memcpy(HP(p), HP(qd_the_port() + P_rgbBk), 6); }

static void T_CopyBits(uint16_t op)
{
    (void)op;
    uint32_t mask = ARG32(); int16_t mode = ARGS16(); uint32_t dr = ARG32(), sr = ARG32(), dst = ARG32(), src = ARG32();
    copy_bits(src, dst, rd_rect(sr), rd_rect(dr), mode, mask);
}

static void T_CopyMask(uint16_t op)
{
    (void)op;
    uint32_t dr = ARG32(), mr = ARG32(), sr = ARG32(), dst = ARG32(), msk = ARG32(), src = ARG32();
    (void)mr; (void)msk;
    copy_bits(src, dst, rd_rect(sr), rd_rect(dr), 0, 0);
}

static void T_ScrollRect(uint16_t op)
{
    (void)op;
    uint32_t upd = ARG32(); int16_t dv = ARGS16(), dh = ARGS16(); uint32_t r = ARG32();
    QRect src = rd_rect(r), dst = src;
    dst.top += dv; dst.bottom += dv; dst.left += dh; dst.right += dh;
    uint32_t port = qd_the_port();
    copy_bits(port + P_portBits, port + P_portBits, src, dst, 0, 0);
    if (upd) qd_set_rgn(upd, src);
}

static void T_LocalToGlobal(uint16_t op)
{
    (void)op;
    uint32_t p = ARG32(); PixDesc pd;
    if (!qd_port_pixels(qd_the_port(), &pd)) return;
    wrw(p, (uint16_t)((int16_t)rdw(p) - pd.bounds.top));
    wrw(p + 2, (uint16_t)((int16_t)rdw(p + 2) - pd.bounds.left));
}
static void T_GlobalToLocal(uint16_t op)
{
    (void)op;
    uint32_t p = ARG32(); PixDesc pd;
    if (!qd_port_pixels(qd_the_port(), &pd)) return;
    wrw(p, (uint16_t)((int16_t)rdw(p) + pd.bounds.top));
    wrw(p + 2, (uint16_t)((int16_t)rdw(p + 2) + pd.bounds.left));
}

static void T_GetPixel(uint16_t op)
{
    (void)op;
    int16_t v = ARGS16(), h = ARGS16(); PixDesc pd;
    bool black = false;
    if (qd_port_pixels(qd_the_port(), &pd) && h >= pd.bounds.left && h < pd.bounds.right && v >= pd.bounds.top && v < pd.bounds.bottom)
        black = rdb(pd.base + (uint32_t)(v - pd.bounds.top) * pd.rowbytes + (h - pd.bounds.left)) != 0;
    RET8(black);
}

static void T_GetCPixel(uint16_t op)
{
    (void)op;
    uint32_t rgb = ARG32(); int16_t v = ARGS16(), h = ARGS16(); PixDesc pd;
    uint16_t r = 0, g = 0, b = 0;
    if (qd_port_pixels(qd_the_port(), &pd) && h >= pd.bounds.left && h < pd.bounds.right && v >= pd.bounds.top && v < pd.bounds.bottom)
        qd_index_to_rgb(pd.ctab, (uint8_t)rdb(pd.base + (uint32_t)(v - pd.bounds.top) * pd.rowbytes + (h - pd.bounds.left)), &r, &g, &b);
    set_rgb(rgb, r, g, b);
}

static void T_SetCPixel(uint16_t op)
{
    (void)op;
    uint32_t rgb = ARG32(); int16_t v = ARGS16(), h = ARGS16();
    uint32_t port = qd_the_port(); PixDesc pd;
    if (!qd_port_pixels(port, &pd)) return;
    if (h >= pd.bounds.left && h < pd.bounds.right && v >= pd.bounds.top && v < pd.bounds.bottom)
        wrb(pd.base + (uint32_t)(v - pd.bounds.top) * pd.rowbytes + (h - pd.bounds.left),
            qd_rgb_to_index(pd.ctab, (uint16_t)rdw(rgb), (uint16_t)rdw(rgb + 2), (uint16_t)rdw(rgb + 4)));
}

/* Toolbox Random: Park-Miller on qd.randSeed */
static void T_Random(uint16_t op)
{
    (void)op;
    uint32_t seedp = qd_globals - 126;
    uint32_t s = rdl(seedp);
    if (s == 0) s = 1;
    uint64_t x = (uint64_t)(s & 0x7FFFFFFF) * 16807u;
    uint32_t r = (uint32_t)(x % 0x7FFFFFFFu);
    wrl(seedp, r);
    uint16_t v = (uint16_t)r;
    if (v == 0x8000) v = 0;
    RET16(v);
}

/* fixed point / misc math */
static void T_FixMul(uint16_t op) { (void)op; int32_t b = (int32_t)ARG32(), a = (int32_t)ARG32(); RET32((uint32_t)(int32_t)(((int64_t)a * b) >> 16)); }
static void T_FixRatio(uint16_t op) { (void)op; int16_t d = ARGS16(), n = ARGS16(); RET32(d ? (uint32_t)(((int64_t)n << 16) / d) : 0x7FFFFFFF); }
static void T_FixRound(uint16_t op) { (void)op; int32_t f = (int32_t)ARG32(); RET16((uint16_t)((f + 0x8000) >> 16)); }
static void T_HiWord(uint16_t op) { (void)op; RET16(ARG32() >> 16); }
static void T_LoWord(uint16_t op) { (void)op; RET16(ARG32() & 0xFFFF); }
static void T_LongMul(uint16_t op)
{
    (void)op;
    uint32_t res = ARG32(); int32_t b = (int32_t)ARG32(), a = (int32_t)ARG32();
    int64_t p = (int64_t)a * b;
    wrl(res, (uint32_t)((uint64_t)p >> 32)); wrl(res + 4, (uint32_t)p);
}
static void T_BitTst(uint16_t op) { (void)op; int32_t n = (int32_t)ARG32(); uint32_t p = ARG32(); RET8((rdb(p + n / 8) >> (7 - n % 8)) & 1); }
static void T_BitSet(uint16_t op) { (void)op; int32_t n = (int32_t)ARG32(); uint32_t p = ARG32(); wrb(p + n / 8, rdb(p + n / 8) | (0x80 >> (n % 8))); }
static void T_BitClr(uint16_t op) { (void)op; int32_t n = (int32_t)ARG32(); uint32_t p = ARG32(); wrb(p + n / 8, rdb(p + n / 8) & ~(0x80 >> (n % 8))); }

/* cursor: Mac semantics, visible while the hide level is 0 */
static int cursor_level;
static void T_nop0(uint16_t op) { (void)op; }
static void T_InitCursor(uint16_t op) { (void)op; cursor_level = 0; plat_show_cursor(true); }
static void T_HideCursor(uint16_t op) { (void)op; if (--cursor_level == -1) plat_show_cursor(false); }
static void T_ShowCursor(uint16_t op) { (void)op; if (cursor_level < 0 && ++cursor_level == 0) plat_show_cursor(true); }
static void T_SetCursor(uint16_t op) { (void)op; (void)ARG32(); }
static void T_GetCursor(uint16_t op) { (void)op; int16_t id = ARGS16(); RET32(res_get(OSTYPE('C','U','R','S'), id, false)); }

static void T_StdText(uint16_t op) { (void)op; (void)ARG32(); (void)ARG32(); (void)ARG32(); (void)ARG16(); }

void qd_init(void)
{
    g_screen_fb = mm_new_ptr(SCREEN_W * SCREEN_H, true);
    /* main GDevice + PixMap + device colour table */
    uint32_t ctab = qd_new_ctab_from(NULL, 0);
    uint32_t pmh = mm_new_handle(PIXMAP_SIZE, true);
    uint32_t pm = rdl(pmh);
    QRect scr = { 0, 0, SCREEN_H, SCREEN_W };
    wrl(pm + PM_baseAddr, g_screen_fb);
    wrw(pm + PM_rowBytes, 0x8000 | SCREEN_W);
    wr_rect(pm + PM_bounds, scr);
    wrl(pm + 22, 0x00480000); wrl(pm + 26, 0x00480000);
    wrw(pm + PM_pixelSize, 8); wrw(pm + 34, 1); wrw(pm + 36, 8);
    wrl(pm + PM_pmTable, ctab);
    g_main_device = mm_new_handle(GDEVICE_SIZE, true);
    uint32_t gd = rdl(g_main_device);
    wrw(gd + GD_type, 0); /* clutType */
    wrw(gd + GD_flags, (1 << 0) | (1 << 11) | (1 << 13) | (1 << 14) | (1 << 15));
    wrl(gd + GD_PMap, pmh);
    wrl(gd + GD_nextGD, 0);
    wr_rect(gd + GD_rect, scr);
    g_cur_device = g_main_device;
    wrl(LM_MainDevice, g_main_device);
    wrl(LM_TheGDevice, g_main_device);
    wrl(LM_DeviceList, g_main_device);
    wrl(LM_ScrnBase, g_screen_fb);
    for (int i = 0; i < 256; i++) {
        uint16_t r, g, b; qd_index_to_rgb(ctab, (uint8_t)i, &r, &g, &b);
        g_hw_pal[i][0] = (uint8_t)(r >> 8); g_hw_pal[i][1] = (uint8_t)(g >> 8); g_hw_pal[i][2] = (uint8_t)(b >> 8);
    }
    /* GrayRgn: the desktop */
    wrl(LM_GrayRgn, qd_new_rgn(scr));
    wrw(LM_MBarHeight, 20);
    text_init();
}

void qd_register_traps(void)
{
    trap_register(0xA86E, T_InitGraf, "InitGraf");
    trap_register(0xA86F, T_OpenPort, "OpenPort");
    trap_register(0xA86D, T_InitPort, "InitPort");
    trap_register(0xAA00, T_OpenCPort, "OpenCPort");
    trap_register(0xAA01, T_OpenCPort, "InitCPort");
    trap_register(0xA87D, T_ClosePort, "ClosePort");
    trap_register(0xAA02, T_ClosePort, "CloseCPort");
    trap_register(0xA873, T_SetPort, "SetPort");
    trap_register(0xA874, T_GetPort, "GetPort");
    trap_register(0xA878, T_SetOrigin, "SetOrigin");
    trap_register(0xA876, T_PortSize, "PortSize");
    trap_register(0xA877, T_MovePortTo, "MovePortTo");
    trap_register(0xA87B, T_ClipRect, "ClipRect");
    trap_register(0xA879, T_SetClip, "SetClip");
    trap_register(0xA87A, T_GetClip, "GetClip");
    trap_register(0xA87C, T_BackPat, "BackPat");
    trap_register(0xA89D, T_PenPat, "PenPat");
    trap_register(0xA89B, T_PenSize, "PenSize");
    trap_register(0xA89C, T_PenMode, "PenMode");
    trap_register(0xA89E, T_PenNormal, "PenNormal");
    trap_register(0xA896, T_HidePen, "HidePen");
    trap_register(0xA897, T_ShowPen, "ShowPen");
    trap_register(0xA89A, T_GetPen, "GetPen");
    trap_register(0xA898, T_GetPenState, "GetPenState");
    trap_register(0xA899, T_SetPenState, "SetPenState");
    trap_register(0xA893, T_MoveTo, "MoveTo");
    trap_register(0xA894, T_Move, "Move");
    trap_register(0xA891, T_LineTo, "LineTo");
    trap_register(0xA892, T_Line, "Line");
    trap_register(0xA8A7, T_SetRect, "SetRect");
    trap_register(0xA8A8, T_OffsetRect, "OffsetRect");
    trap_register(0xA8A9, T_InsetRect, "InsetRect");
    trap_register(0xA8AA, T_SectRect, "SectRect");
    trap_register(0xA8AB, T_UnionRect, "UnionRect");
    trap_register(0xA8AD, T_PtInRect, "PtInRect");
    trap_register(0xA8A6, T_EqualRect, "EqualRect");
    trap_register(0xA8AE, T_EmptyRect, "EmptyRect");
    trap_register(0xA8AC, T_Pt2Rect, "Pt2Rect");
    trap_register(0xA880, T_SetPt, "SetPt");
    trap_register(0xA87E, T_AddPt, "AddPt");
    trap_register(0xA87F, T_SubPt, "SubPt");
    trap_register(0xA881, T_EqualPt, "EqualPt");
    trap_register(0xA8A2, T_PaintRect, "PaintRect");
    trap_register(0xA8A1, T_FrameRect, "FrameRect");
    trap_register(0xA8A3, T_EraseRect, "EraseRect");
    trap_register(0xA8A4, T_InverRect, "InvertRect");
    trap_register(0xA8A5, T_FillRect, "FillRect");
    trap_register(0xA8B0, T_FrameRoundRect, "FrameRoundRect");
    trap_register(0xA8B1, T_PaintRoundRect, "PaintRoundRect");
    trap_register(0xA8B2, T_EraseRoundRect, "EraseRoundRect");
    trap_register(0xA8B3, T_InverRoundRect, "InvertRoundRect");
    trap_register(0xA8B4, T_FillRoundRect, "FillRoundRect");
    trap_register(0xA8B7, T_FrameOval, "FrameOval");
    trap_register(0xA8B8, T_PaintOval, "PaintOval");
    trap_register(0xA8B9, T_EraseOval, "EraseOval");
    trap_register(0xA8D8, T_NewRgn, "NewRgn");
    trap_register(0xA8D9, T_DisposeRgn, "DisposeRgn");
    trap_register(0xA8DC, T_CopyRgn, "CopyRgn");
    trap_register(0xA8DD, T_SetEmptyRgn, "SetEmptyRgn");
    trap_register(0xA8DE, T_SetRectRgn, "SetRectRgn");
    trap_register(0xA8DF, T_RectRgn, "RectRgn");
    trap_register(0xA8E0, T_OffsetRgn, "OffsetRgn");
    trap_register(0xA8E1, T_InsetRgn, "InsetRgn");
    trap_register(0xA8E4, T_SectRgn, "SectRgn");
    trap_register(0xA8E5, T_UnionRgn, "UnionRgn");
    trap_register(0xA8E6, T_DiffRgn, "DiffRgn");
    trap_register(0xA8E7, T_XorRgn, "XorRgn");
    trap_register(0xA8E2, T_EmptyRgn, "EmptyRgn");
    trap_register(0xA8E3, T_EqualRgn, "EqualRgn");
    trap_register(0xA8E8, T_PtInRgn, "PtInRgn");
    trap_register(0xA8E9, T_RectInRgn, "RectInRgn");
    trap_register(0xA8DA, T_OpenRgn, "OpenRgn");
    trap_register(0xA8DB, T_CloseRgn, "CloseRgn");
    trap_register(0xA8D3, T_PaintRgn, "PaintRgn");
    trap_register(0xA8D4, T_EraseRgn, "EraseRgn");
    trap_register(0xA8D2, T_FrameRgn, "FrameRgn");
    trap_register(0xA8D5, T_InverRgn, "InvertRgn");
    trap_register(0xA8D6, T_FillRgn, "FillRgn");
    trap_register(0xA862, T_ForeColor, "ForeColor");
    trap_register(0xA863, T_BackColor, "BackColor");
    trap_register(0xAA14, T_RGBForeColor, "RGBForeColor");
    trap_register(0xAA15, T_RGBBackColor, "RGBBackColor");
    trap_register(0xAA19, T_GetForeColor, "GetForeColor");
    trap_register(0xAA1A, T_GetBackColor, "GetBackColor");
    trap_register(0xA8EC, T_CopyBits, "CopyBits");
    trap_register(0xA817, T_CopyMask, "CopyMask");
    trap_register(0xA8EF, T_ScrollRect, "ScrollRect");
    trap_register(0xA870, T_LocalToGlobal, "LocalToGlobal");
    trap_register(0xA871, T_GlobalToLocal, "GlobalToLocal");
    trap_register(0xA865, T_GetPixel, "GetPixel");
    trap_register(0xAA17, T_GetCPixel, "GetCPixel");
    trap_register(0xAA16, T_SetCPixel, "SetCPixel");
    trap_register(0xA861, T_Random, "Random");
    trap_register(0xA868, T_FixMul, "FixMul");
    trap_register(0xA869, T_FixRatio, "FixRatio");
    trap_register(0xA86C, T_FixRound, "FixRound");
    trap_register(0xA86A, T_HiWord, "HiWord");
    trap_register(0xA86B, T_LoWord, "LoWord");
    trap_register(0xA867, T_LongMul, "LongMul");
    trap_register(0xA85D, T_BitTst, "BitTst");
    trap_register(0xA85E, T_BitSet, "BitSet");
    trap_register(0xA85F, T_BitClr, "BitClr");
    trap_register(0xA850, T_InitCursor, "InitCursor");
    trap_register(0xA851, T_SetCursor, "SetCursor");
    trap_register(0xA852, T_HideCursor, "HideCursor");
    trap_register(0xA853, T_ShowCursor, "ShowCursor");
    trap_register(0xA856, T_nop0, "ObscureCursor");
    trap_register(0xA9B9, T_GetCursor, "GetCursor");
    trap_register(0xA882, T_StdText, "StdText");
}
