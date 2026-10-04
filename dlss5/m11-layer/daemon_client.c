/* SPDX-License-Identifier: Apache-2.0 */
/* =====================================================================
 * daemon_client - TCP exchange with the m11d daemon (see daemon_client.h).
 * Verbatim move from nr_layer_win.c.
 * ===================================================================== */
#include "daemon_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string.h>

long nr_port = NR_DEFAULT_PORT;

static int winsock_up(void)
{
    static int attempted, ok;
    if (attempted) return ok;
    attempted = 1;
    WSADATA wd;
    ok = WSAStartup(MAKEWORD(2, 2), &wd) == 0;
    return ok;
}

int exchange(const void *header, size_t header_size, const void *payload,
             size_t payload_size, void *reply, size_t reply_size)
{
    if (!winsock_up()) return -2;
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return -2;
    DWORD tv = 1500;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((u_short)nr_port);
    /* Bound the connect: a blocking connect() to a wedged daemon (listen
     * backlog saturated while its serial accept loop is stuck inside a
     * frame) stalls the game render thread for the full TCP SYN
     * retransmission timeout (~21 s) -> AppHang. Do it non-blocking with
     * the same 1.5 s budget as send/recv. */
    u_long nbio = 1;
    ioctlsocket(fd, FIONBIO, &nbio);
    int conn_rc = connect(fd, (struct sockaddr *)&address, sizeof address);
    if (conn_rc != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wf, ef;
        struct timeval ctv;
        ctv.tv_sec = 1;
        ctv.tv_usec = 500000;
        FD_ZERO(&wf); FD_SET(fd, &wf);
        FD_ZERO(&ef); FD_SET(fd, &ef);
        int sel = select(0, NULL, &wf, &ef, &ctv);
        if (sel > 0 && FD_ISSET(fd, &wf) && !FD_ISSET(fd, &ef)) {
            int soerr = 0;
            int slen = sizeof soerr;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &slen);
            conn_rc = soerr ? -1 : 0;
        } else {
            conn_rc = -1;
        }
    }
    nbio = 0;
    ioctlsocket(fd, FIONBIO, &nbio);
    if (conn_rc != 0) {
        closesocket(fd);
        return -2;
    }
    const unsigned char *out = (const unsigned char *)header;
    for (size_t sent = 0; sent < header_size; ) {
        int n = send(fd, (const char *)out + sent, (int)(header_size - sent), 0);
        if (n <= 0) { closesocket(fd); return -1; }
        sent += (size_t)n;
    }
    out = (const unsigned char *)payload;
    for (size_t sent = 0; sent < payload_size; ) {
        int n = send(fd, (const char *)out + sent, (int)(payload_size - sent), 0);
        if (n <= 0) { closesocket(fd); return -1; }
        sent += (size_t)n;
    }
    unsigned char *in = (unsigned char *)reply;
    for (size_t got = 0; got < reply_size; ) {
        int n = recv(fd, (char *)in + got, (int)(reply_size - got), 0);
        if (n <= 0) { closesocket(fd); return -1; }
        got += (size_t)n;
    }
    closesocket(fd);
    return 0;
}

int nr_ctrl(uint32_t cmd, uint32_t payload_arg)
{
    uint32_t hdr[4] = { 0x5443524Eu, cmd, payload_arg, 0 };
    uint32_t rep[4] = { 0 };
    int rc = exchange(hdr, sizeof hdr, NULL, 0, rep, sizeof rep);
    if (rc) return rc;
    return (rep[0] == 0x5443524Eu && rep[1]) ? 0 : 1;
}
