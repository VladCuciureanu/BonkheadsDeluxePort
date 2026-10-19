/* OS Utilities, Gestalt, packages (List Manager, Standard File, Apple Events,
 * International Utilities), popup menus and assorted traps. */
#include "qd.h"
#include "res.h"
#include "file.h"
#include "event.h"
#include "sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#endif

/* ------------------------------------------------------------ string helpers */
void pstr_to_c(uint32_t p, char *out, size_t outsz)
{
    size_t n = p ? rdb(p) : 0;
    if (n >= outsz) n = outsz - 1;
    for (size_t i = 0; i < n; i++) out[i] = (char)rdb(p + 1 + (uint32_t)i);
    out[n] = 0;
}

void c_to_pstr(const char *s, uint32_t p)
{
    size_t n = strlen(s);
    if (n > 255) n = 255;
    wrb(p, (uint8_t)n);
    memcpy(HP(p + 1), s, n);
}

uint32_t ticks_now(void) { return plat_ticks(); }

uint32_t SDL_rand_hack(void) { return (uint32_t)time(NULL) * 2654435761u ^ (uint32_t)plat_micros(); }

/* ------------------------------------------------------------ Gestalt */
static int16_t gestalt(uint32_t sel, uint32_t *resp)
{
    switch (sel) {
    case 0x61646472: *resp = 7; return 0;              /* 'addr': 32-bit addressing */
    case 0x73797376: *resp = 0x0761; return 0;         /* 'sysv' 7.6.1 */
    case 0x71642020: *resp = 0x0230; return 0;         /* 'qd  ' 32-bit Color QD */
    case 0x666F6C64: *resp = 1; return 0;              /* 'fold' FindFolder present */
    case 0x70726F63: *resp = 3; return 0;              /* 'proc' 68020 */
    case 0x63707520: *resp = 2; return 0;              /* 'cpu ' */
    case 0x66707520: *resp = 0; return 0;              /* 'fpu ' none */
    case 0x6D6D7520: *resp = 2; return 0;              /* 'mmu ' */
    case 0x72616D20: case 0x6C72616D: *resp = MEM_SIZE; return 0; /* 'ram ', 'lram' */
    case 0x736E6420: *resp = 0x0F; return 0;           /* 'snd ' */
    case 0x6F732020: *resp = 0x0F; return 0;           /* 'os  ' */
    case 0x6B626420: *resp = 5; return 0;              /* 'kbd ' extended */
    case 0x61746C6B: *resp = 0x3C; return 0;           /* 'atlk' AppleTalk 60 */
    case 0x74696D65: *resp = 3; return 0;              /* 'tmgr' */
    case 0x616C6973: *resp = 1; return 0;              /* 'alis' */
    default: return -5551;                             /* gestaltUndefSelectorErr */
    }
}

static void T_Gestalt(uint16_t op)
{
    (void)op;
    uint32_t resp = 0;
    int16_t e = gestalt(D[0], &resp);
    A[0] = resp;
    os_result(e);
}

/* ------------------------------------------------------------ dates */
static void secs_to_date(uint32_t secs, uint32_t rec)
{
    time_t t = (time_t)((int64_t)secs - 2082844800LL);
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    wrw(rec, (uint16_t)(tmv.tm_year + 1900)); wrw(rec + 2, (uint16_t)(tmv.tm_mon + 1)); wrw(rec + 4, (uint16_t)tmv.tm_mday);
    wrw(rec + 6, (uint16_t)tmv.tm_hour); wrw(rec + 8, (uint16_t)tmv.tm_min); wrw(rec + 10, (uint16_t)tmv.tm_sec);
    wrw(rec + 12, (uint16_t)(tmv.tm_wday + 1));
}
static void T_ReadDateTime(uint16_t op) { (void)op; wrl(A[0], plat_secs_since_1904()); os_result(0); }
static void T_SetDateTime(uint16_t op) { (void)op; os_result(0); }
static void T_Secs2Date(uint16_t op) { (void)op; secs_to_date(D[0], A[0]); }
static void T_Date2Secs(uint16_t op)
{
    (void)op;
    uint32_t r = A[0];
    struct tm tmv; memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = (int16_t)rdw(r) - 1900; tmv.tm_mon = (int16_t)rdw(r + 2) - 1; tmv.tm_mday = (int16_t)rdw(r + 4);
    tmv.tm_hour = (int16_t)rdw(r + 6); tmv.tm_min = (int16_t)rdw(r + 8); tmv.tm_sec = (int16_t)rdw(r + 10);
#if defined(_WIN32)
    time_t t = _mkgmtime(&tmv);
#else
    time_t t = timegm(&tmv);
#endif
    D[0] = (uint32_t)((int64_t)t + 2082844800LL);
}
static void T_Microseconds(uint16_t op) { (void)op; uint64_t us = plat_micros(); A[0] = (uint32_t)(us >> 32); D[0] = (uint32_t)us; }

/* ------------------------------------------------------------ misc OS */
void app_quit(void);
static void T_ExitToShell(uint16_t op) { (void)op; app_quit(); }
static void T_SysError(uint16_t op) { (void)op; log_fatal("SysError %d", (int16_t)D[0]); }
static void T_Debugger(uint16_t op) { (void)op; log_warn("Debugger trap ignored"); }
static void T_DebugStr(uint16_t op) { (void)op; char s[256]; pstr_to_c(ARG32(), s, sizeof s); log_warn("DebugStr: %s", s); }
static void T_SwapMMUMode(uint16_t op) { (void)op; D[0] = 1; }
static void T_HWPriv(uint16_t op) { (void)op; os_result(0); }
static void T_noop_os(uint16_t op) { (void)op; os_result(0); }
static void T_SysEnvirons(uint16_t op)
{
    (void)op;
    uint32_t p = A[0];
    memset(HP(p), 0, 16);
    wrw(p, 2); wrw(p + 2, 13); wrw(p + 4, 0x0761); wrw(p + 6, 3); wrb(p + 8, 0); wrb(p + 9, 1); wrw(p + 10, 5); wrw(p + 12, 0x3C);
    os_result(0);
}
static void T_GetString(uint16_t op)
{
    (void)op;
    int16_t id = ARGS16();
    RET32(res_get(OSTYPE('S','T','R',' '), id, false));
}
static void T_UprString(uint16_t op)
{
    (void)op;
    for (uint32_t i = 0; i < (D[0] & 0xFFFF); i++) { uint8_t c = (uint8_t)rdb(A[0] + i); if (c >= 'a' && c <= 'z') wrb(A[0] + i, c - 32); }
}
static void T_CmpString(uint16_t op)
{
    (void)op;
    uint32_t la = D[0] >> 16, lb = D[0] & 0xFFFF;
    bool case_sens = (op & 0x0400) != 0;
    int diff = la != lb;
    for (uint32_t i = 0; !diff && i < la; i++) {
        uint8_t a = (uint8_t)rdb(A[0] + i), b = (uint8_t)rdb(A[1] + i);
        if (!case_sens) { if (a >= 'a' && a <= 'z') a -= 32; if (b >= 'a' && b <= 'z') b -= 32; }
        diff = a != b;
    }
    D[0] = (uint32_t)diff;
    fZ = !diff;
}
static void T_RelString(uint16_t op)
{
    (void)op;
    uint32_t la = D[0] >> 16, lb = D[0] & 0xFFFF;
    int r = 0;
    for (uint32_t i = 0; i < la && i < lb && !r; i++) {
        uint8_t a = (uint8_t)rdb(A[0] + i), b = (uint8_t)rdb(A[1] + i);
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        r = a < b ? -1 : a > b ? 1 : 0;
    }
    if (!r) r = la < lb ? -1 : la > lb ? 1 : 0;
    D[0] = (uint32_t)r;
    fZ = r == 0; fN = r < 0;
}
static void T_Munger(uint16_t op)
{
    (void)op;
    uint32_t len2 = ARG32(), p2 = ARG32(), len1 = ARG32(), p1 = ARG32(); int32_t off = (int32_t)ARG32(); uint32_t h = ARG32();
    (void)len2; (void)p2;
    uint32_t sz = mm_handle_size(h);
    uint8_t *b = HP(rdl(h));
    for (uint32_t i = (uint32_t)off; p1 && i + len1 <= sz; i++)
        if (!memcmp(b + i, HP(p1), len1)) { RET32(i); return; }
    RET32(0xFFFFFFFF);
}
static void T_ZeroScrap(uint16_t op) { (void)op; RET32(0); }
static void T_GetScrap(uint16_t op) { (void)op; (void)ARG32(); (void)ARG32(); (void)ARG32(); RET32((uint32_t)-102); }
static void T_TEInit(uint16_t op) { (void)op; }
static void T_ControlStripDispatch(uint16_t op) { (void)op; uint16_t sel = (uint16_t)D[0]; if (sel == 0x0101) (void)ARG16(); RET8(0); }
static void T_OpenDeskAcc(uint16_t op) { (void)op; (void)ARG32(); RET16(0); }

/* FindFolder & friends live behind _AliasDispatch ($A823) selector 0 */
static void T_AliasDispatch(uint16_t op)
{
    (void)op;
    uint16_t sel = (uint16_t)D[0];
    if (sel == 0) { /* FindFolder(vRefNum, folderType, create, &foundVRefNum, &foundDirID) */
        uint32_t pdir = ARG32(), pvref = ARG32(); (void)ARG8(); (void)ARG32(); (void)ARG16();
        wrw(pvref, (uint16_t)FS_VREFNUM); wrl(pdir, FS_ROOT_DIR);
        RET16(0);
        return;
    }
    log_fatal("AliasDispatch selector %d not implemented", sel);
}

/* ------------------------------------------------------------ Apple Events (Pack8) */
static void T_Pack8(uint16_t op)
{
    (void)op;
    uint16_t sel = (uint16_t)D[0];
    A[7] += ((sel >> 8) & 0xFF) * 2;
    RET16((sel & 0xFF) == 0x1B ? 0 : 0);
}

/* ------------------------------------------------------------ Binary-Decimal conversion (Pack7) */
static void T_Pack7(uint16_t op)
{
    (void)op;
    int16_t sel = ARGS16();
    if (sel == 0) {          /* NumToString: D0 = number, A0 = Str255 */
        char b[32]; snprintf(b, sizeof b, "%ld", (long)(int32_t)D[0]);
        c_to_pstr(b, A[0]);
    } else if (sel == 1) {   /* StringToNum: A0 = Str255 -> D0 */
        char b[256]; pstr_to_c(A[0], b, sizeof b);
        D[0] = (uint32_t)(int32_t)strtol(b, NULL, 10);
    } else {
        log_fatal("Pack7 selector %d not implemented", sel);
    }
}

/* ------------------------------------------------------------ International Utilities (Pack6) */
static void T_Pack6(uint16_t op)
{
    (void)op;
    int16_t sel = ARGS16();
    log_warn("Pack6 selector %d ignored", sel);
}

/* ------------------------------------------------------------ simple modal UI helpers (used by StdFile / popup) */
static void ui_fill(QRect r, uint8_t c) { for (int y = r.top; y < r.bottom; y++) if (y >= 0 && y < SCREEN_H) for (int x = r.left; x < r.right; x++) if (x >= 0 && x < SCREEN_W) wrb(g_screen_fb + (uint32_t)y * SCREEN_W + x, c); }
static void ui_frame(QRect r, uint8_t c)
{
    QRect a = { r.top, r.left, (int16_t)(r.top + 1), r.right }, b = { (int16_t)(r.bottom - 1), r.left, r.bottom, r.right };
    QRect l = { r.top, r.left, r.bottom, (int16_t)(r.left + 1) }, rr = { r.top, (int16_t)(r.right - 1), r.bottom, r.right };
    ui_fill(a, c); ui_fill(b, c); ui_fill(l, c); ui_fill(rr, c);
}
static uint32_t ui_port;
static void ui_text(int x, int y, const char *s, uint8_t fg)
{
    if (!ui_port) { ui_port = mm_new_ptr(CGRAFPORT_SIZE, true); qd_init_port(ui_port, true); }
    uint16_t r, g, b; qd_index_to_rgb(qd_device_ctab(), fg, &r, &g, &b);
    wrw(ui_port + P_rgbFg, r); wrw(ui_port + P_rgbFg + 2, g); wrw(ui_port + P_rgbFg + 4, b);
    wrw(ui_port + P_txFont, 3); wrw(ui_port + P_txSize, 12);
    wrw(ui_port + P_pnLoc, (uint16_t)y); wrw(ui_port + P_pnLoc + 2, (uint16_t)x);
    text_draw(ui_port, (const uint8_t *)s, (int)strlen(s));
}
static uint8_t *ui_save(QRect r)
{
    int w = r.right - r.left, h = r.bottom - r.top;
    uint8_t *b = (uint8_t *)malloc((size_t)w * h);
    for (int y = 0; y < h; y++) memcpy(b + (size_t)y * w, HP(g_screen_fb + (uint32_t)(r.top + y) * SCREEN_W + r.left), w);
    return b;
}
static void ui_restore(QRect r, uint8_t *b)
{
    int w = r.right - r.left, h = r.bottom - r.top;
    for (int y = 0; y < h; y++) memcpy(HP(g_screen_fb + (uint32_t)(r.top + y) * SCREEN_W + r.left), b + (size_t)y * w, w);
    free(b);
}
static uint8_t BLACK, WHITE, HILITE, GREY;
static void ui_colors(void)
{
    uint32_t ct = qd_device_ctab();
    BLACK = qd_rgb_to_index(ct, 0, 0, 0); WHITE = qd_rgb_to_index(ct, 0xFFFF, 0xFFFF, 0xFFFF);
    HILITE = qd_rgb_to_index(ct, 0x3333, 0x3333, 0x9999); GREY = qd_rgb_to_index(ct, 0xCCCC, 0xCCCC, 0xCCCC);
}

/* wait for a key or click; returns key char (or 0 for click) and fills mouse pos */
static int ui_wait_input(int *mx, int *my, bool *dbl)
{
    uint32_t ev = mm_new_ptr(16, true);
    static uint32_t last_click; static int lx, ly;
    int r = -1;
    while (r < 0) {
        evt_idle();
        screen_present(false);
        if (evt_quit_requested()) { r = 27; break; }
        if (evt_next(0x002A, ev, true)) {   /* mouseDown | keyDown | autoKey */
            uint16_t what = (uint16_t)rdw(ev);
            if (what == 1) {
                *my = (int16_t)rdw(ev + 10); *mx = (int16_t)rdw(ev + 12);
                uint32_t t = rdl(ev + 6);
                *dbl = (t - last_click < 30 && abs(*mx - lx) < 4 && abs(*my - ly) < 4);
                last_click = t; lx = *mx; ly = *my;
                r = 0;
            } else r = (int)(rdl(ev + 2) & 0xFF) | (int)((rdl(ev + 2) & 0xFF00) << 8);
        }
        plat_sleep_ms(5);
    }
    mm_dispose_ptr(ev);
    return r;
}

/* popup menu: returns chosen item (1-based) or 0 */
int popup_run(uint32_t menu, int top, int left, int cur)
{
    ui_colors();
    char items[64][64]; int n = 0;
    uint32_t p = rdl(menu);
    uint32_t q = p + 14 + 1 + rdb(p + 14);
    while (rdb(q) && n < 64) { pstr_to_c(q, items[n], sizeof items[n]); n++; q += 1 + rdb(q) + 4; }
    if (!n) return 0;
    int w = 40;
    for (int i = 0; i < n; i++) { int l = (int)strlen(items[i]) * 8 + 30; if (l > w) w = l; }
    int ih = 18;
    int t = top - (cur > 0 ? (cur - 1) * ih : 0);
    if (t < 0) t = 0;
    if (t + n * ih + 2 > SCREEN_H) t = SCREEN_H - n * ih - 2;
    QRect r = { (int16_t)t, (int16_t)left, (int16_t)(t + n * ih + 2), (int16_t)(left + w) };
    if (r.right > SCREEN_W) { r.left = (int16_t)(SCREEN_W - w); r.right = SCREEN_W; }
    QRect sr = { r.top, r.left, (int16_t)(r.bottom + 2 > SCREEN_H ? SCREEN_H : r.bottom + 2), (int16_t)(r.right + 2 > SCREEN_W ? SCREEN_W : r.right + 2) };
    uint8_t *saved = ui_save(sr);
    int sel = 0;
    for (;;) {
        const InputState *in = plat_input();
        sel = 0;
        if (in->mouse_x >= r.left && in->mouse_x < r.right && in->mouse_y > r.top && in->mouse_y < r.bottom - 1)
            sel = (in->mouse_y - r.top - 1) / ih + 1;
        ui_fill(r, WHITE); ui_frame(r, BLACK);
        for (int i = 0; i < n; i++) {
            QRect ir = { (int16_t)(r.top + 1 + i * ih), (int16_t)(r.left + 1), (int16_t)(r.top + 1 + (i + 1) * ih), (int16_t)(r.right - 1) };
            bool hl = sel == i + 1;
            if (hl) ui_fill(ir, BLACK);
            if (i + 1 == cur) ui_text(ir.left + 4, ir.top + 13, "*", hl ? WHITE : BLACK);
            ui_text(ir.left + 16, ir.top + 13, items[i], hl ? WHITE : BLACK);
        }
        screen_present(true);
        evt_idle();
        if (!plat_input()->button) break;
        plat_sleep_ms(10);
    }
    ui_restore(sr, saved);
    screen_present(true);
    return sel;
}

/* ------------------------------------------------------------ Standard File (Pack3) */
static bool file_matches(const char *dir, const char *name, uint32_t types, int ntypes)
{
    if (ntypes <= 0) return true;
    char fp[1200]; snprintf(fp, sizeof fp, "%s/%s.finf", dir, name);
    FILE *f = fopen(fp, "rb");
    if (!f) return false;
    uint8_t b[8] = { 0 };
    size_t r = fread(b, 1, 8, f); (void)r;
    fclose(f);
    uint32_t t = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    for (int i = 0; i < ntypes; i++) if (rdl(types + i * 4) == t) return true;
    return false;
}

static int list_files(char names[][64], int max, uint32_t types, int ntypes)
{
    int n = 0;
#if defined(_WIN32)
    char pat[1100]; snprintf(pat, sizeof pat, "%s/*.finf", g_game_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char nm[260]; snprintf(nm, sizeof nm, "%s", fd.cFileName);
        size_t l = strlen(nm); if (l > 5) nm[l - 5] = 0;
        if (n < max && strlen(nm) < 64 && file_matches(g_game_dir, nm, types, ntypes)) snprintf(names[n++], 64, "%s", nm);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(g_game_dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".finf")) continue;
        char nm[260]; snprintf(nm, sizeof nm, "%s", e->d_name); nm[l - 5] = 0;
        if (n < max && strlen(nm) < 64 && file_matches(g_game_dir, nm, types, ntypes)) snprintf(names[n++], 64, "%s", nm);
    }
    closedir(d);
#endif
    return n;
}

static void fill_reply(uint32_t reply, bool good, const char *name, bool replacing)
{
    memset(HP(reply), 0, 88);
    wrb(reply, good); wrb(reply + 1, replacing);
    wrw(reply + 6, (uint16_t)FS_VREFNUM); wrl(reply + 8, FS_ROOT_DIR);
    c_to_pstr(name, reply + 12);
}

static void std_put_file(uint32_t prompt, uint32_t defname, uint32_t reply)
{
    ui_colors();
    char ptxt[256], name[64];
    pstr_to_c(prompt, ptxt, sizeof ptxt); pstr_to_c(defname, name, sizeof name);
    QRect r = { 150, 120, 290, 520 };
    uint8_t *saved = ui_save(r);
    bool ok = false, done = false;
    while (!done) {
        ui_fill(r, WHITE); ui_frame(r, BLACK);
        QRect in = { 190, 140, 214, 500 };
        ui_text(140, 178, ptxt, BLACK);
        ui_fill(in, WHITE); ui_frame(in, BLACK);
        ui_text(146, 207, name, BLACK);
        QRect save = { 240, 400, 264, 500 }, cancel = { 240, 290, 264, 390 };
        ui_frame(save, BLACK); ui_frame(cancel, BLACK);
        ui_text(430, 257, "Save", BLACK); ui_text(316, 257, "Cancel", BLACK);
        screen_present(true);
        int mx, my; bool dbl;
        int k = ui_wait_input(&mx, &my, &dbl);
        if (k == 0) {
            if (mx >= save.left && mx < save.right && my >= save.top && my < save.bottom) { ok = true; done = true; }
            if (mx >= cancel.left && mx < cancel.right && my >= cancel.top && my < cancel.bottom) done = true;
        } else {
            int ch = k & 0xFF;
            if (ch == 13 || ch == 3) { ok = name[0] != 0; done = ok; }
            else if (ch == 27) done = true;
            else if (ch == 8) { size_t l = strlen(name); if (l) name[l - 1] = 0; }
            else if (ch >= 32 && ch != ':' && strlen(name) < 31) { size_t l = strlen(name); name[l] = (char)ch; name[l + 1] = 0; }
        }
    }
    ui_restore(r, saved);
    screen_present(true);
    char path[1024]; fs_resolve(FS_VREFNUM, FS_ROOT_DIR, name, path, sizeof path, false);
    char fp[1100]; snprintf(fp, sizeof fp, "%s.finf", path);
    fill_reply(reply, ok, name, ok && fs_exists(fp));
}

static void std_get_file(uint32_t types, int ntypes, uint32_t reply)
{
    ui_colors();
    static char names[64][64];
    int n = list_files(names, 64, types, ntypes);
    QRect r = { 100, 140, 380, 500 };
    uint8_t *saved = ui_save(r);
    int sel = n ? 0 : -1, top = 0;
    bool ok = false, done = false;
    while (!done) {
        ui_fill(r, WHITE); ui_frame(r, BLACK);
        ui_text(160, 124, n ? "Open which game?" : "No saved games found.", BLACK);
        QRect lr = { 134, 160, 334, 480 };
        ui_frame(lr, BLACK);
        for (int i = 0; i < 11 && top + i < n; i++) {
            QRect ir = { (int16_t)(lr.top + 1 + i * 18), (int16_t)(lr.left + 1), (int16_t)(lr.top + 1 + (i + 1) * 18), (int16_t)(lr.right - 1) };
            bool hl = top + i == sel;
            if (hl) ui_fill(ir, BLACK);
            ui_text(ir.left + 6, ir.top + 13, names[top + i], hl ? WHITE : BLACK);
        }
        QRect open = { 344, 380, 368, 480 }, cancel = { 344, 270, 368, 370 };
        ui_frame(open, BLACK); ui_frame(cancel, BLACK);
        ui_text(412, 361, "Open", n ? BLACK : GREY); ui_text(296, 361, "Cancel", BLACK);
        screen_present(true);
        int mx, my; bool dbl;
        int k = ui_wait_input(&mx, &my, &dbl);
        if (k == 0) {
            if (mx >= lr.left && mx < lr.right && my > lr.top && my < lr.bottom) {
                int i = top + (my - lr.top - 1) / 18;
                if (i < n) { sel = i; if (dbl) { ok = true; done = true; } }
            }
            if (n && mx >= open.left && mx < open.right && my >= open.top && my < open.bottom) { ok = true; done = true; }
            if (mx >= cancel.left && mx < cancel.right && my >= cancel.top && my < cancel.bottom) done = true;
        } else {
            int ch = k & 0xFF, code = (k >> 16) & 0xFF;
            if ((ch == 13 || ch == 3) && n) { ok = true; done = true; }
            else if (ch == 27) done = true;
            else if (code == 0x7E || ch == 30) { if (sel > 0) sel--; }
            else if (code == 0x7D || ch == 31) { if (sel < n - 1) sel++; }
            if (sel < top) top = sel;
            if (sel >= top + 11) top = sel - 10;
        }
    }
    ui_restore(r, saved);
    screen_present(true);
    fill_reply(reply, ok && sel >= 0, ok && sel >= 0 ? names[sel] : "", false);
    if (ok && sel >= 0) {
        char fp[1200]; snprintf(fp, sizeof fp, "%s/%s.finf", g_game_dir, names[sel]);
        FILE *f = fopen(fp, "rb");
        if (f) { uint8_t b[4]; if (fread(b, 1, 4, f) == 4) memcpy(HP(reply + 2), b, 4); fclose(f); }
    }
}

static void T_Pack3(uint16_t op)
{
    (void)op;
    int16_t sel = ARGS16();
    switch (sel) {
    case 5: { uint32_t reply = ARG32(), def = ARG32(), prompt = ARG32(); std_put_file(prompt, def, reply); return; }
    case 6: { uint32_t reply = ARG32(), types = ARG32(); int16_t n = ARGS16(); (void)ARG32(); std_get_file(types, n, reply); return; }
    default: log_fatal("Standard File selector %d not implemented", sel);
    }
}

/* ------------------------------------------------------------ List Manager (Pack0) */
typedef struct { uint32_t h; uint32_t port; QRect view; int cellh; int rows; char cells[64][64]; int sel; } ListInfo;
static ListInfo lists[4];
static ListInfo *list_of(uint32_t h) { for (int i = 0; i < 4; i++) if (lists[i].h == h && h) return &lists[i]; return NULL; }

static void list_draw(ListInfo *l)
{
    uint32_t port = l->port;
    PixDesc pd;
    if (!qd_port_pixels(port, &pd)) return;
    uint32_t ct = pd.ctab;
    uint8_t black = qd_rgb_to_index(ct, 0, 0, 0), white = qd_rgb_to_index(ct, 0xFFFF, 0xFFFF, 0xFFFF);
    static const uint8_t solid[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    qd_fill_rect(port, l->view, solid, 8, white, white);
    uint32_t saved = qd_the_port(); qd_set_port(port);
    uint8_t fg[6]; memcpy(fg, HP(port + P_rgbFg), 6);
    uint16_t of = (uint16_t)rdw(port + P_txFont), os = (uint16_t)rdw(port + P_txSize);
    wrw(port + P_txFont, 3); wrw(port + P_txSize, 12);
    for (int i = 0; i < l->rows; i++) {
        QRect cr = { (int16_t)(l->view.top + i * l->cellh), l->view.left, (int16_t)(l->view.top + (i + 1) * l->cellh), l->view.right };
        if (cr.top >= l->view.bottom) break;
        bool hl = i == l->sel;
        if (hl) qd_fill_rect(port, cr, solid, 8, black, black);
        uint16_t c = hl ? 0xFFFF : 0;
        wrw(port + P_rgbFg, c); wrw(port + P_rgbFg + 2, c); wrw(port + P_rgbFg + 4, c);
        wrw(port + P_pnLoc, (uint16_t)(cr.top + l->cellh - 4)); wrw(port + P_pnLoc + 2, (uint16_t)(cr.left + 4));
        text_draw(port, (const uint8_t *)l->cells[i], (int)strlen(l->cells[i]));
    }
    memcpy(HP(port + P_rgbFg), fg, 6);
    wrw(port + P_txFont, of); wrw(port + P_txSize, os);
    qd_set_port(saved);
}

static void list_sync(ListInfo *l)
{
    uint32_t p = rdl(l->h);
    QRect db = { 0, 0, (int16_t)l->rows, 1 };
    wr_rect(p + 72, db);
    wrw(p + 56, (uint16_t)(l->sel >= 0 ? l->sel : 0)); wrw(p + 58, 0);
}

static void T_Pack0(uint16_t op)
{
    (void)op;
    int16_t sel = ARGS16();
    switch (sel) {
    case 0x44: { /* LNew */
        (void)ARG8(); (void)ARG8(); (void)ARG8(); (void)ARG8();
        uint32_t win = ARG32(); (void)ARG16(); uint32_t csize = ARG32(); (void)ARG32(); uint32_t rview = ARG32();
        ListInfo *l = NULL;
        for (int i = 0; i < 4; i++) if (!lists[i].h) { l = &lists[i]; break; }
        if (!l) l = &lists[0];
        memset(l, 0, sizeof *l);
        l->h = mm_new_handle(96, true);
        l->port = win;
        l->view = rd_rect(rview);
        l->cellh = (int16_t)(csize >> 16); if (l->cellh <= 0) l->cellh = 16;
        l->sel = -1;
        uint32_t p = rdl(l->h);
        wr_rect(p, l->view); wrl(p + 8, win); wrl(p + 16, csize);
        list_sync(l);
        RET32(l->h);
        return;
    }
    case 0x58: { /* LSetCell(dataPtr, dataLen, cell, lHandle) */
        uint32_t h = ARG32(); uint32_t cell = ARG32(); int16_t len = ARGS16(); uint32_t data = ARG32();
        ListInfo *l = list_of(h);
        int row = (int16_t)(cell >> 16);
        if (l && row >= 0 && row < 64) {
            int n = len < 63 ? len : 63;
            memcpy(l->cells[row], HP(data), n); l->cells[row][n] = 0;
            list_draw(l);
        }
        return;
    }
    case 0x08: { /* LAddRow(count, rowNum, lHandle): short */
        uint32_t h = ARG32(); int16_t row = ARGS16(); int16_t count = ARGS16();
        ListInfo *l = list_of(h);
        if (!l) { RET16(0); return; }
        if (row < 0 || row > l->rows) row = (int16_t)l->rows;
        for (int k = 0; k < count && l->rows < 64; k++) {
            memmove(l->cells[row + 1], l->cells[row], (size_t)(l->rows - row) * 64);
            l->cells[row][0] = 0;
            l->rows++;
        }
        list_sync(l);
        RET16(row);
        return;
    }
    case 0x24: { /* LDelRow(count, rowNum, lHandle) */
        uint32_t h = ARG32(); int16_t row = ARGS16(); int16_t count = ARGS16();
        ListInfo *l = list_of(h);
        if (!l) return;
        if (count == 0) { l->rows = 0; l->sel = -1; }
        else for (int k = 0; k < count && row < l->rows; k++) {
            memmove(l->cells[row], l->cells[row + 1], (size_t)(l->rows - row - 1) * 64);
            l->rows--;
        }
        if (l->sel >= l->rows) l->sel = -1;
        list_sync(l);
        list_draw(l);
        return;
    }
    case 0x18: { /* LClick(pt, modifiers, lHandle): Boolean */
        uint32_t h = ARG32(); (void)ARG16(); uint32_t pt = ARG32();
        ListInfo *l = list_of(h);
        if (!l) { RET8(0); return; }
        int v = (int16_t)(pt >> 16);
        int row = (v - l->view.top) / l->cellh;
        static uint32_t last_t; static int last_row = -1;
        bool dbl = false;
        if (row >= 0 && row < l->rows) {
            dbl = row == last_row && plat_ticks() - last_t < 30;
            l->sel = row; last_row = row; last_t = plat_ticks();
        }
        list_sync(l);
        list_draw(l);
        while (plat_input()->button) { evt_idle(); plat_sleep_ms(5); }
        RET8(dbl);
        return;
    }
    case 0x3C: { /* LGetSelect(next, VAR cell, lHandle): Boolean */
        uint32_t h = ARG32(); uint32_t pcell = ARG32(); bool next = ARG8() != 0;
        ListInfo *l = list_of(h);
        if (!l || l->sel < 0) { RET8(0); return; }
        int from = next ? (int16_t)rdw(pcell) : l->sel;
        bool found = l->sel >= from;
        if (found) { wrw(pcell, (uint16_t)l->sel); wrw(pcell + 2, 0); }
        RET8(found);
        return;
    }
    case 0x28: { /* LDispose */
        uint32_t h = ARG32();
        ListInfo *l = list_of(h);
        if (l) { mm_dispose_handle(l->h); l->h = 0; }
        return;
    }
    case 0x64: { /* LUpdate(rgn, lHandle) */
        uint32_t h = ARG32(); (void)ARG32();
        ListInfo *l = list_of(h);
        if (l) list_draw(l);
        return;
    }
    case 0x2C: { uint32_t h = ARG32(); (void)ARG8(); (void)h; return; } /* LDoDraw */
    case 0x00: { uint32_t h = ARG32(); (void)ARG8(); (void)h; return; } /* LActivate */
    default:
        log_fatal("List Manager selector %d not implemented", sel);
    }
}

static void T_InitPack(uint16_t op) { (void)op; (void)ARG16(); }
static void T_InitAllPacks(uint16_t op) { (void)op; }

void misc_register_traps(void)
{
    trap_register(0xA1AD, T_Gestalt, "Gestalt");
    trap_register(0xA039, T_ReadDateTime, "ReadDateTime");
    trap_register(0xA03A, T_SetDateTime, "SetDateTime");
    trap_register(0xA9C6, T_Secs2Date, "Secs2Date");
    trap_register(0xA9C7, T_Date2Secs, "Date2Secs");
    trap_register(0xA193, T_Microseconds, "Microseconds");
    trap_register(0xA9F4, T_ExitToShell, "ExitToShell");
    trap_register(0xA9C9, T_SysError, "SysError");
    trap_register(0xA9FF, T_Debugger, "Debugger");
    trap_register(0xABFF, T_DebugStr, "DebugStr");
    trap_register(0xA05D, T_SwapMMUMode, "SwapMMUMode");
    trap_register(0xA198, T_HWPriv, "HWPriv");
    trap_register(0xA090, T_SysEnvirons, "SysEnvirons");
    trap_register(0xA9BA, T_GetString, "GetString");
    trap_register(0xA054, T_UprString, "UprString");
    trap_register(0xA03C, T_CmpString, "CmpString");
    trap_register(0xA050, T_RelString, "RelString");
    trap_register(0xA9E0, T_Munger, "Munger");
    trap_register(0xA9FC, T_ZeroScrap, "ZeroScrap");
    trap_register(0xA9FD, T_GetScrap, "GetScrap");
    trap_register(0xA9CC, T_TEInit, "TEInit");
    trap_register(0xAAF2, T_ControlStripDispatch, "ControlStripDispatch");
    trap_register(0xA9B6, T_OpenDeskAcc, "OpenDeskAcc");
    trap_register(0xA823, T_AliasDispatch, "AliasDispatch");
    trap_register(0xA816, T_Pack8, "Pack8");
    trap_register(0xA9ED, T_Pack6, "Pack6");
    trap_register(0xA9EE, T_Pack7, "Pack7");
    trap_register(0xA9EA, T_Pack3, "Pack3");
    trap_register(0xA9E7, T_Pack0, "Pack0");
    trap_register(0xA9E5, T_InitPack, "InitPack");
    trap_register(0xA9E6, T_InitAllPacks, "InitAllPacks");
}
