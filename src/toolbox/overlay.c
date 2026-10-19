#include "overlay.h"
#include "qd.h"
#include "netcfg.h"
#include <stdio.h>

#define BANNER_H 40

static char line[2][160];
static bool active;
static bool dirty = true;
static uint32_t pixels;     /* banner strip background (emulated memory) */
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

static void ensure_strip(void)
{
    if (!pixels) pixels = mm_new_ptr(SCREEN_W * BANNER_H, true);
}

/* the strip background lives in emulated memory; its text is hi-res overlay text */
static void render(const char *l1, const char *l2)
{
    ensure_strip();
    uint32_t ct = qd_device_ctab();
    uint8_t bg = qd_rgb_to_index(ct, 0x1111, 0x1111, 0x1111);
    uint8_t edge = qd_rgb_to_index(ct, 0xFFFF, 0xCCCC, 0x0000);
    memset(HP(pixels), bg, SCREEN_W * BANNER_H);
    memset(HP(pixels), edge, SCREEN_W * 2);
    int top = SCREEN_H - BANNER_H;
    int w1 = text_overlay_width(l1, true), w2 = text_overlay_width(l2, false);
    text_overlay_set(0, l1, (SCREEN_W - w1) / 2 > 6 ? (SCREEN_W - w1) / 2 : 6, top + 17, true, 0xFF, 0xCC, 0x00);
    text_overlay_set(1, l2, (SCREEN_W - w2) / 2 > 6 ? (SCREEN_W - w2) / 2 : 6, top + 33, false, 0xFF, 0xFF, 0xFF);
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
    if (!active && !h) {
        if (hint || !dirty) { text_overlay_clear(); dirty = true; }
        hint = 0;
        return fb;
    }
    if (h != hint) { hint = h; dirty = true; }
    if (dirty) {
        if (active) render(line[0], line[1]);
        else if (h == 1) {
            char l2[160];
            snprintf(l2, sizeof l2, "Your address is shown while you wait. Over the internet, forward UDP port %u to this computer.", g_netcfg.port);
            render("NET HOST: host a game. Your friend connects to you by choosing NET JOIN.", l2);
        } else {
            render("NET JOIN: join a friend who chose NET HOST.",
                   "You'll be asked for their address; leave it empty to find a host on your local network.");
        }
        dirty = false;
    }
    memcpy(composed, fb, sizeof composed);
    memcpy(composed + (SCREEN_H - BANNER_H) * SCREEN_W, HP(pixels), SCREEN_W * BANNER_H);
    return composed;
}
