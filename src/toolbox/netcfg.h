#pragma once
#include "platform/udp.h"

typedef struct {
    uint16_t port;            /* UDP port the joiner listens on (default 24731) */
    UdpAddr connect[8];       /* explicit peers to probe (--connect host[:port]) */
    int nconnect;
    int hold_ticks;           /* max ticks a game packet waits for the game to consume the previous one */
    bool redundancy;          /* send every game packet twice */
    bool raw;                 /* deliver packets exactly as received (no hold / dedup) */
    bool dedup;               /* strip repeated registered events (experimental) */
    int sim_latency_ms;       /* testing: delay every outgoing datagram */
    int sim_loss_pct;         /* testing: drop this percentage of outgoing datagrams */
} NetConfig;

extern NetConfig g_netcfg;
void net_init(void);
void net_poll(void);
uint32_t SDL_rand_hack(void);
