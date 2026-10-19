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
#include "stb_truetype.h"

typedef struct {
    int16_t id;
    bool loaded, ok;
    uint8_t *data;
    stbtt_fontinfo info;
} Face;

enum { FONT_SANS = 0, FONT_SANS_BOLD, FONT_MONO, FONT_SERIF, FONT_GAME, NFACES };
static Face faces[NFACES];
static int16_t game_font_id = -1;
static char game_font_name[64];

static const char *host_candidates[NFACES][12] = {
    { "/System/Library/Fonts/Geneva.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
      "C:/Windows/Fonts/tahoma.ttf", "C:/Windows/Fonts/verdana.ttf", "C:/Windows/Fonts/arial.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", NULL },
    { "/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/Library/Fonts/Arial Bold.ttf",
      "C:/Windows/Fonts/tahomabd.ttf", "C:/Windows/Fonts/verdanab.ttf", "C:/Windows/Fonts/arialbd.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", NULL },
    { "/System/Library/Fonts/Monaco.ttf", "C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cour.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf", NULL },
    { "/System/Library/Fonts/Supplemental/Times New Roman.ttf", "C:/Windows/Fonts/times.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf", "/usr/share/fonts/TTF/DejaVuSerif.ttf", NULL },
    { NULL },
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
static int uni(uint8_t c) { return c < 128 ? c : macroman[c - 128]; }

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
    float sc = stbtt_ScaleForMappingEmToPixels(&f->info, (float)size);
    int adv, lsb;
    int gi = stbtt_FindGlyphIndex(&f->info, uni(ch));
    stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
    g->adv = (int)(adv * sc + 0.5f);
    g->bits = stbtt_GetGlyphBitmap(&f->info, sc, sc, gi, &g->w, &g->h, &g->xoff, &g->yoff);
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
    int startx = x;
    for (int i = 0; i < len; i++) {
        Glyph *g = glyph(f, size, s[i]);
        if (mode == 0) { /* srcCopy: paint background cell */
            int a, d, l; metrics(port, &a, &d, &l);
            static const uint8_t solid[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
            QRect cell = { (int16_t)(y - a), (int16_t)x, (int16_t)(y + d), (int16_t)(x + g->adv) };
            qd_fill_rect(port, cell, solid, 8, bk, bk);
        }
        for (int pass = 0; pass < (bold ? 2 : 1); pass++) {
            for (int gy = 0; gy < g->h; gy++) {
                int py = y + g->yoff + gy;
                if (py < clip.top || py >= clip.bottom) continue;
                uint8_t *row = HP(pd.base + (uint32_t)(py - pd.bounds.top) * pd.rowbytes);
                for (int gx = 0; gx < g->w; gx++) {
                    if (g->bits[gy * g->w + gx] < 110) continue;
                    int px = x + g->xoff + gx + pass;
                    if (px < clip.left || px >= clip.right) continue;
                    uint8_t *d = row + (px - pd.bounds.left);
                    if (mode == 2) *d ^= 0xFF;
                    else if (mode == 3) *d = bk;
                    else *d = fg;
                }
            }
        }
        x += g->adv + (bold ? 1 : 0);
        if (s[i] == ' ') x += (int)((int32_t)rdl(port + P_spExtra) >> 16);
    }
    if (underline) {
        static const uint8_t solid[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
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
    if (id) { game_font_id = id; snprintf(game_font_name, sizeof game_font_name, "%s", name); }
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
