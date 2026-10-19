/* Resource Manager over extracted resource forks ("<name>.rsrc" host files). */
#include "toolbox.h"
#include "res.h"
#include "file.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint32_t type;
    int16_t id;
    uint8_t attrs;
    char name[256];
    bool has_name;
    uint8_t *data;   /* host copy (authoritative while not loaded) */
    uint32_t len;
    uint32_t handle; /* emulated handle when loaded */
    bool removed;
} ResEnt;

typedef struct {
    bool open;
    char path[1024];
    int16_t refnum;
    bool dirty;
    bool readonly;
    uint16_t file_attrs;
    ResEnt *ents;
    int n, cap;
    int16_t next;  /* next file down the search chain (0 = end) */
} ResFile;

#define MAX_RES_FILES 32
static ResFile files[MAX_RES_FILES];
static int16_t top_file;       /* most recently opened (head of chain) */
static int16_t cur_file;
static int16_t app_file;
static bool res_load = true;

static void set_res_err(int16_t e) { wrw(LM_ResErr, (uint16_t)e); }
int16_t res_error(void) { return (int16_t)rdw(LM_ResErr); }

static ResFile *rf(int16_t ref)
{
    if (ref <= 0 || ref > MAX_RES_FILES) return NULL;
    ResFile *f = &files[ref - 1];
    return f->open ? f : NULL;
}

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static ResEnt *add_ent(ResFile *f)
{
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 64;
        f->ents = (ResEnt *)realloc(f->ents, sizeof(ResEnt) * f->cap);
    }
    ResEnt *e = &f->ents[f->n++];
    memset(e, 0, sizeof *e);
    return e;
}

static bool parse_fork(ResFile *f, const uint8_t *buf, size_t len)
{
    if (len < 16) return len == 0;
    uint32_t doff = be32(buf), moff = be32(buf + 4);
    if (moff + 30 > len) return false;
    const uint8_t *m = buf + moff;
    f->file_attrs = be16(m + 22);
    uint16_t tl = be16(m + 24), nl = be16(m + 26);
    const uint8_t *t = m + tl;
    int ntypes = (int16_t)be16(t) + 1;
    for (int i = 0; i < ntypes; i++) {
        const uint8_t *te = t + 2 + i * 8;
        uint32_t type = be32(te);
        int n = be16(te + 4) + 1;
        uint16_t ref = be16(te + 6);
        for (int j = 0; j < n; j++) {
            const uint8_t *re = t + ref + j * 12;
            ResEnt *e = add_ent(f);
            e->type = type;
            e->id = (int16_t)be16(re);
            uint16_t noff = be16(re + 2);
            e->attrs = re[4];
            uint32_t off = ((uint32_t)re[5] << 16) | ((uint32_t)re[6] << 8) | re[7];
            if (noff != 0xFFFF) {
                const uint8_t *np = m + nl + noff;
                memcpy(e->name, np + 1, np[0]);
                e->name[np[0]] = 0;
                e->has_name = true;
            }
            const uint8_t *dp = buf + doff + off;
            e->len = be32(dp);
            e->data = (uint8_t *)malloc(e->len ? e->len : 1);
            memcpy(e->data, dp + 4, e->len);
        }
    }
    return true;
}

static uint8_t *ent_bytes(ResEnt *e, uint32_t *len)
{
    if (e->handle && mm_is_handle(e->handle) && rdl(e->handle)) {
        *len = mm_handle_size(e->handle);
        return HP(rdl(e->handle));
    }
    *len = e->len;
    return e->data;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }

static bool write_fork(ResFile *f)
{
    /* collect types */
    uint32_t types[256]; int ntypes = 0;
    size_t datalen = 0, namelen = 0;
    int live = 0;
    for (int i = 0; i < f->n; i++) {
        ResEnt *e = &f->ents[i];
        if (e->removed) continue;
        live++;
        int k;
        for (k = 0; k < ntypes; k++) if (types[k] == e->type) break;
        if (k == ntypes && ntypes < 256) types[ntypes++] = e->type;
        uint32_t l; ent_bytes(e, &l);
        datalen += 4 + l;
        if (e->has_name) namelen += 1 + strlen(e->name);
    }
    size_t tl_size = 2 + ntypes * 8 + live * 12;
    size_t map_size = 28 + tl_size + namelen;
    size_t total = 256 + datalen + map_size;
    uint8_t *buf = (uint8_t *)calloc(1, total);
    uint8_t *map = buf + 256 + datalen;
    put32(buf, 256); put32(buf + 4, (uint32_t)(256 + datalen)); put32(buf + 8, (uint32_t)datalen); put32(buf + 12, (uint32_t)map_size);
    memcpy(map, buf, 16);
    put16(map + 22, f->file_attrs & ~0x00A0);
    put16(map + 24, 28);
    put16(map + 26, (uint16_t)(28 + tl_size));
    uint8_t *tl = map + 28;
    put16(tl, (uint16_t)(ntypes - 1));
    size_t dpos = 0, npos = 0;
    int refpos = 2 + ntypes * 8;
    for (int k = 0; k < ntypes; k++) {
        int cnt = 0;
        uint8_t *te = tl + 2 + k * 8;
        put32(te, types[k]);
        put16(te + 6, (uint16_t)refpos);
        for (int i = 0; i < f->n; i++) {
            ResEnt *e = &f->ents[i];
            if (e->removed || e->type != types[k]) continue;
            uint8_t *re = tl + refpos + cnt * 12;
            put16(re, (uint16_t)e->id);
            if (e->has_name) {
                put16(re + 2, (uint16_t)npos);
                uint8_t *np = map + 28 + tl_size + npos;
                size_t sl = strlen(e->name);
                np[0] = (uint8_t)sl; memcpy(np + 1, e->name, sl);
                npos += 1 + sl;
            } else put16(re + 2, 0xFFFF);
            re[4] = e->attrs & ~0x02; /* clear resChanged */
            re[5] = (uint8_t)(dpos >> 16); re[6] = (uint8_t)(dpos >> 8); re[7] = (uint8_t)dpos;
            uint32_t l; uint8_t *src = ent_bytes(e, &l);
            put32(buf + 256 + dpos, l);
            memcpy(buf + 256 + dpos + 4, src, l);
            dpos += 4 + l;
            cnt++;
        }
        put16(te + 4, (uint16_t)(cnt - 1));
        refpos += cnt * 12;
    }
    FILE *fp = fopen(f->path, "wb");
    if (!fp) { free(buf); return false; }
    fwrite(buf, 1, total, fp);
    fclose(fp);
    free(buf);
    f->dirty = false;
    return true;
}

int16_t res_open_path(const char *path, bool readonly)
{
    for (int i = 0; i < MAX_RES_FILES; i++)
        if (files[i].open && strcmp(files[i].path, path) == 0) { cur_file = files[i].refnum; set_res_err(0); return files[i].refnum; }
    FILE *fp = fopen(path, "rb");
    if (!fp) { set_res_err(fnfErr); return -1; }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
    if (fread(buf, 1, len, fp) != (size_t)len) len = 0;
    fclose(fp);
    int slot = -1;
    for (int i = 0; i < MAX_RES_FILES; i++) if (!files[i].open) { slot = i; break; }
    if (slot < 0) { free(buf); set_res_err(-42); return -1; }
    ResFile *f = &files[slot];
    memset(f, 0, sizeof *f);
    f->open = true;
    snprintf(f->path, sizeof f->path, "%s", path);
    f->refnum = (int16_t)(slot + 1);
    f->readonly = readonly;
    if (!parse_fork(f, buf, len)) {
        free(buf); f->open = false; set_res_err(-199 /* mapReadErr */); return -1;
    }
    free(buf);
    f->next = top_file;
    top_file = f->refnum;
    cur_file = f->refnum;
    wrw(LM_CurResFile, (uint16_t)cur_file);
    set_res_err(0);
    log_trace("OpenResFile %s -> %d (%d resources)", path, f->refnum, f->n);
    return f->refnum;
}

bool res_create_path(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp) { fclose(fp); set_res_err(dupFNErr); return false; }
    ResFile tmp; memset(&tmp, 0, sizeof tmp);
    snprintf(tmp.path, sizeof tmp.path, "%s", path);
    bool ok = write_fork(&tmp);
    set_res_err(ok ? 0 : ioErr);
    return ok;
}

void res_set_app_file(int16_t ref) { app_file = ref; wrw(0x0900, (uint16_t)ref); }
int16_t res_cur_file(void) { return cur_file; }

void res_close(int16_t ref)
{
    ResFile *f = rf(ref);
    if (!f) { set_res_err(-1 /* resFNotFound */); return; }
    if (f->dirty && !f->readonly) write_fork(f);
    for (int i = 0; i < f->n; i++) {
        ResEnt *e = &f->ents[i];
        if (e->handle && !e->removed && mm_is_handle(e->handle)) mm_dispose_handle(e->handle);
        free(e->data);
    }
    free(f->ents);
    /* unlink from chain */
    int16_t *pp = &top_file;
    while (*pp && *pp != ref) pp = &files[*pp - 1].next;
    if (*pp == ref) *pp = f->next;
    f->open = false;
    if (cur_file == ref) cur_file = top_file;
    wrw(LM_CurResFile, (uint16_t)cur_file);
    set_res_err(0);
}

static ResEnt *find_in(ResFile *f, uint32_t type, int16_t id)
{
    for (int i = 0; i < f->n; i++)
        if (!f->ents[i].removed && f->ents[i].type == type && f->ents[i].id == id) return &f->ents[i];
    return NULL;
}

static ResEnt *find_named_in(ResFile *f, uint32_t type, const char *name)
{
    for (int i = 0; i < f->n; i++)
        if (!f->ents[i].removed && f->ents[i].type == type && f->ents[i].has_name && strcmp(f->ents[i].name, name) == 0)
            return &f->ents[i];
    return NULL;
}

static ResEnt *find_handle(uint32_t h, ResFile **owner)
{
    if (!h) return NULL;
    for (int k = 0; k < MAX_RES_FILES; k++) {
        ResFile *f = &files[k];
        if (!f->open) continue;
        for (int i = 0; i < f->n; i++)
            if (f->ents[i].handle == h && !f->ents[i].removed) { if (owner) *owner = f; return &f->ents[i]; }
    }
    return NULL;
}

void port_patch_resource(uint32_t type, int16_t id, uint32_t h);

static uint32_t load_ent(ResEnt *e)
{
    if (e->handle && mm_is_handle(e->handle)) {
        if (!rdl(e->handle) && res_load) {
            mm_set_handle_size(e->handle, e->len);
            memcpy(HP(rdl(e->handle)), e->data, e->len);
        }
        return e->handle;
    }
    if (!res_load) {
        e->handle = mm_new_empty_handle();
    } else {
        e->handle = mm_handle_from_bytes(e->data, e->len);
    }
    if (e->handle) mm_hset_state(e->handle, HF_RESOURCE | ((e->attrs & 0x20) ? HF_PURGE : 0) | ((e->attrs & 0x10) ? HF_LOCKED : 0));
    if (e->handle && rdl(e->handle)) port_patch_resource(e->type, e->id, e->handle);
    return e->handle;
}

/* System-file resources the game asks for that we synthesise */
static uint32_t synth_resource(uint32_t type, int16_t id)
{
    /* -16096: Chooser owner name, -16413: Sharing Setup computer name */
    if (type == OSTYPE('S','T','R',' ') && (id == -16096 || id == -16413)) {
        const char *n = getenv("USERNAME");
        if (!n || !*n) n = getenv("USER");
        if (!n || !*n) n = getenv("COMPUTERNAME");
        if (!n || !*n) n = "Player";
        uint8_t buf[64]; size_t l = strlen(n); if (l > 31) l = 31;
        buf[0] = (uint8_t)l; memcpy(buf + 1, n, l);
        return mm_handle_from_bytes(buf, (uint32_t)l + 1);
    }
    return 0;
}

uint32_t res_get(uint32_t type, int16_t id, bool one)
{
    int16_t r = cur_file;
    while (r) {
        ResFile *f = rf(r);
        if (!f) break;
        ResEnt *e = find_in(f, type, id);
        if (e) { set_res_err(0); return load_ent(e); }
        if (one) break;
        r = f->next;
    }
    uint32_t h = synth_resource(type, id);
    if (h) { set_res_err(0); return h; }
    set_res_err(resNotFound);
    return 0;
}

uint32_t res_get_named(uint32_t type, const char *name, bool one)
{
    int16_t r = cur_file;
    while (r) {
        ResFile *f = rf(r);
        if (!f) break;
        ResEnt *e = find_named_in(f, type, name);
        if (e) { set_res_err(0); return load_ent(e); }
        if (one) break;
        r = f->next;
    }
    set_res_err(resNotFound);
    return 0;
}

/* direct access for HLE modules: copy of resource bytes without allocating a handle */
const uint8_t *res_peek(uint32_t type, int16_t id, uint32_t *len)
{
    int16_t r = cur_file;
    while (r) {
        ResFile *f = rf(r);
        if (!f) break;
        ResEnt *e = find_in(f, type, id);
        if (e) return ent_bytes(e, len);
        r = f->next;
    }
    /* fall back to every open file (e.g. app resources after UseResFile) */
    for (int k = 0; k < MAX_RES_FILES; k++) {
        if (!files[k].open) continue;
        ResEnt *e = find_in(&files[k], type, id);
        if (e) return ent_bytes(e, len);
    }
    return NULL;
}

static uint32_t ostype_arg(void) { return ARG32(); }

/* ============================ traps ============================ */
static void T_GetResource(uint16_t op) { (void)op; int16_t id = ARGS16(); uint32_t t = ostype_arg(); RET32(res_get(t, id, false)); }
static void T_Get1Resource(uint16_t op) { (void)op; int16_t id = ARGS16(); uint32_t t = ostype_arg(); RET32(res_get(t, id, true)); }
static void T_GetNamedResource(uint16_t op)
{
    (void)op;
    uint32_t nm = ARG32(); uint32_t t = ostype_arg();
    char name[256]; pstr_to_c(nm, name, sizeof name);
    RET32(res_get_named(t, name, false));
}
static void T_Get1NamedResource(uint16_t op)
{
    (void)op;
    uint32_t nm = ARG32(); uint32_t t = ostype_arg();
    char name[256]; pstr_to_c(nm, name, sizeof name);
    RET32(res_get_named(t, name, true));
}
static void T_ReleaseResource(uint16_t op)
{
    (void)op;
    uint32_t h = ARG32();
    ResFile *f; ResEnt *e = find_handle(h, &f);
    if (!e) { set_res_err(-192); return; }
    if (e->attrs & 0x02) { /* changed: keep data */
        uint32_t l; uint8_t *b = ent_bytes(e, &l);
        uint8_t *copy = (uint8_t *)malloc(l ? l : 1); memcpy(copy, b, l);
        free(e->data); e->data = copy; e->len = l;
    }
    mm_dispose_handle(h);
    e->handle = 0;
    set_res_err(0);
}
static void T_DetachResource(uint16_t op)
{
    (void)op;
    uint32_t h = ARG32();
    ResFile *f; ResEnt *e = find_handle(h, &f);
    if (!e) { set_res_err(-192); return; }
    uint32_t l; uint8_t *b = ent_bytes(e, &l);
    uint8_t *copy = (uint8_t *)malloc(l ? l : 1); memcpy(copy, b, l);
    free(e->data); e->data = copy; e->len = l;
    e->handle = 0;
    mm_hset_state(h, mm_hget_state(h) & ~HF_RESOURCE);
    set_res_err(0);
}
static void T_LoadResource(uint16_t op)
{
    (void)op;
    uint32_t h = ARG32();
    ResEnt *e = find_handle(h, NULL);
    if (!e) { set_res_err(-192); return; }
    bool saved = res_load; res_load = true; load_ent(e); res_load = saved;
    set_res_err(0);
}
static void T_SizeRsrc(uint16_t op)
{
    (void)op;
    uint32_t h = ARG32();
    ResEnt *e = find_handle(h, NULL);
    if (!e) { set_res_err(-192); RET32(0xFFFFFFFF); return; }
    uint32_t l; ent_bytes(e, &l);
    set_res_err(0);
    RET32(l);
}
static void T_HomeResFile(uint16_t op)
{
    (void)op;
    uint32_t h = ARG32();
    ResFile *f; ResEnt *e = find_handle(h, &f);
    RET16(e ? f->refnum : -1);
    set_res_err(e ? 0 : -192);
}
static void T_CurResFile(uint16_t op) { (void)op; RET16(cur_file); }
static void T_UseResFile(uint16_t op)
{
    (void)op;
    int16_t r = ARGS16();
    if (r == 0) r = app_file;  /* System file maps to the application here */
    if (!rf(r)) { set_res_err(-193); return; }
    cur_file = r;
    wrw(LM_CurResFile, (uint16_t)r);
    set_res_err(0);
}
static void T_SetResLoad(uint16_t op) { (void)op; res_load = ARG8() != 0; wrb(LM_ResLoad, res_load ? 0xFF : 0); }
static void T_ResError(uint16_t op) { (void)op; RET16(rdw(LM_ResErr)); }
static void T_CloseResFile(uint16_t op) { (void)op; int16_t r = ARGS16(); if (r != app_file) res_close(r); }
static void T_UpdateResFile(uint16_t op)
{
    (void)op;
    int16_t r = ARGS16();
    ResFile *f = rf(r);
    if (!f) { set_res_err(-193); return; }
    if (f->dirty && !f->readonly) write_fork(f);
    set_res_err(0);
}
static void T_OpenResFile(uint16_t op)
{
    (void)op;
    uint32_t nm = ARG32();
    char name[256]; pstr_to_c(nm, name, sizeof name);
    char path[1024];
    if (!fs_resolve(0, 0, name, path, sizeof path, true)) { set_res_err(fnfErr); RET16(-1); return; }
    RET16(res_open_path(path, false));
}
static void T_CreateResFile(uint16_t op)
{
    (void)op;
    uint32_t nm = ARG32();
    char name[256]; pstr_to_c(nm, name, sizeof name);
    char path[1024];
    fs_resolve(0, 0, name, path, sizeof path, true);
    res_create_path(path);
}
static void T_ChangedResource(uint16_t op)
{
    (void)op;
    uint32_t h = ARG32();
    ResFile *f; ResEnt *e = find_handle(h, &f);
    if (!e) { set_res_err(-192); return; }
    e->attrs |= 0x02;
    f->dirty = true;
    set_res_err(0);
}
static void T_WriteResource(uint16_t op)
{
    (void)op;
    uint32_t h = ARG32();
    ResFile *f; ResEnt *e = find_handle(h, &f);
    if (!e) { set_res_err(-192); return; }
    if (f->dirty && !f->readonly) write_fork(f);
    set_res_err(0);
}
static void T_AddResource(uint16_t op)
{
    (void)op;
    uint32_t nm = ARG32(); int16_t id = ARGS16(); uint32_t type = ARG32(); uint32_t h = ARG32();
    ResFile *f = rf(cur_file);
    if (!f || !h) { set_res_err(-194 /* addResFailed */); return; }
    /* as on the Mac: a handle that already is a resource cannot be added again */
    if (find_handle(h, NULL)) { set_res_err(-194 /* addResFailed */); return; }
    ResEnt *old = find_in(f, type, id);
    if (old) { old->removed = true; old->handle = 0; }
    ResEnt *e = add_ent(f);
    e->type = type; e->id = id; e->attrs = 0x02; e->handle = h;
    if (nm && rdb(nm)) { pstr_to_c(nm, e->name, sizeof e->name); e->has_name = true; }
    e->len = 0; e->data = (uint8_t *)malloc(1);
    port_patch_resource(type, id, h);
    mm_hset_state(h, mm_hget_state(h) | HF_RESOURCE);
    f->dirty = true;
    set_res_err(0);
}
static void T_RmveResource(uint16_t op)
{
    (void)op;
    uint32_t h = ARG32();
    ResFile *f; ResEnt *e = find_handle(h, &f);
    if (!e) { set_res_err(-192); return; }
    e->removed = true;
    e->handle = 0;
    mm_hset_state(h, mm_hget_state(h) & ~HF_RESOURCE);
    f->dirty = true;
    set_res_err(0);
}
static int count_res(uint32_t type, bool one, int index, ResEnt **out)
{
    int cnt = 0;
    int16_t r = cur_file;
    while (r) {
        ResFile *f = rf(r);
        if (!f) break;
        for (int i = 0; i < f->n; i++) {
            if (f->ents[i].removed || f->ents[i].type != type) continue;
            cnt++;
            if (cnt == index && out) { *out = &f->ents[i]; return cnt; }
        }
        if (one) break;
        r = f->next;
    }
    return cnt;
}
static void T_CountResources(uint16_t op) { (void)op; uint32_t t = ARG32(); RET16(count_res(t, false, -1, NULL)); set_res_err(0); }
static void T_Count1Resources(uint16_t op) { (void)op; uint32_t t = ARG32(); RET16(count_res(t, true, -1, NULL)); set_res_err(0); }
static void T_GetIndResource(uint16_t op)
{
    (void)op;
    int16_t idx = ARGS16(); uint32_t t = ARG32();
    ResEnt *e = NULL; count_res(t, false, idx, &e);
    RET32(e ? load_ent(e) : 0);
    set_res_err(e ? 0 : resNotFound);
}
static void T_Get1IxResource(uint16_t op)
{
    (void)op;
    int16_t idx = ARGS16(); uint32_t t = ARG32();
    ResEnt *e = NULL; count_res(t, true, idx, &e);
    RET32(e ? load_ent(e) : 0);
    set_res_err(e ? 0 : resNotFound);
}
static int list_types(bool one, uint32_t *out, int max)
{
    int n = 0;
    int16_t r = cur_file;
    while (r) {
        ResFile *f = rf(r);
        if (!f) break;
        for (int i = 0; i < f->n; i++) {
            if (f->ents[i].removed) continue;
            int k; for (k = 0; k < n; k++) if (out[k] == f->ents[i].type) break;
            if (k == n && n < max) out[n++] = f->ents[i].type;
        }
        if (one) break;
        r = f->next;
    }
    return n;
}
static void T_CountTypes(uint16_t op) { (void)op; uint32_t t[512]; RET16(list_types(false, t, 512)); }
static void T_Count1Types(uint16_t op) { (void)op; uint32_t t[512]; RET16(list_types(true, t, 512)); }
static void T_GetIndType(uint16_t op)
{
    (void)op;
    int16_t idx = ARGS16(); uint32_t p = ARG32();
    uint32_t t[512]; int n = list_types(false, t, 512);
    wrl(p, (idx >= 1 && idx <= n) ? t[idx - 1] : 0);
}
static void T_Get1IxType(uint16_t op)
{
    (void)op;
    int16_t idx = ARGS16(); uint32_t p = ARG32();
    uint32_t t[512]; int n = list_types(true, t, 512);
    wrl(p, (idx >= 1 && idx <= n) ? t[idx - 1] : 0);
}
static void T_GetResInfo(uint16_t op)
{
    (void)op;
    uint32_t pname = ARG32(), ptype = ARG32(), pid = ARG32(), h = ARG32();
    ResEnt *e = find_handle(h, NULL);
    if (!e) { set_res_err(-192); return; }
    if (pid) wrw(pid, (uint16_t)e->id);
    if (ptype) wrl(ptype, e->type);
    if (pname) { if (e->has_name) c_to_pstr(e->name, pname); else wrb(pname, 0); }
    set_res_err(0);
}
static void T_SetResInfo(uint16_t op)
{
    (void)op;
    uint32_t pname = ARG32(); int16_t id = ARGS16(); uint32_t h = ARG32();
    ResFile *f; ResEnt *e = find_handle(h, &f);
    if (!e) { set_res_err(-192); return; }
    e->id = id;
    if (pname) { pstr_to_c(pname, e->name, sizeof e->name); e->has_name = true; }
    f->dirty = true;
    set_res_err(0);
}
static void T_GetResAttrs(uint16_t op)
{
    (void)op;
    uint32_t h = ARG32();
    ResEnt *e = find_handle(h, NULL);
    RET16(e ? e->attrs : 0);
    set_res_err(e ? 0 : -192);
}
static void T_SetResAttrs(uint16_t op)
{
    (void)op;
    uint16_t a = ARG16(); uint32_t h = ARG32();
    ResEnt *e = find_handle(h, NULL);
    if (e) e->attrs = (uint8_t)a;
    set_res_err(e ? 0 : -192);
}
static void T_Unique1ID(uint16_t op)
{
    (void)op;
    uint32_t t = ARG32();
    ResFile *f = rf(cur_file);
    int16_t id = 128;
    while (f && find_in(f, t, id)) id++;
    RET16(id);
}
static void T_GetResFileAttrs(uint16_t op) { (void)op; int16_t r = ARGS16(); ResFile *f = rf(r); RET16(f ? f->file_attrs : 0); }
static void T_SetResFileAttrs(uint16_t op) { (void)op; uint16_t a = ARG16(); int16_t r = ARGS16(); ResFile *f = rf(r); if (f) f->file_attrs = a; }
static void T_SetResPurge(uint16_t op) { (void)op; (void)ARG8(); }
static void T_OpenRFPerm(uint16_t op)
{
    (void)op;
    int8_t perm = (int8_t)ARG8(); int16_t vref = ARGS16(); uint32_t nm = ARG32();
    char name[256]; pstr_to_c(nm, name, sizeof name);
    char path[1024];
    if (!fs_resolve(vref, 0, name, path, sizeof path, true)) { set_res_err(fnfErr); RET16(-1); return; }
    RET16(res_open_path(path, perm == 1));
}
static void T_HOpenResFile(uint16_t op)
{
    (void)op;
    int8_t perm = (int8_t)ARG8(); uint32_t nm = ARG32(); int32_t dir = (int32_t)ARG32(); int16_t vref = ARGS16();
    char name[256]; pstr_to_c(nm, name, sizeof name);
    char path[1024];
    if (!fs_resolve(vref, dir, name, path, sizeof path, true)) { set_res_err(fnfErr); RET16(-1); return; }
    RET16(res_open_path(path, perm == 1));
}
static void T_HCreateResFile(uint16_t op)
{
    (void)op;
    uint32_t nm = ARG32(); int32_t dir = (int32_t)ARG32(); int16_t vref = ARGS16();
    char name[256]; pstr_to_c(nm, name, sizeof name);
    char path[1024];
    fs_resolve(vref, dir, name, path, sizeof path, true);
    res_create_path(path);
}
static void T_RGetResource(uint16_t op) { T_GetResource(op); }
static void T_MaxSizeRsrc(uint16_t op) { T_SizeRsrc(op); }

void res_register_traps(void)
{
    trap_register(0xA9A0, T_GetResource, "GetResource");
    trap_register(0xA81F, T_Get1Resource, "Get1Resource");
    trap_register(0xA9A1, T_GetNamedResource, "GetNamedResource");
    trap_register(0xA820, T_Get1NamedResource, "Get1NamedResource");
    trap_register(0xA9A3, T_ReleaseResource, "ReleaseResource");
    trap_register(0xA992, T_DetachResource, "DetachResource");
    trap_register(0xA9A2, T_LoadResource, "LoadResource");
    trap_register(0xA9A5, T_SizeRsrc, "SizeRsrc");
    trap_register(0xA821, T_MaxSizeRsrc, "MaxSizeRsrc");
    trap_register(0xA9A4, T_HomeResFile, "HomeResFile");
    trap_register(0xA994, T_CurResFile, "CurResFile");
    trap_register(0xA998, T_UseResFile, "UseResFile");
    trap_register(0xA99B, T_SetResLoad, "SetResLoad");
    trap_register(0xA9AF, T_ResError, "ResError");
    trap_register(0xA99A, T_CloseResFile, "CloseResFile");
    trap_register(0xA999, T_UpdateResFile, "UpdateResFile");
    trap_register(0xA997, T_OpenResFile, "OpenResFile");
    trap_register(0xA9B1, T_CreateResFile, "CreateResFile");
    trap_register(0xA9AA, T_ChangedResource, "ChangedResource");
    trap_register(0xA9B0, T_WriteResource, "WriteResource");
    trap_register(0xA9AB, T_AddResource, "AddResource");
    trap_register(0xA9AD, T_RmveResource, "RemoveResource");
    trap_register(0xA99C, T_CountResources, "CountResources");
    trap_register(0xA80D, T_Count1Resources, "Count1Resources");
    trap_register(0xA99D, T_GetIndResource, "GetIndResource");
    trap_register(0xA80E, T_Get1IxResource, "Get1IxResource");
    trap_register(0xA99E, T_CountTypes, "CountTypes");
    trap_register(0xA81C, T_Count1Types, "Count1Types");
    trap_register(0xA99F, T_GetIndType, "GetIndType");
    trap_register(0xA80F, T_Get1IxType, "Get1IxType");
    trap_register(0xA9A8, T_GetResInfo, "GetResInfo");
    trap_register(0xA9A9, T_SetResInfo, "SetResInfo");
    trap_register(0xA9A6, T_GetResAttrs, "GetResAttrs");
    trap_register(0xA9A7, T_SetResAttrs, "SetResAttrs");
    trap_register(0xA810, T_Unique1ID, "Unique1ID");
    trap_register(0xA9C1, T_Unique1ID, "UniqueID");
    trap_register(0xA9F6, T_GetResFileAttrs, "GetResFileAttrs");
    trap_register(0xA9F7, T_SetResFileAttrs, "SetResFileAttrs");
    trap_register(0xA993, T_SetResPurge, "SetResPurge");
    trap_register(0xA9C4, T_OpenRFPerm, "OpenRFPerm");
    trap_register(0xA81A, T_HOpenResFile, "HOpenResFile");
    trap_register(0xA81B, T_HCreateResFile, "HCreateResFile");
    trap_register(0xA80C, T_RGetResource, "RGetResource");
}

int16_t res_find_named_id(uint32_t type, const char *name)
{
    for (int k = 0; k < MAX_RES_FILES; k++) {
        if (!files[k].open) continue;
        ResEnt *e = find_named_in(&files[k], type, name);
        if (e) return e->id;
    }
    return 0;
}
