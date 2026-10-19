/* Minimal non-blocking UDP sockets (Winsock / BSD sockets). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct { uint32_t ip; uint16_t port; } UdpAddr;   /* host byte order */

bool udp_init(void);
int  udp_open(uint16_t port);                 /* returns handle or -1; port 0 = ephemeral */
void udp_close(int s);
uint16_t udp_local_port(int s);
bool udp_send(int s, UdpAddr to, const void *data, size_t len);
int  udp_recv(int s, UdpAddr *from, void *buf, size_t cap);   /* -1 when nothing pending */
bool udp_resolve(const char *host, uint16_t default_port, UdpAddr *out); /* "host[:port]" */
void udp_addr_str(UdpAddr a, char *out, size_t sz);
