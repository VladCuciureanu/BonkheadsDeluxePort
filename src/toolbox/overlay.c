#include "overlay.h"
#include "qd.h"
#include "netcfg.h"
#include <stdio.h>

#define BANNER_H 40

static char line[2][160];
static bool active;
static bool dirty = true;
static uint32_t port;       /* emulated colour port drawing into `pixels` */
static uint32_t pixels;
static uint8_t composed[SCREEN_W * SCREEN_H];

/* main menu hot spots (from the game's GetPtLoc table) */
static const QRect NET_HOST_RECT = { 126, 406, 166, 596 };
static const QRect NET_JOIN_RECT = { 172, 406, 212, 596 };
static int hint;            /* 0 none, 1 host, 2 join */

bool win_exists_refcon(uint32_t refcon);

void overlay_banner(const char *l1, const char *l2)
{
    const char *a = l1 ? l1 : "", *b = l2 ? l2 : "";
    if (strcmp(a, line[0]) || strcmp(b, line[1])) dirty = true;
    snprintf(line[0], sizeof line[0], "%s", a);
    snprintf(line[1], sizeof line[1], "%s", b);
    active = line[0][0] || line[1][0];
}

static void ensure_port(void)
{
    if (port) return;
    pixels = mm_new_ptr(SCREEN_W * BANNER_H, true);
    port = mm_new_ptr(CGRAFPORT_SIZE, true);
    qd_init_port(port, true);
    uint32_t pm = rdl(rdl(port + P_portBits));
    QRect r = { 0, 0, BANNER_H, SCREEN_W };
    wrl(pm + PM_baseAddr, pixels);
    wrw(pm + PM_rowBytes, 0x8000 | SCREEN_W);
    wr_rect(pm + PM_bounds, r);
    wrl(pm + PM_pmTable, qd_device_ctab());
    wr_rect(port + P_portRect, r);
    qd_set_rgn(rdl(port + P_visRgn), r);
}

static void set_rgb(uint32_t a, uint16_t r, uint16_t g, uint16_t b) { wrw(a, r); wrw(a + 2, g); wrw(a + 4, b); }

static void render(const char *l1, const char *l2)
{
    ensure_port();
    wrl(rdl(rdl(port + P_portBits)) + PM_pmTable, qd_device_ctab());
    uint32_t ct = qd_device_ctab();
    uint8_t bg = qd_rgb_to_index(ct, 0x1111, 0x1111, 0x1111);
    uint8_t edge = qd_rgb_to_index(ct, 0xFFFF, 0xCCCC, 0x0000);
    memset(HP(pixels), bg, SCREEN_W * BANNER_H);
    memset(HP(pixels), edge, SCREEN_W * 2);
    wrw(port + P_txFont, 3); wrw(port + P_txSize, 12);
    const char *ls[2] = { l1, l2 };
    for (int i = 0; i < 2; i++) {
        if (!ls[i][0]) continue;
        wrb(port + P_txFace, i == 0 ? 1 : 0);
        if (i == 0) set_rgb(port + P_rgbFg, 0xFFFF, 0xCCCC, 0x0000);
        else set_rgb(port + P_rgbFg, 0xFFFF, 0xFFFF, 0xFFFF);
        int w = text_width(port, (const uint8_t *)ls[i], (int)strlen(ls[i]));
        wrw(port + P_pnLoc, (uint16_t)(17 + i * 16));
        wrw(port + P_pnLoc + 2, (uint16_t)((SCREEN_W - w) / 2 > 6 ? (SCREEN_W - w) / 2 : 6));
        text_draw(port, (const uint8_t *)ls[i], (int)strlen(ls[i]));
    }
}

static int current_hint(void)
{
    if (!win_exists_refcon(10500)) return 0;   /* main menu window */
    const InputState *in = plat_input();
    QRect rs[2] = { NET_HOST_RECT, NET_JOIN_RECT };
    for (int i = 0; i < 2; i++)
        if (in->mouse_y >= rs[i].top && in->mouse_y < rs[i].bottom && in->mouse_x >= rs[i].left && in->mouse_x < rs[i].right)
            return i + 1;
    return 0;
}

const uint8_t *overlay_compose(const uint8_t *fb)
{
    int h = active ? 0 : current_hint();
    if (!active && !h) { hint = 0; return fb; }
    if (h != hint) { hint = h; dirty = true; }
    if (dirty) {
        if (active) render(line[0], line[1]);
        else if (h == 1) render("NET HOST: you connect to a friend who chose NET JOIN.",
                                "You'll be asked for their address; leave it empty to find players on your local network.");
        else {
            char l2[160];
            snprintf(l2, sizeof l2, "Your address is shown while you wait. Over the internet, forward UDP port %u to this computer.", g_netcfg.port);
            render("NET JOIN: you wait for a friend to connect to you (they choose NET HOST).", l2);
        }
        dirty = false;
    }
    memcpy(composed, fb, sizeof composed);
    memcpy(composed + (SCREEN_H - BANNER_H) * SCREEN_W, HP(pixels), SCREEN_W * BANNER_H);
    return composed;
}
