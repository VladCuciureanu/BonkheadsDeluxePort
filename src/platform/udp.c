#include "udp.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define closesock closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define closesock close
#endif

#define MAX_SOCKS 8
static sock_t socks[MAX_SOCKS];
static bool used[MAX_SOCKS];

bool udp_init(void)
{
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
    return true;
}

int udp_open(uint16_t port)
{
    int slot = -1;
    for (int i = 0; i < MAX_SOCKS; i++) if (!used[i]) { slot = i; break; }
    if (slot < 0) return -1;
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BAD_SOCK) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { closesock(s); return -1; }
#if defined(_WIN32)
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
    socks[slot] = s;
    used[slot] = true;
    return slot;
}

void udp_close(int s)
{
    if (s < 0 || s >= MAX_SOCKS || !used[s]) return;
    closesock(socks[s]);
    used[s] = false;
}

uint16_t udp_local_port(int s)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    if (s < 0 || !used[s] || getsockname(socks[s], (struct sockaddr *)&a, &l) != 0) return 0;
    return ntohs(a.sin_port);
}

bool udp_send(int s, UdpAddr to, const void *data, size_t len)
{
    if (s < 0 || !used[s]) return false;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(to.ip);
    a.sin_port = htons(to.port);
    return sendto(socks[s], (const char *)data, (int)len, 0, (struct sockaddr *)&a, sizeof a) == (int)len;
}

int udp_recv(int s, UdpAddr *from, void *buf, size_t cap)
{
    if (s < 0 || !used[s]) return -1;
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    int n = (int)recvfrom(socks[s], (char *)buf, (int)cap, 0, (struct sockaddr *)&a, &l);
    if (n < 0) return -1;
    if (from) { from->ip = ntohl(a.sin_addr.s_addr); from->port = ntohs(a.sin_port); }
    return n;
}

bool udp_resolve(const char *host, uint16_t default_port, UdpAddr *out)
{
    char h[256];
    snprintf(h, sizeof h, "%s", host);
    uint16_t port = default_port;
    char *c = strrchr(h, ':');
    if (c) { *c = 0; port = (uint16_t)atoi(c + 1); }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(h, NULL, &hints, &res) != 0 || !res) return false;
    out->ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    out->port = port;
    freeaddrinfo(res);
    return true;
}

void udp_addr_str(UdpAddr a, char *out, size_t sz)
{
    snprintf(out, sz, "%u.%u.%u.%u:%u", (a.ip >> 24) & 255, (a.ip >> 16) & 255, (a.ip >> 8) & 255, a.ip & 255, a.port);
}
