/* Font Manager / text drawing using stb_truetype.
 *
 * Classic Mac bitmap fonts (Chicago, Geneva, Helvetica...) are not available,
 * so they are substituted by host TrueType fonts. TrueType 'sfnt' resources
 * shipped with the game (FOND/sfnt "Thwack") are rendered directly. Glyphs
 * are thresholded so text stays exact on 8-bit indexed surfaces. */
#include "qd.h"
#include "res.h"
#include <stdio.h>
#include <stdlib.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

typedef struct {
    int16_t id;
    bool loaded, ok;
    uint8_t *data;
    stbtt_fontinfo info;
} Face;

enum { FONT_SANS = 0, FONT_SANS_BOLD, FONT_MONO, FONT_SERIF, FONT_GAME, FONT_SYMBOL, NFACES };
static Face faces[NFACES];
static int16_t game_font_id = -1;
static char game_font_name[64];

/* OS user-interface fonts first, then common fallbacks */
static const char *host_candidates[NFACES][20] = {
    { "C:/Windows/Fonts/segoeui.ttf", "/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/google-noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/NotoSans-Regular.ttf",
      "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
      "C:/Windows/Fonts/tahoma.ttf", "C:/Windows/Fonts/arial.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", NULL },
    { "C:/Windows/Fonts/segoeuib.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/Library/Fonts/Arial Bold.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf",
      "/usr/share/fonts/google-noto/NotoSans-Bold.ttf", "/usr/share/fonts/TTF/NotoSans-Bold.ttf",
      "C:/Windows/Fonts/tahomabd.ttf", "C:/Windows/Fonts/arialbd.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
      "/System/Library/Fonts/SFNS.ttf", NULL },
    { "C:/Windows/Fonts/consola.ttf", "/System/Library/Fonts/SFNSMono.ttf", "/System/Library/Fonts/Monaco.ttf",
      "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf", "C:/Windows/Fonts/cour.ttf", NULL },
    { "C:/Windows/Fonts/georgia.ttf", "/System/Library/Fonts/Supplemental/Georgia.ttf",
      "/System/Library/Fonts/Supplemental/Times New Roman.ttf", "/usr/share/fonts/truetype/noto/NotoSerif-Regular.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf", "/usr/share/fonts/TTF/DejaVuSerif.ttf", "C:/Windows/Fonts/times.ttf", NULL },
    { NULL },
    /* symbols missing from UI fonts (e.g. the Command key) */
    { "C:/Windows/Fonts/seguisym.ttf", "/System/Library/Fonts/Apple Symbols.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans.ttf", NULL },
};

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc(n > 0 ? n : 1);
    if (fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static bool load_game_font(Face *f)
{
    /* FOND resources map family ids to 'sfnt' ids: scan for the first sfnt */
    uint32_t len;
    for (int16_t id = 1024; id < 32000 && game_font_id < 0; id++) {
        if (res_peek(OSTYPE('F','O','N','D'), id, &len)) { game_font_id = id; }
        if (id == 2000) id = 31000;
    }
    const uint8_t *p = NULL;
    if (game_font_id >= 0) {
        const uint8_t *fond = res_peek(OSTYPE('F','O','N','D'), game_font_id, &len);
        /* font association table at offset 52: count-1, then (size, style, id) */
        if (fond && len > 60) {
            int n = ((fond[52] << 8) | fond[53]) + 1;
            for (int i = 0; i < n && !p; i++) {
                int16_t rid = (int16_t)((fond[54 + i * 6 + 4] << 8) | fond[54 + i * 6 + 5]);
                p = res_peek(OSTYPE('s','f','n','t'), rid, &len);
            }
        }
    }
    if (!p) return false;
    f->data = (uint8_t *)malloc(len);
    memcpy(f->data, p, len);
    return stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0)) != 0;
}

static Face *face_get(int which)
{
    Face *f = &faces[which];
    if (f->loaded) return f->ok ? f : NULL;
    f->loaded = true;
    if (which == FONT_GAME) {
        f->ok = load_game_font(f);
    } else {
        for (int i = 0; host_candidates[which][i]; i++) {
            size_t len;
            uint8_t *d = read_file(host_candidates[which][i], &len);
            if (!d) continue;
            int off = stbtt_GetFontOffsetForIndex(d, 0);
            if (off >= 0 && stbtt_InitFont(&f->info, d, off)) { f->data = d; f->ok = true; break; }
            free(d);
        }
    }
    if (!f->ok) {
        if (which == FONT_SANS_BOLD) return face_get(FONT_SANS);
        if (which != FONT_SANS && which != FONT_GAME) return face_get(FONT_SANS);
        if (which == FONT_SANS) return face_get(FONT_GAME);
        return NULL;
    }
    return f;
}

static Face *face_for(int16_t font, uint8_t txface)
{
    if (font == game_font_id && game_font_id >= 0) { Face *g = face_get(FONT_GAME); if (g) return g; }
    switch (font) {
    case 0: return face_get(FONT_SANS_BOLD);     /* Chicago (system font) */
    case 4: case 22: return face_get(FONT_MONO);  /* Monaco, Courier */
    case 2: case 20: return face_get(FONT_SERIF); /* New York, Times */
    default: return face_get((txface & 1) ? FONT_SANS_BOLD : FONT_SANS);
    }
}

static int eff_size(uint32_t port)
{
    int s = (int16_t)rdw(port + P_txSize);
    if (s <= 0) s = 12;
    return s;
}

/* Mac Roman 0x80..0xFF -> Unicode */
static const uint16_t macroman[128] = {
    0xC4,0xC5,0xC7,0xC9,0xD1,0xD6,0xDC,0xE1,0xE0,0xE2,0xE4,0xE3,0xE5,0xE7,0xE9,0xE8,
    0xEA,0xEB,0xED,0xEC,0xEE,0xEF,0xF1,0xF3,0xF2,0xF4,0xF6,0xF5,0xFA,0xF9,0xFB,0xFC,
    0x2020,0xB0,0xA2,0xA3,0xA7,0x2022,0xB6,0xDF,0xAE,0xA9,0x2122,0xB4,0xA8,0x2260,0xC6,0xD8,
    0x221E,0xB1,0x2264,0x2265,0xA5,0xB5,0x2202,0x2211,0x220F,0x3C0,0x222B,0xAA,0xBA,0x3A9,0xE6,0xF8,
    0xBF,0xA1,0xAC,0x221A,0x192,0x2248,0x2206,0xAB,0xBB,0x2026,0xA0,0xC0,0xC3,0xD5,0x152,0x153,
    0x2013,0x2014,0x201C,0x201D,0x2018,0x2019,0xF7,0x25CA,0xFF,0x178,0x2044,0x20AC,0x2039,0x203A,0xFB01,0xFB02,
    0x2021,0xB7,0x201A,0x201E,0x2030,0xC2,0xCA,0xC1,0xCB,0xC8,0xCD,0xCE,0xCF,0xCC,0xD3,0xD4,
    0xF8FF,0xD2,0xDA,0xDB,0xD9,0x131,0x2C6,0x2DC,0xAF,0x2D8,0x2D9,0x2DA,0xB8,0x2DD,0x2DB,0x2C7,
};
static int uni(uint8_t c)
{
    switch (c) {   /* Chicago puts these symbols in the control range */
    case 0x11: return 0x2318;  /* Command key */
    case 0x12: return 0x2713;  /* check mark */
    case 0x13: return 0x25C6;  /* diamond */
    case 0x14: return 0xF8FF;  /* Apple logo */
    default: return c < 128 ? c : macroman[c - 128];
    }
}

/* use a symbol font when the face lacks the character */
static Face *face_get(int which);
static Face *glyph_face(Face *f, int cp, int *gi)
{
    *gi = stbtt_FindGlyphIndex(&f->info, cp);
    if (*gi || cp < 0x2000) return f;
    Face *sym = face_get(FONT_SYMBOL);
    if (sym && sym != f) { int g2 = stbtt_FindGlyphIndex(&sym->info, cp); if (g2) { *gi = g2; return sym; } }
    return f;
}

typedef struct { Face *face; int size; uint8_t ch; bool used; int w, h, xoff, yoff, adv; uint8_t *bits; } Glyph;
#define GCACHE 2048
static Glyph gcache[GCACHE];

static Glyph *glyph(Face *f, int size, uint8_t ch)
{
    unsigned hsh = ((unsigned)(uintptr_t)f * 31u + (unsigned)size * 131u + ch) % GCACHE;
    Glyph *g = &gcache[hsh];
    if (g->used && g->face == f && g->size == size && g->ch == ch) return g;
    free(g->bits);
    memset(g, 0, sizeof *g);
    g->used = true; g->face = f; g->size = size; g->ch = ch;
    int gi;
    Face *gf = glyph_face(f, uni(ch), &gi);
    float sc = stbtt_ScaleForMappingEmToPixels(&gf->info, (float)size);
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&gf->info, gi, &adv, &lsb);
    g->adv = (int)(adv * sc + 0.5f);
    g->bits = stbtt_GetGlyphBitmap(&gf->info, sc, sc, gi, &g->w, &g->h, &g->xoff, &g->yoff);
    return g;
}

static void metrics(uint32_t port, int *asc, int *desc, int *lead)
{
    Face *f = face_for((int16_t)rdw(port + P_txFont), (uint8_t)rdb(port + P_txFace));
    int size = eff_size(port);
    if (!f) { *asc = size * 3 / 4; *desc = size / 4; *lead = 1; return; }
    int a, d, l;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &l);
    float sc = stbtt_ScaleForMappingEmToPixels(&f->info, (float)size);
    *asc = (int)(a * sc + 0.5f); *desc = (int)(-d * sc + 0.5f); *lead = (int)(l * sc + 0.5f);
}

int text_width(uint32_t port, const uint8_t *s, int len)
{
    Face *f = face_for((int16_t)rdw(port + P_txFont), (uint8_t)rdb(port + P_txFace));
    int size = eff_size(port);
    bool bold = rdb(port + P_txFace) & 1;
    int w = 0;
    for (int i = 0; i < len; i++) {
        if (!f) { w += size / 2; continue; }
        w += glyph(f, size, s[i])->adv + (bold ? 1 : 0);
        if (s[i] == ' ') w += (int)((int32_t)rdl(port + P_spExtra) >> 16);
    }
    return w;
}

void text_font_info(uint32_t port, int *ascent, int *descent, int *widmax, int *leading)
{
    metrics(port, ascent, descent, leading);
    *widmax = eff_size(port);
}

uint8_t qd_rgb15_to_index(uint32_t ctab, int r5, int g5, int b5);
bool g_text_hires = true;

/* ------------------------------------------------------------ hi-res screen text
 * Text drawn onto the screen is not rasterised into the 640x480 framebuffer.
 * It is kept as a list of strings laid out in 640x480 coordinates and drawn
 * by the platform at the window's real resolution. An entry disappears when
 * anything paints over its area (QuickDraw hooks) or when the pixels under
 * it change (checksum), e.g. the game's own blitters overwrite it. */
typedef struct {
    bool used, overlay;
    Face *face; int size; bool synth_bold, underline;
    uint8_t color_idx;           /* screen text: palette index, so it fades with the CLUT */
    uint8_t rgb[3];              /* overlay text: fixed colour */
    int n; uint8_t ch[160]; int16_t gx[160];
    int base_y;
    QRect gbox;                  /* unclipped glyph extents (screen coords) */
    QRect box;                   /* clipped extents: what the entry covers */
    uint32_t hash;
    int handle; float scale; int bw, bh;
} HiText;
#define MAXHT 600
static HiText ht[MAXHT];

static uint32_t fb_hash(QRect r)
{
    uint32_t h = 2166136261u;
    for (int y = r.top; y < r.bottom; y++) {
        const uint8_t *p = HP(g_screen_fb + (uint32_t)y * SCREEN_W + r.left);
        for (int x = 0; x < r.right - r.left; x++) h = (h ^ p[x]) * 16777619u;
    }
    return h;
}

static void ht_free(HiText *e)
{
    if (e->handle) plat_text_destroy(e->handle);
    memset(e, 0, sizeof *e);
}

void hitext_damage_screen(QRect r)
{
    if (rect_empty(r)) return;
    for (int i = 0; i < MAXHT; i++) {
        HiText *e = &ht[i];
        if (e->used && !e->overlay && !rect_empty(rect_sect(e->box, r))) ht_free(e);
    }
}

/* drawing into a window covered by others only damages what it can reach:
 * text lying entirely inside a covering window survives */
void hitext_damage_port(QRect r, uint32_t port)
{
    if (rect_empty(r)) return;
    QRect oc[32];
    int n = port ? win_occluders(port, oc, 32) : 0;
    for (int i = 0; i < MAXHT; i++) {
        HiText *e = &ht[i];
        if (!e->used || e->overlay || rect_empty(rect_sect(e->box, r))) continue;
        bool covered = false;
        for (int k = 0; k < n && !covered; k++) {
            QRect in = rect_sect(e->box, oc[k]);
            covered = !memcmp(&in, &e->box, sizeof in);
        }
        if (!covered) ht_free(e);
    }
}

static HiText *ht_alloc(void)
{
    for (int i = 0; i < MAXHT; i++) if (!ht[i].used) return &ht[i];
    /* full: recycle the first screen entry */
    for (int i = 0; i < MAXHT; i++) if (!ht[i].overlay) { ht_free(&ht[i]); return &ht[i]; }
    return NULL;
}

static bool ht_layout(HiText *e, Face *f, int size, bool bold, bool underline, const uint8_t *s, int len, int x, int y,
                      int spextra)
{
    if (len > 160) len = 160;
    e->face = f; e->size = size; e->synth_bold = bold; e->underline = underline;
    e->n = len; e->base_y = y;
    QRect g = { 32767, 32767, -32767, -32767 };
    for (int i = 0; i < len; i++) {
        Glyph *gl = glyph(f, size, s[i]);
        e->ch[i] = s[i]; e->gx[i] = (int16_t)x;
        if (gl->w > 0 && gl->h > 0) {
            QRect r = { (int16_t)(y + gl->yoff), (int16_t)(x + gl->xoff), (int16_t)(y + gl->yoff + gl->h),
                        (int16_t)(x + gl->xoff + gl->w + (bold ? 1 : 0)) };
            g = rect_union(g.right < g.left ? r : g, r);
        }
        x += gl->adv + (bold ? 1 : 0);
        if (s[i] == ' ') x += spextra;
    }
    if (underline) {
        QRect u = { (int16_t)(y + 1), e->gx[0], (int16_t)(y + 2), (int16_t)x };
        g = rect_union(g.right < g.left ? u : g, u);
    }
    if (g.right <= g.left) return false;
    e->gbox = g;
    return true;
}

static uint32_t hitext_cur_port;
static bool hitext_port_covered(QRect box)
{
    QRect oc[32];
    int n = win_occluders(hitext_cur_port, oc, 32);
    for (int i = 0; i < n; i++) if (!rect_empty(rect_sect(oc[i], box))) return true;
    return false;
}

static void hitext_add(Face *f, int size, bool bold, bool underline, uint8_t color, const uint8_t *s, int len,
                       int x, int y, int spextra, QRect clip)
{
    HiText tmp; memset(&tmp, 0, sizeof tmp);
    if (!ht_layout(&tmp, f, size, bold, underline, s, len, x, y, spextra)) return;
    QRect scr = { 0, 0, SCREEN_H, SCREEN_W };
    tmp.box = rect_sect(rect_sect(tmp.gbox, clip), scr);
    if (rect_empty(tmp.box)) return;
    if (hitext_port_covered(tmp.box)) return;
    /* the same string redrawn at the same place replaces the old entry */
    for (int i = 0; i < MAXHT; i++) {
        HiText *e = &ht[i];
        if (e->used && !e->overlay && !memcmp(&e->box, &tmp.box, sizeof tmp.box)) ht_free(e);
    }
    HiText *e = ht_alloc();
    if (!e) return;
    *e = tmp;
    e->used = true;
    e->color_idx = color;
    e->hash = fb_hash(e->box);
}

/* Saving the pixels under a window/popup must also save the screen text there
 * (it isn't in the pixels); restoring puts both back. */
typedef struct HiTextSnap { int n; HiText e[]; } HiTextSnap;

HiTextSnap *hitext_save(QRect r)
{
    int n = 0;
    for (int i = 0; i < MAXHT; i++)
        if (ht[i].used && !ht[i].overlay && !rect_empty(rect_sect(ht[i].box, r))) n++;
    HiTextSnap *s = (HiTextSnap *)malloc(sizeof(HiTextSnap) + sizeof(HiText) * (n ? n : 1));
    s->n = 0;
    for (int i = 0; i < MAXHT; i++)
        if (ht[i].used && !ht[i].overlay && !rect_empty(rect_sect(ht[i].box, r))) {
            s->e[s->n] = ht[i];
            s->e[s->n].handle = 0;  /* re-rasterised on restore */
            s->n++;
        }
    return s;
}

void hitext_restore(HiTextSnap *s)
{
    if (!s) return;
    for (int i = 0; i < s->n; i++) {
        HiText *e = ht_alloc();
        if (!e) break;
        *e = s->e[i];
        e->hash = fb_hash(e->box);   /* pixels underneath were just restored */
    }
    free(s);
}

/* overlay (port UI) text, independent of the game's pixels */
void text_overlay_set(int slot, const char *s, int x, int baseline, bool bold, uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < MAXHT; i++) if (ht[i].used && ht[i].overlay && ht[i].color_idx == slot) ht_free(&ht[i]);
    if (!s || !*s) return;
    Face *f = face_get(bold ? FONT_SANS_BOLD : FONT_SANS);
    if (!f) return;
    HiText *e = ht_alloc();
    if (!e) return;
    memset(e, 0, sizeof *e);
    if (!ht_layout(e, f, 12, false, false, (const uint8_t *)s, (int)strlen(s), x, baseline, 0)) return;
    e->used = true; e->overlay = true;
    e->box = e->gbox;
    e->color_idx = (uint8_t)slot;
    e->rgb[0] = r; e->rgb[1] = g; e->rgb[2] = b;
}

int text_overlay_width(const char *s, bool bold)
{
    Face *f = face_get(bold ? FONT_SANS_BOLD : FONT_SANS);
    int w = 0;
    if (!f) return (int)strlen(s) * 6;
    for (const char *p = s; *p; p++) w += glyph(f, 12, (uint8_t)*p)->adv;
    return w;
}

void text_overlay_clear(void)
{
    for (int i = 0; i < MAXHT; i++) if (ht[i].used && ht[i].overlay) ht_free(&ht[i]);
}

static void ht_rasterize(HiText *e, float S)
{
    if (e->handle) { plat_text_destroy(e->handle); e->handle = 0; }
    QRect g = e->gbox;
    int ox = g.left - 1, oy = g.top - 1;
    int bw = (int)((g.right - g.left + 3) * S + 1), bh = (int)((g.bottom - g.top + 3) * S + 1);
    if (bw <= 0 || bh <= 0 || bw > 4096 || bh > 1024) return;
    uint8_t *a = (uint8_t *)calloc((size_t)bw * bh, 1);
    float base = (e->base_y - oy) * S;
    int passes = e->synth_bold ? 2 : 1;
    for (int i = 0; i < e->n; i++) {
        int gi;
        Face *gf = glyph_face(e->face, uni(e->ch[i]), &gi);
        float sc = stbtt_ScaleForMappingEmToPixels(&gf->info, e->size * S);
        for (int pass = 0; pass < passes; pass++) {
            float px = (e->gx[i] - ox + pass) * S;
            float shift = px - (float)(int)px;
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBoxSubpixel(&gf->info, gi, sc, sc, shift, 0, &x0, &y0, &x1, &y1);
            int w = x1 - x0, h = y1 - y0;
            if (w <= 0 || h <= 0) continue;
            uint8_t *tmp = (uint8_t *)malloc((size_t)w * h);
            stbtt_MakeGlyphBitmapSubpixel(&gf->info, tmp, w, h, w, sc, sc, shift, 0, gi);
            int dx = (int)px + x0, dy = (int)(base + 0.5f) + y0;
            for (int yy = 0; yy < h; yy++) {
                int ty = dy + yy;
                if (ty < 0 || ty >= bh) continue;
                for (int xx = 0; xx < w; xx++) {
                    int tx = dx + xx;
                    if (tx < 0 || tx >= bw) continue;
                    uint8_t v = tmp[yy * w + xx];
                    uint8_t *d = &a[ty * bw + tx];
                    if (v > *d) *d = v;
                }
            }
            free(tmp);
        }
    }
    if (e->underline) {
        int uy = (int)(base + S), uh = S > 1 ? (int)S : 1;
        int ux0 = (int)((e->gx[0] - ox) * S), ux1 = (int)((g.right - ox) * S);
        for (int yy = uy; yy < uy + uh && yy < bh; yy++)
            for (int xx = ux0 < 0 ? 0 : ux0; xx < ux1 && xx < bw; xx++) a[yy * bw + xx] = 255;
    }
    /* mask to the clipped box */
    if (memcmp(&e->box, &e->gbox, sizeof e->box)) {
        int cx0 = (int)((e->box.left - ox) * S), cx1 = (int)((e->box.right - ox) * S);
        int cy0 = (int)((e->box.top - oy) * S), cy1 = (int)((e->box.bottom - oy) * S);
        for (int yy = 0; yy < bh; yy++)
            for (int xx = 0; xx < bw; xx++)
                if (xx < cx0 || xx >= cx1 || yy < cy0 || yy >= cy1) a[yy * bw + xx] = 0;
    }
    e->handle = plat_text_create(a, bw, bh);
    e->scale = S; e->bw = bw; e->bh = bh;
    free(a);
}

/* drop entries whose pixels changed underneath; build the platform draw list */
int text_hires_draws(PlatTextDraw *out, int max)
{
    float S = plat_output_scale();
    int n = 0;
    for (int i = 0; i < MAXHT && n < max; i++) {
        HiText *e = &ht[i];
        if (!e->used) continue;
        if (!e->overlay && fb_hash(e->box) != e->hash) { ht_free(e); continue; }
        if (!e->handle || e->scale != S) ht_rasterize(e, S);
        if (!e->handle) continue;
        PlatTextDraw *d = &out[n++];
        d->handle = e->handle;
        d->x = (float)(e->gbox.left - 1); d->y = (float)(e->gbox.top - 1);
        d->sx = 0; d->sy = 0; d->sw = (float)e->bw; d->sh = (float)e->bh;
        if (e->overlay) { d->r = e->rgb[0]; d->g = e->rgb[1]; d->b = e->rgb[2]; }
        else { d->r = g_hw_pal[e->color_idx][0]; d->g = g_hw_pal[e->color_idx][1]; d->b = g_hw_pal[e->color_idx][2]; }
    }
    return n;
}

static void draw_glyphs(uint32_t port, int x, int y, const uint8_t *s, int len)
{
    Face *f = face_for((int16_t)rdw(port + P_txFont), (uint8_t)rdb(port + P_txFace));
    if (!f) return;
    PixDesc pd;
    if (!qd_port_pixels(port, &pd) || pd.depth != 8) return;
    QRect clip = rect_sect(qd_port_clip(port), pd.bounds);
    int size = eff_size(port);
    uint8_t face = (uint8_t)rdb(port + P_txFace);
    bool bold = face & 1, underline = face & 4;
    int mode = (int16_t)rdw(port + P_txMode) & 0x3F;
    uint8_t fg = qd_fg_index(port), bk = qd_bk_index(port);
    int spextra = (int)((int32_t)rdl(port + P_spExtra) >> 16);
    bool synth_bold = bold && f != face_get(FONT_SANS_BOLD);
    static const uint8_t solid[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    if (mode == 0) { /* srcCopy: paint the background cells first */
        int a, d, l; metrics(port, &a, &d, &l);
        int cx = x;
        for (int i = 0; i < len; i++) {
            Glyph *g = glyph(f, size, s[i]);
            QRect cell = { (int16_t)(y - a), (int16_t)cx, (int16_t)(y + d), (int16_t)(cx + g->adv) };
            qd_fill_rect(port, cell, solid, 8, bk, bk);
            cx += g->adv + (bold ? 1 : 0);
            if (s[i] == ' ') cx += spextra;
        }
    }
    bool onscreen = pd.base == g_screen_fb;
    if (g_text_hires && onscreen && (mode == 0 || mode == 1 || mode == 8 || mode == 9 || mode == 36)) {
        /* screen text goes to the hi-res layer (coordinates relative to the screen) */
        QRect sclip = { (int16_t)(clip.top - pd.bounds.top), (int16_t)(clip.left - pd.bounds.left),
                        (int16_t)(clip.bottom - pd.bounds.top), (int16_t)(clip.right - pd.bounds.left) };
        hitext_cur_port = port;
        hitext_add(f, size, synth_bold, underline, fg, s, len, x - pd.bounds.left, y - pd.bounds.top, spextra, sclip);
        return;
    }
    /* offscreen (or special modes): anti-aliased into the 8-bit pixels */
    uint16_t fr = (uint16_t)rdw(port + P_rgbFg), fgc = (uint16_t)rdw(port + P_rgbFg + 2), fb = (uint16_t)rdw(port + P_rgbFg + 4);
    static uint16_t pal_rgb[256][3];
    static uint32_t pal_ctab, pal_seed;
    if (pd.ctab && rdl(pd.ctab) && (pal_ctab != pd.ctab || pal_seed != rdl(rdl(pd.ctab)))) {
        pal_ctab = pd.ctab; pal_seed = rdl(rdl(pd.ctab));
        for (int i = 0; i < 256; i++) qd_index_to_rgb(pd.ctab, (uint8_t)i, &pal_rgb[i][0], &pal_rgb[i][1], &pal_rgb[i][2]);
    }
    bool aa = (mode == 0 || mode == 1 || mode == 8 || mode == 9 || mode == 36) && pd.ctab && rdl(pd.ctab);
    if (aa) { fr = pal_rgb[fg][0]; fgc = pal_rgb[fg][1]; fb = pal_rgb[fg][2]; }
    int startx = x;
    for (int i = 0; i < len; i++) {
        Glyph *g = glyph(f, size, s[i]);
        for (int pass = 0; pass < (synth_bold ? 2 : 1); pass++) {
            for (int gy = 0; gy < g->h; gy++) {
                int py = y + g->yoff + gy;
                if (py < clip.top || py >= clip.bottom) continue;
                uint8_t *row = HP(pd.base + (uint32_t)(py - pd.bounds.top) * pd.rowbytes);
                for (int gx = 0; gx < g->w; gx++) {
                    int c = g->bits[gy * g->w + gx];
                    int px = x + g->xoff + gx + pass;
                    if (px < clip.left || px >= clip.right) continue;
                    uint8_t *d = row + (px - pd.bounds.left);
                    if (!aa) {
                        if (c < 110) continue;
                        if (mode == 2 || mode == 10) *d ^= 0xFF;
                        else if (mode == 3 || mode == 11) *d = bk;
                        else *d = fg;
                        continue;
                    }
                    if (c < 40) continue;
                    if (c >= 215) { *d = fg; continue; }
                    const uint16_t *b = pal_rgb[*d];
                    int r = (fr * c + b[0] * (255 - c)) / 255, gg = (fgc * c + b[1] * (255 - c)) / 255, bb = (fb * c + b[2] * (255 - c)) / 255;
                    *d = qd_rgb15_to_index(pd.ctab, r >> 11, gg >> 11, bb >> 11);
                }
            }
        }
        x += g->adv + (bold ? 1 : 0);
        if (s[i] == ' ') x += spextra;
    }
    if (underline) {
        QRect u = { (int16_t)(y + 1), (int16_t)startx, (int16_t)(y + 2), (int16_t)x };
        qd_fill_rect(port, u, solid, 8, fg, bk);
    }
}

void text_draw(uint32_t port, const uint8_t *s, int len)
{
    int x = (int16_t)rdw(port + P_pnLoc + 2), y = (int16_t)rdw(port + P_pnLoc);
    draw_glyphs(port, x, y, s, len);
    wrw(port + P_pnLoc + 2, (uint16_t)(x + text_width(port, s, len)));
}

void text_init(void) { }

/* ------------------------------------------------------------ traps */
static void T_InitFonts(uint16_t op) { (void)op; }
static void T_TextFont(uint16_t op) { (void)op; wrw(qd_the_port() + P_txFont, ARG16()); }
static void T_TextFace(uint16_t op) { (void)op; wrb(qd_the_port() + P_txFace, (uint8_t)(ARG16() & 0xFF)); }
static void T_TextMode(uint16_t op) { (void)op; wrw(qd_the_port() + P_txMode, ARG16()); }
static void T_TextSize(uint16_t op) { (void)op; wrw(qd_the_port() + P_txSize, ARG16()); }
static void T_SpaceExtra(uint16_t op) { (void)op; wrl(qd_the_port() + P_spExtra, ARG32()); }
static void T_CharExtra(uint16_t op) { (void)op; (void)ARG32(); }
static void T_DrawChar(uint16_t op) { (void)op; uint8_t c = (uint8_t)ARG16(); text_draw(qd_the_port(), &c, 1); }
static void T_DrawString(uint16_t op) { (void)op; uint32_t s = ARG32(); text_draw(qd_the_port(), HP(s + 1), rdb(s)); }
static void T_DrawText(uint16_t op)
{
    (void)op;
    int16_t cnt = ARGS16(), first = ARGS16(); uint32_t p = ARG32();
    text_draw(qd_the_port(), HP(p + first), cnt);
}
static void T_TextWidth(uint16_t op)
{
    (void)op;
    int16_t cnt = ARGS16(), first = ARGS16(); uint32_t p = ARG32();
    RET16(text_width(qd_the_port(), HP(p + first), cnt));
}
static void T_StringWidth(uint16_t op) { (void)op; uint32_t s = ARG32(); RET16(text_width(qd_the_port(), HP(s + 1), rdb(s))); }
static void T_CharWidth(uint16_t op) { (void)op; uint8_t c = (uint8_t)ARG16(); RET16(text_width(qd_the_port(), &c, 1)); }
static void T_GetFontInfo(uint16_t op)
{
    (void)op;
    uint32_t p = ARG32();
    int a, d, w, l; text_font_info(qd_the_port(), &a, &d, &w, &l);
    wrw(p, (uint16_t)a); wrw(p + 2, (uint16_t)d); wrw(p + 4, (uint16_t)w); wrw(p + 6, (uint16_t)l);
}

static const struct { const char *name; int16_t id; } known_fonts[] = {
    { "Chicago", 0 }, { "New York", 2 }, { "Geneva", 3 }, { "Monaco", 4 }, { "Times", 20 },
    { "Helvetica", 21 }, { "Courier", 22 }, { "Symbol", 23 },
};

static int16_t fond_id_by_name(const char *name)
{
    extern int16_t res_find_named_id(uint32_t type, const char *name);
    int16_t id = res_find_named_id(OSTYPE('F','O','N','D'), name);
    if (id) { game_font_id = id; snprintf(game_font_name, sizeof game_font_name, "%.*s", (int)sizeof game_font_name - 1, name); }
    return id;
}

static void T_GetFNum(uint16_t op)
{
    (void)op;
    uint32_t pid = ARG32(), pname = ARG32();
    char name[256]; pstr_to_c(pname, name, sizeof name);
    int16_t id = 0;
    for (size_t i = 0; i < sizeof known_fonts / sizeof known_fonts[0]; i++)
        if (!strcmp(known_fonts[i].name, name)) id = known_fonts[i].id;
    if (!id) id = fond_id_by_name(name);
    wrw(pid, (uint16_t)id);
}
static void T_GetFName(uint16_t op)
{
    (void)op;
    uint32_t pname = ARG32(); int16_t id = ARGS16();
    const char *n = "";
    for (size_t i = 0; i < sizeof known_fonts / sizeof known_fonts[0]; i++) if (known_fonts[i].id == id) n = known_fonts[i].name;
    if (id == game_font_id) n = game_font_name;
    c_to_pstr(n, pname);
}
static void T_RealFont(uint16_t op)
{
    (void)op;
    int16_t size = ARGS16(), id = ARGS16();
    (void)size;
    RET8(id == game_font_id ? face_get(FONT_GAME) != NULL : 1);
}

/* TETextBox(text, length, box, just) */
static void T_TETextBox(uint16_t op)
{
    (void)op;
    int16_t just = ARGS16(); uint32_t box = ARG32(); int32_t len = (int32_t)ARG32(); uint32_t text = ARG32();
    uint32_t port = qd_the_port();
    QRect b = rd_rect(box);
    int a, d, l; metrics(port, &a, &d, &l);
    int lh = a + d + l;
    const uint8_t *s = HP(text);
    int y = b.top + a;
    int i = 0;
    int width = b.right - b.left;
    if (width <= 0) return;
    while (i < len && y - a < b.bottom) {
        /* find line end: CR or wrap at last space that fits */
        int start = i, last_space = -1, j = i;
        while (j < len && s[j] != 13) {
            if (s[j] == ' ') last_space = j;
            if (text_width(port, s + start, j - start + 1) > width) break;
            j++;
        }
        int end = j;
        if (j < len && s[j] != 13 && last_space > start) end = last_space;
        /* always make progress, even if not a single character fits */
        if (end == start && j < len && s[j] != 13) end = start + 1;
        int w = text_width(port, s + start, end - start);
        int x = b.left;
        if (just == 1) x = b.left + (width - w) / 2;
        else if (just == -1) x = b.right - w;
        draw_glyphs(port, x, y, s + start, end - start);
        i = end;
        if (i < len && (s[i] == 13 || s[i] == ' ')) i++;
        y += lh;
    }
}

void text_register_traps(void)
{
    trap_register(0xA8FE, T_InitFonts, "InitFonts");
    trap_register(0xA887, T_TextFont, "TextFont");
    trap_register(0xA888, T_TextFace, "TextFace");
    trap_register(0xA889, T_TextMode, "TextMode");
    trap_register(0xA88A, T_TextSize, "TextSize");
    trap_register(0xA88E, T_SpaceExtra, "SpaceExtra");
    trap_register(0xAA1D, T_CharExtra, "CharExtra");
    trap_register(0xA883, T_DrawChar, "DrawChar");
    trap_register(0xA884, T_DrawString, "DrawString");
    trap_register(0xA885, T_DrawText, "DrawText");
    trap_register(0xA886, T_TextWidth, "TextWidth");
    trap_register(0xA88C, T_StringWidth, "StringWidth");
    trap_register(0xA88D, T_CharWidth, "CharWidth");
    trap_register(0xA88B, T_GetFontInfo, "GetFontInfo");
    trap_register(0xA900, T_GetFNum, "GetFNum");
    trap_register(0xA8FF, T_GetFName, "GetFName");
    trap_register(0xA902, T_RealFont, "RealFont");
    trap_register(0xA9CE, T_TETextBox, "TETextBox");
}
