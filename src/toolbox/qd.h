/* QuickDraw HLE: structure offsets and internal API. */
#pragma once
#include "toolbox.h"
#include "platform/platform.h"

typedef struct { int16_t top, left, bottom, right; } QRect;

/* CGrafPort / GrafPort */
#define P_portBits      2   /* BitMap (GrafPort) or PixMapHandle (CGrafPort) */
#define P_portVersion   6
#define P_portRect     16
#define P_visRgn       24
#define P_clipRgn      28
#define P_bkPat        32   /* bkPixPat handle in a CGrafPort */
#define P_rgbFg        36
#define P_rgbBk        42
#define P_pnLoc        48
#define P_pnSize       52
#define P_pnMode       56
#define P_pnPixPat     58
#define P_fillPixPat   62
#define P_pnVis        66
#define P_txFont       68
#define P_txFace       70
#define P_txMode       72
#define P_txSize       74
#define P_spExtra      76
#define P_fgColor      80
#define P_bkColor      84
#define P_grafProcs   104
#define CGRAFPORT_SIZE 108
/* we keep the 8-byte black/white patterns of colour ports in grafVars-like private storage */
#define P_privPnPat    92   /* (picSave/rgnSave fields reused: 8 bytes pen pattern) */

/* PixMap */
#define PM_baseAddr   0
#define PM_rowBytes   4
#define PM_bounds     6
#define PM_pixelSize 32
#define PM_pmTable   42
#define PIXMAP_SIZE  50

/* GDevice */
#define GD_type      4
#define GD_flags    20
#define GD_PMap     22
#define GD_nextGD   30
#define GD_rect     34
#define GDEVICE_SIZE 62

/* Window / Dialog records */
#define W_kind       108
#define W_visible    110
#define W_hilited    111
#define W_goAway     112
#define W_strucRgn   114
#define W_contRgn    118
#define W_updateRgn  122
#define W_defProc    126
#define W_dataHandle 130
#define W_title      134
#define W_ctlList    140
#define W_next       144
#define W_pic        148
#define W_refCon     152
#define CWINDOW_SIZE 156
#define D_items      156
#define D_textH      160
#define D_editField  164
#define D_editOpen   166
#define D_aDefItem   168
#define DIALOG_SIZE  170

/* resolved destination/source pixels */
typedef struct {
    uint32_t base;      /* emulated address of pixel (0,0) of bounds */
    uint32_t rowbytes;
    QRect bounds;
    int depth;
    uint32_t ctab;      /* ColorTable handle (0 if none) */
} PixDesc;

static inline QRect rd_rect(uint32_t a) { QRect r = { (int16_t)rdw(a), (int16_t)rdw(a + 2), (int16_t)rdw(a + 4), (int16_t)rdw(a + 6) }; return r; }
static inline void wr_rect(uint32_t a, QRect r) { wrw(a, (uint16_t)r.top); wrw(a + 2, (uint16_t)r.left); wrw(a + 4, (uint16_t)r.bottom); wrw(a + 6, (uint16_t)r.right); }
static inline bool rect_empty(QRect r) { return r.bottom <= r.top || r.right <= r.left; }
QRect rect_sect(QRect a, QRect b);
QRect rect_union(QRect a, QRect b);

extern uint32_t g_screen_fb;          /* emulated address of the 640x480 framebuffer */
extern uint32_t g_main_device;        /* GDHandle */
extern uint32_t g_cur_device;
extern uint8_t g_hw_pal[256][3];      /* what the monitor shows (SetEntries only) */
extern bool g_hw_pal_dirty;

uint32_t qd_the_port(void);
void qd_set_port(uint32_t port);
bool qd_port_pixels(uint32_t port, PixDesc *pd);
bool qd_bitmap_pixels(uint32_t bm, PixDesc *pd);
QRect qd_port_clip(uint32_t port);       /* clip ∩ vis in local coords */
uint8_t qd_rgb_to_index(uint32_t ctab, uint16_t r, uint16_t g, uint16_t b);
void qd_index_to_rgb(uint32_t ctab, uint8_t idx, uint16_t *r, uint16_t *g, uint16_t *b);
uint8_t qd_fg_index(uint32_t port);
uint8_t qd_bk_index(uint32_t port);
void qd_fill_rect(uint32_t port, QRect r, const uint8_t pat[8], int mode, uint8_t fg, uint8_t bk);
void qd_init_port(uint32_t port, bool color);
uint32_t qd_new_rgn(QRect r);
void qd_set_rgn(uint32_t rgn, QRect r);
QRect qd_rgn_box(uint32_t rgn);
uint32_t qd_new_ctab_from(const uint8_t *clut, uint32_t len);
uint32_t qd_device_ctab(void);
void qd_mark_dirty(void);

/* screen presentation */
void screen_present(bool force);
void screen_vbl_wait(void);

/* PICT */
void pict_draw(uint32_t port, uint32_t pichandle, QRect dst);
QRect pict_frame(uint32_t pichandle);

/* text */
void text_draw(uint32_t port, const uint8_t *s, int len);
int text_width(uint32_t port, const uint8_t *s, int len);
void text_font_info(uint32_t port, int *ascent, int *descent, int *widmax, int *leading);
void text_init(void);

/* windows (window.c) */
uint32_t win_front(void);
void win_save_under(uint32_t w);
void win_restore_under(uint32_t w);
void win_draw_frame(uint32_t w);
