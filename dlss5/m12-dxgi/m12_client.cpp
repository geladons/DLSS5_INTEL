// m12_client.cpp - TCP exchange with the m11d daemon (see m12_client.h).
#include "m12_client.h"

#include "m12_log.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdlib>

static long daemon_port()
{
    static long port = -1;
    if (port < 0) {
        const char *env = getenv("M12_PORT");
        port = env ? strtol(env, NULL, 10) : 47990;
        if (port <= 0 || port > 65535) port = 47990;
    }
    return port;
}

static bool winsock_up()
{
    static LONG state = 0;  // 0 = untried, 1 = ok, 2 = failed
    if (state == 0) {
        WSADATA wd;
        LONG ok = WSAStartup(MAKEWORD(2, 2), &wd) == 0 ? 1 : 2;
        InterlockedCompareExchange(&state, ok, 0);
        if (ok == 2) m12_logf("WSAStartup failed (%d)", WSAGetLastError());
    }
    return state == 1;
}

int m12_exchange(const uint32_t header[4], const void *payload,
                 std::size_t payload_size, void *reply, std::size_t reply_size)
{
    if (!winsock_up()) return -1;
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return -1;
    DWORD tv = 60000;  // first frame may carry the ~6 s daemon weight load
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((u_short)daemon_port());
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        m12_logf("no daemon on 127.0.0.1:%ld", daemon_port());
        closesocket(fd);
        return -1;
    }
    const unsigned char *out = (const unsigned char *)header;
    for (std::size_t sent = 0; sent < sizeof(uint32_t) * 4;) {
        int n = send(fd, (const char *)out + sent, (int)(sizeof(uint32_t) * 4 - sent), 0);
        if (n <= 0) { closesocket(fd); return -1; }
        sent += (std::size_t)n;
    }
    out = (const unsigned char *)payload;
    for (std::size_t sent = 0; sent < payload_size;) {
        int n = send(fd, (const char *)out + sent, (int)(payload_size - sent), 0);
        if (n <= 0) { closesocket(fd); return -1; }
        sent += (std::size_t)n;
    }
    unsigned char *in = (unsigned char *)reply;
    for (std::size_t got = 0; got < reply_size;) {
        int n = recv(fd, (char *)in + got, (int)(reply_size - got), 0);
        if (n <= 0) { closesocket(fd); return -1; }
        got += (std::size_t)n;
    }
    closesocket(fd);
    return 0;
}
