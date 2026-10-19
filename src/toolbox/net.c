/* AppleTalk (.MPP driver) emulation over UDP. See docs/network.md.
 *
 * The game's own DDP/NBP protocol code runs unchanged; this module carries
 * its datagrams over UDP and implements name registration/lookup with LAN
 * broadcast probes plus explicit --connect addresses. */
#include "toolbox.h"
#include "file.h"
#include "platform/udp.h"
#include "platform/platform.h"
#include "netcfg.h"
#include "overlay.h"
#include <stdio.h>
#include <stdlib.h>

/* ---- game-specific addresses (A5-relative), from the disassembly ---- */
#define G_RX_HDR        (-11376)  /* 17-byte DDP header copy written by the listener */
#define G_RX_PAYLOAD    (-11359)  /* payload buffer, max 587 bytes */
#define G_RX_MAX        587
#define G_RX_CURSLOT    (-32541)  /* byte: which sprite double-buffer is current */
#define G_RX_SLOTS      (-32322)  /* two 0x248-byte receive slots */
#define G_RX_SLOT_SIZE  0x248
#define G_RX_CONSUMED   582       /* offset of the "consumed" word inside a slot */
#define G_MY_NODE_BYTE  (-9623)
#define G_REG_CURSLOT   (-32543)  /* byte: which registered-event slot is current */
#define G_REG_SLOTS     22008     /* two 0x248-byte registered-event slots */
#define G_REG_MAX       12        /* entries the game accepts per packet */

#define MY_SOCKET 0x80
#define PROTO_MAGIC "BONK"
#define PROTO_VER 1
enum { K_DDP = 0, K_PROBE = 1, K_ADVERT = 2 };

NetConfig g_netcfg = { .port = 24731 };
int g_netlog;
#define NETLOG(...) do { if (g_netlog) log_info(__VA_ARGS__); } while (0)

typedef struct { bool used; uint8_t node; UdpAddr addr; char name[64]; } Peer;
static Peer peers[16];

static int sock = -1;
static uint8_t my_node;
static uint32_t abus_vars = 0x3000;   /* fake ABusVars record in low memory */

static bool advertising;
static char adv_object[64], adv_type[64];

static uint32_t lookup_pb;            /* outstanding PLookupName param block */
static uint32_t lookup_started;
static bool joined;                   /* joiner: a host has contacted us */
static char host_addr[64];            /* address typed in the Net Host prompt */
static UdpAddr host_target;
static bool host_target_ok;

bool ui_prompt(const char *title, const char *help1, const char *help2, char *buf, size_t sz);
static uint32_t lookup_last_probe;

static uint32_t fn_process_ddp;       /* ProcessDDPPacket */
static bool in_delivery;

/* received packet queue */
typedef struct { uint8_t type, src_node; uint16_t len; uint32_t seq; uint32_t arrived; uint8_t data[600]; } RxPkt;
#define RXQ 64
static RxPkt rxq[RXQ];
static int rxn;
static uint32_t last_aa_seq;
static bool have_last_seq;

/* registered-event dedup */
static uint32_t seen_ids[256];
static int seen_n;
static uint32_t pending_receipts[16];
static int pending_n;

static uint32_t func_by_name(const char *n);

static void store_seen(uint32_t id)
{
    seen_ids[seen_n % 256] = id;
    seen_n++;
}

static bool was_seen(uint32_t id)
{
    int n = seen_n < 256 ? seen_n : 256;
    for (int i = 0; i < n; i++) if (seen_ids[i] == id) return true;
    return false;
}

static Peer *peer_by_node(uint8_t node)
{
    for (int i = 0; i < 16; i++) if (peers[i].used && peers[i].node == node) return &peers[i];
    return NULL;
}

static Peer *learn_peer(uint8_t node, UdpAddr a, const char *name)
{
    Peer *p = peer_by_node(node);
    if (!p) {
        for (int i = 0; i < 16; i++) if (!peers[i].used) { p = &peers[i]; break; }
        if (!p) p = &peers[0];
        memset(p, 0, sizeof *p);
        p->used = true;
        p->node = node;
    }
    if (p->addr.ip != a.ip || p->addr.port != a.port) {
        char s[64]; udp_addr_str(a, s, sizeof s);
        log_info("net: peer node %d at %s", node, s);
    }
    p->addr = a;
    if (name && name[0]) snprintf(p->name, sizeof p->name, "%s", name);
    return p;
}

/* outgoing delay line for --sim-latency / --sim-loss */
typedef struct { uint64_t due_us; UdpAddr to; uint16_t len; uint8_t buf[708]; } Delayed;
#define DELAYQ 256
static Delayed delayq[DELAYQ];
static int delayn;

static void flush_delayed(void)
{
    uint64_t now = plat_micros();
    int k = 0;
    for (int i = 0; i < delayn; i++) {
        if (delayq[i].due_us <= now) udp_send(sock, delayq[i].to, delayq[i].buf, delayq[i].len);
        else delayq[k++] = delayq[i];
    }
    delayn = k;
}

static void send_raw(UdpAddr to, uint8_t kind, uint8_t type, const uint8_t *data, size_t len)
{
    uint8_t buf[708];
    if (len > sizeof buf - 8) return;
    memcpy(buf, PROTO_MAGIC, 4);
    buf[4] = PROTO_VER; buf[5] = kind; buf[6] = type; buf[7] = my_node;
    if (len) memcpy(buf + 8, data, len);
    if (g_netcfg.sim_loss_pct > 0 && (int)(SDL_rand_hack() % 100) < g_netcfg.sim_loss_pct) return;
    if (g_netcfg.sim_latency_ms > 0 && delayn < DELAYQ) {
        Delayed *d = &delayq[delayn++];
        /* +-20% jitter */
        int jitter = (int)(SDL_rand_hack() % (unsigned)(g_netcfg.sim_latency_ms * 2 / 5 + 1)) - g_netcfg.sim_latency_ms / 5;
        d->due_us = plat_micros() + (uint64_t)(g_netcfg.sim_latency_ms + jitter) * 1000;
        d->to = to; d->len = (uint16_t)(len + 8); memcpy(d->buf, buf, len + 8);
        return;
    }
    udp_send(sock, to, buf, len + 8);
}

/* The host (game's NBP lookup side) listens on the well-known port; the
 * joiner dials out from any free port, so only the host needs forwarding. */
static bool ensure_socket(bool listen)
{
    if (sock >= 0) return true;
    sock = listen ? udp_open(g_netcfg.port) : udp_open(0);
    if (sock < 0 && listen) {
        log_warn("net: UDP port %u busy, using an ephemeral port", g_netcfg.port);
        sock = udp_open(0);
    }
    if (sock < 0) { log_warn("net: cannot open UDP socket"); return false; }
    log_info("net: %s on UDP port %u (node %d)", listen ? "hosting" : "joining", udp_local_port(sock), my_node);
    return true;
}

static void close_socket(void)
{
    if (sock >= 0) udp_close(sock);
    sock = -1;
    memset(peers, 0, sizeof peers);
    rxn = 0;
    have_last_seq = false;
}

void net_init(void)
{
    udp_init();
    my_node = (uint8_t)(2 + (SDL_rand_hack() % 250));
    /* AppleTalk is "open": PortBUse = 1 (bit7 clear) */
    wrb(0x0291, 0x01);
    wrl(0x02D8, abus_vars);
    wrb(abus_vars + 0, my_node);
    wrw(abus_vars + 26, 0);
    wrb(A[5] + G_MY_NODE_BYTE, my_node);
    fn_process_ddp = func_by_name("ProcessDDPPacket");
    if (!fn_process_ddp) log_warn("net: ProcessDDPPacket not found");
}

/* ---------------------------------------------------------------- send */
static void inject_receipts(uint8_t *p, size_t *len)
{
    /* $AA payload: +9 = number of receipts (8 bytes each) at the end */
    while (pending_n > 0 && *len + 8 <= 586) {
        uint32_t id = pending_receipts[--pending_n];
        uint8_t *r = p + *len;
        r[0] = id >> 24; r[1] = id >> 16; r[2] = id >> 8; r[3] = (uint8_t)id;
        r[4] = r[5] = r[6] = r[7] = 0;
        p[9]++;
        *len += 8;
    }
}

static int16_t do_write_ddp(uint32_t pb)
{
    uint32_t wds = rdl(pb + 30);
    uint32_t hdr = rdl(wds + 2);            /* points one byte into the DDP header */
    uint8_t dest_node = (uint8_t)rdb(hdr + 11);
    uint8_t type = (uint8_t)rdb(hdr + 15);
    uint8_t data[700]; size_t len = 0;
    for (uint32_t e = wds + 6;; e += 6) {
        uint16_t l = (uint16_t)rdw(e);
        if (!l) break;
        if (len + l > 600) break;
        memcpy(data + len, HP(rdl(e + 2)), l);
        len += l;
    }
    if (type == 0xAA && len >= 12) inject_receipts(data, &len);
    Peer *p = peer_by_node(dest_node);
    if (!p) { log_trace("net: no route to node %d", dest_node); return 0; }
    send_raw(p->addr, K_DDP, type, data, len);
    if (type == 0xAA && g_netcfg.redundancy) send_raw(p->addr, K_DDP, type, data, len);
    return 0;
}

static bool cur_async;

static void complete_pb(uint32_t pb, int16_t err)
{
    wrw(pb + 16, (uint16_t)err);
    uint32_t compl = rdl(pb + 12);
    /* completion routines only run for asynchronous calls */
    if (compl && cur_async) {
        uint32_t sd[8], sa[8]; memcpy(sd, D, sizeof sd); memcpy(sa, A, sizeof sa);
        uint8_t f[5] = { fN, fZ, fV, fC, fX };
        A[0] = pb; D[0] = (uint32_t)(int32_t)err;
        call_emulated(compl);
        memcpy(D, sd, sizeof sd); memcpy(A, sa, sizeof sa);
        fN = f[0]; fZ = f[1]; fV = f[2]; fC = f[3]; fX = f[4];
    }
}

/* ---------------------------------------------------------------- NBP */
static void entity_strings(uint32_t ent, char *obj, char *type)
{
    pstr_to_c(ent, obj, 64);
    ent += 1 + rdb(ent);
    pstr_to_c(ent, type, 64);
}

/* joiner -> host announcement: "I'm <name>, pick me" */
static void announce(void)
{
    uint8_t r[140]; size_t k = 0;
    size_t a = strlen(adv_object), t = strlen(adv_type);
    r[k++] = (uint8_t)a; memcpy(r + k, adv_object, a); k += a;
    r[k++] = (uint8_t)t; memcpy(r + k, adv_type, t); k += t;
    UdpAddr bc = { 0xFFFFFFFFu, g_netcfg.port };
    send_raw(bc, K_ADVERT, 0, r, k);
    for (int i = 0; i < g_netcfg.nconnect; i++) send_raw(g_netcfg.connect[i], K_ADVERT, 0, r, k);
    if (host_target_ok) send_raw(host_target, K_ADVERT, 0, r, k);
}

/* ---------------------------------------------------------------- settings & UI */
static void cfg_path(char *out, size_t sz) { snprintf(out, sz, "%s/network.cfg", g_game_dir); }

static void cfg_load(void)
{
    char p[1100]; cfg_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "host_address=", 13)) snprintf(host_addr, sizeof host_addr, "%s", line + 13);
    }
    fclose(f);
}

static void cfg_save(void)
{
    char p[1100]; cfg_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "host_address=%s\n", host_addr);
    fclose(f);
}

static void join_prompt(void)
{
    static bool loaded;
    if (!loaded) { cfg_load(); loaded = true; }
    char buf[64]; snprintf(buf, sizeof buf, "%s", host_addr);
    char help2[128];
    snprintf(help2, sizeof help2, "Optionally add :port (default %u). Leave empty to find a host on your local network.", g_netcfg.port);
    host_target_ok = false;
    if (!ui_prompt("Join a friend who chose NET HOST",
                   "Type the address shown on their screen, e.g. 203.0.113.7 or 192.168.1.20", help2, buf, sizeof buf))
        buf[0] = 0;  /* cancelled: look on the local network only */
    snprintf(host_addr, sizeof host_addr, "%s", buf);
    cfg_save();
    if (buf[0]) {
        host_target_ok = udp_resolve(buf, g_netcfg.port, &host_target);
        if (!host_target_ok) log_warn("net: cannot resolve %s", buf);
    }
}

static void update_banner(void)
{
    char l1[160], l2[160];
    uint16_t port = sock >= 0 ? udp_local_port(sock) : g_netcfg.port;
    if (lookup_pb) {
        int found = rdw(lookup_pb + 42);
        if (found) {
            snprintf(l1, sizeof l1, found == 1 ? "Your friend is here!" : "%d players want to join!", found);
            snprintf(l2, sizeof l2, "Click a name in the list, then click OK to start.");
        } else {
            char ip[64] = "this computer's IP";
            udp_local_ip(ip, sizeof ip);
            snprintf(l1, sizeof l1, "Hosting: give your friend your address, then they choose NET JOIN.");
            if (port == 24731)
                snprintf(l2, sizeof l2, "Same network: %s    Internet: your public IP (forward UDP port %u to this computer)", ip, port);
            else
                snprintf(l2, sizeof l2, "Same network: %s:%u    Internet: your public IP:%u (forward UDP port %u)", ip, port, port, port);
        }
        overlay_banner(l1, l2);
    } else if (advertising && !joined) {
        if (host_addr[0] && !host_target_ok)
            snprintf(l1, sizeof l1, "Couldn't find \"%s\". Looking on your local network instead...", host_addr);
        else if (host_addr[0])
            snprintf(l1, sizeof l1, "Contacting %s...", host_addr);
        else
            snprintf(l1, sizeof l1, "Looking for a host on your local network...");
        if (ticks_now() - lookup_started > 600)
            snprintf(l2, sizeof l2, "No answer yet. Check the address; the host must be on NET HOST with UDP port %u forwarded.", g_netcfg.port);
        else
            snprintf(l2, sizeof l2, "Waiting for the host to pick your name and click OK.");
        overlay_banner(l1, l2);
    } else {
        overlay_banner(NULL, NULL);
    }
}

static void add_lookup_result(Peer *p)
{
    if (!lookup_pb) return;
    uint32_t buf = rdl(lookup_pb + 34), size = rdl(lookup_pb + 38);
    uint16_t maxget = (uint16_t)rdw(lookup_pb + 40), got = (uint16_t)rdw(lookup_pb + 42);
    if (got >= maxget) return;
    /* skip existing tuples, refusing duplicates of this node */
    uint32_t q = buf;
    for (int i = 0; i < got; i++) {
        if (rdb(q + 2) == p->node) return;
        q += 5;
        for (int k = 0; k < 3; k++) q += 1 + rdb(q);
    }
    size_t need = 5 + 3 + strlen(p->name) + strlen(adv_type) + 1;
    if (q + need > buf + size) return;
    wrw(q, 0); wrb(q + 2, p->node); wrb(q + 3, MY_SOCKET); wrb(q + 4, 1);
    q += 5;
    c_to_pstr(p->name[0] ? p->name : "Player", q); q += 1 + rdb(q);
    c_to_pstr(adv_type, q); q += 1 + rdb(q);
    c_to_pstr("*", q);
    wrw(lookup_pb + 42, (uint16_t)(got + 1));
}

int16_t net_driver_control(uint32_t pb, bool async)
{
    cur_async = async;
    int16_t cs = (int16_t)rdw(pb + 26);
    switch (cs) {
    case 248: /* POpenSkt: the UDP socket is bound once the role is known */
        wrb(pb + 28, MY_SOCKET);
        complete_pb(pb, 0);
        return 0;
    case 247: /* PCloseSkt */
        advertising = false; lookup_pb = 0;
        close_socket();
        update_banner();
        complete_pb(pb, 0);
        return 0;
    case 246: /* PWriteDDP */
        complete_pb(pb, do_write_ddp(pb));
        return 0;
    case 253: { /* PRegisterName: entityPtr -> NTE; entity strings at nte+9 */
        uint32_t nte = rdl(pb + 30);
        entity_strings(nte + 9, adv_object, adv_type);
        if (!g_netcfg.nconnect) join_prompt();
        advertising = ensure_socket(false);
        joined = false;
        lookup_started = ticks_now();
        lookup_last_probe = 0;
        update_banner();
        log_info("net: joining as \"%s\"", adv_object);
        complete_pb(pb, 0);
        return 0;
    }
    case 251: { /* PLookupName */
        char obj[64];
        entity_strings(rdl(pb + 30), obj, adv_type);
        ensure_socket(true);
        lookup_pb = pb;
        lookup_started = ticks_now();
        wrw(pb + 42, 0);
        wrw(pb + 16, 1); /* in progress until killed */
        lookup_last_probe = 0;
        update_banner();
        for (int i = 0; i < 16; i++) if (peers[i].used && peers[i].name[0]) add_lookup_result(&peers[i]);
        return 0;
    }
    case 250: /* PConfirmName */
        wrb(pb + 38, MY_SOCKET);
        complete_pb(pb, 0);
        return 0;
    case 252: /* PRemoveName */
        advertising = false;
        update_banner();
        complete_pb(pb, 0);
        return 0;
    case 254: { /* PKillNBP: nKillQEl at +28 */
        uint32_t target = rdl(pb + 28);
        if (target && target == lookup_pb) { wrw(lookup_pb + 16, 0); lookup_pb = 0; update_banner(); }
        complete_pb(pb, 0);
        return 0;
    }
    default:
        log_warn("net: unsupported .MPP csCode %d", cs);
        complete_pb(pb, -1);
        return 0;
    }
}

/* ---------------------------------------------------------------- receive */
/* the game only accepts registered entries when the slot it writes into was
 * consumed; entries it would drop must not be marked as seen */
static bool reg_slot_consumed(void)
{
    uint32_t a5 = A[5];
    int cur = rdb(a5 + G_REG_CURSLOT) ? 1 : 0;
    uint32_t slot = a5 + G_REG_SLOTS + (uint32_t)(cur ? 0 : 1) * G_RX_SLOT_SIZE;
    return rdb(slot + G_RX_CONSUMED) != 0;
}

static void dedup_registered(RxPkt *p)
{
    uint8_t *d = p->data;
    if (p->len < 12) return;
    bool ok = reg_slot_consumed();
    if (d[8]) {
        size_t o2 = 12 + (size_t)d[10] * 10;
        for (int i = 0; i < d[8] && o2 + 10 <= p->len; i++) {
            uint16_t ty = (uint16_t)((d[o2] << 8) | d[o2 + 1]);
            uint32_t id = ((uint32_t)d[o2 + 6] << 24) | ((uint32_t)d[o2 + 7] << 16) | ((uint32_t)d[o2 + 8] << 8) | d[o2 + 9];
            NETLOG("net: rx seq %u reg type %02X id %u%s%s", p->seq, ty, id, ok ? "" : " (slot busy)", was_seen(id) ? " dup" : "");
            o2 += 10 + (ty == 0x17 ? 520 : ty == 0x18 ? 287 : 0);
        }
    }
    if (!ok) return;
    int nreg = d[8], nsprites = d[10];
    size_t off = 12 + (size_t)nsprites * 10;
    for (int i = 0; i < nreg; i++) {
        if (off + 10 > p->len) return;
        uint16_t type = (uint16_t)((d[off] << 8) | d[off + 1]);
        uint32_t id = ((uint32_t)d[off + 6] << 24) | ((uint32_t)d[off + 7] << 16) | ((uint32_t)d[off + 8] << 8) | d[off + 9];
        size_t extra = type == 0x17 ? 520 : type == 0x18 ? 287 : 0;
        size_t sz = 10 + extra;
        if (off + sz > p->len) return;
        if (was_seen(id)) {
            NETLOG("net: stripped duplicate id %u, receipt queued", id);
            memmove(d + off, d + off + sz, p->len - off - sz);
            p->len = (uint16_t)(p->len - sz);
            d[8]--;
            nreg--; i--;
            if (pending_n < 16) pending_receipts[pending_n++] = id;
            continue;
        }
        if (i < G_REG_MAX) store_seen(id);
        off += sz;
    }
}

static void deliver(RxPkt *p)
{
    if (!fn_process_ddp) return;
    uint32_t a5 = A[5];
    uint32_t hdr = a5 + G_RX_HDR;
    uint16_t len = p->len > G_RX_MAX ? G_RX_MAX : p->len;
    memset(HP(hdr), 0, 17);
    wrb(hdr + 0, rdb(a5 + G_MY_NODE_BYTE));
    wrb(hdr + 1, p->src_node);
    wrb(hdr + 2, 2);
    wrw(hdr + 3, (uint16_t)(13 + len));
    wrb(hdr + 15, MY_SOCKET);
    wrb(hdr + 16, p->type);
    memcpy(HP(a5 + G_RX_PAYLOAD), p->data, len);
    uint32_t sd[8], sa[8]; memcpy(sd, D, sizeof sd); memcpy(sa, A, sizeof sa);
    uint8_t f[5] = { fN, fZ, fV, fC, fX };
    in_delivery = true;
    call_emulated(fn_process_ddp);
    in_delivery = false;
    memcpy(D, sd, sizeof sd); memcpy(A, sa, sizeof sa);
    fN = f[0]; fZ = f[1]; fV = f[2]; fC = f[3]; fX = f[4];
}

static bool write_slot_consumed(void)
{
    uint32_t a5 = A[5];
    int cur = rdb(a5 + G_RX_CURSLOT) ? 1 : 0;
    uint32_t slot = a5 + G_RX_SLOTS + (uint32_t)(cur ? 0 : 1) * G_RX_SLOT_SIZE;
    return rdw(slot + G_RX_CONSUMED) != 0;
}

static void handle_datagram(UdpAddr from, const uint8_t *buf, int n)
{
    if (n < 8 || memcmp(buf, PROTO_MAGIC, 4) != 0 || buf[4] != PROTO_VER) return;
    uint8_t kind = buf[5], type = buf[6], node = buf[7];
    const uint8_t *pl = buf + 8; int plen = n - 8;
    if (node == my_node) return; /* our own broadcast */
    if (kind == K_PROBE) {
        if (!advertising) return;
        uint8_t r[140]; size_t k = 0;
        size_t a = strlen(adv_object), t = strlen(adv_type);
        r[k++] = (uint8_t)a; memcpy(r + k, adv_object, a); k += a;
        r[k++] = (uint8_t)t; memcpy(r + k, adv_type, t); k += t;
        send_raw(from, K_ADVERT, 0, r, k);
        return;
    }
    if (kind == K_ADVERT) {
        char name[64] = "Player";
        if (plen >= 1 && pl[0] < 64 && pl[0] + 1 <= plen) { memcpy(name, pl + 1, pl[0]); name[pl[0]] = 0; }
        Peer *p = learn_peer(node, from, name);
        add_lookup_result(p);
        return;
    }
    if (kind != K_DDP) return;
    learn_peer(node, from, NULL);
    if (advertising && !joined) { joined = true; update_banner(); }
    if (plen > 600) return;
    if (type == 0xAA && plen >= 4) {
        uint32_t seq = ((uint32_t)pl[0] << 24) | ((uint32_t)pl[1] << 16) | ((uint32_t)pl[2] << 8) | pl[3];
        if (have_last_seq && seq <= last_aa_seq) return;
        for (int i = 0; i < rxn; i++) if (rxq[i].type == 0xAA && rxq[i].seq == seq) return;
        if (rxn == RXQ) { memmove(rxq, rxq + 1, sizeof(RxPkt) * (RXQ - 1)); rxn--; }
        RxPkt *p = &rxq[rxn++];
        p->type = type; p->src_node = node; p->len = (uint16_t)plen; p->seq = seq; p->arrived = ticks_now();
        memcpy(p->data, pl, plen);
        /* keep $AA packets ordered by sequence number */
        for (int i = rxn - 1; i > 0 && rxq[i - 1].type == 0xAA && rxq[i - 1].seq > rxq[i].seq; i--) {
            RxPkt t = rxq[i]; rxq[i] = rxq[i - 1]; rxq[i - 1] = t;
        }
        return;
    }
    if (rxn == RXQ) return;
    RxPkt *p = &rxq[rxn++];
    p->type = type; p->src_node = node; p->len = (uint16_t)plen; p->seq = 0; p->arrived = ticks_now();
    memcpy(p->data, pl, plen);
}

void net_poll(void)
{
    if (sock < 0 || in_delivery) return;
    flush_delayed();
    uint8_t buf[2048];
    UdpAddr from;
    int n;
    while ((n = udp_recv(sock, &from, buf, sizeof buf)) >= 0) handle_datagram(from, buf, n);
    uint32_t now = ticks_now();
    if (advertising && !joined && now - lookup_last_probe >= 30) {
        lookup_last_probe = now;
        announce();
        update_banner();
    }
    /* deliver queued packets: control packets immediately, game packets only
     * when the game consumed the previous one (or after a short timeout) */
    while (rxn > 0) {
        RxPkt *p = &rxq[0];
        if (p->type == 0xAA) {
            if (!g_netcfg.raw && !write_slot_consumed() && now - p->arrived < (uint32_t)g_netcfg.hold_ticks) break;
            if (!g_netcfg.raw && g_netcfg.dedup) dedup_registered(p);
            last_aa_seq = p->seq; have_last_seq = true;
        }
        RxPkt cur = *p;
        memmove(rxq, rxq + 1, sizeof(RxPkt) * (rxn - 1));
        rxn--;
        deliver(&cur);
        if (cur.type == 0xAA && !g_netcfg.raw) break; /* at most one game packet per poll */
    }
}

static uint32_t func_by_name(const char *n)
{
    extern uint32_t recomp_func_by_name(const char *);
    return recomp_func_by_name(n);
}
