/* File Manager / Device Manager over the host filesystem.
 *
 * One virtual volume (vRefNum -1) whose root directory (dirID 2) is the game
 * folder. Each Mac file "X" is stored as host files "X" (data fork),
 * "X.rsrc" (resource fork) and "X.finf" (8 bytes: type + creator).
 */
#include "toolbox.h"
#include "file.h"
#include "res.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#define mkdir_host(p) _mkdir(p)
#else
#include <unistd.h>
#define mkdir_host(p) mkdir(p, 0755)
#endif

char g_game_dir[1024];

typedef struct { bool used; FILE *fp; char path[1024]; bool rsrc; } OpenFile;
#define FILE_REF_BASE 100
#define MAX_OPEN 32
static OpenFile open_files[MAX_OPEN];

bool fs_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

bool fs_resolve(int16_t vref, int32_t dir, const char *name, char *out, size_t outsz, bool rsrc)
{
    (void)vref; (void)dir;
    /* partial pathnames: leading ':' = relative, components separated by ':' */
    char rel[512]; size_t n = 0;
    const char *p = name;
    if (*p == ':') p++;
    /* full paths "Volume:folder:file" - drop the volume name */
    else if (strchr(p, ':')) p = strchr(p, ':') + 1;
    for (; *p && n < sizeof rel - 2; p++) {
        char c = *p;
        if (c == ':') c = '/';
        else if (c == '/' || c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
        else if ((unsigned char)c < 32) continue;
        rel[n++] = c;
    }
    rel[n] = 0;
    if (!n) { snprintf(out, outsz, "%s", g_game_dir); return false; }
    snprintf(out, outsz, "%s/%s%s", g_game_dir, rel, rsrc ? ".rsrc" : "");
    return true;
}

static void finf_path(const char *datapath, char *out, size_t outsz) { snprintf(out, outsz, "%s.finf", datapath); }

static bool file_exists_mac(const char *datapath)
{
    char r[1100]; snprintf(r, sizeof r, "%s.rsrc", datapath);
    char f[1100]; finf_path(datapath, f, sizeof f);
    return fs_exists(datapath) || fs_exists(r) || fs_exists(f);
}

static int16_t create_mac(const char *datapath, uint32_t type, uint32_t creator)
{
    if (file_exists_mac(datapath)) return dupFNErr;
    FILE *fp = fopen(datapath, "wb");
    if (!fp) return ioErr;
    fclose(fp);
    char f[1100]; finf_path(datapath, f, sizeof f);
    fp = fopen(f, "wb");
    if (fp) {
        uint8_t b[8] = { type >> 24, type >> 16, type >> 8, (uint8_t)type, creator >> 24, creator >> 16, creator >> 8, (uint8_t)creator };
        fwrite(b, 1, 8, fp); fclose(fp);
    }
    return noErr;
}

static int16_t delete_mac(const char *datapath)
{
    if (!file_exists_mac(datapath)) return fnfErr;
    char r[1100]; snprintf(r, sizeof r, "%s.rsrc", datapath);
    char f[1100]; finf_path(datapath, f, sizeof f);
    remove(datapath); remove(r); remove(f);
    return noErr;
}

static void read_finfo(const char *datapath, uint32_t finfo)
{
    memset(HP(finfo), 0, 16);
    char f[1100]; finf_path(datapath, f, sizeof f);
    FILE *fp = fopen(f, "rb");
    if (fp) { size_t r = fread(HP(finfo), 1, 8, fp); (void)r; fclose(fp); }
}

static void write_finfo(const char *datapath, uint32_t finfo)
{
    char f[1100]; finf_path(datapath, f, sizeof f);
    FILE *fp = fopen(f, "wb");
    if (fp) { fwrite(HP(finfo), 1, 8, fp); fclose(fp); }
}

static long host_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : 0;
}

static int16_t open_fork(const char *path, bool rsrc, bool write, int16_t *ref)
{
    int slot = -1;
    for (int i = 0; i < MAX_OPEN; i++) if (!open_files[i].used) { slot = i; break; }
    if (slot < 0) return -42; /* tmfoErr */
    FILE *fp = fopen(path, "r+b");
    if (!fp && !write) fp = fopen(path, "rb");
    if (!fp) {
        char dp[1100]; snprintf(dp, sizeof dp, "%s", path);
        if (rsrc) { size_t l = strlen(dp); if (l > 5) dp[l - 5] = 0; }
        if (!file_exists_mac(dp)) return fnfErr;
        fp = fopen(path, "w+b");
        if (!fp) return ioErr;
    }
    open_files[slot].used = true;
    open_files[slot].fp = fp;
    open_files[slot].rsrc = rsrc;
    snprintf(open_files[slot].path, sizeof open_files[slot].path, "%s", path);
    *ref = (int16_t)(FILE_REF_BASE + slot);
    return noErr;
}

static OpenFile *of(int16_t ref)
{
    int i = ref - FILE_REF_BASE;
    if (i < 0 || i >= MAX_OPEN || !open_files[i].used) return NULL;
    return &open_files[i];
}

/* ------------------------------------------------------------ PB helpers */
static void pb_result(uint32_t pb, int16_t err)
{
    wrw(pb + 16, (uint16_t)err);
    os_result(err);
    uint32_t compl = rdl(pb + 12);
    (void)compl; /* completion routines for file I/O are not used by the game */
}

static void pb_name(uint32_t pb, char *out, size_t sz)
{
    uint32_t np = rdl(pb + 18);
    if (np) pstr_to_c(np, out, sz); else out[0] = 0;
}

/* _Open / _HOpen / _OpenRF / _HOpenRF */
static void do_open(uint16_t op, bool rsrc, bool hfs)
{
    uint32_t pb = A[0];
    char name[256]; pb_name(pb, name, sizeof name);
    if (name[0] == '.') {
        /* driver */
        int16_t ref = 0;
        if (!strcmp(name, ".MPP") || !strcmp(name, ".ATP") || !strcmp(name, ".XPP") || !strcmp(name, ".DSP")) ref = -10;
        else if (!strcmp(name, ".Sound")) ref = -4;
        else { pb_result(pb, -43); return; }
        wrw(pb + 24, (uint16_t)ref);
        pb_result(pb, 0);
        return;
    }
    (void)op;
    char path[1024];
    int32_t dir = hfs ? (int32_t)rdl(pb + 48) : 0;
    fs_resolve((int16_t)rdw(pb + 22), dir, name, path, sizeof path, rsrc);
    int16_t ref = 0;
    int16_t e = open_fork(path, rsrc, rdb(pb + 27) >= 2 || rdb(pb + 27) == 0, &ref);
    if (!e) wrw(pb + 24, (uint16_t)ref);
    pb_result(pb, e);
}
static void T_Open(uint16_t op) { do_open(op, false, (op & 0x0200) != 0); }
static void T_OpenRF(uint16_t op) { do_open(op, true, (op & 0x0200) != 0); }

static void T_Close(uint16_t op)
{
    (void)op;
    uint32_t pb = A[0];
    int16_t ref = (int16_t)rdw(pb + 24);
    if (ref < 0) { pb_result(pb, 0); return; }
    OpenFile *f = of(ref);
    if (!f) { pb_result(pb, -51); return; }
    fclose(f->fp);
    f->used = false;
    pb_result(pb, 0);
}

static void seek_pb(OpenFile *f, uint32_t pb)
{
    uint16_t mode = rdw(pb + 44) & 3;
    int32_t off = (int32_t)rdl(pb + 46);
    if (mode == 1) fseek(f->fp, off, SEEK_SET);
    else if (mode == 2) fseek(f->fp, off, SEEK_END);
    else if (mode == 3) fseek(f->fp, off, SEEK_CUR);
}

static void T_Read(uint16_t op)
{
    (void)op;
    uint32_t pb = A[0];
    OpenFile *f = of((int16_t)rdw(pb + 24));
    if (!f) { pb_result(pb, -51); return; }
    seek_pb(f, pb);
    uint32_t want = rdl(pb + 36);
    size_t got = fread(HP(rdl(pb + 32)), 1, want, f->fp);
    wrl(pb + 40, (uint32_t)got);
    wrl(pb + 46, (uint32_t)ftell(f->fp));
    pb_result(pb, got < want ? eofErr : 0);
}

static void T_Write(uint16_t op)
{
    (void)op;
    uint32_t pb = A[0];
    OpenFile *f = of((int16_t)rdw(pb + 24));
    if (!f) { pb_result(pb, -51); return; }
    seek_pb(f, pb);
    uint32_t want = rdl(pb + 36);
    size_t put = fwrite(HP(rdl(pb + 32)), 1, want, f->fp);
    fflush(f->fp);
    wrl(pb + 40, (uint32_t)put);
    wrl(pb + 46, (uint32_t)ftell(f->fp));
    pb_result(pb, put < want ? ioErr : 0);
}

static void T_GetEOF(uint16_t op)
{
    (void)op;
    uint32_t pb = A[0];
    OpenFile *f = of((int16_t)rdw(pb + 24));
    if (!f) { pb_result(pb, -51); return; }
    long cur = ftell(f->fp); fseek(f->fp, 0, SEEK_END);
    wrl(pb + 28, (uint32_t)ftell(f->fp)); fseek(f->fp, cur, SEEK_SET);
    pb_result(pb, 0);
}

static void T_SetEOF(uint16_t op)
{
    (void)op;
    uint32_t pb = A[0];
    OpenFile *f = of((int16_t)rdw(pb + 24));
    if (!f) { pb_result(pb, -51); return; }
    /* portable truncate: rewrite file */
    uint32_t len = rdl(pb + 28);
    fseek(f->fp, 0, SEEK_END);
    long cur = ftell(f->fp);
    if ((long)len > cur) {
        static const uint8_t z[256];
        while (cur < (long)len) { size_t k = (size_t)(len - cur) > sizeof z ? sizeof z : (size_t)(len - cur); fwrite(z, 1, k, f->fp); cur += (long)k; }
    } else if ((long)len < cur) {
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
        fseek(f->fp, 0, SEEK_SET);
        size_t r = fread(buf, 1, len, f->fp); (void)r;
        f->fp = freopen(f->path, "w+b", f->fp);
        if (f->fp) fwrite(buf, 1, len, f->fp);
        free(buf);
    }
    fflush(f->fp);
    pb_result(pb, 0);
}

static void T_GetFPos(uint16_t op)
{
    (void)op;
    uint32_t pb = A[0];
    OpenFile *f = of((int16_t)rdw(pb + 24));
    if (!f) { pb_result(pb, -51); return; }
    wrl(pb + 46, (uint32_t)ftell(f->fp));
    pb_result(pb, 0);
}

static void T_SetFPos(uint16_t op)
{
    (void)op;
    uint32_t pb = A[0];
    OpenFile *f = of((int16_t)rdw(pb + 24));
    if (!f) { pb_result(pb, -51); return; }
    seek_pb(f, pb);
    wrl(pb + 46, (uint32_t)ftell(f->fp));
    pb_result(pb, 0);
}

static void T_Create(uint16_t op)
{
    uint32_t pb = A[0];
    char name[256]; pb_name(pb, name, sizeof name);
    char path[1024];
    fs_resolve((int16_t)rdw(pb + 22), (op & 0x0200) ? (int32_t)rdl(pb + 48) : 0, name, path, sizeof path, false);
    pb_result(pb, create_mac(path, 0x3F3F3F3F, 0x3F3F3F3F));
}

static void T_Delete(uint16_t op)
{
    uint32_t pb = A[0];
    char name[256]; pb_name(pb, name, sizeof name);
    char path[1024];
    fs_resolve((int16_t)rdw(pb + 22), (op & 0x0200) ? (int32_t)rdl(pb + 48) : 0, name, path, sizeof path, false);
    pb_result(pb, delete_mac(path));
}

static void T_GetFileInfo(uint16_t op)
{
    uint32_t pb = A[0];
    char name[256]; pb_name(pb, name, sizeof name);
    char path[1024];
    fs_resolve((int16_t)rdw(pb + 22), (op & 0x0200) ? (int32_t)rdl(pb + 48) : 0, name, path, sizeof path, false);
    if (!file_exists_mac(path)) { pb_result(pb, fnfErr); return; }
    wrb(pb + 30, 0);
    read_finfo(path, pb + 32);
    char rp[1100]; snprintf(rp, sizeof rp, "%s.rsrc", path);
    wrl(pb + 54, (uint32_t)host_size(path));
    wrl(pb + 58, (uint32_t)host_size(path));
    wrl(pb + 64, (uint32_t)host_size(rp));
    wrl(pb + 68, (uint32_t)host_size(rp));
    pb_result(pb, 0);
}

static void T_SetFileInfo(uint16_t op)
{
    uint32_t pb = A[0];
    char name[256]; pb_name(pb, name, sizeof name);
    char path[1024];
    fs_resolve((int16_t)rdw(pb + 22), (op & 0x0200) ? (int32_t)rdl(pb + 48) : 0, name, path, sizeof path, false);
    if (!file_exists_mac(path)) { pb_result(pb, fnfErr); return; }
    write_finfo(path, pb + 32);
    pb_result(pb, 0);
}

static void T_GetVol(uint16_t op)
{
    uint32_t pb = A[0];
    uint32_t np = rdl(pb + 18);
    if (np) c_to_pstr("Bonkheads", np);
    wrw(pb + 22, (uint16_t)FS_VREFNUM);
    if (op & 0x0200) { wrw(pb + 22, (uint16_t)FS_VREFNUM); wrl(pb + 48, FS_ROOT_DIR); }
    pb_result(pb, 0);
}

static void T_ok_pb(uint16_t op) { (void)op; pb_result(A[0], 0); }

static void T_Control(uint16_t op)
{
    uint32_t pb = A[0];
    int16_t ref = (int16_t)rdw(pb + 24);
    if (ref == -10) {
        int16_t e = net_driver_control(pb, (op & 0x0400) != 0);
        os_result(e);
        return;
    }
    pb_result(pb, 0);
}

static void T_Status(uint16_t op) { (void)op; pb_result(A[0], -18 /* statusErr */); }
static void T_KillIO(uint16_t op) { (void)op; pb_result(A[0], 0); }

/* HFSDispatch $A260: selector in D0 */
static void T_HFSDispatch(uint16_t op)
{
    (void)op;
    uint32_t pb = A[0];
    uint16_t sel = (uint16_t)D[0];
    switch (sel) {
    case 0x0009: { /* PBGetCatInfo */
        char name[256]; pb_name(pb, name, sizeof name);
        int16_t idx = (int16_t)rdw(pb + 28);
        if (idx > 0) { pb_result(pb, fnfErr); return; } /* directory enumeration not supported */
        if (idx < 0 || !name[0]) {
            wrb(pb + 30, 0x10); /* directory */
            wrl(pb + 48, FS_ROOT_DIR);
            wrl(pb + 100, 1);
            pb_result(pb, 0); return;
        }
        char path[1024]; fs_resolve(-1, 0, name, path, sizeof path, false);
        if (!file_exists_mac(path)) { pb_result(pb, fnfErr); return; }
        wrb(pb + 30, 0);
        read_finfo(path, pb + 32);
        wrl(pb + 48, 0);
        wrl(pb + 54, (uint32_t)host_size(path));
        wrl(pb + 100, FS_ROOT_DIR);
        pb_result(pb, 0); return;
    }
    case 0x0030: /* PBHGetVolParms */
        pb_result(pb, -50); return;
    case 0x001A: /* OpenWD */
        wrw(pb + 22, (uint16_t)FS_VREFNUM); pb_result(pb, 0); return;
    case 0x001B: /* CloseWD */
    case 0x0015: /* GetWDInfo */
        wrw(pb + 22, (uint16_t)FS_VREFNUM); wrl(pb + 48, FS_ROOT_DIR); pb_result(pb, 0); return;
    case 0x0001: /* OpenWD (alt) */
    default:
        log_warn("HFSDispatch selector %d not supported", sel);
        pb_result(pb, -50); return;
    }
}

/* ---------------------------------------------------- FSSpec (HighLevelFSDispatch $AA52) */
static void spec_path(uint32_t spec, char *out, size_t sz, bool rsrc)
{
    char name[256]; pstr_to_c(spec + 6, name, sizeof name);
    fs_resolve((int16_t)rdw(spec), (int32_t)rdl(spec + 2), name, out, sz, rsrc);
}

static void T_HighLevelFSDispatch(uint16_t op)
{
    (void)op;
    uint16_t sel = (uint16_t)D[0];
    char path[1024];
    switch (sel) {
    case 0x01: { /* FSMakeFSSpec(vRefNum, dirID, fileName, spec): OSErr */
        uint32_t spec = ARG32(), nm = ARG32(); int32_t dir = (int32_t)ARG32(); int16_t vref = ARGS16();
        char name[256]; if (nm) pstr_to_c(nm, name, sizeof name); else name[0] = 0;
        (void)vref; (void)dir;
        /* keep only the leaf name; partial paths are resolved against the root */
        const char *leaf = name; if (*leaf == ':') leaf++;
        wrw(spec, (uint16_t)FS_VREFNUM);
        wrl(spec + 2, FS_ROOT_DIR);
        c_to_pstr(leaf, spec + 6);
        fs_resolve(vref, dir, leaf, path, sizeof path, false);
        RET16(file_exists_mac(path) ? noErr : fnfErr);
        return;
    }
    case 0x02: case 0x03: { /* FSpOpenDF / FSpOpenRF(spec, perm, refNum) */
        uint32_t pref = ARG32(); int8_t perm = (int8_t)ARG8(); uint32_t spec = ARG32();
        spec_path(spec, path, sizeof path, sel == 3);
        int16_t ref = 0;
        int16_t e = open_fork(path, sel == 3, perm != 1, &ref);
        if (!e) wrw(pref, (uint16_t)ref);
        RET16(e);
        return;
    }
    case 0x04: { /* FSpCreate(spec, creator, type, script) */
        (void)ARG16(); uint32_t type = ARG32(), creator = ARG32(), spec = ARG32();
        spec_path(spec, path, sizeof path, false);
        RET16(create_mac(path, type, creator));
        return;
    }
    case 0x06: { /* FSpDelete */
        uint32_t spec = ARG32();
        spec_path(spec, path, sizeof path, false);
        RET16(delete_mac(path));
        return;
    }
    case 0x07: { /* FSpGetFInfo(spec, fndrInfo) */
        uint32_t fi = ARG32(), spec = ARG32();
        spec_path(spec, path, sizeof path, false);
        if (!file_exists_mac(path)) { RET16(fnfErr); return; }
        read_finfo(path, fi);
        RET16(noErr);
        return;
    }
    case 0x08: { /* FSpSetFInfo */
        uint32_t fi = ARG32(), spec = ARG32();
        spec_path(spec, path, sizeof path, false);
        if (!file_exists_mac(path)) { RET16(fnfErr); return; }
        write_finfo(path, fi);
        RET16(noErr);
        return;
    }
    case 0x0D: { /* FSpOpenResFile(spec, perm): refNum */
        int8_t perm = (int8_t)ARG8(); uint32_t spec = ARG32();
        spec_path(spec, path, sizeof path, true);
        RET16(res_open_path(path, perm == 1));
        return;
    }
    case 0x0E: { /* FSpCreateResFile(spec, creator, type, script) */
        (void)ARG16(); uint32_t type = ARG32(), creator = ARG32(), spec = ARG32();
        spec_path(spec, path, sizeof path, false);
        create_mac(path, type, creator);
        char rp[1100]; snprintf(rp, sizeof rp, "%s.rsrc", path);
        res_create_path(rp);
        return;
    }
    case 0x0B: { /* FSpRename(spec, newName) */
        uint32_t nn = ARG32(), spec = ARG32();
        char name[256]; pstr_to_c(nn, name, sizeof name);
        spec_path(spec, path, sizeof path, false);
        char np[1024]; fs_resolve(-1, FS_ROOT_DIR, name, np, sizeof np, false);
        const char *sfx[3] = { "", ".rsrc", ".finf" };
        for (int i = 0; i < 3; i++) {
            char a[1100], b[1100];
            snprintf(a, sizeof a, "%s%s", path, sfx[i]); snprintf(b, sizeof b, "%s%s", np, sfx[i]);
            rename(a, b);
        }
        RET16(noErr);
        return;
    }
    default:
        log_fatal("HighLevelFSDispatch selector %d not implemented", sel);
    }
}

/* Standard File and Alias Manager live in stdfile.c */

void file_register_traps(void)
{
    trap_register(0xA000, T_Open, "Open");
    trap_register(0xA00A, T_OpenRF, "OpenRF");
    trap_register(0xA001, T_Close, "Close");
    trap_register(0xA002, T_Read, "Read");
    trap_register(0xA003, T_Write, "Write");
    trap_register(0xA004, T_Control, "Control");
    trap_register(0xA005, T_Status, "Status");
    trap_register(0xA006, T_KillIO, "KillIO");
    trap_register(0xA011, T_GetEOF, "GetEOF");
    trap_register(0xA012, T_SetEOF, "SetEOF");
    trap_register(0xA018, T_GetFPos, "GetFPos");
    trap_register(0xA044, T_SetFPos, "SetFPos");
    trap_register(0xA008, T_Create, "Create");
    trap_register(0xA009, T_Delete, "Delete");
    trap_register(0xA00C, T_GetFileInfo, "GetFileInfo");
    trap_register(0xA00D, T_SetFileInfo, "SetFileInfo");
    trap_register(0xA014, T_GetVol, "GetVol");
    trap_register(0xA015, T_ok_pb, "SetVol");
    trap_register(0xA013, T_ok_pb, "FlushVol");
    trap_register(0xA045, T_ok_pb, "FlushFile");
    trap_register(0xA007, T_GetVol, "GetVolInfo");
    trap_register(0xA060, T_HFSDispatch, "HFSDispatch");
    trap_register(0xAA52, T_HighLevelFSDispatch, "HighLevelFSDispatch");
}
