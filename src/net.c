/*
 * net.c — generic outbound TCP client builtins for Nova.
 *
 *   net_connect(host, port)  -> handle (>0)          throw on failure
 *   net_send(h, data)        -> true                 throw on broken pipe
 *   net_recv(h)              -> string ("" = EOF)    throw on timeout/error
 *   net_recv_line(h)         -> string ("" = EOF)    throw on timeout/error
 *   net_close(h)             -> bool (never throws)
 *   net_last_error()         -> string
 *
 * The http_client package is built on these; smtp_send (mail.c) keeps its
 * own private plumbing so neither can break the other.
 *
 * Hardening (same posture as the NV-01..NV-13 audit):
 *   - at most NV_NET_MAX concurrent sockets (fd exhaustion guard)
 *   - every send/recv bounded by SO_RCVTIMEO / SO_SNDTIMEO
 *   - connect() is non-blocking with a select() cap, so a dead host cannot
 *     wedge a CGI process for the kernel's default ~2 minutes
 *   - recv payloads are capped per call; the caller (http_client) caps the
 *     total response size
 *   - no shell, no filesystem interplay: the only injection surface is the
 *     payload itself, which the package validates before sending
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "nova.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET nv_sock_t;
  #define NV_SOCK_INVALID  INVALID_SOCKET
  #define nv_close_sock(s) closesocket(s)
  #define NV_SOCK_EAGAIN   WSAEWOULDBLOCK
  #define NV_SOCK_EINTR    WSAEINTR
  #define sock_errno()     WSAGetLastError()
  #define nv_sock_rc(r)    ((r) == SOCKET_ERROR)
#else
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <sys/select.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <unistd.h>
  typedef int nv_sock_t;
  #define NV_SOCK_INVALID  (-1)
  #define nv_close_sock(s) close(s)
  #define NV_SOCK_EAGAIN   EAGAIN
  #define NV_SOCK_EINTR    EINTR
  #define sock_errno()     errno
  #define nv_sock_rc(r)    ((r) < 0)
#endif

#define NV_NET_MAX       64        /* concurrent open sockets */
#define NV_NET_TIMEOUT   15        /* seconds per send/recv */
#define NV_NET_CONNECT   10        /* seconds for connect() */
#define NV_NET_CHUNK     65536     /* max bytes per net_recv */
#define NV_NET_LINE      8192      /* max bytes per net_recv_line */

static nv_sock_t g_socks[NV_NET_MAX];
static bool      g_inuse[NV_NET_MAX];
static char      g_net_error[512];
#ifdef _WIN32
static int       g_wsa = 0;       /* WSAStartup refcount */
#endif

/* record the message and throw: returns NULL so callers can
 * `return net_err(I, "...")` directly */
static NVValue *net_err(Interp *I, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_net_error, sizeof(g_net_error), fmt, ap);
    va_end(ap);
    return nv_err(I, "%s", g_net_error);
}

/* ---------- handle table ---------- */

static nv_sock_t handle_sock(long long h) {
    if (h < 1 || h > NV_NET_MAX) return NV_SOCK_INVALID;
    if (!g_inuse[h - 1]) return NV_SOCK_INVALID;
    return g_socks[h - 1];
}

static bool handle_valid(long long h) { return handle_sock(h) != NV_SOCK_INVALID; }

void nv_net_close_all(void) {
    for (int i = 0; i < NV_NET_MAX; i++) {
        if (g_inuse[i]) {
            nv_close_sock(g_socks[i]);
            g_inuse[i] = false;
        }
        g_socks[i] = NV_SOCK_INVALID;
    }
#ifdef _WIN32
    if (g_wsa > 0) { WSACleanup(); g_wsa = 0; }
#endif
}

/* ---------- socket plumbing ---------- */

static void set_timeouts(nv_sock_t s) {
    struct timeval tv = { NV_NET_TIMEOUT, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
}

static void set_nonblock(nv_sock_t s, bool nb) {
#ifdef _WIN32
    u_long mode = nb ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return;
    if (nb) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
    fcntl(s, F_SETFL, flags);
#endif
}

/* resolve + connect with a hard cap on how long a dead host can hold us */
static nv_sock_t tcp_connect(Interp *I, const char *host, int port) {
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0 || !res) {
        net_err(I, "net_connect: cannot resolve host \"%s\"", host);
        return NV_SOCK_INVALID;
    }
    nv_sock_t sock = NV_SOCK_INVALID;
    for (rp = res; rp; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == NV_SOCK_INVALID) continue;
        set_timeouts(sock);
        set_nonblock(sock, true);
        int rc = connect(sock, rp->ai_addr, rp->ai_addrlen);
        bool ok = (rc == 0);
        if (!ok) {
            int e = sock_errno();
            if (e == NV_SOCK_EAGAIN || e == EINPROGRESS || e == EWOULDBLOCK) {
                fd_set wset;
                FD_ZERO(&wset);
                FD_SET(sock, &wset);
                struct timeval tv = { NV_NET_CONNECT, 0 };
                if (select((int)sock + 1, NULL, &wset, NULL, &tv) > 0) {
                    int soerr = 0;
                    socklen_t elen = sizeof(soerr);
                    getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&soerr, &elen);
                    ok = (soerr == 0);
                }
            }
        }
        if (ok) {
            set_nonblock(sock, false);
            int one = 1;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
            freeaddrinfo(res);
            return sock;
        }
        nv_close_sock(sock);
        sock = NV_SOCK_INVALID;
    }
    freeaddrinfo(res);
    net_err(I, "net_connect: cannot connect to %s:%d", host, port);
    return NV_SOCK_INVALID;
}

/* ---------- argument helpers ---------- */

static bool arg_handle(Interp *I, const char *fn, NVValue **a, long long *out) {
    if (!nv_arg_int(I, fn, 0, a)) return false;
    long long h = a[0]->as.i;
    if (!handle_valid(h)) {
        net_err(I, "%s: invalid socket handle %lld", fn, h);
        return false;
    }
    *out = h;
    return true;
}

/* ---------- builtins ---------- */

static NVValue *nb_net_connect(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "net_connect", 0, a)) return NULL;
    if (!nv_arg_int(I, "net_connect", 1, a)) return NULL;
    const char *host = a[0]->as.s;
    long long port = a[1]->as.i;
    if (a[0]->slen == 0 || a[0]->slen > 253)
        return net_err(I, "net_connect: host must be 1..253 characters");
    if (port < 1 || port > 65535)
        return net_err(I, "net_connect: port must be 1..65535, got %lld", port);

    int slot = -1;
    for (int i = 0; i < NV_NET_MAX; i++) if (!g_inuse[i]) { slot = i; break; }
    if (slot < 0)
        return net_err(I, "net_connect: too many open sockets (max %d) — net_close() one first", NV_NET_MAX);

#ifdef _WIN32
    if (g_wsa == 0) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return net_err(I, "net_connect: WSAStartup failed");
        g_wsa = 1;
    }
#endif

    nv_sock_t s = tcp_connect(I, host, (int)port);
    if (s == NV_SOCK_INVALID) return NULL;          /* net_err already threw */
    g_socks[slot] = s;
    g_inuse[slot] = true;
    return nv_int(I, slot + 1);
}

static NVValue *nb_net_send(Interp *I, NVValue **a, int n) {
    (void)n;
    long long h;
    if (!arg_handle(I, "net_send", a, &h)) return NULL;
    NVValue *data = nv_to_string(I, a[1]);
    if (!data || data->type != T_STR)
        return net_err(I, "net_send: data must be convertible to a string");
    nv_sock_t s = handle_sock(h);
    size_t off = 0;
    while (off < data->slen) {
        int r = (int)send(s, data->as.s + off, (int)(data->slen - off), 0);
        if (nv_sock_rc(r)) {
            int e = sock_errno();
            if (e == NV_SOCK_EINTR) continue;
            return net_err(I, "net_send: connection lost after %zu bytes (errno %d)", off, e);
        }
        off += (size_t)r;
    }
    return nv_bool(I, true);
}

static NVValue *nb_net_recv(Interp *I, NVValue **a, int n) {
    (void)n;
    long long h;
    if (!arg_handle(I, "net_recv", a, &h)) return NULL;
    nv_sock_t s = handle_sock(h);
    char buf[NV_NET_CHUNK];
    for (;;) {
        int r = (int)recv(s, buf, sizeof(buf), 0);
        if (r == 0) return nv_strl(I, "", 0);                    /* clean EOF */
        if (!nv_sock_rc(r)) return nv_strl(I, buf, (size_t)r);
        int e = sock_errno();
        if (e == NV_SOCK_EINTR) continue;
        if (e == NV_SOCK_EAGAIN || e == EWOULDBLOCK)
            return net_err(I, "net_recv: timed out after %d s", NV_NET_TIMEOUT);
        return net_err(I, "net_recv: connection error (errno %d)", e);
    }
}

static NVValue *nb_net_recv_line(Interp *I, NVValue **a, int n) {
    (void)n;
    long long h;
    if (!arg_handle(I, "net_recv_line", a, &h)) return NULL;
    nv_sock_t s = handle_sock(h);
    char buf[NV_NET_LINE];
    size_t ln = 0;
    bool got_any = false;
    for (;;) {
        char c;
        int r = (int)recv(s, &c, 1, 0);
        if (r == 0) {                                            /* EOF */
            if (!got_any) return nv_strl(I, "", 0);
            break;                                               /* EOF mid-line: return partial */
        }
        if (nv_sock_rc(r)) {
            int e = sock_errno();
            if (e == NV_SOCK_EINTR) continue;
            if (e == NV_SOCK_EAGAIN || e == EWOULDBLOCK)
                return net_err(I, "net_recv_line: timed out after %d s", NV_NET_TIMEOUT);
            return net_err(I, "net_recv_line: connection error (errno %d)", e);
        }
        got_any = true;
        if (c == '\n') break;
        if (c != '\r' && ln + 1 < sizeof(buf)) buf[ln++] = c;
    }
    return nv_strl(I, buf, ln);
}

static NVValue *nb_net_close(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_int(I, "net_close", 0, a)) return NULL;
    long long h = a[0]->as.i;
    if (!handle_valid(h)) return nv_bool(I, false);
    nv_close_sock(handle_sock(h));
    g_inuse[h - 1] = false;
    g_socks[h - 1] = NV_SOCK_INVALID;
    return nv_bool(I, true);
}

static NVValue *nb_net_last_error(Interp *I, NVValue **a, int n) {
    (void)a; (void)n;
    return nv_str(I, g_net_error);
}

/* ================= registration ================= */

void nv_register_net(Interp *I) {
    nv_reg_native(I, "net_connect",    nb_net_connect,    2, 2);
    nv_reg_native(I, "net_send",       nb_net_send,       2, 2);
    nv_reg_native(I, "net_recv",       nb_net_recv,       1, 1);
    nv_reg_native(I, "net_recv_line",  nb_net_recv_line,  1, 1);
    nv_reg_native(I, "net_close",      nb_net_close,      1, 1);
    nv_reg_native(I, "net_last_error", nb_net_last_error, 0, 0);
}
