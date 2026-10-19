/* Host platform services on top of SDL3: video, input, time, audio. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SCREEN_W 640
#define SCREEN_H 480

typedef struct {
    uint8_t keymap[16];     /* Mac KeyMap bit array (GetKeys order) */
    int mouse_x, mouse_y;   /* in 640x480 screen coordinates */
    bool button;
    bool caps_lock;
    uint16_t modifiers;     /* Event Manager modifier bits */
} InputState;

typedef struct { uint16_t what; uint32_t message; int16_t h, v; uint16_t modifiers; } PlatEvent;
enum { PEV_MOUSEDOWN = 1, PEV_MOUSEUP = 2, PEV_KEYDOWN = 3, PEV_KEYUP = 4, PEV_AUTOKEY = 5 };

bool plat_init(int scale, bool fullscreen);
void plat_shutdown(void);
void plat_pump(void);                                    /* process OS events */
/* a hi-res text bitmap drawn over the game: (x,y) in 640x480 coordinates,
 * (sx,sy,sw,sh) the source rectangle in the bitmap's own output pixels */
typedef struct { int handle; float x, y; float sx, sy, sw, sh; uint8_t r, g, b; } PlatTextDraw;
void plat_present(const uint8_t *fb, int pitch, const uint8_t pal[256][3], const PlatTextDraw *draws, int ndraws);
float plat_output_scale(void);                 /* output pixels per 640x480 pixel */
int plat_text_create(const uint8_t *alpha, int w, int h);
void plat_text_destroy(int handle);
void plat_request_capture(const char *path);   /* save the next presented frame (BMP) */
const InputState *plat_input(void);
bool plat_pop_event(PlatEvent *ev);
void plat_flush_events(void);
bool plat_quit_requested(void);
uint32_t plat_ticks(void);                               /* 60 Hz since start */
uint64_t plat_micros(void);
void plat_sleep_ms(int ms);
uint32_t plat_secs_since_1904(void);
void plat_set_title(const char *t);
void plat_show_cursor(bool show);
void plat_script_key(double at, int mac, int frames);
void plat_script_click(double at, int x, int y);
void plat_script_move(double at, int x, int y);
bool plat_save_bmp(const char *path, const uint8_t *fb, int pitch, const uint8_t pal[256][3]);
