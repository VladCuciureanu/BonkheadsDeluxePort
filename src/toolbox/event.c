/* Event Manager, timing and screen presentation. */
#include "qd.h"
#include "event.h"
#include "netcfg.h"
#include "sound.h"
#include "overlay.h"
#include <stdlib.h>

/* ------------------------------------------------------------ presentation */
static uint64_t last_present_us;
static uint32_t last_tick_seen;
static int same_tick_calls;

void screen_present(bool force)
{
    uint64_t now = plat_micros();
    if (!force && now - last_present_us < 16000) return;
    last_present_us = now;
    static PlatTextDraw draws[700];
    const uint8_t *frame = overlay_compose(HP(g_screen_fb));
    int n = text_hires_draws(draws, 700);
    plat_present(frame, SCREEN_W, (const uint8_t (*)[3])g_hw_pal, draws, n);
    g_hw_pal_dirty = false;
}

/* debug: --shot SECONDS FILE saves the screen and exits */
double g_shot_after = -1;
char g_shot_path[512];

volatile uint64_t g_last_idle_us;

void evt_idle(void)
{
    g_last_idle_us = plat_micros();
    plat_pump();
    if (g_shot_after >= 0 && plat_micros() / 1e6 >= g_shot_after) {
        plat_request_capture(g_shot_path);
        screen_present(true);
        extern void debug_dump_globals(void);
        debug_dump_globals();
        log_info("screenshot saved to %s", g_shot_path);
        extern void app_quit(void);
        app_quit();
    }
    if (plat_quit_requested()) {
        extern void app_quit(void);
        app_quit();
    }
    net_poll();
    sound_update();
    screen_present(false);
    uint32_t t = plat_ticks();
    wrl(LM_Ticks, t);
    wrl(LM_Time, plat_secs_since_1904());
    memcpy(HP(LM_KeyMap), plat_input()->keymap, 16);
}

void screen_vbl_wait(void)
{
    uint32_t t = plat_ticks();
    evt_idle();
    screen_present(true);
    while (plat_ticks() == t) { plat_sleep_ms(1); evt_idle(); }
}

bool evt_quit_requested(void) { return plat_quit_requested(); }
bool evt_button_down(void) { evt_idle(); return plat_input()->button; }

void evt_mouse_local(uint32_t port, int *h, int *v)
{
    const InputState *in = plat_input();
    *h = in->mouse_x; *v = in->mouse_y;
    PixDesc pd;
    if (port && qd_port_pixels(port, &pd)) { *h += pd.bounds.left; *v += pd.bounds.top; }
}

/* ------------------------------------------------------------ OS event queue (emulated EvQEl) */
#define QSIZE 32
#define EVQEL 22
static uint32_t qpool;
static int qhead, qcount;
static uint32_t evq_hdr;   /* EvQHdr-like: qFlags, qHead, qTail */

void evt_init(void)
{
    qpool = mm_new_ptr(QSIZE * EVQEL, true);
    evq_hdr = mm_new_ptr(10, true);
    wrl(0x014A, evq_hdr); /* EventQueue low-memory header */
}

static uint32_t post_event(uint16_t what, uint32_t msg)
{
    if (qcount == QSIZE) { qhead = (qhead + 1) % QSIZE; qcount--; }
    int slot = (qhead + qcount) % QSIZE;
    uint32_t e = qpool + slot * EVQEL;
    const InputState *in = plat_input();
    wrl(e, 0); wrw(e + 4, 4);
    wrw(e + 6, what); wrl(e + 8, msg); wrl(e + 12, plat_ticks());
    wrw(e + 16, (uint16_t)in->mouse_y); wrw(e + 18, (uint16_t)in->mouse_x);
    wrw(e + 20, in->modifiers);
    qcount++;
    return e;
}

/* move platform input events into the OS queue */
static void drain_platform(void)
{
    PlatEvent pe;
    while (plat_pop_event(&pe)) {
        uint32_t e = post_event(pe.what, pe.message);
        wrw(e + 16, (uint16_t)pe.v); wrw(e + 18, (uint16_t)pe.h); wrw(e + 20, pe.modifiers);
    }
}

bool evt_next(uint16_t mask, uint32_t ev, bool remove)
{
    drain_platform();
    for (int i = 0; i < qcount; i++) {
        int slot = (qhead + i) % QSIZE;
        uint32_t e = qpool + slot * EVQEL;
        uint16_t what = (uint16_t)rdw(e + 6);
        if (!(mask & (1u << what))) continue;
        memcpy(HP(ev), HP(e + 6), 16);
        if (remove) {
            /* remove slot i by shifting later entries down */
            for (int k = i; k < qcount - 1; k++) {
                uint32_t a = qpool + ((qhead + k) % QSIZE) * EVQEL, b = qpool + ((qhead + k + 1) % QSIZE) * EVQEL;
                memcpy(HP(a), HP(b), EVQEL);
            }
            qcount--;
        }
        return true;
    }
    /* null event */
    const InputState *in = plat_input();
    wrw(ev, 0); wrl(ev + 2, 0); wrl(ev + 6, plat_ticks());
    wrw(ev + 10, (uint16_t)in->mouse_y); wrw(ev + 12, (uint16_t)in->mouse_x);
    wrw(ev + 14, in->modifiers);
    return false;
}

static void flush_events(uint16_t mask, uint16_t stop)
{
    drain_platform();
    int i = 0;
    while (i < qcount) {
        uint32_t e = qpool + ((qhead + i) % QSIZE) * EVQEL;
        uint16_t what = (uint16_t)rdw(e + 6);
        if (stop & (1u << what)) break;
        if (mask & (1u << what)) {
            for (int k = i; k < qcount - 1; k++) {
                uint32_t a = qpool + ((qhead + k) % QSIZE) * EVQEL, b = qpool + ((qhead + k + 1) % QSIZE) * EVQEL;
                memcpy(HP(a), HP(b), EVQEL);
            }
            qcount--;
        } else i++;
    }
}

/* ------------------------------------------------------------ traps */
static void T_TickCount(uint16_t op)
{
    (void)op;
    uint32_t t = plat_ticks();
    if (t == last_tick_seen) {
        /* busy-wait loops on TickCount: yield the CPU a little */
        if (++same_tick_calls > 64) { plat_sleep_ms(1); same_tick_calls = 0; }
    } else { last_tick_seen = t; same_tick_calls = 0; }
    evt_idle();
    RET32(plat_ticks());
}

static void T_GetKeys(uint16_t op)
{
    (void)op;
    uint32_t p = ARG32();
    evt_idle();
    memcpy(HP(p), plat_input()->keymap, 16);
}

static void T_Button(uint16_t op) { (void)op; evt_idle(); RET8(plat_input()->button); }
static void T_StillDown(uint16_t op) { (void)op; evt_idle(); RET8(plat_input()->button); }
static void T_WaitMouseUp(uint16_t op) { (void)op; evt_idle(); RET8(plat_input()->button); }
static void T_GetMouse(uint16_t op)
{
    (void)op;
    uint32_t p = ARG32();
    evt_idle();
    int h, v; evt_mouse_local(qd_the_port(), &h, &v);
    wrw(p, (uint16_t)v); wrw(p + 2, (uint16_t)h);
}

static void T_GetNextEvent(uint16_t op)
{
    (void)op;
    uint32_t ev = ARG32(); uint16_t mask = ARG16();
    evt_idle();
    RET8(evt_next(mask, ev, true));
}

static void T_WaitNextEvent(uint16_t op)
{
    (void)op;
    uint32_t rgn = ARG32(), sleep = ARG32(), ev = ARG32(); uint16_t mask = ARG16();
    (void)rgn;
    uint32_t until = plat_ticks() + (sleep > 30 ? 30 : sleep);
    for (;;) {
        evt_idle();
        if (evt_next(mask, ev, true)) { RET8(1); return; }
        if (plat_ticks() >= until) break;
        plat_sleep_ms(2);
    }
    RET8(0);
}

static void T_EventAvail(uint16_t op)
{
    (void)op;
    uint32_t ev = ARG32(); uint16_t mask = ARG16();
    evt_idle();
    RET8(evt_next(mask, ev, false));
}

static void T_FlushEvents(uint16_t op) { (void)op; evt_idle(); flush_events((uint16_t)D[0], (uint16_t)(D[0] >> 16)); D[0] = 0; }
static void T_GetOSEvent(uint16_t op) { (void)op; evt_idle(); bool r = evt_next((uint16_t)D[0], A[0], true); os_result(r ? 0 : -1); }
static void T_OSEventAvail(uint16_t op) { (void)op; evt_idle(); bool r = evt_next((uint16_t)D[0], A[0], false); os_result(r ? 0 : -1); }
static void T_PostEvent(uint16_t op)
{
    uint32_t e = post_event((uint16_t)A[0], D[0]);
    if (op & 0x0100) A[0] = e;  /* PPostEvent returns the queue element */
    os_result(0);
}

static void T_Delay(uint16_t op)
{
    (void)op;
    uint32_t until = plat_ticks() + A[0];
    while (plat_ticks() < until) { evt_idle(); plat_sleep_ms(1); }
    D[0] = plat_ticks();
}

static void T_GetDblTime(uint16_t op) { (void)op; RET32(30); }
static void T_GetCaretTime(uint16_t op) { (void)op; RET32(32); }
static void T_SystemTask(uint16_t op) { (void)op; evt_idle(); }
static void T_SystemClick(uint16_t op) { (void)op; (void)ARG32(); (void)ARG32(); }
static void T_SystemEvent(uint16_t op) { (void)op; (void)ARG32(); RET8(0); }

/* Gamepad bindings: the game keeps each player's left/right/jump key as a
 * word global (key code in the low byte), set from the preferences when a
 * game starts. Until then they are all zero and the pads press nothing. */
static int pad_binding(int player, int action)
{
    static const int16_t off[2][3] = { { -32640, -32642, -32644 }, { -32646, -32648, -32650 } };
    if (!A[5]) return -1;
    const int16_t *o = off[player & 1];
    if (!rdw(A[5] + o[0]) && !rdw(A[5] + o[1]) && !rdw(A[5] + o[2])) return -1;
    return rdw(A[5] + o[action]) & 0x7F;
}

void event_register_traps(void)
{
    plat_set_pad_binding(pad_binding);
    trap_register(0xA975, T_TickCount, "TickCount");
    trap_register(0xA976, T_GetKeys, "GetKeys");
    trap_register(0xA974, T_Button, "Button");
    trap_register(0xA973, T_StillDown, "StillDown");
    trap_register(0xA977, T_WaitMouseUp, "WaitMouseUp");
    trap_register(0xA972, T_GetMouse, "GetMouse");
    trap_register(0xA970, T_GetNextEvent, "GetNextEvent");
    trap_register(0xA860, T_WaitNextEvent, "WaitNextEvent");
    trap_register(0xA971, T_EventAvail, "EventAvail");
    trap_register(0xA032, T_FlushEvents, "FlushEvents");
    trap_register(0xA031, T_GetOSEvent, "GetOSEvent");
    trap_register(0xA030, T_OSEventAvail, "OSEventAvail");
    trap_register(0xA02F, T_PostEvent, "PostEvent");
    trap_register(0xA03B, T_Delay, "Delay");
    trap_register(0xA840, T_GetDblTime, "GetDblTime");
    trap_register(0xA841, T_GetCaretTime, "GetCaretTime");
    trap_register(0xA9B4, T_SystemTask, "SystemTask");
    trap_register(0xA9B3, T_SystemClick, "SystemClick");
    trap_register(0xA9B2, T_SystemEvent, "SystemEvent");
}
