#include "platform.h"
#include "log.h"
#include <SDL3/SDL.h>
#include <string.h>
#include <time.h>

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static InputState in;
static bool quit_req;
static uint64_t t0;
static SDL_FRect view;          /* where the 640x480 image lands, in output pixels */
static float view_scale = 1;
static char capture_path[512];  /* save the next presented frame here */

#define MAXTEXT 1024
static SDL_Texture *texts[MAXTEXT];

#define EVQ 64
static PlatEvent evq[EVQ];
static int evh, evt;

/* SDL scancode -> Mac virtual key code (ADB extended keyboard) */
static int16_t sc2mac[SDL_SCANCODE_COUNT];

static void build_keymap(void)
{
    for (int i = 0; i < SDL_SCANCODE_COUNT; i++) sc2mac[i] = -1;
    static const struct { SDL_Scancode sc; int16_t mac; } t[] = {
        {SDL_SCANCODE_A, 0x00}, {SDL_SCANCODE_S, 0x01}, {SDL_SCANCODE_D, 0x02}, {SDL_SCANCODE_F, 0x03},
        {SDL_SCANCODE_H, 0x04}, {SDL_SCANCODE_G, 0x05}, {SDL_SCANCODE_Z, 0x06}, {SDL_SCANCODE_X, 0x07},
        {SDL_SCANCODE_C, 0x08}, {SDL_SCANCODE_V, 0x09}, {SDL_SCANCODE_B, 0x0B}, {SDL_SCANCODE_Q, 0x0C},
        {SDL_SCANCODE_W, 0x0D}, {SDL_SCANCODE_E, 0x0E}, {SDL_SCANCODE_R, 0x0F}, {SDL_SCANCODE_Y, 0x10},
        {SDL_SCANCODE_T, 0x11}, {SDL_SCANCODE_1, 0x12}, {SDL_SCANCODE_2, 0x13}, {SDL_SCANCODE_3, 0x14},
        {SDL_SCANCODE_4, 0x15}, {SDL_SCANCODE_6, 0x16}, {SDL_SCANCODE_5, 0x17}, {SDL_SCANCODE_EQUALS, 0x18},
        {SDL_SCANCODE_9, 0x19}, {SDL_SCANCODE_7, 0x1A}, {SDL_SCANCODE_MINUS, 0x1B}, {SDL_SCANCODE_8, 0x1C},
        {SDL_SCANCODE_0, 0x1D}, {SDL_SCANCODE_RIGHTBRACKET, 0x1E}, {SDL_SCANCODE_O, 0x1F}, {SDL_SCANCODE_U, 0x20},
        {SDL_SCANCODE_LEFTBRACKET, 0x21}, {SDL_SCANCODE_I, 0x22}, {SDL_SCANCODE_P, 0x23}, {SDL_SCANCODE_RETURN, 0x24},
        {SDL_SCANCODE_L, 0x25}, {SDL_SCANCODE_J, 0x26}, {SDL_SCANCODE_APOSTROPHE, 0x27}, {SDL_SCANCODE_K, 0x28},
        {SDL_SCANCODE_SEMICOLON, 0x29}, {SDL_SCANCODE_BACKSLASH, 0x2A}, {SDL_SCANCODE_COMMA, 0x2B},
        {SDL_SCANCODE_SLASH, 0x2C}, {SDL_SCANCODE_N, 0x2D}, {SDL_SCANCODE_M, 0x2E}, {SDL_SCANCODE_PERIOD, 0x2F},
        {SDL_SCANCODE_TAB, 0x30}, {SDL_SCANCODE_SPACE, 0x31}, {SDL_SCANCODE_GRAVE, 0x32},
        {SDL_SCANCODE_BACKSPACE, 0x33}, {SDL_SCANCODE_ESCAPE, 0x35}, {SDL_SCANCODE_LGUI, 0x37},
        {SDL_SCANCODE_RGUI, 0x37}, {SDL_SCANCODE_LSHIFT, 0x38}, {SDL_SCANCODE_CAPSLOCK, 0x39},
        {SDL_SCANCODE_LALT, 0x3A}, {SDL_SCANCODE_LCTRL, 0x3B}, {SDL_SCANCODE_RSHIFT, 0x3C},
        {SDL_SCANCODE_RALT, 0x3D}, {SDL_SCANCODE_RCTRL, 0x3E}, {SDL_SCANCODE_KP_PERIOD, 0x41},
        {SDL_SCANCODE_KP_MULTIPLY, 0x43}, {SDL_SCANCODE_KP_PLUS, 0x45}, {SDL_SCANCODE_NUMLOCKCLEAR, 0x47},
        {SDL_SCANCODE_KP_DIVIDE, 0x4B}, {SDL_SCANCODE_KP_ENTER, 0x4C}, {SDL_SCANCODE_KP_MINUS, 0x4E},
        {SDL_SCANCODE_KP_EQUALS, 0x51}, {SDL_SCANCODE_KP_0, 0x52}, {SDL_SCANCODE_KP_1, 0x53},
        {SDL_SCANCODE_KP_2, 0x54}, {SDL_SCANCODE_KP_3, 0x55}, {SDL_SCANCODE_KP_4, 0x56}, {SDL_SCANCODE_KP_5, 0x57},
        {SDL_SCANCODE_KP_6, 0x58}, {SDL_SCANCODE_KP_7, 0x59}, {SDL_SCANCODE_KP_8, 0x5B}, {SDL_SCANCODE_KP_9, 0x5C},
        {SDL_SCANCODE_F5, 0x60}, {SDL_SCANCODE_F6, 0x61}, {SDL_SCANCODE_F7, 0x62}, {SDL_SCANCODE_F3, 0x63},
        {SDL_SCANCODE_F8, 0x64}, {SDL_SCANCODE_F9, 0x65}, {SDL_SCANCODE_F11, 0x67}, {SDL_SCANCODE_F10, 0x6D},
        {SDL_SCANCODE_F12, 0x6F}, {SDL_SCANCODE_INSERT, 0x72}, {SDL_SCANCODE_HOME, 0x73},
        {SDL_SCANCODE_PAGEUP, 0x74}, {SDL_SCANCODE_DELETE, 0x75}, {SDL_SCANCODE_F4, 0x76}, {SDL_SCANCODE_END, 0x77},
        {SDL_SCANCODE_F2, 0x78}, {SDL_SCANCODE_PAGEDOWN, 0x79}, {SDL_SCANCODE_F1, 0x7A},
        {SDL_SCANCODE_LEFT, 0x7B}, {SDL_SCANCODE_RIGHT, 0x7C}, {SDL_SCANCODE_DOWN, 0x7D}, {SDL_SCANCODE_UP, 0x7E},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) sc2mac[t[i].sc] = t[i].mac;
}

static void push_ev(PlatEvent e)
{
    int n = (evt + 1) % EVQ;
    if (n == evh) return;
    evq[evt] = e;
    evt = n;
}

bool plat_init(int scale, bool fullscreen)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        log_warn("SDL_Init: %s", SDL_GetError());
        return false;
    }
    build_keymap();
    if (scale < 1) scale = 2;
    win = SDL_CreateWindow("Bonkheads Deluxe", SCREEN_W * scale, SCREEN_H * scale,
                           SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!win) { log_warn("SDL_CreateWindow: %s", SDL_GetError()); return false; }
    ren = SDL_CreateRenderer(win, NULL);
    if (!ren) { log_warn("SDL_CreateRenderer: %s", SDL_GetError()); return false; }
    SDL_SetRenderVSync(ren, 0);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    t0 = SDL_GetTicksNS();
    return true;
}

void plat_shutdown(void)
{
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
}

static uint16_t mac_modifiers(void)
{
    SDL_Keymod m = SDL_GetModState();
    uint16_t r = 0;
    if (m & SDL_KMOD_GUI) r |= 0x0100;
    if (m & SDL_KMOD_SHIFT) r |= 0x0200;
    if (m & SDL_KMOD_CAPS) r |= 0x0400;
    if (m & SDL_KMOD_ALT) r |= 0x0800;
    if (m & SDL_KMOD_CTRL) r |= 0x1000;
    if (!in.button) r |= 0x0080; /* btnState: set when button is UP */
    return r;
}

static uint8_t key_char(SDL_Keycode k, SDL_Keymod m, int16_t mac)
{
    if (k >= 32 && k < 127) {
        char c = (char)k;
        if ((m & SDL_KMOD_SHIFT) && c >= 'a' && c <= 'z') c = (char)(c - 32);
        else if (m & SDL_KMOD_SHIFT) {
            const char *from = "1234567890-=[]\\;',./`", *to = "!@#$%^&*()_+{}|:\"<>?~";
            const char *p = strchr(from, c);
            if (p) c = to[p - from];
        }
        return (uint8_t)c;
    }
    switch (mac) {
    case 0x24: return 13; case 0x4C: return 3; case 0x30: return 9; case 0x33: return 8; case 0x35: return 27;
    case 0x7B: return 28; case 0x7C: return 29; case 0x7E: return 30; case 0x7D: return 31; case 0x75: return 127;
    default: return 0;
    }
}

static void set_key(int16_t mac, bool down)
{
    if (mac < 0) return;
    if (down) in.keymap[mac >> 3] |= (uint8_t)(1u << (mac & 7));
    else in.keymap[mac >> 3] &= (uint8_t)~(1u << (mac & 7));
}

static void update_view(void)
{
    int ow = SCREEN_W, oh = SCREEN_H;
    SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
    float sx = (float)ow / SCREEN_W, sy = (float)oh / SCREEN_H;
    view_scale = sx < sy ? sx : sy;
    view.w = SCREEN_W * view_scale; view.h = SCREEN_H * view_scale;
    view.x = (ow - view.w) / 2; view.y = (oh - view.h) / 2;
}

float plat_output_scale(void) { update_view(); return view_scale; }

static void map_mouse(float wx, float wy)
{
    int ww = 1, wh = 1, pw = 1, ph = 1;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    float px = wx * pw / (float)ww, py = wy * ph / (float)wh;
    update_view();
    in.mouse_x = (int)((px - view.x) / view_scale);
    in.mouse_y = (int)((py - view.y) / view_scale);
}

/* scripted key presses for automated testing (--press) */
typedef struct { double at; int16_t mac; int frames; int state; int x, y; } ScriptKey;  /* mac < 0: mouse click */
static ScriptKey script[64];
static int nscript;

void plat_script_click(double at, int x, int y)
{
    if (nscript < 64) { ScriptKey *k = &script[nscript++]; k->at = at; k->mac = -1; k->frames = 6; k->x = x; k->y = y; }
}

void plat_script_move(double at, int x, int y)
{
    if (nscript < 64) { ScriptKey *k = &script[nscript++]; k->at = at; k->mac = -2; k->x = x; k->y = y; }
}

void plat_script_key(double at, int mac, int frames)
{
    if (nscript < 64) { script[nscript].at = at; script[nscript].mac = (int16_t)mac; script[nscript].frames = frames > 0 ? frames : 6; nscript++; }
}

static uint8_t key_char(SDL_Keycode k, SDL_Keymod m, int16_t mac);

static uint8_t script_char(int16_t mac)
{
    for (int sc = 0; sc < SDL_SCANCODE_COUNT; sc++)
        if (sc2mac[sc] == mac) return key_char(SDL_GetKeyFromScancode((SDL_Scancode)sc, SDL_KMOD_NONE, false), SDL_KMOD_NONE, mac);
    return 0;
}

static void run_script(void)
{
    double now = (double)(SDL_GetTicksNS() - t0) / 1e9;
    for (int i = 0; i < nscript; i++) {
        ScriptKey *k = &script[i];
        if (k->mac == -2) {   /* pointer move only */
            if (k->state == 0 && now >= k->at) { k->state = 2; in.mouse_x = k->x; in.mouse_y = k->y; }
            continue;
        }
        if (k->mac < 0) {
            if (k->state == 0 && now >= k->at - 0.2) { k->state = 3; in.mouse_x = k->x; in.mouse_y = k->y; }
            if (k->state == 3 && now >= k->at) {
                k->state = 1; in.mouse_x = k->x; in.mouse_y = k->y; in.button = true;
                PlatEvent pe = {0}; pe.what = PEV_MOUSEDOWN; pe.h = (int16_t)k->x; pe.v = (int16_t)k->y; pe.modifiers = mac_modifiers();
                push_ev(pe);
            } else if (k->state == 1 && now >= k->at + k->frames / 60.0) {
                k->state = 2; in.button = false;
                PlatEvent pe = {0}; pe.what = PEV_MOUSEUP; pe.h = (int16_t)k->x; pe.v = (int16_t)k->y; pe.modifiers = mac_modifiers();
                push_ev(pe);
            }
            continue;
        }
        if (k->state == 0 && now >= k->at) {
            k->state = 1;
            set_key(k->mac, true);
            PlatEvent pe = {0};
            pe.what = PEV_KEYDOWN; pe.message = ((uint32_t)k->mac << 8) | script_char(k->mac); pe.modifiers = mac_modifiers();
            push_ev(pe);
        } else if (k->state == 1 && now >= k->at + k->frames / 60.0) {
            k->state = 2;
            set_key(k->mac, false);
            PlatEvent pe = {0};
            pe.what = PEV_KEYUP; pe.message = ((uint32_t)k->mac << 8) | script_char(k->mac); pe.modifiers = mac_modifiers();
            push_ev(pe);
        }
    }
}

void plat_pump(void)
{
    SDL_Event e;
    run_script();
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            quit_req = true;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            bool down = e.type == SDL_EVENT_KEY_DOWN;
            if (down && e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)) {
                bool fs = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) != 0;
                SDL_SetWindowFullscreen(win, !fs);
                break;
            }
            int16_t mac = sc2mac[e.key.scancode];
            if (e.key.scancode != SDL_SCANCODE_CAPSLOCK) set_key(mac, down);
            if (mac >= 0) {
                PlatEvent pe = {0};
                pe.what = down ? (e.key.repeat ? PEV_AUTOKEY : PEV_KEYDOWN) : PEV_KEYUP;
                pe.message = ((uint32_t)mac << 8) | key_char(e.key.key, e.key.mod, mac);
                pe.h = (int16_t)in.mouse_x; pe.v = (int16_t)in.mouse_y;
                pe.modifiers = mac_modifiers();
                push_ev(pe);
            }
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            if (!nscript) map_mouse(e.motion.x, e.motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            map_mouse(e.button.x, e.button.y);
            if (e.button.button != SDL_BUTTON_LEFT) break;
            in.button = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            PlatEvent pe = {0};
            pe.what = in.button ? PEV_MOUSEDOWN : PEV_MOUSEUP;
            pe.h = (int16_t)in.mouse_x; pe.v = (int16_t)in.mouse_y;
            pe.modifiers = mac_modifiers();
            push_ev(pe);
            break;
        }
        default:
            break;
        }
    }
    in.caps_lock = (SDL_GetModState() & SDL_KMOD_CAPS) != 0;
    set_key(0x39, in.caps_lock);
    in.modifiers = mac_modifiers();
}

int plat_text_create(const uint8_t *alpha, int w, int h)
{
    int slot = -1;
    for (int i = 1; i < MAXTEXT; i++) if (!texts[i]) { slot = i; break; }
    if (slot < 0 || w <= 0 || h <= 0) return 0;
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return 0;
    uint8_t *px = (uint8_t *)SDL_malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) { px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 255; px[i * 4 + 3] = alpha[i]; }
    SDL_UpdateTexture(t, NULL, px, w * 4);
    SDL_free(px);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    texts[slot] = t;
    return slot;
}

void plat_text_destroy(int handle)
{
    if (handle > 0 && handle < MAXTEXT && texts[handle]) { SDL_DestroyTexture(texts[handle]); texts[handle] = NULL; }
}

void plat_request_capture(const char *path) { SDL_strlcpy(capture_path, path, sizeof capture_path); }

void plat_present(const uint8_t *fb, int pitch, const uint8_t pal[256][3], const PlatTextDraw *draws, int ndraws)
{
    void *pixels; int tpitch;
    if (!SDL_LockTexture(tex, NULL, &pixels, &tpitch)) return;
    uint32_t lut[256];
    for (int i = 0; i < 256; i++) lut[i] = 0xFF000000u | ((uint32_t)pal[i][0] << 16) | ((uint32_t)pal[i][1] << 8) | pal[i][2];
    for (int y = 0; y < SCREEN_H; y++) {
        uint32_t *d = (uint32_t *)((uint8_t *)pixels + (size_t)y * tpitch);
        const uint8_t *s = fb + (size_t)y * pitch;
        for (int x = 0; x < SCREEN_W; x++) d[x] = lut[s[x]];
    }
    SDL_UnlockTexture(tex);
    update_view();
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderTexture(ren, tex, NULL, &view);
    for (int i = 0; i < ndraws; i++) {
        const PlatTextDraw *d = &draws[i];
        if (d->handle <= 0 || d->handle >= MAXTEXT || !texts[d->handle]) continue;
        SDL_SetTextureColorMod(texts[d->handle], d->r, d->g, d->b);
        SDL_FRect src = { d->sx, d->sy, d->sw, d->sh };
        SDL_FRect dst = { view.x + d->x * view_scale, view.y + d->y * view_scale, d->sw, d->sh };
        SDL_RenderTexture(ren, texts[d->handle], &src, &dst);
    }
    if (capture_path[0]) {
        SDL_Surface *shot = SDL_RenderReadPixels(ren, NULL);
        if (shot) { SDL_SaveBMP(shot, capture_path); SDL_DestroySurface(shot); }
        capture_path[0] = 0;
    }
    SDL_RenderPresent(ren);
}

bool plat_save_bmp(const char *path, const uint8_t *fb, int pitch, const uint8_t pal[256][3])
{
    SDL_Surface *s = SDL_CreateSurface(SCREEN_W, SCREEN_H, SDL_PIXELFORMAT_XRGB8888);
    if (!s) return false;
    for (int y = 0; y < SCREEN_H; y++) {
        uint32_t *d = (uint32_t *)((uint8_t *)s->pixels + (size_t)y * s->pitch);
        for (int x = 0; x < SCREEN_W; x++) {
            uint8_t v = fb[(size_t)y * pitch + x];
            d[x] = 0xFF000000u | ((uint32_t)pal[v][0] << 16) | ((uint32_t)pal[v][1] << 8) | pal[v][2];
        }
    }
    bool ok = SDL_SaveBMP(s, path);
    SDL_DestroySurface(s);
    return ok;
}

const InputState *plat_input(void) { return &in; }

bool plat_pop_event(PlatEvent *ev)
{
    if (evh == evt) return false;
    *ev = evq[evh];
    evh = (evh + 1) % EVQ;
    return true;
}

void plat_flush_events(void) { evh = evt; }
bool plat_quit_requested(void) { return quit_req; }

uint64_t plat_micros(void) { return (SDL_GetTicksNS() - t0) / 1000; }
uint32_t plat_ticks(void) { return (uint32_t)((SDL_GetTicksNS() - t0) * 60 / 1000000000ull); }
void plat_sleep_ms(int ms) { SDL_Delay((Uint32)ms); }

uint32_t plat_secs_since_1904(void)
{
    /* local time, as the classic Mac clock was */
    time_t now = time(NULL);
    struct tm gt;
#if defined(_WIN32)
    gmtime_s(&gt, &now);
#else
    gmtime_r(&now, &gt);
#endif
    gt.tm_isdst = -1;
    /* mktime interprets the UTC fields as local time: the difference is the zone offset */
    time_t offset = now - mktime(&gt);
    return (uint32_t)((uint64_t)(now + offset) + 2082844800ull);
}

void plat_show_cursor(bool show)
{
    if (show) SDL_ShowCursor(); else SDL_HideCursor();
}

void plat_set_title(const char *t) { if (win) SDL_SetWindowTitle(win, t); }
