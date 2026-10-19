/* Dialog Manager, Control Manager and alerts. */
#include "qd.h"
#include "runtime/layout.h"
#include "res.h"
#include "event.h"
#include <stdio.h>
#include <stdlib.h>

uint32_t win_create(uint32_t storage, QRect bounds, uint32_t title, bool visible, int16_t proc,
                    uint32_t behind, bool goaway, uint32_t refcon, uint32_t size);
void win_dispose(uint32_t w, bool free_storage);

enum { IT_USER = 0, IT_BUTTON = 4, IT_CHECK = 5, IT_RADIO = 6, IT_RESCTL = 7, IT_STATIC = 8, IT_EDIT = 16, IT_ICON = 32, IT_PICT = 64 };

typedef struct {
    uint8_t type;      /* including the 0x80 disabled bit */
    QRect r;
    uint32_t h;        /* control handle / text handle / user proc / resource handle */
    int16_t resid;
    bool hidden;
} DItem;

typedef struct {
    uint32_t dlg;
    int n;
    DItem it[64];
    int edit;          /* current edit item (1-based) or 0 */
    bool sel_all;
    bool drawn;
    bool needs_update;   /* send the filter one update event, like a newly shown window */
} DlgInfo;

#define MAXDLG 8
static DlgInfo dlgs[MAXDLG];
static char param_text[4][256];

static DlgInfo *dinfo(uint32_t d)
{
    for (int i = 0; i < MAXDLG; i++) if (dlgs[i].dlg == d && d) return &dlgs[i];
    return NULL;
}

/* ---- control records (ControlRecord layout) ---- */
#define C_owner 4
#define C_rect 8
#define C_vis 16
#define C_hilite 17
#define C_value 18
#define C_min 20
#define C_max 22
#define C_defproc 24
#define C_title 40
#define CTRL_SIZE 296

static uint32_t new_control(uint32_t owner, QRect r, const char *title, int kind, int value, int min, int max)
{
    uint32_t h = mm_new_handle(CTRL_SIZE, true);
    uint32_t c = rdl(h);
    wrl(c + C_owner, owner);
    wr_rect(c + C_rect, r);
    wrb(c + C_vis, 0xFF);
    wrw(c + C_value, (uint16_t)value); wrw(c + C_min, (uint16_t)min); wrw(c + C_max, (uint16_t)max);
    wrl(c + C_defproc, (uint32_t)kind);
    c_to_pstr(title, c + C_title);
    return h;
}

/* ---- drawing helpers ---- */
static const uint8_t SOLID[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint8_t idx_rgb(uint32_t port, uint16_t r, uint16_t g, uint16_t b)
{
    PixDesc pd; uint32_t ct = qd_port_pixels(port, &pd) ? pd.ctab : 0;
    return qd_rgb_to_index(ct, r, g, b);
}

static void box(uint32_t port, QRect r, uint8_t c) { qd_fill_rect(port, r, SOLID, 8, c, c); }
static void frame(uint32_t port, QRect r, uint8_t c, int t)
{
    QRect a = { r.top, r.left, (int16_t)(r.top + t), r.right }, b = { (int16_t)(r.bottom - t), r.left, r.bottom, r.right };
    QRect l = { r.top, r.left, r.bottom, (int16_t)(r.left + t) }, rr = { r.top, (int16_t)(r.right - t), r.bottom, r.right };
    box(port, a, c); box(port, b, c); box(port, l, c); box(port, rr, c);
}

static void with_font(uint32_t port, int font, int size, int face, void (*fn)(void *), void *arg)
{
    uint16_t of = (uint16_t)rdw(port + P_txFont), os = (uint16_t)rdw(port + P_txSize);
    uint8_t oface = (uint8_t)rdb(port + P_txFace);
    wrw(port + P_txFont, (uint16_t)font); wrw(port + P_txSize, (uint16_t)size); wrb(port + P_txFace, (uint8_t)face);
    fn(arg);
    wrw(port + P_txFont, of); wrw(port + P_txSize, os); wrb(port + P_txFace, oface);
}

static void expand_params(const uint8_t *in, int len, char *out, size_t outsz)
{
    size_t o = 0;
    for (int i = 0; i < len && o + 1 < outsz; i++) {
        if (in[i] == '^' && i + 1 < len && in[i + 1] >= '0' && in[i + 1] <= '3') {
            const char *p = param_text[in[i + 1] - '0'];
            while (*p && o + 1 < outsz) out[o++] = *p++;
            i++;
        } else out[o++] = (char)in[i];
    }
    out[o] = 0;
}

typedef struct { uint32_t port; QRect r; const char *s; int just; } TextBoxArgs;
static void textbox_cb(void *a)
{
    TextBoxArgs *t = (TextBoxArgs *)a;
    /* reuse TETextBox semantics: push args and call the trap implementation */
    size_t n = strlen(t->s);
    uint32_t tmp = mm_new_ptr((uint32_t)n + 1, false);
    memcpy(HP(tmp), t->s, n);
    uint32_t rp = mm_new_ptr(8, false);
    wr_rect(rp, t->r);
    PUSH32(tmp); PUSH32((uint32_t)n); PUSH32(rp); PUSH16((uint16_t)t->just);
    trap_dispatch(0xA9CE);
    mm_dispose_ptr(rp); mm_dispose_ptr(tmp);
}

static void draw_text_in(uint32_t port, QRect r, const char *s, int just, int font, int size, int face)
{
    TextBoxArgs a = { port, r, s, just };
    with_font(port, font, size, face, textbox_cb, &a);
}

static void control_draw(uint32_t ch)
{
    if (!ch || !rdl(ch)) return;
    uint32_t c = rdl(ch);
    if (!rdb(c + C_vis)) return;
    uint32_t port = rdl(c + C_owner);
    QRect r = rd_rect(c + C_rect);
    int kind = (int)rdl(c + C_defproc);
    char title[256]; pstr_to_c(c + C_title, title, sizeof title);
    uint8_t black = idx_rgb(port, 0, 0, 0), white = idx_rgb(port, 0xFFFF, 0xFFFF, 0xFFFF), grey = idx_rgb(port, 0x8888, 0x8888, 0x8888);
    bool dim = rdb(c + C_hilite) == 255;
    bool pressed = rdb(c + C_hilite) > 0 && rdb(c + C_hilite) < 254;
    uint8_t fg = dim ? grey : black;
    if (kind == 0) {
        box(port, r, pressed ? black : white);
        frame(port, r, fg, 1);
        QRect tr = r; tr.top = (int16_t)(r.top + (r.bottom - r.top - 14) / 2); tr.bottom = r.bottom;
        uint8_t saved[6]; memcpy(saved, HP(port + P_rgbFg), 6);
        uint32_t saved_idx = rdl(port + P_fgColor);
        if (pressed) qd_set_fg_rgb(port, 0xFFFF, 0xFFFF, 0xFFFF);
        else if (dim) qd_set_fg_rgb(port, 0x8888, 0x8888, 0x8888);
        draw_text_in(port, tr, title, 1, 0, 12, 0);
        memcpy(HP(port + P_rgbFg), saved, 6);
        wrl(port + P_fgColor, saved_idx);
    } else if (kind == 1 || kind == 2) {
        int cy = (r.top + r.bottom) / 2;
        QRect b = { (int16_t)(cy - 6), r.left, (int16_t)(cy + 6), (int16_t)(r.left + 12) };
        box(port, r, white);
        if (kind == 2) {
            /* radio buttons are round: the classic 12x12 circle, with a 6x6 dot when on */
            static const char *const ring[12] = {
                "....####....", "..##....##..", ".#........#.", ".#........#.", "#..........#", "#..........#",
                "#..........#", "#..........#", ".#........#.", ".#........#.", "..##....##..", "....####....",
            };
            static const char *const dot[12] = {
                "", "", "", "....####....", "...######...", "...######...",
                "...######...", "...######...", "....####....", "", "", "",
            };
            bool on = rdw(c + C_value) != 0;
            for (int y = 0; y < 12; y++)
                for (int x = 0; x < 12; x++)
                    if (ring[y][x] == '#' || (on && dot[y][0] && dot[y][x] == '#')) {
                        QRect px = { (int16_t)(b.top + y), (int16_t)(b.left + x), (int16_t)(b.top + y + 1), (int16_t)(b.left + x + 1) };
                        box(port, px, fg);
                    }
        } else {
            box(port, b, white);
            frame(port, b, fg, 1);
        }
        if (rdw(c + C_value) && kind == 1) {
            for (int i = 2; i < 10; i++) {
                QRect p1 = { (int16_t)(b.top + i), (int16_t)(b.left + i), (int16_t)(b.top + i + 1), (int16_t)(b.left + i + 1) };
                QRect p2 = { (int16_t)(b.top + i), (int16_t)(b.right - 1 - i), (int16_t)(b.top + i + 1), (int16_t)(b.right - i) };
                box(port, p1, fg); box(port, p2, fg);
            }
        }
        QRect tr = { (int16_t)(cy - 8), (int16_t)(r.left + 17), r.bottom, r.right };
        draw_text_in(port, tr, title, 0, 0, 12, 0);
    }
}

static int item_kind(const DItem *it) { return it->type & 0x7F; }

static uint32_t text_handle_from(const uint8_t *s, int len)
{
    uint32_t h = mm_new_handle((uint32_t)len, false);
    if (len) memcpy(HP(rdl(h)), s, len);
    return h;
}

static void draw_item(DlgInfo *di, int i)
{
    DItem *it = &di->it[i];
    if (it->hidden) return;
    uint32_t port = di->dlg;
    int k = item_kind(it);
    uint8_t black = idx_rgb(port, 0, 0, 0), white = idx_rgb(port, 0xFFFF, 0xFFFF, 0xFFFF);
    switch (k) {
    case IT_BUTTON: case IT_CHECK: case IT_RADIO:
        control_draw(it->h);
        break;
    case IT_STATIC: case IT_EDIT: {
        char s[1024];
        uint32_t len = it->h ? mm_handle_size(it->h) : 0;
        if (len > 1000) len = 1000;
        if (k == IT_STATIC) expand_params(it->h ? HP(rdl(it->h)) : (const uint8_t *)"", (int)len, s, sizeof s);
        else { memcpy(s, it->h ? HP(rdl(it->h)) : (const uint8_t *)"", len); s[len] = 0; }
        if (k == IT_EDIT) {
            QRect f = { (int16_t)(it->r.top - 3), (int16_t)(it->r.left - 3), (int16_t)(it->r.bottom + 3), (int16_t)(it->r.right + 3) };
            box(port, f, white);
            frame(port, f, black, 1);
            if (di->edit == i + 1) {
                int w = 0;
                uint16_t of = (uint16_t)rdw(port + P_txFont), os = (uint16_t)rdw(port + P_txSize);
                wrw(port + P_txFont, 0); wrw(port + P_txSize, 12);
                w = text_width(port, (const uint8_t *)s, (int)strlen(s));
                wrw(port + P_txFont, of); wrw(port + P_txSize, os);
                if (di->sel_all && s[0]) {
                    QRect hl = { it->r.top, it->r.left, (int16_t)(it->r.top + 16), (int16_t)(it->r.left + w + 1) };
                    box(port, hl, idx_rgb(port, 0xAAAA, 0xAAAA, 0xFFFF));
                } else {
                    QRect caret = { (int16_t)(it->r.top + 1), (int16_t)(it->r.left + w + 1), (int16_t)(it->r.top + 15), (int16_t)(it->r.left + w + 2) };
                    box(port, caret, black);
                }
            }
        }
        draw_text_in(port, it->r, s, 0, 0, 12, 0);
        break;
    }
    case IT_PICT: {
        uint32_t h = it->h ? it->h : res_get(OSTYPE('P','I','C','T'), it->resid, false);
        if (h) pict_draw(port, h, it->r);
        break;
    }
    case IT_ICON: {
        uint32_t len; const uint8_t *p = res_peek(OSTYPE('c','i','c','n'), it->resid, &len);
        (void)p; /* colour icons are decorative in this game's dialogs */
        break;
    }
    case IT_USER:
        if (it->h && it->h != 0xFFFFFFFFu) {
            uint32_t sd[8], sa[8]; memcpy(sd, D, sizeof sd); memcpy(sa, A, sizeof sa);
            PUSH32(di->dlg); PUSH16((uint16_t)(i + 1));
            call_emulated(it->h);
            memcpy(D, sd, sizeof sd); memcpy(A, sa, sizeof sa);
        }
        break;
    default:
        break;
    }
}

static void draw_dialog(DlgInfo *di)
{
    uint32_t saved = qd_the_port();
    qd_set_port(di->dlg);
    for (int i = 0; i < di->n; i++) draw_item(di, i);
    di->drawn = true;
    qd_set_port(saved);
}

static void redraw_item(DlgInfo *di, int i)
{
    if (!rdb(di->dlg + W_visible)) return;
    uint32_t saved = qd_the_port();
    qd_set_port(di->dlg);
    DItem *it = &di->it[i];
    int k = item_kind(it);
    if (k == IT_STATIC || k == IT_EDIT) {
        QRect r = it->r;
        if (k == IT_EDIT) { r.top -= 3; r.left -= 3; r.bottom += 3; r.right += 3; }
        box(di->dlg, r, idx_rgb(di->dlg, 0xFFFF, 0xFFFF, 0xFFFF));
    }
    draw_item(di, i);
    qd_set_port(saved);
}

/* ---- creation ---- */
static uint32_t build_dialog(uint32_t storage, QRect bounds, uint32_t title, bool visible, int16_t proc, uint32_t behind,
                             bool goaway, uint32_t refcon, int16_t ditl_id, const uint8_t *ditl, uint32_t ditl_len)
{
    uint32_t d = win_create(storage, bounds, title, false, proc, behind, goaway, refcon, DIALOG_SIZE);
    wrw(d + W_kind, 2);
    DlgInfo *di = NULL;
    for (int i = 0; i < MAXDLG; i++) if (!dlgs[i].dlg) { di = &dlgs[i]; break; }
    if (!di) di = &dlgs[0];
    memset(di, 0, sizeof *di);
    di->dlg = d;
    di->needs_update = true;
    if (!ditl) ditl = res_peek(OSTYPE('D','I','T','L'), ditl_id, &ditl_len);
    if (ditl && ditl_len >= 2) {
        int n = ((ditl[0] << 8) | ditl[1]) + 1;
        const uint8_t *p = ditl + 2;
        for (int i = 0; i < n && i < 64 && p + 14 <= ditl + ditl_len; i++) {
            DItem *it = &di->it[i];
            QRect r = { (int16_t)((p[4] << 8) | p[5]), (int16_t)((p[6] << 8) | p[7]), (int16_t)((p[8] << 8) | p[9]), (int16_t)((p[10] << 8) | p[11]) };
            it->r = r;
            it->type = p[12];
            int len = p[13];
            const uint8_t *data = p + 14;
            int k = it->type & 0x7F;
            char s[256]; memcpy(s, data, len); s[len] = 0;
            if (k == IT_BUTTON || k == IT_CHECK || k == IT_RADIO) it->h = new_control(d, r, s, k - IT_BUTTON, 0, 0, 1);
            else if (k == IT_STATIC || k == IT_EDIT) it->h = text_handle_from(data, len);
            else if (k == IT_ICON || k == IT_PICT || k == IT_RESCTL) it->resid = (int16_t)((data[0] << 8) | data[1]);
            else it->h = 0;
            if (k == IT_EDIT && !di->edit) { di->edit = i + 1; di->sel_all = true; }
            p += 14 + len + (len & 1);
            di->n = i + 1;
        }
    }
    wrl(d + D_items, mm_new_handle(4, true));
    wrw(d + D_editField, (uint16_t)(di->edit - 1));
    wrw(d + D_aDefItem, 1);
    if (visible) {
        extern void win_show_public(uint32_t w);
        win_show_public(d);
        draw_dialog(di);
    }
    return d;
}

uint32_t dialog_from_resource(int16_t id, uint32_t storage, uint32_t behind)
{
    uint32_t len; const uint8_t *p = res_peek(OSTYPE('D','L','O','G'), id, &len);
    if (!p) { log_warn("DLOG %d not found", id); return 0; }
    QRect b = { (int16_t)((p[0] << 8) | p[1]), (int16_t)((p[2] << 8) | p[3]), (int16_t)((p[4] << 8) | p[5]), (int16_t)((p[6] << 8) | p[7]) };
    int16_t proc = (int16_t)((p[8] << 8) | p[9]);
    bool vis = p[10] != 0, goaway = p[12] != 0;
    uint32_t refcon = ((uint32_t)p[14] << 24) | ((uint32_t)p[15] << 16) | ((uint32_t)p[16] << 8) | p[17];
    int16_t items = (int16_t)((p[18] << 8) | p[19]);
    uint32_t t = mm_new_ptr(256, true);
    memcpy(HP(t), p + 20, p[20] + 1);
    /* System 7 auto-position: centre on the main screen */
    size_t tl = 21 + p[20]; if (tl & 1) tl++;
    if (tl + 2 <= len) {
        uint16_t pos = (uint16_t)((p[tl] << 8) | p[tl + 1]);
        if (pos == 0x280A || pos == 0x300A || pos == 0x700A || pos == 0xB00A) {
            int w = b.right - b.left, h = b.bottom - b.top;
            b.left = (int16_t)((SCREEN_W - w) / 2); b.right = (int16_t)(b.left + w);
            b.top = (int16_t)((SCREEN_H - h) / 3); b.bottom = (int16_t)(b.top + h);
        }
    }
    uint32_t d = build_dialog(storage, b, t, vis, proc, behind, goaway, refcon, items, NULL, 0);
    mm_dispose_ptr(t);
    return d;
}

static void dispose_dialog(uint32_t d, bool free_storage)
{
    DlgInfo *di = dinfo(d);
    if (di) {
        for (int i = 0; i < di->n; i++) {
            int k = item_kind(&di->it[i]);
            if ((k >= IT_BUTTON && k <= IT_RADIO) || k == IT_STATIC || k == IT_EDIT)
                if (di->it[i].h && mm_is_handle(di->it[i].h)) mm_dispose_handle(di->it[i].h);
        }
        di->dlg = 0;
    }
    win_dispose(d, free_storage);
}

static bool find_text_handle(uint32_t h, DlgInfo **pdi, int *pi)
{
    for (int k = 0; k < MAXDLG; k++) {
        if (!dlgs[k].dlg) continue;
        for (int i = 0; i < dlgs[k].n; i++) if (dlgs[k].it[i].h == h) { *pdi = &dlgs[k]; *pi = i; return true; }
    }
    return false;
}

/* ---- event handling shared by ModalDialog / DialogSelect ---- */
static int item_at(DlgInfo *di, int h, int v)
{
    for (int i = 0; i < di->n; i++) {
        DItem *it = &di->it[i];
        if (it->hidden) continue;
        if (v >= it->r.top && v < it->r.bottom && h >= it->r.left && h < it->r.right) return i + 1;
    }
    return 0;
}

static void edit_key(DlgInfo *di, uint8_t ch)
{
    DItem *it = &di->it[di->edit - 1];
    uint32_t len = mm_handle_size(it->h);
    if (ch == 8 || ch == 127) {
        if (di->sel_all) len = 0;
        else if (len) len--;
        mm_set_handle_size(it->h, len);
    } else if (ch >= 32) {
        if (di->sel_all) len = 0;
        if (len < 255) { mm_set_handle_size(it->h, len + 1); wrb(rdl(it->h) + len, ch); }
    } else return;
    di->sel_all = false;
    redraw_item(di, di->edit - 1);
}

static void next_edit(DlgInfo *di)
{
    int start = di->edit;
    for (int k = 1; k <= di->n; k++) {
        int i = (start + k - 1) % di->n;
        if (item_kind(&di->it[i]) == IT_EDIT && !di->it[i].hidden) {
            int old = di->edit;
            di->edit = i + 1; di->sel_all = true;
            wrw(di->dlg + D_editField, (uint16_t)i);
            if (old) redraw_item(di, old - 1);
            redraw_item(di, i);
            return;
        }
    }
}

static bool track_item(DlgInfo *di, int item)
{
    DItem *it = &di->it[item - 1];
    int k = item_kind(it);
    if (k != IT_BUTTON && k != IT_CHECK && k != IT_RADIO) return true;
    uint32_t c = rdl(it->h);
    if (k == IT_BUTTON) { wrb(c + C_hilite, 1); redraw_item(di, item - 1); }
    bool inside = true;
    while (evt_button_down()) {
        evt_idle();
        int h, v; evt_mouse_local(di->dlg, &h, &v);
        bool now = v >= it->r.top && v < it->r.bottom && h >= it->r.left && h < it->r.right;
        if (now != inside && k == IT_BUTTON) { wrb(c + C_hilite, now ? 1 : 0); redraw_item(di, item - 1); }
        inside = now;
    }
    if (k == IT_BUTTON) { wrb(c + C_hilite, 0); redraw_item(di, item - 1); }
    return inside;
}

/* returns item hit or 0; ev is an emulated EventRecord */
static int dialog_default_event(DlgInfo *di, uint32_t ev)
{
    uint16_t what = (uint16_t)rdw(ev);
    if (what == 1) { /* mouseDown */
        int16_t v = (int16_t)rdw(ev + 10), h = (int16_t)rdw(ev + 12);
        uint32_t saved = qd_the_port(); qd_set_port(di->dlg);
        PUSH32(((uint32_t)(uint16_t)v << 16) | (uint16_t)h);
        uint32_t ptp = A[7];
        PUSH32(ptp); trap_dispatch(0xA871); /* GlobalToLocal */
        v = (int16_t)rdw(ptp); h = (int16_t)rdw(ptp + 2);
        A[7] += 4;
        qd_set_port(saved);
        int item = item_at(di, h, v);
        if (!item) return 0;
        DItem *it = &di->it[item - 1];
        if (it->type & 0x80) {
            if (item_kind(it) == IT_EDIT && di->edit != item) { di->edit = item - 1 ? item - 1 : 0; next_edit(di); }
            return 0;
        }
        if (item_kind(it) == IT_EDIT) { if (di->edit != item) { di->edit = item - 1; next_edit(di); } return item; }
        if (!track_item(di, item)) return 0;
        return item;
    }
    if (what == 3 || what == 5) { /* keyDown / autoKey */
        uint8_t ch = (uint8_t)rdl(ev + 2);
        uint16_t mods = (uint16_t)rdw(ev + 14);
        if (ch == 13 || ch == 3) {
            int def = (int16_t)rdw(di->dlg + D_aDefItem);
            if (def >= 1 && def <= di->n && !(di->it[def - 1].type & 0x80)) return def;
            return 0;
        }
        if ((ch == 27) || ((mods & 0x0100) && ch == '.')) {
            if (di->n >= 2 && item_kind(&di->it[1]) == IT_BUTTON) return 2;
            return 0;
        }
        if (ch == 9) { next_edit(di); return 0; }
        if (di->edit) { edit_key(di, ch); return di->edit; }
    }
    return 0;
}

/* call a Pascal filter proc: FUNCTION(dialog, VAR event, VAR itemHit): BOOLEAN */
static bool call_filter(uint32_t filter, uint32_t dlg, uint32_t ev, uint32_t itemp)
{
    uint32_t sd[8], sa[8]; memcpy(sd, D, sizeof sd); memcpy(sa, A, sizeof sa);
    PUSH16(0);
    PUSH32(dlg); PUSH32(ev); PUSH32(itemp);
    call_emulated(filter);
    bool r = (rdw(A[7]) >> 8) != 0;
    A[7] += 2;
    memcpy(D, sd, sizeof sd); memcpy(A, sa, sizeof sa);
    return r;
}

static uint32_t modal_front_dialog(void)
{
    uint32_t w = win_front();
    if (w && dinfo(w)) return w;
    for (int i = 0; i < MAXDLG; i++) if (dlgs[i].dlg && rdb(dlgs[i].dlg + W_visible)) return dlgs[i].dlg;
    return 0;
}

static void T_ModalDialog(uint16_t op)
{
    (void)op;
    uint32_t itemp = ARG32(), filter = ARG32();
    uint32_t d = modal_front_dialog();
    DlgInfo *di = d ? dinfo(d) : NULL;
    if (!di) { wrw(itemp, 1); return; }
    if (!di->drawn) draw_dialog(di);
    uint32_t ev = mm_new_ptr(16, true);
    for (;;) {
        evt_idle();
        if (evt_quit_requested()) { wrw(itemp, 2); break; }
        if (di->needs_update) {
            /* one update event per newly shown dialog, as the Window Manager would */
            memset(HP(ev), 0, 16);
            wrw(ev, 6); wrl(ev + 2, d); wrl(ev + 6, ticks_now());
            di->needs_update = false;
        } else if (!evt_next(0xFFFF & ~0x0400, ev, true)) {
            memset(HP(ev), 0, 16); wrl(ev + 6, ticks_now());
            const InputState *in = plat_input();
            wrw(ev + 10, (uint16_t)in->mouse_y); wrw(ev + 12, (uint16_t)in->mouse_x); wrw(ev + 14, in->modifiers);
        }
        wrw(itemp, 0);
        if (filter && call_filter(filter, d, ev, itemp)) break;
        uint16_t what = (uint16_t)rdw(ev);
        if (what == 6 && filter) continue; /* filter handled drawing of user items */
        int item = dialog_default_event(di, ev);
        if (item) { wrw(itemp, (uint16_t)item); break; }
    }
    mm_dispose_ptr(ev);
}

static void T_IsDialogEvent(uint16_t op)
{
    (void)op;
    uint32_t ev = ARG32();
    uint16_t what = (uint16_t)rdw(ev);
    uint32_t d = modal_front_dialog();
    RET8(d && (what == 1 || what == 3 || what == 5 || what == 6 || what == 8 || what == 0));
}

static void T_DialogSelect(uint16_t op)
{
    (void)op;
    uint32_t itemp = ARG32(), dp = ARG32(), ev = ARG32();
    uint32_t d = modal_front_dialog();
    DlgInfo *di = d ? dinfo(d) : NULL;
    if (!di) { RET8(0); return; }
    if (!di->drawn) draw_dialog(di);
    int item = dialog_default_event(di, ev);
    wrl(dp, d);
    wrw(itemp, (uint16_t)item);
    RET8(item != 0);
}

/* ---- traps ---- */
static void T_InitDialogs(uint16_t op) { (void)op; (void)ARG32(); }
static void T_GetNewDialog(uint16_t op)
{
    (void)op;
    uint32_t behind = ARG32(), storage = ARG32(); int16_t id = ARGS16();
    RET32(dialog_from_resource(id, storage, behind));
}
static void T_NewDialog(uint16_t op)
{
    (void)op;
    uint32_t items = ARG32(), refcon = ARG32(); bool goaway = ARG8() != 0; uint32_t behind = ARG32(); int16_t proc = ARGS16();
    bool vis = ARG8() != 0; uint32_t title = ARG32(), bounds = ARG32(), storage = ARG32();
    RET32(build_dialog(storage, rd_rect(bounds), title, vis, proc, behind, goaway, refcon, 0,
                       items ? HP(rdl(items)) : NULL, items ? mm_handle_size(items) : 0));
}
static void T_DisposeDialog(uint16_t op) { (void)op; dispose_dialog(ARG32(), true); }
static void T_CloseDialog(uint16_t op) { (void)op; dispose_dialog(ARG32(), false); }
static void T_DrawDialog(uint16_t op) { (void)op; uint32_t d = ARG32(); DlgInfo *di = dinfo(d); if (di) draw_dialog(di); }
static void T_UpdateDialog(uint16_t op) { (void)op; (void)ARG32(); uint32_t d = ARG32(); DlgInfo *di = dinfo(d); if (di) draw_dialog(di); }
static void T_GetDialogItem(uint16_t op)
{
    (void)op;
    uint32_t pbox = ARG32(), ph = ARG32(), ptype = ARG32(); int16_t item = ARGS16(); uint32_t d = ARG32();
    DlgInfo *di = dinfo(d);
    if (!di || item < 1 || item > di->n) { if (ptype) wrw(ptype, 0); if (ph) wrl(ph, 0); return; }
    DItem *it = &di->it[item - 1];
    if (ptype) wrw(ptype, it->type);
    if (ph) wrl(ph, it->h);
    if (pbox) wr_rect(pbox, it->r);
}
static void T_SetDialogItem(uint16_t op)
{
    (void)op;
    uint32_t pbox = ARG32(), h = ARG32(); int16_t type = ARGS16(); int16_t item = ARGS16(); uint32_t d = ARG32();
    DlgInfo *di = dinfo(d);
    if (!di || item < 1 || item > 64) return;
    if (item > di->n) di->n = item;
    DItem *it = &di->it[item - 1];
    it->type = (uint8_t)type; it->h = h; it->r = rd_rect(pbox);
}
static void T_GetDialogItemText(uint16_t op)
{
    (void)op;
    uint32_t s = ARG32(), h = ARG32();
    uint32_t len = h && rdl(h) ? mm_handle_size(h) : 0;
    if (len > 255) len = 255;
    wrb(s, (uint8_t)len);
    if (len) memcpy(HP(s + 1), HP(rdl(h)), len);
}
static void T_SetDialogItemText(uint16_t op)
{
    (void)op;
    uint32_t s = ARG32(), h = ARG32();
    if (!h) return;
    uint32_t len = rdb(s);
    mm_set_handle_size(h, len);
    if (len) memcpy(HP(rdl(h)), HP(s + 1), len);
    DlgInfo *di; int i;
    if (find_text_handle(h, &di, &i)) redraw_item(di, i);
}
static void T_SelectDialogItemText(uint16_t op)
{
    (void)op;
    int16_t end = ARGS16(), start = ARGS16(); int16_t item = ARGS16(); uint32_t d = ARG32();
    DlgInfo *di = dinfo(d);
    if (!di || item < 1 || item > di->n) return;
    int old = di->edit;
    di->edit = item;
    di->sel_all = end > start;
    wrw(d + D_editField, (uint16_t)(item - 1));
    if (old && old != item) redraw_item(di, old - 1);
    redraw_item(di, item - 1);
}
static void T_FindDialogItem(uint16_t op)
{
    (void)op;
    uint32_t pt = ARG32(); uint32_t d = ARG32();
    DlgInfo *di = dinfo(d);
    RET16(di ? item_at(di, (int16_t)pt, (int16_t)(pt >> 16)) - 1 : -1);
}
static void T_HideDialogItem(uint16_t op) { (void)op; int16_t item = ARGS16(); uint32_t d = ARG32(); DlgInfo *di = dinfo(d); if (di && item >= 1 && item <= di->n) di->it[item - 1].hidden = true; }
static void T_ShowDialogItem(uint16_t op) { (void)op; int16_t item = ARGS16(); uint32_t d = ARG32(); DlgInfo *di = dinfo(d); if (di && item >= 1 && item <= di->n) { di->it[item - 1].hidden = false; redraw_item(di, item - 1); } }
static void T_ParamText(uint16_t op)
{
    (void)op;
    uint32_t p3 = ARG32(), p2 = ARG32(), p1 = ARG32(), p0 = ARG32();
    uint32_t ps[4] = { p0, p1, p2, p3 };
    for (int i = 0; i < 4; i++) { if (ps[i]) pstr_to_c(ps[i], param_text[i], sizeof param_text[i]); else param_text[i][0] = 0; }
}
static void T_nop_dlg(uint16_t op) { (void)op; (void)ARG16(); }
static void T_ErrorSound(uint16_t op) { (void)op; (void)ARG32(); }
static void T_DialogDispatch(uint16_t op)
{
    (void)op;
    uint16_t sel = (uint16_t)D[0];
    switch (sel) {
    case 0x0203: RET16(0); return; /* GetStdFilterProc(&proc): proc = 0 */
    case 0x0304: { int16_t item = ARGS16(); uint32_t d = ARG32(); wrw(d + D_aDefItem, (uint16_t)item); RET16(0); return; } /* SetDialogDefaultItem */
    case 0x0305: (void)ARG16(); (void)ARG32(); RET16(0); return; /* SetDialogCancelItem */
    case 0x0306: (void)ARG8(); (void)ARG32(); RET16(0); return;  /* SetDialogTracksCursor */
    default: log_warn("DialogDispatch %04X ignored", sel); A[7] += ((sel >> 8) & 0xFF) * 2; RET16(0); return;
    }
}

/* ---- alerts ---- */
static int run_alert(int16_t id, uint32_t filter)
{
    uint32_t len; const uint8_t *p = res_peek(OSTYPE('A','L','R','T'), id, &len);
    if (!p) { log_warn("ALRT %d missing", id); return 1; }
    QRect b = { (int16_t)((p[0] << 8) | p[1]), (int16_t)((p[2] << 8) | p[3]), (int16_t)((p[4] << 8) | p[5]), (int16_t)((p[6] << 8) | p[7]) };
    int w = b.right - b.left, h = b.bottom - b.top;
    if (b.left < 0 || b.right > SCREEN_W || b.top < 0 || b.bottom > SCREEN_H || len >= 14) {
        b.left = (int16_t)((SCREEN_W - w) / 2); b.right = (int16_t)(b.left + w);
        b.top = (int16_t)((SCREEN_H - h) / 3); b.bottom = (int16_t)(b.top + h);
    }
    int16_t items = (int16_t)((p[8] << 8) | p[9]);
    uint32_t d = build_dialog(0, b, 0, true, 1, 0xFFFFFFFFu, false, 0, items, NULL, 0);
    DlgInfo *di = dinfo(d);
    if (di) draw_dialog(di);
    uint32_t itemp = mm_new_ptr(2, true);
    PUSH32(filter); PUSH32(itemp);
    T_ModalDialog(0);
    int item = (int16_t)rdw(itemp);
    mm_dispose_ptr(itemp);
    dispose_dialog(d, true);
    return item;
}
static void T_Alert(uint16_t op) { (void)op; uint32_t f = ARG32(); int16_t id = ARGS16(); RET16(run_alert(id, f)); }

/* ---- Control Manager ---- */
static void T_GetControlValue(uint16_t op) { (void)op; uint32_t c = ARG32(); RET16(c && rdl(c) ? rdw(rdl(c) + C_value) : 0); }
static void T_SetControlValue(uint16_t op)
{
    (void)op;
    int16_t v = ARGS16(); uint32_t c = ARG32();
    if (!c || !rdl(c)) return;
    int16_t mn = (int16_t)rdw(rdl(c) + C_min), mx = (int16_t)rdw(rdl(c) + C_max);
    if (v < mn) v = mn;
    if (v > mx) v = mx;
    wrw(rdl(c) + C_value, (uint16_t)v);
    uint32_t saved = qd_the_port(); qd_set_port(rdl(rdl(c) + C_owner)); control_draw(c); qd_set_port(saved);
}
static void T_GetControlMinimum(uint16_t op) { (void)op; uint32_t c = ARG32(); RET16(rdw(rdl(c) + C_min)); }
static void T_GetControlMaximum(uint16_t op) { (void)op; uint32_t c = ARG32(); RET16(rdw(rdl(c) + C_max)); }
static void T_SetControlMinimum(uint16_t op) { (void)op; int16_t v = ARGS16(); uint32_t c = ARG32(); wrw(rdl(c) + C_min, (uint16_t)v); }
static void T_SetControlMaximum(uint16_t op) { (void)op; int16_t v = ARGS16(); uint32_t c = ARG32(); wrw(rdl(c) + C_max, (uint16_t)v); }
static void T_HiliteControl(uint16_t op)
{
    (void)op;
    int16_t v = ARGS16(); uint32_t c = ARG32();
    if (!c || !rdl(c)) return;
    wrb(rdl(c) + C_hilite, (uint8_t)v);
    uint32_t saved = qd_the_port(); qd_set_port(rdl(rdl(c) + C_owner)); control_draw(c); qd_set_port(saved);
}
static void T_HideControl(uint16_t op)
{
    (void)op;
    uint32_t c = ARG32();
    if (!c || !rdl(c)) return;
    wrb(rdl(c) + C_vis, 0);
    uint32_t port = rdl(rdl(c) + C_owner);
    if (port && rdb(port + W_visible)) {
        QRect r = rd_rect(rdl(c) + C_rect);
        r.top -= 4; r.left -= 4; r.bottom += 4; r.right += 4;   /* default-button ring too */
        box(port, r, idx_rgb(port, 0xFFFF, 0xFFFF, 0xFFFF));
    }
}
static void T_ShowControl(uint16_t op) { (void)op; uint32_t c = ARG32(); if (c && rdl(c)) { wrb(rdl(c) + C_vis, 0xFF); control_draw(c); } }
static void T_DrawControls(uint16_t op) { (void)op; uint32_t w = ARG32(); DlgInfo *di = dinfo(w); if (di) draw_dialog(di); }
static void T_Draw1Control(uint16_t op) { (void)op; control_draw(ARG32()); }
static void T_GetControlTitle(uint16_t op) { (void)op; uint32_t s = ARG32(), c = ARG32(); memcpy(HP(s), HP(rdl(c) + C_title), rdb(rdl(c) + C_title) + 1); }
static void T_SetControlTitle(uint16_t op) { (void)op; uint32_t s = ARG32(), c = ARG32(); memcpy(HP(rdl(c) + C_title), HP(s), rdb(s) + 1); control_draw(c); }
static void T_GetControlReference(uint16_t op) { (void)op; uint32_t c = ARG32(); RET32(rdl(rdl(c) + 36)); }
static void T_SetControlReference(uint16_t op) { (void)op; uint32_t v = ARG32(), c = ARG32(); wrl(rdl(c) + 36, v); }
static void T_TrackControl(uint16_t op) { (void)op; (void)ARG32(); (void)ARG32(); (void)ARG32(); RET16(10); }
static void T_FindControl(uint16_t op) { (void)op; uint32_t pc = ARG32(); (void)ARG32(); (void)ARG32(); wrl(pc, 0); RET16(0); }
static void T_KillControls(uint16_t op) { (void)op; (void)ARG32(); }
static void T_DisposeControl(uint16_t op) { (void)op; uint32_t c = ARG32(); if (c && mm_is_handle(c)) mm_dispose_handle(c); }
static void T_NewControl(uint16_t op)
{
    (void)op;
    uint32_t refcon = ARG32(); int16_t proc = ARGS16(), mx = ARGS16(), mn = ARGS16(), val = ARGS16();
    bool vis = ARG8() != 0; uint32_t title = ARG32(), r = ARG32(), w = ARG32();
    char t[256]; pstr_to_c(title, t, sizeof t);
    uint32_t h = new_control(w, rd_rect(r), t, proc & 3, val, mn, mx);
    wrl(rdl(h) + 36, refcon);
    if (!vis) wrb(rdl(h) + C_vis, 0);
    RET32(h);
}

void dialog_register_traps(void)
{
    trap_register(0xA97B, T_InitDialogs, "InitDialogs");
    trap_register(0xA97C, T_GetNewDialog, "GetNewDialog");
    trap_register(0xA97D, T_NewDialog, "NewDialog");
    trap_register(0xA983, T_DisposeDialog, "DisposeDialog");
    trap_register(0xA982, T_CloseDialog, "CloseDialog");
    trap_register(0xA981, T_DrawDialog, "DrawDialog");
    trap_register(0xA978, T_UpdateDialog, "UpdateDialog");
    trap_register(0xA991, T_ModalDialog, "ModalDialog");
    trap_register(0xA97F, T_IsDialogEvent, "IsDialogEvent");
    trap_register(0xA980, T_DialogSelect, "DialogSelect");
    trap_register(0xA98D, T_GetDialogItem, "GetDialogItem");
    trap_register(0xA98E, T_SetDialogItem, "SetDialogItem");
    trap_register(0xA990, T_GetDialogItemText, "GetDialogItemText");
    trap_register(0xA98F, T_SetDialogItemText, "SetDialogItemText");
    trap_register(0xA97E, T_SelectDialogItemText, "SelectDialogItemText");
    trap_register(0xA984, T_FindDialogItem, "FindDialogItem");
    trap_register(0xA827, T_HideDialogItem, "HideDialogItem");
    trap_register(0xA828, T_ShowDialogItem, "ShowDialogItem");
    trap_register(0xA98B, T_ParamText, "ParamText");
    trap_register(0xA979, T_nop_dlg, "CouldDialog");
    trap_register(0xA97A, T_nop_dlg, "FreeDialog");
    trap_register(0xA989, T_nop_dlg, "CouldAlert");
    trap_register(0xA98A, T_nop_dlg, "FreeAlert");
    trap_register(0xA98C, T_ErrorSound, "ErrorSound");
    trap_register(0xAA68, T_DialogDispatch, "DialogDispatch");
    trap_register(0xA985, T_Alert, "Alert");
    trap_register(0xA986, T_Alert, "StopAlert");
    trap_register(0xA987, T_Alert, "NoteAlert");
    trap_register(0xA988, T_Alert, "CautionAlert");
    trap_register(0xA960, T_GetControlValue, "GetControlValue");
    trap_register(0xA963, T_SetControlValue, "SetControlValue");
    trap_register(0xA961, T_GetControlMinimum, "GetControlMinimum");
    trap_register(0xA962, T_GetControlMaximum, "GetControlMaximum");
    trap_register(0xA964, T_SetControlMinimum, "SetControlMinimum");
    trap_register(0xA965, T_SetControlMaximum, "SetControlMaximum");
    trap_register(0xA95D, T_HiliteControl, "HiliteControl");
    trap_register(0xA958, T_HideControl, "HideControl");
    trap_register(0xA957, T_ShowControl, "ShowControl");
    trap_register(0xA969, T_DrawControls, "DrawControls");
    trap_register(0xA96D, T_Draw1Control, "Draw1Control");
    trap_register(0xA95E, T_GetControlTitle, "GetControlTitle");
    trap_register(0xA95F, T_SetControlTitle, "SetControlTitle");
    trap_register(0xA95A, T_GetControlReference, "GetControlReference");
    trap_register(0xA95B, T_SetControlReference, "SetControlReference");
    trap_register(0xA968, T_TrackControl, "TrackControl");
    trap_register(0xA96C, T_FindControl, "FindControl");
    trap_register(0xA956, T_KillControls, "KillControls");
    trap_register(0xA955, T_DisposeControl, "DisposeControl");
    trap_register(0xA954, T_NewControl, "NewControl");
}
