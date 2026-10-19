/* QuickDraw PICT (v1 and v2) playback for DrawPicture. Bitmap opcodes are
 * fully supported; vector opcodes are drawn approximately; text is skipped. */
#include "qd.h"
#include "res.h"
#include <stdlib.h>

uint8_t qd_rgb15_to_index(uint32_t ctab, int r5, int g5, int b5);

typedef struct {
    const uint8_t *p, *end;
    bool v2;
    QRect frame, dst;
    uint32_t port;
    uint32_t dctab;
    bool ok;
} Pic;

static uint16_t g16(Pic *pc) { if (pc->p + 2 > pc->end) { pc->ok = false; return 0; } uint16_t v = (uint16_t)((pc->p[0] << 8) | pc->p[1]); pc->p += 2; return v; }
static uint32_t g32(Pic *pc) { uint32_t hi = g16(pc); return (hi << 16) | g16(pc); }
static uint8_t g8(Pic *pc) { if (pc->p >= pc->end) { pc->ok = false; return 0; } return *pc->p++; }
static void skip(Pic *pc, size_t n) { if (pc->p + n > pc->end) { pc->ok = false; pc->p = pc->end; } else pc->p += n; }
static QRect grect(Pic *pc) { QRect r; r.top = (int16_t)g16(pc); r.left = (int16_t)g16(pc); r.bottom = (int16_t)g16(pc); r.right = (int16_t)g16(pc); return r; }

static QRect map_rect(Pic *pc, QRect r)
{
    int fw = pc->frame.right - pc->frame.left, fh = pc->frame.bottom - pc->frame.top;
    int dw = pc->dst.right - pc->dst.left, dh = pc->dst.bottom - pc->dst.top;
    if (fw <= 0 || fh <= 0) return r;
    QRect o;
    o.left = (int16_t)(pc->dst.left + (long)(r.left - pc->frame.left) * dw / fw);
    o.right = (int16_t)(pc->dst.left + (long)(r.right - pc->frame.left) * dw / fw);
    o.top = (int16_t)(pc->dst.top + (long)(r.top - pc->frame.top) * dh / fh);
    o.bottom = (int16_t)(pc->dst.top + (long)(r.bottom - pc->frame.top) * dh / fh);
    return o;
}

static int unpackbits(const uint8_t *src, int srclen, uint8_t *dst, int dstlen)
{
    int i = 0, o = 0;
    while (i < srclen && o < dstlen) {
        int8_t c = (int8_t)src[i++];
        if (c >= 0) {
            int n = c + 1;
            for (int k = 0; k < n && i < srclen && o < dstlen; k++) dst[o++] = src[i++];
        } else if (c != -128) {
            int n = 1 - c;
            uint8_t v = i < srclen ? src[i++] : 0;
            for (int k = 0; k < n && o < dstlen; k++) dst[o++] = v;
        }
    }
    return o;
}

/* 16-bit packbits variant (packType 3): counts are in words */
static int unpackbits16(const uint8_t *src, int srclen, uint8_t *dst, int dstlen)
{
    int i = 0, o = 0;
    while (i < srclen && o < dstlen) {
        int8_t c = (int8_t)src[i++];
        if (c >= 0) {
            int n = (c + 1) * 2;
            for (int k = 0; k < n && i < srclen && o < dstlen; k++) dst[o++] = src[i++];
        } else if (c != -128) {
            int n = 1 - c;
            uint8_t a = i < srclen ? src[i++] : 0, b = i < srclen ? src[i++] : 0;
            for (int k = 0; k < n && o + 1 < dstlen; k++) { dst[o++] = a; dst[o++] = b; }
        }
    }
    return o;
}

/* write decoded index image (w x h, covering source rect `bounds`) to the port */
static void blit_indices(Pic *pc, const uint8_t *img, QRect bounds, QRect sr, QRect dr_pic, int mode)
{
    PixDesc d;
    if (!qd_port_pixels(pc->port, &d) || d.depth != 8) return;
    QRect dr = map_rect(pc, dr_pic);
    QRect clip = rect_sect(qd_port_clip(pc->port), d.bounds);
    QRect dc = rect_sect(dr, clip);
    if (rect_empty(dc)) return;
    int bw = bounds.right - bounds.left;
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top, dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    uint8_t bk = qd_bk_index(pc->port);
    for (int y = dc.top; y < dc.bottom; y++) {
        int sy = sr.top + (int)((long)(y - dr.top) * sh / dh);
        if (sy < bounds.top || sy >= bounds.bottom) continue;
        const uint8_t *srow = img + (size_t)(sy - bounds.top) * bw;
        uint8_t *drow = HP(d.base + (uint32_t)(y - d.bounds.top) * d.rowbytes);
        for (int x = dc.left; x < dc.right; x++) {
            int sx = sr.left + (int)((long)(x - dr.left) * sw / dw);
            if (sx < bounds.left || sx >= bounds.right) continue;
            uint8_t v = srow[sx - bounds.left];
            if (mode == 36 && v == bk) continue;
            if (mode == 1 && v == 0) continue;
            drow[x - d.bounds.left] = v;
        }
    }
}

static void op_bits(Pic *pc, uint16_t op)
{
    bool direct = (op == 0x9A || op == 0x9B);
    bool packed = (op == 0x98 || op == 0x99 || direct);
    bool has_rgn = (op & 1) != 0;
    if (direct) g32(pc); /* baseAddr placeholder */
    uint16_t rb = g16(pc);
    bool pixmap = (rb & 0x8000) != 0;
    rb &= 0x3FFF;
    QRect bounds = grect(pc);
    int pixsize = 1, packtype = 0, cmpcount = 1;
    uint8_t lut[256];
    for (int i = 0; i < 256; i++) lut[i] = (uint8_t)i;
    if (pixmap) {
        g16(pc); /* pmVersion */
        packtype = g16(pc);
        g32(pc); g32(pc); g32(pc); /* packSize, hRes, vRes */
        g16(pc); /* pixelType */
        pixsize = g16(pc);
        cmpcount = g16(pc);
        g16(pc); /* cmpSize */
        g32(pc); g32(pc); g32(pc); /* planeBytes, pmTable, reserved */
        if (!direct) {
            g32(pc); /* ctSeed */
            uint16_t flags = g16(pc);
            int n = (int16_t)g16(pc) + 1;
            for (int i = 0; i < n && pc->ok; i++) {
                uint16_t v = g16(pc), r = g16(pc), g = g16(pc), b = g16(pc);
                int idx = (flags & 0x8000) ? i : (v & 0xFF);
                lut[idx] = qd_rgb_to_index(pc->dctab, r, g, b);
            }
        }
    } else {
        /* 1-bit bitmap: 0 = background, 1 = foreground */
        for (int i = 0; i < 256; i++) lut[i] = 0;
        lut[0] = qd_bk_index(pc->port);
        lut[1] = qd_fg_index(pc->port);
    }
    QRect sr = grect(pc), dr = grect(pc);
    int mode = (int16_t)g16(pc);
    if (has_rgn) { uint16_t sz = g16(pc); skip(pc, sz >= 2 ? sz - 2 : 0); }
    if (!pc->ok) return;
    int w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) { pc->ok = false; return; }
    uint8_t *img = (uint8_t *)malloc((size_t)w * h);
    uint8_t *row = (uint8_t *)malloc(rb * 4 + 16);
    for (int y = 0; y < h && pc->ok; y++) {
        int got;
        if (!packed || rb < 8 || (direct && packtype == 1)) {
            if (pc->p + rb > pc->end) { pc->ok = false; break; }
            memcpy(row, pc->p, rb); pc->p += rb; got = rb;
        } else {
            int cnt = rb > 250 ? g16(pc) : g8(pc);
            if (pc->p + cnt > pc->end) { pc->ok = false; break; }
            if (direct && packtype == 3) got = unpackbits16(pc->p, cnt, row, rb);
            else got = unpackbits(pc->p, cnt, row, rb * 4 + 16);
            pc->p += cnt;
        }
        (void)got;
        uint8_t *out = img + (size_t)y * w;
        if (!direct) {
            for (int x = 0; x < w; x++) {
                int v;
                switch (pixsize) {
                case 1: v = (row[x >> 3] >> (7 - (x & 7))) & 1; break;
                case 2: v = (row[x >> 2] >> (6 - 2 * (x & 3))) & 3; break;
                case 4: v = (row[x >> 1] >> (4 - 4 * (x & 1))) & 15; break;
                default: v = row[x]; break;
                }
                out[x] = lut[v];
            }
        } else if (pixsize == 16) {
            for (int x = 0; x < w; x++) {
                uint16_t px = (uint16_t)((row[2 * x] << 8) | row[2 * x + 1]);
                out[x] = qd_rgb15_to_index(pc->dctab, (px >> 10) & 31, (px >> 5) & 31, px & 31);
            }
        } else {
            /* 32-bit: packType 4 stores components planar (A? R G B), else ARGB/xRGB */
            for (int x = 0; x < w; x++) {
                int r, g, b;
                if (packtype == 4 || packtype == 0) {
                    int off = cmpcount == 4 ? w : 0;
                    r = row[off + x]; g = row[off + w + x]; b = row[off + 2 * w + x];
                } else if (packtype == 2) {
                    r = row[3 * x]; g = row[3 * x + 1]; b = row[3 * x + 2];
                } else {
                    r = row[4 * x + 1]; g = row[4 * x + 2]; b = row[4 * x + 3];
                }
                out[x] = qd_rgb15_to_index(pc->dctab, r >> 3, g >> 3, b >> 3);
            }
        }
    }
    if (pc->ok) blit_indices(pc, img, bounds, sr, dr, mode);
    free(row);
    free(img);
    if (pc->v2 && ((pc->p - (pc->end - 0)) & 1)) { /* alignment handled by caller */ }
}

static void paint_pic_rect(Pic *pc, QRect r, int verb)
{
    QRect d = map_rect(pc, r);
    uint32_t port = pc->port;
    static const uint8_t black[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t fg = qd_fg_index(port), bk = qd_bk_index(port);
    switch (verb) {
    case 0: { /* frame */
        QRect t = { d.top, d.left, (int16_t)(d.top + 1), d.right }, b = { (int16_t)(d.bottom - 1), d.left, d.bottom, d.right };
        QRect l = { d.top, d.left, d.bottom, (int16_t)(d.left + 1) }, rr = { d.top, (int16_t)(d.right - 1), d.bottom, d.right };
        qd_fill_rect(port, t, black, 8, fg, bk); qd_fill_rect(port, b, black, 8, fg, bk);
        qd_fill_rect(port, l, black, 8, fg, bk); qd_fill_rect(port, rr, black, 8, fg, bk);
        break;
    }
    case 1: case 4: qd_fill_rect(port, d, black, 8, fg, bk); break;   /* paint, fill */
    case 2: qd_fill_rect(port, d, black, 8, bk, bk); break;           /* erase */
    case 3: qd_fill_rect(port, d, black, 10, fg, bk); break;          /* invert */
    }
}

QRect pict_frame(uint32_t h)
{
    QRect z = { 0, 0, 0, 0 };
    if (!h || !rdl(h) || mm_handle_size(h) < 10) return z;
    return rd_rect(rdl(h) + 2);
}

void pict_draw(uint32_t port, uint32_t h, QRect dst)
{
    if (!h || !rdl(h)) return;
    uint32_t len = mm_handle_size(h);
    if (len < 12) return;
    Pic pc;
    pc.p = HP(rdl(h)); pc.end = pc.p + len;
    pc.port = port;
    pc.ok = true;
    PixDesc pd;
    pc.dctab = qd_port_pixels(port, &pd) ? pd.ctab : 0;
    if (!pc.dctab) pc.dctab = qd_device_ctab();
    g16(&pc);
    pc.frame = grect(&pc);
    pc.dst = dst;
    /* version */
    if (pc.p[0] == 0x11 && pc.p[1] == 0x01) { pc.v2 = false; pc.p += 2; }
    else if (pc.p[0] == 0x00 && pc.p[1] == 0x11) { pc.v2 = true; pc.p += 4; }
    else { log_warn("unknown PICT version"); return; }
    QRect last = { 0, 0, 0, 0 };
    while (pc.ok && pc.p < pc.end) {
        if (pc.v2 && ((pc.p - HP(rdl(h))) & 1)) pc.p++;
        uint16_t op = pc.v2 ? g16(&pc) : g8(&pc);
        if (!pc.ok) break;
        if (op == 0x00FF || op == 0xFF) break;
        switch (op) {
        case 0x0000: break;
        case 0x0001: { uint16_t sz = g16(&pc); skip(&pc, sz >= 2 ? sz - 2 : 0); break; }
        case 0x0002: case 0x0009: case 0x000A: case 0x0010: skip(&pc, 8); break;
        case 0x0003: case 0x0005: case 0x0008: case 0x000D: case 0x0015: case 0x0016: case 0x0023: case 0x00A0: skip(&pc, 2); break;
        case 0x0004: skip(&pc, 1); break;
        case 0x0006: case 0x0007: case 0x000B: case 0x000C: case 0x000E: case 0x000F: case 0x0021: skip(&pc, 4); break;
        case 0x0011: skip(&pc, pc.v2 ? 2 : 1); break;
        case 0x0012: case 0x0013: case 0x0014: { /* pixpat */
            uint16_t type = g16(&pc); skip(&pc, 8);
            if (type == 1) {
                uint16_t rb = g16(&pc) & 0x3FFF; QRect b = grect(&pc); skip(&pc, 36);
                g32(&pc); g16(&pc); int n = (int16_t)g16(&pc) + 1; skip(&pc, (size_t)n * 8);
                int hgt = b.bottom - b.top;
                for (int y = 0; y < hgt && pc.ok; y++) {
                    if (rb < 8) skip(&pc, rb);
                    else { int c = rb > 250 ? g16(&pc) : g8(&pc); skip(&pc, c); }
                }
            } else if (type == 2) skip(&pc, 6);
            break;
        }
        case 0x001A: case 0x001B: {
            uint16_t r = g16(&pc), g = g16(&pc), b = g16(&pc);
            uint32_t a = port + (op == 0x1A ? P_rgbFg : P_rgbBk);
            wrw(a, r); wrw(a + 2, g); wrw(a + 4, b);
            break;
        }
        case 0x001D: case 0x001F: skip(&pc, 6); break;
        case 0x001C: case 0x001E: case 0x0017: case 0x0018: case 0x0019: break;
        case 0x0020: skip(&pc, 8); break;
        case 0x0022: skip(&pc, 6); break;
        case 0x0028: skip(&pc, 4); skip(&pc, g8(&pc)); break;
        case 0x0029: case 0x002A: skip(&pc, 1); skip(&pc, g8(&pc)); break;
        case 0x002B: skip(&pc, 2); skip(&pc, g8(&pc)); break;
        case 0x002C: case 0x002D: case 0x002E: case 0x002F: { uint16_t n = g16(&pc); skip(&pc, n); break; }
        case 0x0090: case 0x0091: case 0x0098: case 0x0099: case 0x009A: case 0x009B:
            op_bits(&pc, op); break;
        case 0x00A1: skip(&pc, 2); { uint16_t n = g16(&pc); skip(&pc, n); } break;
        case 0x0C00: skip(&pc, 24); break;
        default:
            if (op >= 0x30 && op <= 0x37) { last = grect(&pc); paint_pic_rect(&pc, last, op & 7); }
            else if (op >= 0x38 && op <= 0x3F) paint_pic_rect(&pc, last, op & 7);
            else if ((op >= 0x40 && op <= 0x47) || (op >= 0x50 && op <= 0x57)) { last = grect(&pc); paint_pic_rect(&pc, last, op & 7); }
            else if ((op >= 0x48 && op <= 0x4F) || (op >= 0x58 && op <= 0x5F)) paint_pic_rect(&pc, last, op & 7);
            else if (op >= 0x60 && op <= 0x67) skip(&pc, 12);
            else if (op >= 0x68 && op <= 0x6F) skip(&pc, 4);
            else if ((op >= 0x70 && op <= 0x77) || (op >= 0x80 && op <= 0x87)) { uint16_t sz = g16(&pc); skip(&pc, sz >= 2 ? sz - 2 : 0); }
            else if ((op >= 0x78 && op <= 0x7F) || (op >= 0x88 && op <= 0x8F)) {}
            else if ((op >= 0x92 && op <= 0x97) || (op >= 0x9C && op <= 0x9F) || (op >= 0xA2 && op <= 0xAF)) { uint16_t n = g16(&pc); skip(&pc, n); }
            else if (op >= 0xB0 && op <= 0xCF) {}
            else if (op >= 0xD0 && op <= 0xFE) { uint32_t n = g32(&pc); skip(&pc, n); }
            else if (op >= 0x0100 && op <= 0x7FFF) skip(&pc, (size_t)(op >> 8) * 2);
            else if (op >= 0x8000 && op <= 0x80FF) {}
            else if (op >= 0x8100) { uint32_t n = g32(&pc); skip(&pc, n); }
            else { log_warn("PICT opcode %04X unsupported", op); pc.ok = false; }
        }
    }
}

/* ------------------------------------------------------------ traps */
static void T_GetPicture(uint16_t op) { (void)op; int16_t id = ARGS16(); RET32(res_get(OSTYPE('P','I','C','T'), id, false)); }
static void T_DrawPicture(uint16_t op)
{
    (void)op;
    uint32_t r = ARG32(), h = ARG32();
    pict_draw(qd_the_port(), h, rd_rect(r));
}
static void T_KillPicture(uint16_t op) { (void)op; uint32_t h = ARG32(); if (h && mm_is_handle(h)) mm_dispose_handle(h); }
static uint32_t recording;
static void T_OpenPicture(uint16_t op)
{
    (void)op;
    uint32_t r = ARG32();
    /* we do not record drawing: produce an empty v2 picture with the frame */
    uint8_t pic[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x11, 0x02, 0xFF, 0x00, 0xFF };
    uint32_t h = mm_handle_from_bytes(pic, sizeof pic);
    wrw(rdl(h), sizeof pic);
    memcpy(HP(rdl(h) + 2), HP(r), 8);
    recording = h;
    RET32(h);
}
static void T_ClosePicture(uint16_t op) { (void)op; recording = 0; }
static void T_PicComment(uint16_t op) { (void)op; (void)ARG32(); (void)ARG16(); (void)ARG16(); }

void pict_register_traps(void)
{
    trap_register(0xA9BC, T_GetPicture, "GetPicture");
    trap_register(0xA8F6, T_DrawPicture, "DrawPicture");
    trap_register(0xA8F5, T_KillPicture, "KillPicture");
    trap_register(0xA8F3, T_OpenPicture, "OpenPicture");
    trap_register(0xA8F4, T_ClosePicture, "ClosePicture");
    trap_register(0xA8F2, T_PicComment, "PicComment");
}
