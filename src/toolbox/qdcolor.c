/* Color QuickDraw: GDevices, colour tables, SetEntries (the hardware CLUT),
 * Palette Manager and GWorlds. */
#include "qd.h"
#include "res.h"

static void bump_seed(uint32_t ctab)
{
    if (!ctab || !rdl(ctab)) return;
    static uint32_t seed = 1000;
    wrl(rdl(ctab), ++seed);
}

static uint32_t copy_ctab(uint32_t ctab)
{
    if (!ctab || !rdl(ctab)) return qd_new_ctab_from(NULL, 0);
    uint32_t n = mm_handle_size(ctab);
    uint32_t h = mm_new_handle(n, false);
    memcpy(HP(rdl(h)), HP(rdl(ctab)), n);
    return h;
}

/* ------------------------------------------------------------ GDevice */
static void T_GetMainDevice(uint16_t op) { (void)op; RET32(g_main_device); }
static void T_GetDeviceList(uint16_t op) { (void)op; RET32(g_main_device); }
static void T_GetNextDevice(uint16_t op) { (void)op; (void)ARG32(); RET32(0); }
static void T_GetGDevice(uint16_t op) { (void)op; RET32(g_cur_device); }
static void T_SetGDevice(uint16_t op) { (void)op; uint32_t gd = ARG32(); g_cur_device = gd ? gd : g_main_device; wrl(LM_TheGDevice, g_cur_device); }
static void T_GetMaxDevice(uint16_t op) { (void)op; (void)ARG32(); RET32(g_main_device); }
static void T_TestDeviceAttribute(uint16_t op)
{
    (void)op;
    int16_t attr = ARGS16(); uint32_t gd = ARG32();
    RET8(gd && rdl(gd) && ((rdw(rdl(gd) + GD_flags) >> attr) & 1));
}
static void T_SetDeviceAttribute(uint16_t op)
{
    (void)op;
    uint8_t v = ARG8(); int16_t attr = ARGS16(); uint32_t gd = ARG32();
    if (!gd || !rdl(gd)) return;
    uint16_t f = (uint16_t)rdw(rdl(gd) + GD_flags);
    if (v) f |= (uint16_t)(1 << attr); else f &= (uint16_t)~(1 << attr);
    wrw(rdl(gd) + GD_flags, f);
}
static void T_InitGDevice(uint16_t op) { (void)op; (void)ARG32(); (void)ARG16(); (void)ARG16(); }

/* ------------------------------------------------------------ colour tables */
static void T_GetCTable(uint16_t op)
{
    (void)op;
    int16_t id = ARGS16();
    uint32_t len;
    const uint8_t *p = res_peek(OSTYPE('c','l','u','t'), id, &len);
    if (p) { RET32(qd_new_ctab_from(p, len)); return; }
    if (id == 8 || id == 72 || id == 127) { RET32(qd_new_ctab_from(NULL, 0)); return; }
    RET32(0);
}
static void T_DisposeCTable(uint16_t op) { (void)op; uint32_t h = ARG32(); if (h && mm_is_handle(h)) mm_dispose_handle(h); }
static void T_MakeITable(uint16_t op) { (void)op; (void)ARG16(); (void)ARG32(); (void)ARG32(); }
static void T_CTabChanged_tb(uint16_t op) { (void)op; bump_seed(ARG32()); }

static void T_Color2Index(uint16_t op)
{
    (void)op;
    uint32_t rgb = ARG32();
    RET32(qd_rgb_to_index(qd_device_ctab(), (uint16_t)rdw(rgb), (uint16_t)rdw(rgb + 2), (uint16_t)rdw(rgb + 4)));
}
static void T_Index2Color(uint16_t op)
{
    (void)op;
    uint32_t rgb = ARG32(); uint32_t idx = ARG32();
    uint16_t r, g, b; qd_index_to_rgb(qd_device_ctab(), (uint8_t)idx, &r, &g, &b);
    wrw(rgb, r); wrw(rgb + 2, g); wrw(rgb + 4, b);
}
static void T_InvertColor(uint16_t op) { (void)op; uint32_t rgb = ARG32(); wrw(rgb, ~rdw(rgb)); wrw(rgb + 2, ~rdw(rgb + 2)); wrw(rgb + 4, ~rdw(rgb + 4)); }
static void T_RealColor(uint16_t op) { (void)op; (void)ARG32(); RET8(1); }

/* SetEntries(start, count, aTable): changes the hardware CLUT and the device table */
static void T_SetEntries(uint16_t op)
{
    (void)op;
    uint32_t table = ARG32(); int16_t count = ARGS16(); int16_t start = ARGS16();
    uint32_t dct = qd_device_ctab();
    uint32_t dp = rdl(dct);
    for (int i = 0; i <= count; i++) {
        uint32_t e = table + i * 8;
        int idx = start < 0 ? (rdw(e) & 0xFF) : (start + i) & 0xFF;
        uint16_t r = (uint16_t)rdw(e + 2), g = (uint16_t)rdw(e + 4), b = (uint16_t)rdw(e + 6);
        g_hw_pal[idx][0] = (uint8_t)(r >> 8); g_hw_pal[idx][1] = (uint8_t)(g >> 8); g_hw_pal[idx][2] = (uint8_t)(b >> 8);
        uint32_t de = dp + 8 + idx * 8;
        wrw(de + 2, r); wrw(de + 4, g); wrw(de + 6, b);
    }
    bump_seed(dct);
    g_hw_pal_dirty = true;
    screen_vbl_wait();
}

static void T_SaveEntries(uint16_t op)
{
    (void)op;
    uint32_t sel = ARG32(), result = ARG32(), src = ARG32();
    (void)sel;
    uint32_t s = src ? rdl(src) : rdl(qd_device_ctab());
    uint32_t n = mm_handle_size(result);
    if (n > mm_handle_size(src ? src : qd_device_ctab())) n = mm_handle_size(src ? src : qd_device_ctab());
    memcpy(HP(rdl(result)), HP(s), n);
}
static void T_RestoreEntries(uint16_t op)
{
    (void)op;
    uint32_t sel = ARG32(), dst = ARG32(), src = ARG32();
    (void)sel; (void)dst;
    uint32_t p = rdl(src);
    int n = (int16_t)rdw(p + 6) + 1;
    uint32_t dp = rdl(qd_device_ctab());
    for (int i = 0; i < n && i < 256; i++) {
        uint32_t e = p + 8 + i * 8;
        int idx = rdw(e) & 0xFF;
        memcpy(HP(dp + 8 + idx * 8 + 2), HP(e + 2), 6);
        g_hw_pal[idx][0] = (uint8_t)(rdw(e + 2) >> 8); g_hw_pal[idx][1] = (uint8_t)(rdw(e + 4) >> 8); g_hw_pal[idx][2] = (uint8_t)(rdw(e + 6) >> 8);
    }
    g_hw_pal_dirty = true;
}

/* ------------------------------------------------------------ Palette Manager */
static void T_InitPalettes(uint16_t op) { (void)op; }
static void T_NewPalette(uint16_t op)
{
    (void)op;
    (void)ARG16(); (void)ARG16(); uint32_t ct = ARG32(); int16_t n = ARGS16();
    uint32_t h = mm_new_handle(16 + (uint32_t)(n > 0 ? n : 1) * 16, true);
    wrw(rdl(h), (uint16_t)n);
    (void)ct;
    RET32(h);
}
static void T_GetNewPalette(uint16_t op) { (void)op; (void)ARG16(); RET32(mm_new_handle(16, true)); }
static void T_DisposePalette(uint16_t op) { (void)op; uint32_t h = ARG32(); if (h && mm_is_handle(h)) mm_dispose_handle(h); }
static void T_SetPalette(uint16_t op) { (void)op; (void)ARG8(); (void)ARG32(); (void)ARG32(); }
static void T_ActivatePalette(uint16_t op) { (void)op; (void)ARG32(); }
static void T_GetPalette(uint16_t op) { (void)op; (void)ARG32(); RET32(0); }
static void T_PaletteDispatch(uint16_t op)
{
    (void)op;
    uint16_t sel = (uint16_t)D[0];
    switch (sel) {
    case 0x0A14: { /* HasDepth(gd, depth, whichFlags, flags): short */
        (void)ARG16(); (void)ARG16(); int16_t depth = ARGS16(); (void)ARG32();
        RET16(depth == 8 ? 0x0100 : 0);
        return;
    }
    case 0x0A13: /* SetDepth(gd, depth, whichFlags, flags): OSErr */
        (void)ARG16(); (void)ARG16(); (void)ARG16(); (void)ARG32();
        RET16(0);
        return;
    default:
        log_warn("PaletteDispatch selector %04X ignored", sel);
        A[7] += (sel >> 8) * 2;
        RET32(0);
    }
}

/* ------------------------------------------------------------ GWorlds ($AB1D) */
static uint32_t new_gworld(int16_t depth, QRect b, uint32_t ctab)
{
    if (depth == 0) depth = 8;
    if (depth != 8) log_warn("NewGWorld depth %d treated as 8", depth);
    int w = b.right - b.left, h = b.bottom - b.top;
    uint32_t rowbytes = ((uint32_t)w + 3) & ~3u;
    uint32_t pixels = mm_new_ptr(rowbytes * (uint32_t)h + 16, true);
    if (!pixels) return 0;
    uint32_t port = mm_new_ptr(CGRAFPORT_SIZE, true);
    qd_init_port(port, true);
    uint32_t pmh = rdl(port + P_portBits);
    uint32_t pm = rdl(pmh);
    wrl(pm + PM_baseAddr, pixels);
    wrw(pm + PM_rowBytes, (uint16_t)(0x8000 | rowbytes));
    wr_rect(pm + PM_bounds, b);
    wrw(pm + PM_pixelSize, 8);
    wrl(pm + PM_pmTable, copy_ctab(ctab ? ctab : qd_device_ctab()));
    qd_set_fg_rgb(port, 0, 0, 0);
    qd_set_bk_rgb(port, 0xFFFF, 0xFFFF, 0xFFFF);
    wr_rect(port + P_portRect, b);
    qd_set_rgn(rdl(port + P_visRgn), b);
    /* white background like a fresh GWorld */
    memset(HP(pixels), 0, rowbytes * (uint32_t)h);
    return port;
}

static void dispose_gworld(uint32_t gw)
{
    if (!gw) return;
    uint32_t pmh = rdl(gw + P_portBits);
    if (pmh && mm_is_handle(pmh) && rdl(pmh)) {
        uint32_t pm = rdl(pmh);
        mm_dispose_ptr(rdl(pm + PM_baseAddr));
        uint32_t ct = rdl(pm + PM_pmTable);
        if (ct && mm_is_handle(ct)) mm_dispose_handle(ct);
        mm_dispose_handle(pmh);
    }
    if (rdl(gw + P_visRgn)) mm_dispose_handle(rdl(gw + P_visRgn));
    if (rdl(gw + P_clipRgn)) mm_dispose_handle(rdl(gw + P_clipRgn));
    if (qd_the_port() == gw) qd_set_port(0);
    mm_dispose_ptr(gw);
}

static void T_QDExtensions(uint16_t op)
{
    (void)op;
    uint32_t sel = D[0];
    switch (sel) {
    case 0x00160000: { /* NewGWorld */
        uint32_t flags = ARG32(), gd = ARG32(), ct = ARG32(), br = ARG32(); int16_t depth = ARGS16(); uint32_t out = ARG32();
        (void)flags; (void)gd;
        uint32_t gw = new_gworld(depth, rd_rect(br), ct);
        wrl(out, gw);
        RET16(gw ? 0 : memFullErr);
        return;
    }
    case 0x00040001: (void)ARG32(); RET8(1); return;          /* LockPixels */
    case 0x00040002: (void)ARG32(); return;                    /* UnlockPixels */
    case 0x00040004: dispose_gworld(ARG32()); return;          /* DisposeGWorld */
    case 0x00040007: bump_seed(ARG32()); return;               /* CTabChanged */
    case 0x00040008: (void)ARG32(); return;                    /* PixPatChanged */
    case 0x00040009: (void)ARG32(); return;                    /* PortChanged */
    case 0x0004000A: (void)ARG32(); return;                    /* GDeviceChanged */
    case 0x0004000B: (void)ARG32(); return;                    /* AllowPurgePixels */
    case 0x0004000C: (void)ARG32(); return;                    /* NoPurgePixels */
    case 0x0004000D: (void)ARG32(); RET32(0); return;          /* GetPixelsState */
    case 0x0004000E: (void)ARG32(); (void)ARG32(); return;     /* SetPixelsState */
    case 0x0004000F: {                                         /* GetPixBaseAddr */
        uint32_t pm = ARG32();
        RET32(pm && rdl(pm) ? rdl(rdl(pm) + PM_baseAddr) : 0);
        return;
    }
    case 0x00040017: { uint32_t gw = ARG32(); RET32(gw ? rdl(gw + P_portBits) : 0); return; } /* GetGWorldPixMap */
    case 0x00080005: {                                         /* GetGWorld(port*, gdh*) */
        uint32_t gdp = ARG32(), pp = ARG32();
        wrl(pp, qd_the_port()); wrl(gdp, g_cur_device);
        return;
    }
    case 0x00080006: {                                         /* SetGWorld(port, gdh) */
        uint32_t gd = ARG32(), port = ARG32();
        qd_set_port(port);
        if (gd) g_cur_device = gd;
        return;
    }
    case 0x00040016: { uint32_t gw = ARG32(); (void)gw; RET32(g_main_device); return; } /* GetGWorldDevice */
    case 0x00040020: (void)ARG32(); RET8(0); return;           /* PixMap32Bit */
    default:
        log_fatal("QDExtensions selector %08X not implemented", sel);
    }
}

void qdcolor_register_traps(void)
{
    trap_register(0xAA2A, T_GetMainDevice, "GetMainDevice");
    trap_register(0xAA29, T_GetDeviceList, "GetDeviceList");
    trap_register(0xAA2B, T_GetNextDevice, "GetNextDevice");
    trap_register(0xAA32, T_GetGDevice, "GetGDevice");
    trap_register(0xAA31, T_SetGDevice, "SetGDevice");
    trap_register(0xAA27, T_GetMaxDevice, "GetMaxDevice");
    trap_register(0xAA2C, T_TestDeviceAttribute, "TestDeviceAttribute");
    trap_register(0xAA2D, T_SetDeviceAttribute, "SetDeviceAttribute");
    trap_register(0xAA2E, T_InitGDevice, "InitGDevice");
    trap_register(0xAA18, T_GetCTable, "GetCTable");
    trap_register(0xAA24, T_DisposeCTable, "DisposeCTable");
    trap_register(0xAA39, T_MakeITable, "MakeITable");
    trap_register(0xAA33, T_Color2Index, "Color2Index");
    trap_register(0xAA34, T_Index2Color, "Index2Color");
    trap_register(0xAA35, T_InvertColor, "InvertColor");
    trap_register(0xAA36, T_RealColor, "RealColor");
    trap_register(0xAA3F, T_SetEntries, "SetEntries");
    trap_register(0xAA49, T_SaveEntries, "SaveEntries");
    trap_register(0xAA4A, T_RestoreEntries, "RestoreEntries");
    trap_register(0xAA90, T_InitPalettes, "InitPalettes");
    trap_register(0xAA91, T_NewPalette, "NewPalette");
    trap_register(0xAA92, T_GetNewPalette, "GetNewPalette");
    trap_register(0xAA93, T_DisposePalette, "DisposePalette");
    trap_register(0xAA94, T_ActivatePalette, "ActivatePalette");
    trap_register(0xAA95, T_SetPalette, "SetPalette");
    trap_register(0xAA96, T_GetPalette, "GetPalette");
    trap_register(0xAAA2, T_PaletteDispatch, "PaletteDispatch");
    trap_register(0xAB1D, T_QDExtensions, "QDExtensions");
    (void)T_CTabChanged_tb;
}
