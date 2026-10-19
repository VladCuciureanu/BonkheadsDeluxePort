/* Window Manager and Menu Manager.
 *
 * Every window is a colour port whose PixMap aliases the screen framebuffer
 * (bounds offset by the window's global position). Pixels under a window are
 * saved when it appears and restored when it goes away, standing in for the
 * update events the game never expected from a full-screen window. */
#include "qd.h"
#include "res.h"
#include <stdlib.h>

uint8_t qd_rgn_to_black(void);
#define MAXWIN 32
typedef struct { uint32_t w; uint8_t *under; QRect ur; bool own_storage; HiTextSnap *text_under; } WinInfo;
static WinInfo wins[MAXWIN];   /* index 0 = front */
static int nwins;

static WinInfo *info(uint32_t w)
{
    for (int i = 0; i < nwins; i++) if (wins[i].w == w) return &wins[i];
    return NULL;
}

static QRect struct_rect(uint32_t w);

/* Screen rectangles of visible windows in front of `w` (which would clip any
 * drawing into w on a real Mac). Returns the count; 0 if w is not a window. */
int win_occluders(uint32_t w, QRect *out, int max)
{
    int k = -1;
    for (int i = 0; i < nwins; i++) if (wins[i].w == w) { k = i; break; }
    if (k <= 0) return 0;
    int n = 0;
    for (int i = 0; i < k && n < max; i++)
        if (rdb(wins[i].w + W_visible)) out[n++] = struct_rect(wins[i].w);
    return n;
}

bool win_exists_refcon(uint32_t refcon)
{
    for (int i = 0; i < nwins; i++) if (rdl(wins[i].w + W_refCon) == refcon) return true;
    return false;
}

uint32_t win_front(void)
{
    for (int i = 0; i < nwins; i++) if (rdb(wins[i].w + W_visible)) return wins[i].w;
    return 0;
}

static QRect global_content(uint32_t w)
{
    PixDesc pd;
    qd_port_pixels(w, &pd);
    QRect pr = rd_rect(w + P_portRect);
    QRect g = { (int16_t)(pr.top - pd.bounds.top), (int16_t)(pr.left - pd.bounds.left),
                (int16_t)(pr.bottom - pd.bounds.top), (int16_t)(pr.right - pd.bounds.left) };
    return g;
}

static int frame_margin(uint32_t w)
{
    int16_t proc = (int16_t)rdw(w + W_defProc + 2);
    switch (proc & 0xF) {
    case 1: return 8;   /* dBoxProc */
    case 2: return 1;   /* plainDBox */
    case 3: return 3;   /* altDBoxProc */
    case 5: return 8;   /* movableDBoxProc */
    default: return 2;
    }
}

static QRect struct_rect(uint32_t w)
{
    QRect g = global_content(w);
    int m = frame_margin(w);
    int16_t proc = (int16_t)rdw(w + W_defProc + 2);
    int top = (proc == 0 || proc == 4 || proc == 8 || proc == 5) ? 20 : m;
    QRect s = { (int16_t)(g.top - top), (int16_t)(g.left - m), (int16_t)(g.bottom + m + 2), (int16_t)(g.right + m + 2) };
    QRect scr = { 0, 0, SCREEN_H, SCREEN_W };
    return rect_sect(s, scr);
}

void win_save_under(uint32_t w)
{
    WinInfo *wi = info(w);
    if (!wi) return;
    free(wi->under); wi->under = NULL;
    QRect r = struct_rect(w);
    if (rect_empty(r)) return;
    /* full-screen windows don't need restoring */
    if (r.top == 0 && r.left == 0 && r.bottom == SCREEN_H && r.right == SCREEN_W) { wi->ur = r; return; }
    int ww = r.right - r.left, hh = r.bottom - r.top;
    wi->under = (uint8_t *)malloc((size_t)ww * hh);
    wi->text_under = hitext_save(r);
    for (int y = 0; y < hh; y++) memcpy(wi->under + (size_t)y * ww, HP(g_screen_fb + (uint32_t)(r.top + y) * SCREEN_W + r.left), ww);
    wi->ur = r;
}

void win_restore_under(uint32_t w)
{
    WinInfo *wi = info(w);
    if (!wi || !wi->under) return;
    QRect r = wi->ur;
    int ww = r.right - r.left, hh = r.bottom - r.top;
    hitext_damage_screen(r);
    for (int y = 0; y < hh; y++) memcpy(HP(g_screen_fb + (uint32_t)(r.top + y) * SCREEN_W + r.left), wi->under + (size_t)y * ww, ww);
    free(wi->under); wi->under = NULL;
    hitext_restore(wi->text_under); wi->text_under = NULL;
}

static void fill_screen(QRect r, uint8_t idx)
{
    QRect scr = { 0, 0, SCREEN_H, SCREEN_W };
    r = rect_sect(r, scr);
    hitext_damage_screen(r);
    for (int y = r.top; y < r.bottom; y++) memset(HP(g_screen_fb + (uint32_t)y * SCREEN_W + r.left), idx, (size_t)(r.right - r.left));
}

void win_draw_frame(uint32_t w)
{
    QRect g = global_content(w);
    int16_t proc = (int16_t)rdw(w + W_defProc + 2);
    uint32_t ct = qd_device_ctab();
    uint8_t black = qd_rgb_to_index(ct, 0, 0, 0), white = qd_rgb_to_index(ct, 0xFFFF, 0xFFFF, 0xFFFF);
    uint8_t grey = qd_rgb_to_index(ct, 0xCCCC, 0xCCCC, 0xCCCC);
    QRect s = struct_rect(w);
    if (s.top == 0 && s.left == 0 && s.bottom >= SCREEN_H && s.right >= SCREEN_W && rect_empty(rect_sect(s, g)) == false
        && g.top <= 0 && g.left <= 0) return;
    switch (proc & 0xF) {
    case 2: { QRect o = { (int16_t)(g.top - 1), (int16_t)(g.left - 1), (int16_t)(g.bottom + 1), (int16_t)(g.right + 1) }; fill_screen(o, black); break; }
    case 3: {
        QRect sh = { (int16_t)(g.top + 2), (int16_t)(g.left + 2), (int16_t)(g.bottom + 3), (int16_t)(g.right + 3) }; fill_screen(sh, black);
        QRect o = { (int16_t)(g.top - 1), (int16_t)(g.left - 1), (int16_t)(g.bottom + 1), (int16_t)(g.right + 1) }; fill_screen(o, black);
        break;
    }
    case 1: case 5: {
        QRect o = { (int16_t)(g.top - 8), (int16_t)(g.left - 8), (int16_t)(g.bottom + 8), (int16_t)(g.right + 8) }; fill_screen(o, black);
        QRect i1 = { (int16_t)(g.top - 7), (int16_t)(g.left - 7), (int16_t)(g.bottom + 7), (int16_t)(g.right + 7) }; fill_screen(i1, white);
        QRect i2 = { (int16_t)(g.top - 5), (int16_t)(g.left - 5), (int16_t)(g.bottom + 5), (int16_t)(g.right + 5) }; fill_screen(i2, black);
        QRect i3 = { (int16_t)(g.top - 4), (int16_t)(g.left - 4), (int16_t)(g.bottom + 4), (int16_t)(g.right + 4) }; fill_screen(i3, white);
        break;
    }
    default: {
        QRect o = { (int16_t)(g.top - 20), (int16_t)(g.left - 1), (int16_t)(g.bottom + 1), (int16_t)(g.right + 1) }; fill_screen(o, black);
        QRect t = { (int16_t)(g.top - 19), g.left, (int16_t)(g.top - 1), g.right }; fill_screen(t, grey);
        break;
    }
    }
    (void)white;
}

static void erase_content(uint32_t w)
{
    uint32_t saved = qd_the_port();
    qd_set_port(w);
    static const uint8_t solid[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    qd_fill_rect(w, rd_rect(w + P_portRect), solid, 8, qd_bk_index(w), qd_bk_index(w));
    qd_set_port(saved);
}

static void set_global_pos(uint32_t w, int16_t h, int16_t v)
{
    uint32_t pm = rdl(rdl(w + P_portBits));
    QRect pr = rd_rect(w + P_portRect);
    QRect b = { (int16_t)(pr.top - v), (int16_t)(pr.left - h), (int16_t)(pr.top - v + SCREEN_H), (int16_t)(pr.left - h + SCREEN_W) };
    wr_rect(pm + PM_bounds, b);
    QRect scr_local = b;
    qd_set_rgn(rdl(w + P_visRgn), rect_sect(pr, scr_local));
    QRect g = global_content(w);
    qd_set_rgn(rdl(w + W_contRgn), g);
    qd_set_rgn(rdl(w + W_strucRgn), struct_rect(w));
}

static void link_list(void)
{
    for (int i = 0; i < nwins; i++) wrl(wins[i].w + W_next, i + 1 < nwins ? wins[i + 1].w : 0);
    wrl(LM_WindowList, nwins ? wins[0].w : 0);
}

void pal_window_front(uint32_t win);

static void show(uint32_t w)
{
    if (rdb(w + W_visible)) return;
    wrb(w + W_visible, 1);
    win_save_under(w);
    win_draw_frame(w);
    erase_content(w);
}

void win_show_public(uint32_t w) { show(w); }

static void hide(uint32_t w)
{
    if (!rdb(w + W_visible)) return;
    wrb(w + W_visible, 0);
    win_restore_under(w);
}

uint32_t win_create(uint32_t storage, QRect bounds, uint32_t title, bool visible, int16_t proc,
                    uint32_t behind, bool goaway, uint32_t refcon, uint32_t size)
{
    bool own = !storage;
    uint32_t w = storage ? storage : mm_new_ptr(size, true);
    if (storage) memset(HP(w), 0, size);
    qd_init_port(w, true);
    QRect pr = { 0, 0, (int16_t)(bounds.bottom - bounds.top), (int16_t)(bounds.right - bounds.left) };
    wr_rect(w + P_portRect, pr);
    wrw(w + W_kind, 8);
    wrb(w + W_visible, 0);
    wrb(w + W_goAway, goaway);
    wrl(w + W_strucRgn, qd_new_rgn(pr));
    wrl(w + W_contRgn, qd_new_rgn(pr));
    wrl(w + W_updateRgn, qd_new_rgn(pr));
    wrl(w + W_defProc, (uint32_t)(uint16_t)proc);
    wrl(w + W_refCon, refcon);
    char t[256] = "";
    if (title) pstr_to_c(title, t, sizeof t);
    uint32_t th = mm_new_handle(256, true);
    c_to_pstr(t, rdl(th));
    wrl(w + W_title, th);
    /* insert into window list */
    if (nwins < MAXWIN) {
        int pos = 0;
        if (behind == 0) pos = nwins;                  /* behind = nil: at the back */
        else if (behind == 0xFFFFFFFFu) pos = 0;       /* in front */
        else for (pos = 0; pos < nwins && wins[pos].w != behind; pos++) {}
        if (behind != 0 && behind != 0xFFFFFFFFu && pos < nwins) pos++;
        memmove(&wins[pos + 1], &wins[pos], sizeof(WinInfo) * (nwins - pos));
        memset(&wins[pos], 0, sizeof(WinInfo));
        wins[pos].w = w; wins[pos].own_storage = own;
        nwins++;
        link_list();
    }
    set_global_pos(w, bounds.left, bounds.top);
    qd_set_port(w);
    if (visible) show(w);
    return w;
}

void win_dispose(uint32_t w, bool free_storage)
{
    WinInfo *wi = info(w);
    if (!wi) return;
    hide(w);
    bool own = wi->own_storage;
    int i = (int)(wi - wins);
    memmove(&wins[i], &wins[i + 1], sizeof(WinInfo) * (nwins - i - 1));
    nwins--;
    link_list();
    uint32_t regs[] = { P_visRgn, P_clipRgn, W_strucRgn, W_contRgn, W_updateRgn };
    for (size_t k = 0; k < sizeof regs / sizeof regs[0]; k++) { uint32_t h = rdl(w + regs[k]); if (h && mm_is_handle(h)) mm_dispose_handle(h); wrl(w + regs[k], 0); }
    uint32_t pmh = rdl(w + P_portBits);
    if (pmh && mm_is_handle(pmh)) mm_dispose_handle(pmh);
    if (rdl(w + W_title) && mm_is_handle(rdl(w + W_title))) mm_dispose_handle(rdl(w + W_title));
    if (qd_the_port() == w) qd_set_port(win_front());
    if (free_storage && own) mm_dispose_ptr(w);
}

/* ------------------------------------------------------------ traps */
static void T_InitWindows(uint16_t op) { (void)op; }
static void new_window_common(uint32_t size)
{
    uint32_t refcon = ARG32(); bool goaway = ARG8() != 0; uint32_t behind = ARG32(); int16_t proc = ARGS16();
    bool vis = ARG8() != 0; uint32_t title = ARG32(); uint32_t bounds = ARG32(); uint32_t storage = ARG32();
    RET32(win_create(storage, rd_rect(bounds), title, vis, proc, behind, goaway, refcon, size));
}
static void T_NewWindow(uint16_t op) { (void)op; new_window_common(CWINDOW_SIZE); }
static void T_NewCWindow(uint16_t op) { (void)op; new_window_common(CWINDOW_SIZE); }
static void T_GetNewWindow(uint16_t op)
{
    (void)op;
    uint32_t behind = ARG32(), storage = ARG32(); int16_t id = ARGS16();
    uint32_t len; const uint8_t *p = res_peek(OSTYPE('W','I','N','D'), id, &len);
    if (!p) { RET32(0); return; }
    QRect b = { (int16_t)((p[0] << 8) | p[1]), (int16_t)((p[2] << 8) | p[3]), (int16_t)((p[4] << 8) | p[5]), (int16_t)((p[6] << 8) | p[7]) };
    int16_t proc = (int16_t)((p[8] << 8) | p[9]);
    uint32_t t = mm_new_ptr(256, true);
    memcpy(HP(t), p + 18, p[18] + 1);
    RET32(win_create(storage, b, t, p[10] != 0, proc, behind, p[12] != 0, ((uint32_t)p[14] << 24) | ((uint32_t)p[15] << 16) | ((uint32_t)p[16] << 8) | p[17], CWINDOW_SIZE));
    mm_dispose_ptr(t);
}
static void T_DisposeWindow(uint16_t op) { (void)op; win_dispose(ARG32(), true); }
static void T_CloseWindow(uint16_t op) { (void)op; win_dispose(ARG32(), false); }
static void T_ShowWindow(uint16_t op) { (void)op; show(ARG32()); }
static void T_HideWindow(uint16_t op) { (void)op; hide(ARG32()); }
static void T_ShowHide(uint16_t op) { (void)op; bool s = ARG8() != 0; uint32_t w = ARG32(); if (s) show(w); else hide(w); }
static void bring_front(uint32_t w)
{
    WinInfo *wi = info(w);
    if (!wi) return;
    WinInfo t = *wi;
    int i = (int)(wi - wins);
    memmove(&wins[1], &wins[0], sizeof(WinInfo) * i);
    wins[0] = t;
    link_list();
    pal_window_front(w);
}
static void T_SelectWindow(uint16_t op) { (void)op; uint32_t w = ARG32(); bring_front(w); wrb(w + W_hilited, 1); }
static void T_BringToFront(uint16_t op) { (void)op; bring_front(ARG32()); }
static void T_SendBehind(uint16_t op) { (void)op; (void)ARG32(); (void)ARG32(); }
static void T_HiliteWindow(uint16_t op) { (void)op; bool f = ARG8() != 0; uint32_t w = ARG32(); wrb(w + W_hilited, f); }
static void T_FrontWindow(uint16_t op) { (void)op; RET32(win_front()); }
static void T_MoveWindow(uint16_t op)
{
    (void)op;
    (void)ARG8(); int16_t v = ARGS16(), h = ARGS16(); uint32_t w = ARG32();
    bool vis = rdb(w + W_visible) != 0;
    if (vis) hide(w);
    set_global_pos(w, h, v);
    if (vis) show(w);
}
static void T_SizeWindow(uint16_t op)
{
    (void)op;
    (void)ARG8(); int16_t hh = ARGS16(), ww = ARGS16(); uint32_t w = ARG32();
    QRect pr = rd_rect(w + P_portRect); pr.right = pr.left + ww; pr.bottom = pr.top + hh;
    wr_rect(w + P_portRect, pr);
}
static void T_SetWTitle(uint16_t op) { (void)op; uint32_t t = ARG32(); uint32_t w = ARG32(); if (rdl(w + W_title)) memcpy(HP(rdl(rdl(w + W_title))), HP(t), rdb(t) + 1); }
static void T_GetWTitle(uint16_t op) { (void)op; uint32_t t = ARG32(); uint32_t w = ARG32(); if (rdl(w + W_title)) memcpy(HP(t), HP(rdl(rdl(w + W_title))), rdb(rdl(rdl(w + W_title))) + 1); else wrb(t, 0); }
static void T_SetWRefCon(uint16_t op) { (void)op; uint32_t r = ARG32(); uint32_t w = ARG32(); wrl(w + W_refCon, r); }
static void T_GetWRefCon(uint16_t op) { (void)op; uint32_t w = ARG32(); RET32(rdl(w + W_refCon)); }
static void T_FindWindow(uint16_t op)
{
    (void)op;
    uint32_t pw = ARG32(); uint32_t pt = ARG32();
    int16_t v = (int16_t)(pt >> 16), h = (int16_t)pt;
    for (int i = 0; i < nwins; i++) {
        uint32_t w = wins[i].w;
        if (!rdb(w + W_visible)) continue;
        QRect g = global_content(w);
        if (v >= g.top && v < g.bottom && h >= g.left && h < g.right) { wrl(pw, w); RET16(3); return; }
        QRect s = struct_rect(w);
        if (v >= s.top && v < s.bottom && h >= s.left && h < s.right) { wrl(pw, w); RET16(4); return; }
    }
    wrl(pw, 0);
    RET16(0);
}
static void T_nop_ptr(uint16_t op) { (void)op; (void)ARG32(); }
static void T_nop_ptr_ptr(uint16_t op) { (void)op; (void)ARG32(); (void)ARG32(); }
static void T_TrackGoAway(uint16_t op) { (void)op; (void)ARG32(); (void)ARG32(); RET8(1); }
static void T_DragWindow(uint16_t op) { (void)op; (void)ARG32(); (void)ARG32(); (void)ARG32(); }
static void T_GrowWindow(uint16_t op) { (void)op; (void)ARG32(); (void)ARG32(); (void)ARG32(); RET32(0); }
static uint32_t wmgr_port;
static void T_GetWMgrPort(uint16_t op)
{
    (void)op;
    uint32_t p = ARG32();
    if (!wmgr_port) { wmgr_port = mm_new_ptr(CGRAFPORT_SIZE, true); qd_init_port(wmgr_port, true); }
    wrl(p, wmgr_port);
}
static void T_PaintOne(uint16_t op)
{
    (void)op;
    uint32_t rgn = ARG32(); uint32_t w = ARG32();
    if (w) return;
    fill_screen(qd_rgn_box(rgn), qd_rgn_to_black());
}
static void T_PaintBehind(uint16_t op) { (void)op; (void)ARG32(); (void)ARG32(); }
static void T_CalcVis(uint16_t op) { (void)op; (void)ARG32(); }

/* ------------------------------------------------------------ menus */
static uint32_t menus[32];
static int nmenus;
static void T_InitMenus(uint16_t op) { (void)op; }
static void T_GetMenu(uint16_t op)
{
    (void)op;
    int16_t id = ARGS16();
    uint32_t len; const uint8_t *p = res_peek(OSTYPE('M','E','N','U'), id, &len);
    RET32(p ? mm_handle_from_bytes(p, len) : 0);
}
static void T_NewMenu(uint16_t op)
{
    (void)op;
    uint32_t t = ARG32(); int16_t id = ARGS16();
    uint8_t buf[300] = { 0 };
    buf[0] = (uint8_t)(id >> 8); buf[1] = (uint8_t)id;
    buf[10] = 0xFF; buf[11] = 0xFF; buf[12] = 0xFF; buf[13] = 0xFF;
    memcpy(buf + 14, HP(t), rdb(t) + 1);
    RET32(mm_handle_from_bytes(buf, 14 + rdb(t) + 2));
}
static void T_InsertMenu(uint16_t op) { (void)op; (void)ARG16(); uint32_t m = ARG32(); if (nmenus < 32) menus[nmenus++] = m; }
static void T_DeleteMenu(uint16_t op)
{
    (void)op;
    int16_t id = ARGS16();
    for (int i = 0; i < nmenus; i++)
        if ((int16_t)rdw(rdl(menus[i])) == id) { memmove(&menus[i], &menus[i + 1], sizeof(uint32_t) * (nmenus - i - 1)); nmenus--; break; }
}
static void T_GetMenuHandle(uint16_t op)
{
    (void)op;
    int16_t id = ARGS16();
    for (int i = 0; i < nmenus; i++) if ((int16_t)rdw(rdl(menus[i])) == id) { RET32(menus[i]); return; }
    RET32(0);
}
static void T_DrawMenuBar(uint16_t op) { (void)op; }
static void T_ClearMenuBar(uint16_t op) { (void)op; nmenus = 0; }
static void T_GetNewMBar(uint16_t op) { (void)op; (void)ARG16(); RET32(mm_new_handle(4, true)); }
static void T_SetMenuBar(uint16_t op) { (void)op; (void)ARG32(); }
static void T_GetMenuBar(uint16_t op) { (void)op; RET32(mm_new_handle(4, true)); }
static void T_HiliteMenu(uint16_t op) { (void)op; (void)ARG16(); }
static void T_MenuSelect(uint16_t op) { (void)op; (void)ARG32(); RET32(0); }
static void T_MenuKey(uint16_t op) { (void)op; (void)ARG16(); RET32(0); }
static void T_AppendResMenu(uint16_t op) { (void)op; (void)ARG32(); (void)ARG32(); }
static void T_AppendMenu(uint16_t op) { (void)op; (void)ARG32(); (void)ARG32(); }
static void T_nop_menu_item(uint16_t op) { (void)op; (void)ARG16(); (void)ARG32(); }
static void T_CalcMenuSize(uint16_t op) { (void)op; (void)ARG32(); }

/* walk MENU items: returns pointer to item n (1-based) name pstr, or 0 */
static uint32_t menu_item(uint32_t mh, int n, int *count)
{
    uint32_t p = rdl(mh);
    uint32_t q = p + 14 + 1 + rdb(p + 14);
    int i = 0;
    while (rdb(q)) {
        i++;
        if (i == n) { if (count) *count = i; return q; }
        q += 1 + rdb(q) + 4;
    }
    if (count) *count = i;
    return 0;
}
static void T_CountMItems(uint16_t op) { (void)op; uint32_t m = ARG32(); int c = 0; menu_item(m, 9999, &c); RET16(c); }
static void T_GetMenuItemText(uint16_t op)
{
    (void)op;
    uint32_t s = ARG32(); int16_t item = ARGS16(); uint32_t m = ARG32();
    uint32_t q = menu_item(m, item, NULL);
    if (q) memcpy(HP(s), HP(q), rdb(q) + 1); else wrb(s, 0);
}
static void T_SetMenuItemText(uint16_t op) { (void)op; (void)ARG32(); (void)ARG16(); (void)ARG32(); }
static void T_SetItemMark(uint16_t op)
{
    (void)op;
    int16_t mark = ARGS16(); int16_t item = ARGS16(); uint32_t m = ARG32();
    uint32_t q = menu_item(m, item, NULL);
    if (q) wrb(q + 1 + rdb(q) + 2, (uint8_t)mark);
}
static void T_CheckItem(uint16_t op)
{
    (void)op;
    bool c = ARG8() != 0; int16_t item = ARGS16(); uint32_t m = ARG32();
    uint32_t q = menu_item(m, item, NULL);
    if (q) wrb(q + 1 + rdb(q) + 2, c ? 0x12 : 0);
}

/* PopUpMenuSelect(menu, top, left, popUpItem): long */
static void T_PopUpMenuSelect(uint16_t op)
{
    (void)op;
    int16_t cur = ARGS16(), left = ARGS16(), top = ARGS16(); uint32_t m = ARG32();
    extern int popup_run(uint32_t menu, int top, int left, int cur);
    int item = popup_run(m, top, left, cur);
    RET32(item > 0 ? ((uint32_t)(uint16_t)rdw(rdl(m)) << 16) | (uint16_t)item : 0);
}

uint8_t qd_rgn_to_black(void) { return qd_rgb_to_index(qd_device_ctab(), 0, 0, 0); }

void window_register_traps(void)
{
    trap_register(0xA912, T_InitWindows, "InitWindows");
    trap_register(0xA913, T_NewWindow, "NewWindow");
    trap_register(0xAA45, T_NewCWindow, "NewCWindow");
    trap_register(0xA9BD, T_GetNewWindow, "GetNewWindow");
    trap_register(0xAA46, T_GetNewWindow, "GetNewCWindow");
    trap_register(0xA914, T_DisposeWindow, "DisposeWindow");
    trap_register(0xA92D, T_CloseWindow, "CloseWindow");
    trap_register(0xA915, T_ShowWindow, "ShowWindow");
    trap_register(0xA916, T_HideWindow, "HideWindow");
    trap_register(0xA908, T_ShowHide, "ShowHide");
    trap_register(0xA91F, T_SelectWindow, "SelectWindow");
    trap_register(0xA920, T_BringToFront, "BringToFront");
    trap_register(0xA921, T_SendBehind, "SendBehind");
    trap_register(0xA91C, T_HiliteWindow, "HiliteWindow");
    trap_register(0xA924, T_FrontWindow, "FrontWindow");
    trap_register(0xA91B, T_MoveWindow, "MoveWindow");
    trap_register(0xA91D, T_SizeWindow, "SizeWindow");
    trap_register(0xA91A, T_SetWTitle, "SetWTitle");
    trap_register(0xA919, T_GetWTitle, "GetWTitle");
    trap_register(0xA918, T_SetWRefCon, "SetWRefCon");
    trap_register(0xA917, T_GetWRefCon, "GetWRefCon");
    trap_register(0xA92C, T_FindWindow, "FindWindow");
    trap_register(0xA922, T_nop_ptr, "BeginUpdate");
    trap_register(0xA923, T_nop_ptr, "EndUpdate");
    trap_register(0xA928, T_nop_ptr, "InvalRect");
    trap_register(0xA92A, T_nop_ptr, "ValidRect");
    trap_register(0xA927, T_nop_ptr, "InvalRgn");
    trap_register(0xA929, T_nop_ptr, "ValidRgn");
    trap_register(0xA904, T_nop_ptr, "DrawGrowIcon");
    trap_register(0xA91E, T_TrackGoAway, "TrackGoAway");
    trap_register(0xA925, T_DragWindow, "DragWindow");
    trap_register(0xA92B, T_GrowWindow, "GrowWindow");
    trap_register(0xA910, T_GetWMgrPort, "GetWMgrPort");
    trap_register(0xAA48, T_GetWMgrPort, "GetCWMgrPort");
    trap_register(0xA90C, T_PaintOne, "PaintOne");
    trap_register(0xA90D, T_PaintBehind, "PaintBehind");
    trap_register(0xA909, T_CalcVis, "CalcVis");
    trap_register(0xA92E, T_nop_ptr_ptr, "SetWindowPic");
    trap_register(0xA930, T_InitMenus, "InitMenus");
    trap_register(0xA9BF, T_GetMenu, "GetMenu");
    trap_register(0xA931, T_NewMenu, "NewMenu");
    trap_register(0xA935, T_InsertMenu, "InsertMenu");
    trap_register(0xA936, T_DeleteMenu, "DeleteMenu");
    trap_register(0xA949, T_GetMenuHandle, "GetMenuHandle");
    trap_register(0xA937, T_DrawMenuBar, "DrawMenuBar");
    trap_register(0xA934, T_ClearMenuBar, "ClearMenuBar");
    trap_register(0xA9C0, T_GetNewMBar, "GetNewMBar");
    trap_register(0xA93C, T_SetMenuBar, "SetMenuBar");
    trap_register(0xA93B, T_GetMenuBar, "GetMenuBar");
    trap_register(0xA938, T_HiliteMenu, "HiliteMenu");
    trap_register(0xA93D, T_MenuSelect, "MenuSelect");
    trap_register(0xA93E, T_MenuKey, "MenuKey");
    trap_register(0xA94D, T_AppendResMenu, "AppendResMenu");
    trap_register(0xA933, T_AppendMenu, "AppendMenu");
    trap_register(0xA939, T_nop_menu_item, "EnableItem");
    trap_register(0xA93A, T_nop_menu_item, "DisableItem");
    trap_register(0xA948, T_CalcMenuSize, "CalcMenuSize");
    trap_register(0xA950, T_CountMItems, "CountMItems");
    trap_register(0xA946, T_GetMenuItemText, "GetMenuItemText");
    trap_register(0xA947, T_SetMenuItemText, "SetMenuItemText");
    trap_register(0xA944, T_SetItemMark, "SetItemMark");
    trap_register(0xA945, T_CheckItem, "CheckItem");
    trap_register(0xA80B, T_PopUpMenuSelect, "PopUpMenuSelect");
}
