/*
 * mail.c — e-mail builtins for Nova.
 *
 * Two transports, both implemented with plain C11 and OS APIs only:
 *
 *   - mail_send(opts)   hand the message to the local sendmail binary
 *                       (sendmail / exim / postfix all provide one —
 *                       this is how PHP's mail() works on cPanel).
 *   - smtp_send(opts)   talk SMTP directly over TCP (plain, no TLS —
 *                       use a local smarthost or port 25; TLS without
 *                       third-party libraries is out of Nova's scope).
 *   - mail_last_error() human-readable failure reason after a false return.
 *
 * Common option map for both:
 *   { "from": req, "to": str|array req, "subject": str,
 *     "text": str, "html": str, "cc": str|array, "bcc": str|array,
 *     "reply_to": str, "headers": map,                     (both)
 *     "host": str, "port": int, "user": str, "pass": str } (smtp_send)
 */
/* expose POSIX APIs (addrinfo, popen, access) under strict -std=c11 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "nova.h"

#include <errno.h>
#include <time.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET nv_sock_t;
  #define NV_SOCK_INVALID  INVALID_SOCKET
  #define nv_close_sock(s) closesocket(s)
#else
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <netdb.h>
  #include <unistd.h>
  typedef int nv_sock_t;
  #define NV_SOCK_INVALID  (-1)
  #define nv_close_sock(s) close(s)
#endif

/* ================= shared state ================= */

static char g_mail_error[256];

static void mail_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_mail_error, sizeof(g_mail_error), fmt, ap);
    va_end(ap);
}

/* forward declarations used by write_message */
static void fprintf_or_sb(SB *out, FILE *f, const char *fmt, ...);
static void fputs_or_sb(SB *out, FILE *f, const char *s);

/* ================= option map helpers ================= */

static const char *opt_str(NVValue *opts, const char *key) {
    NVValue *v = NULL;
    if (!nvtab_get(opts->as.map, key, &v)) return NULL;
    if (v->type != T_STR || v->slen == 0) return NULL;
    return v->as.s;
}

/* "to"/"cc"/"bcc" accept a string or an array of strings; append to SB */
static bool opt_addrs(NVValue *opts, const char *key, SB *out, int *count) {
    NVValue *v = NULL;
    if (!nvtab_get(opts->as.map, key, &v)) return true;   /* absent = fine */
    if (v->type == T_STR) {
        if (v->slen == 0) return true;
        if (out->n > 0) sb_puts(out, ", ");
        sb_putsl(out, v->as.s, v->slen);
        (*count)++;
        return true;
    }
    if (v->type == T_ARR) {
        for (size_t i = 0; i < v->as.arr->n; i++) {
            NVValue *e = v->as.arr->items[i];
            if (e->type != T_STR || e->slen == 0) continue;
            if (out->n > 0) sb_puts(out, ", ");
            sb_putsl(out, e->as.s, e->slen);
            (*count)++;
        }
        return true;
    }
    mail_err("option \"%s\" must be a string or an array of strings", key);
    return false;
}

static const char *opt_body_text(NVValue *opts) {
    const char *t = opt_str(opts, "text");
    return t ? t : opt_str(opts, "body");
}

/* ================= CRLF sanitation =================
 *
 * Every header field below can carry user input (a web form's subject line,
 * an address typed into a contact form). A bare CR or LF would terminate the
 * header and let an attacker inject Bcc:, extra bodies or — over SMTP —
 * whole commands like "RCPT TO:<victim>" (email injection / spoofing).
 * Nova strips CR, LF and NUL from all fields before they reach sendmail or
 * the SMTP wire. */

static bool has_crlf(const char *s) {
    for (; *s; s++)
        if (*s == '\r' || *s == '\n' || *s == '\0') return true;
    return false;
}

/* copy s into out with CR/LF/NUL removed */
static void clean_line(SB *out, const char *s) {
    for (; *s; s++) {
        if (*s == '\r' || *s == '\n' || *s == '\0') continue;
        sb_putc(out, *s);
    }
}

/* strip CR/LF/NUL from an already-built recipient list in place */
static void sb_strip_crlf(SB *b) {
    if (!b->p || b->n == 0) return;      /* nothing collected yet */
    size_t o = 0;
    for (size_t i = 0; i < b->n; i++) {
        char c = b->p[i];
        if (c == '\r' || c == '\n' || c == '\0') continue;
        b->p[o++] = c;
    }
    b->n = o;
    b->p[o] = '\0';
}

/* ================= message assembly ================= */

#define MIME_BOUNDARY "=_nova_2b7f9d51e3"

/* Write the RFC 5322 message (headers + body) to `f` (sendmail path) or
 * into `out` (SMTP DATA path). `with_bcc` embeds the Bcc header — sendmail
 * -t consumes it and strips it; with SMTP the envelope is built from RCPT
 * TO, so the header must stay out. */
static bool write_message(Interp *I, NVValue *opts, SB *out, FILE *f, bool with_bcc) {
    const char *from    = opt_str(opts, "from");
    const char *subject = opt_str(opts, "subject");
    const char *reply   = opt_str(opts, "reply_to");
    const char *text    = opt_body_text(opts);
    const char *html    = opt_str(opts, "html");

    SB to, cc, bcc;
    sb_init(&to); sb_init(&cc); sb_init(&bcc);
    int n_to = 0, n_cc = 0, n_bcc = 0;
    bool ok = opt_addrs(opts, "to", &to, &n_to) &&
              opt_addrs(opts, "cc", &cc, &n_cc) &&
              opt_addrs(opts, "bcc", &bcc, &n_bcc);
    if (!ok) { sb_free(&to); sb_free(&cc); sb_free(&bcc); return false; }
    sb_strip_crlf(&to);
    sb_strip_crlf(&cc);
    sb_strip_crlf(&bcc);

    /* sanitized header fields (CR/LF/NUL removed) */
    SB from_s, subj_s, reply_s;
    sb_init(&from_s); sb_init(&subj_s); sb_init(&reply_s);
    if (from) clean_line(&from_s, from);
    if (subject) clean_line(&subj_s, subject);
    if (reply) clean_line(&reply_s, reply);
    const char *from_c = from_s.p;
    const char *subject_c = subj_s.p;
    const char *reply_c = reply_s.p;

    if (!from_c || !*from_c) {
        mail_err("missing required option \"from\"");
        sb_free(&to); sb_free(&cc); sb_free(&bcc);
        sb_free(&from_s); sb_free(&subj_s); sb_free(&reply_s);
        return false;
    }
    if (n_to + n_cc + n_bcc == 0) {
        mail_err("no recipients: set \"to\", \"cc\" or \"bcc\"");
        sb_free(&to); sb_free(&cc); sb_free(&bcc);
        sb_free(&from_s); sb_free(&subj_s); sb_free(&reply_s);
        return false;
    }

    char date[80];
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S +0000", &tmv);

    fprintf_or_sb(out, f, "From: %s\r\n", from_c);
    if (to.n > 0)  fprintf_or_sb(out, f, "To: %s\r\n", to.p);
    if (cc.n > 0)  fprintf_or_sb(out, f, "Cc: %s\r\n", cc.p);
    if (with_bcc && bcc.n > 0) fprintf_or_sb(out, f, "Bcc: %s\r\n", bcc.p);
    fprintf_or_sb(out, f, "Subject: %s\r\n", subject_c ? subject_c : "");
    if (reply_c && reply_c[0]) fprintf_or_sb(out, f, "Reply-To: %s\r\n", reply_c);
    fprintf_or_sb(out, f, "Date: %s\r\n", date);
    fprintf_or_sb(out, f, "MIME-Version: 1.0\r\n");
    fprintf_or_sb(out, f, "X-Mailer: Nova/%s mail builtins\r\n", NOVA_VERSION);

    /* extra headers from the options map */
    NVValue *hmap = NULL;
    if (nvtab_get(opts->as.map, "headers", &hmap) && hmap->type == T_MAP) {
        NVTable *t = hmap->as.map;
        for (size_t k = 0; k < t->on; k++) {
            TEntry *e = &t->ents[t->order[k]];
            if (e->state != 1 || !e->key) continue;
            NVValue *sv = nv_to_string(I, e->val);
            SB kclean, vclean;
            sb_init(&kclean); sb_init(&vclean);
            clean_line(&kclean, e->key);
            clean_line(&vclean, sv->as.s);
            if (kclean.n > 0)
                fprintf_or_sb(out, f, "%s: %s\r\n", kclean.p, vclean.p);
            sb_free(&kclean); sb_free(&vclean);
        }
    }

    if (text && html) {
        /* multipart/alternative: plain-text fallback + HTML body */
        fprintf_or_sb(out, f, "Content-Type: multipart/alternative; boundary=\"%s\"\r\n\r\n",
                      MIME_BOUNDARY);
        fprintf_or_sb(out, f, "--%s\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n",
                      MIME_BOUNDARY);
        fputs_or_sb(out, f, text);
        fprintf_or_sb(out, f, "\r\n--%s\r\nContent-Type: text/html; charset=utf-8\r\n\r\n",
                      MIME_BOUNDARY);
        fputs_or_sb(out, f, html);
        fprintf_or_sb(out, f, "\r\n--%s--\r\n", MIME_BOUNDARY);
    } else if (html) {
        fprintf_or_sb(out, f, "Content-Type: text/html; charset=utf-8\r\n\r\n");
        fputs_or_sb(out, f, html);
    } else {
        fprintf_or_sb(out, f, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
        fputs_or_sb(out, f, text ? text : "");
    }

    sb_free(&to); sb_free(&cc); sb_free(&bcc);
    sb_free(&from_s); sb_free(&subj_s); sb_free(&reply_s);
    return true;
}

/* printf into FILE* and/or SB — used by write_message */
static void fprintf_or_sb(SB *out, FILE *f, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (f) fputs(buf, f);
    if (out) sb_puts(out, buf);
}

static void fputs_or_sb(SB *out, FILE *f, const char *s) {
    if (f) fputs(s, f);
    if (out) sb_puts(out, s);
}

/* ================= sendmail transport ================= */

static NVValue *nb_mail_send(Interp *I, NVValue **a, int n) {
    (void)I; (void)n;
    g_mail_error[0] = '\0';
    if (!nv_arg_map(I, "mail_send", 0, a)) return NULL;

#ifdef _WIN32
    (void)a;
    mail_err("mail_send() needs a local sendmail binary; not available on Windows — use smtp_send()");
    return nv_bool(I, false);
#else
    const char *cand[4];
    const char *env = getenv("NOVA_SENDMAIL");
    cand[0] = env && *env ? env : "/usr/sbin/sendmail";
    cand[1] = "/usr/lib/sendmail";
    cand[2] = "/usr/bin/sendmail";
    cand[3] = "/usr/local/bin/sendmail";

    const char *bin = NULL;
    for (int i = 0; i < 4; i++) {
        if (access(cand[i], X_OK) == 0) { bin = cand[i]; break; }
    }
    if (!bin) {
        mail_err("no local sendmail found (tried NOVA_SENDMAIL, /usr/sbin, /usr/lib, /usr/bin)");
        return nv_bool(I, false);
    }

    SB msg; sb_init(&msg);
    if (!write_message(I, a[0], &msg, NULL, true)) { sb_free(&msg); return nv_bool(I, false); }

    /* popen runs through /bin/sh -c, so quote the binary path; -t makes
     * sendmail take recipients from To/Cc/Bcc headers (and strip Bcc). */
    char cmd[600];
    if (snprintf(cmd, sizeof(cmd), "'%s' -t", bin) >= (int)sizeof(cmd)) {
        mail_err("sendmail path too long");
        sb_free(&msg);
        return nv_bool(I, false);
    }
    FILE *m = popen(cmd, "w");
    if (!m) {
        mail_err("popen(\"%s\") failed: %s", cmd, strerror(errno));
        sb_free(&msg);
        return nv_bool(I, false);
    }
    fwrite(msg.p, 1, msg.n, m);
    int rc = pclose(m);
    sb_free(&msg);
    if (rc != 0) {
        mail_err("sendmail exited with status %d", rc);
        return nv_bool(I, false);
    }
    return nv_bool(I, true);
#endif
}

/* ================= SMTP transport ================= */

#define SMTP_TIMEOUT_SEC 30

/* --- base64 (AUTH LOGIN) --- */
static void b64_encode(const char *in, size_t n, SB *out) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 3) {
        unsigned b0 = (unsigned char)in[i];
        unsigned b1 = (i + 1 < n) ? (unsigned char)in[i + 1] : 0;
        unsigned b2 = (i + 2 < n) ? (unsigned char)in[i + 2] : 0;
        sb_putc(out, tbl[b0 >> 2]);
        sb_putc(out, tbl[((b0 & 3) << 4) | (b1 >> 4)]);
        sb_putc(out, (i + 1 < n) ? tbl[((b1 & 15) << 2) | (b2 >> 6)] : '=');
        sb_putc(out, (i + 2 < n) ? tbl[b2 & 63] : '=');
    }
}

/* --- socket plumbing --- */

static nv_sock_t smtp_connect(const char *host, int port) {
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        mail_err("cannot resolve host \"%s\"", host);
        return NV_SOCK_INVALID;
    }
    nv_sock_t sock = NV_SOCK_INVALID;
    for (rp = res; rp; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == NV_SOCK_INVALID) continue;
        struct timeval tv = { SMTP_TIMEOUT_SEC, 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) break;
        nv_close_sock(sock);
        sock = NV_SOCK_INVALID;
    }
    freeaddrinfo(res);
    if (sock == NV_SOCK_INVALID) mail_err("cannot connect to %s:%d", host, port);
    return sock;
}

/* Read one SMTP reply (handles multiline "250-..." continuations).
 * Returns the reply code or -1 on error; full text appended to `log`. */
static int smtp_reply(nv_sock_t sock, SB *log) {
    char line[1024];
    for (;;) {
        size_t n = 0;
        int c = 0;
        while (n + 1 < sizeof(line)) {
            int r = (int)recv(sock, (char *)&c, 1, 0);
            if (r <= 0) return -1;
            if (c == '\n') break;
            if (c != '\r') line[n++] = (char)c;
        }
        line[n] = '\0';
        if (log) { sb_puts(log, line); sb_puts(log, "\r\n"); }
        if (n >= 3 && line[0] >= '0' && line[0] <= '9' &&
            line[1] >= '0' && line[1] <= '9' && line[2] >= '0' && line[2] <= '9') {
            int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
            if (n == 3 || line[3] != '-') return code;   /* final line */
        } else {
            return -1;   /* garbage reply */
        }
    }
}

/* Send one command line (must end with "\r\n") and check its reply. */
static bool smtp_cmd_raw(nv_sock_t sock, const char *line, int expect, SB *log) {
    size_t len = strlen(line);
    size_t off = 0;
    while (off < len) {
        int r = (int)send(sock, line + off, (int)(len - off), 0);
        if (r <= 0) { mail_err("connection lost while sending \"%s\"", line); return false; }
        off += (size_t)r;
    }
    int code = smtp_reply(sock, log);
    if (code != expect) {
        mail_err("SMTP: expected %d, got %d", expect, code);
        return false;
    }
    return true;
}

/* send DATA payload with dot-stuffing, then expect 250 */
static bool smtp_data(nv_sock_t sock, const char *msg, size_t len, SB *log) {
    SB chunk; sb_init(&chunk);
    /* dot-stuffing: any line starting with '.' gets another '.' prefix */
    sb_putc(&chunk, '.');
    for (size_t i = 0; i < len; i++) {
        sb_putc(&chunk, msg[i]);
        if (msg[i] == '\n' && i + 1 < len && msg[i + 1] == '.')
            sb_putc(&chunk, '.');
    }
    sb_puts(&chunk, "\r\n.\r\n");
    size_t off = 0;
    bool ok = true;
    while (off < chunk.n) {
        int r = (int)send(sock, chunk.p + off, (int)(chunk.n - off), 0);
        if (r <= 0) { ok = false; break; }
        off += (size_t)r;
    }
    sb_free(&chunk);
    if (!ok) { mail_err("connection lost sending message body"); return false; }
    int code = smtp_reply(sock, log);
    if (code != 250) { mail_err("SMTP: DATA rejected (code %d)", code); return false; }
    return true;
}

static NVValue *nb_smtp_send(Interp *I, NVValue **a, int n) {
    (void)n;
    g_mail_error[0] = '\0';
    if (!nv_arg_map(I, "smtp_send", 0, a)) return NULL;
    NVValue *opts = a[0];

    const char *host = opt_str(opts, "host");
    if (!host) { mail_err("missing required option \"host\""); return nv_bool(I, false); }
    const char *from = opt_str(opts, "from");
    if (!from) { mail_err("missing required option \"from\""); return nv_bool(I, false); }

    long long port = 25;
    NVValue *pv = NULL;
    if (nvtab_get(opts->as.map, "port", &pv) && pv->type == T_INT)
        port = pv->as.i;
    if (port < 1 || port > 65535) port = 25;

    /* build the message first (envelope recipients come from the same opts) */
    SB msg; sb_init(&msg);
    if (!write_message(I, opts, &msg, NULL, false)) { sb_free(&msg); return nv_bool(I, false); }

    SB log; sb_init(&log);
    bool ok = true;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        mail_err("WSAStartup failed");
        ok = false;
    }
#endif

    nv_sock_t sock = NV_SOCK_INVALID;
    if (ok) {
        sock = smtp_connect(host, (int)port);
        if (sock == NV_SOCK_INVALID) ok = false;
    }

    char cmd[600];
    if (ok) {
        int code = smtp_reply(sock, &log);          /* banner 220 */
        if (code != 220) {
            mail_err("SMTP: bad greeting from %s:%lld (code %d)", host, port, code);
            ok = false;
        }
    }
    if (ok) {
        /* EHLO first, fall back to HELO for ancient servers */
        if (!smtp_cmd_raw(sock, "EHLO nova\r\n", 250, &log) &&
            !smtp_cmd_raw(sock, "HELO nova\r\n", 250, &log))
            ok = false;
    }
    if (ok) {
        const char *user = opt_str(opts, "user");
        const char *pass = opt_str(opts, "pass");
        if (user && pass) {
            SB u64, p64;
            sb_init(&u64); sb_init(&p64);
            b64_encode(user, strlen(user), &u64);
            b64_encode(pass, strlen(pass), &p64);
            sb_puts(&u64, "\r\n"); sb_puts(&p64, "\r\n");
            ok = smtp_cmd_raw(sock, "AUTH LOGIN\r\n", 334, &log) &&
                 smtp_cmd_raw(sock, u64.p, 334, &log) &&
                 smtp_cmd_raw(sock, p64.p, 235, &log);
            sb_free(&u64); sb_free(&p64);
        }
    }
    if (ok) {
        if (has_crlf(from)) {
            mail_err("option \"from\" must not contain CR or LF characters");
            ok = false;
        } else if (strchr(from, '<') == NULL)
            snprintf(cmd, sizeof(cmd), "MAIL FROM:<%s>\r\n", from);
        else
            snprintf(cmd, sizeof(cmd), "MAIL FROM:%s\r\n", from);
        if (ok) ok = smtp_cmd_raw(sock, cmd, 250, &log);
    }
    if (ok) {
        /* one RCPT TO per recipient across to / cc / bcc */
        const char *keys[3] = { "to", "cc", "bcc" };
        for (int ki = 0; ki < 3 && ok; ki++) {
            NVValue *v = NULL;
            if (!nvtab_get(opts->as.map, keys[ki], &v)) continue;
            if (v->type == T_STR) {
                if (v->slen == 0) continue;
                if (has_crlf(v->as.s)) {
                    mail_err("address in \"%s\" must not contain CR or LF", keys[ki]);
                    ok = false;
                    break;
                }
                if (strchr(v->as.s, '<'))
                    snprintf(cmd, sizeof(cmd), "RCPT TO:%s\r\n", v->as.s);
                else
                    snprintf(cmd, sizeof(cmd), "RCPT TO:<%s>\r\n", v->as.s);
                ok = smtp_cmd_raw(sock, cmd, 250, &log);
                if (!ok) mail_err("SMTP: RCPT TO rejected for \"%s\"", keys[ki]);
            } else if (v->type == T_ARR) {
                for (size_t i = 0; i < v->as.arr->n && ok; i++) {
                    NVValue *e = v->as.arr->items[i];
                    if (e->type != T_STR || e->slen == 0) continue;
                    if (has_crlf(e->as.s)) {
                        mail_err("address in \"%s\" must not contain CR or LF", keys[ki]);
                        ok = false;
                        break;
                    }
                    if (strchr(e->as.s, '<'))
                        snprintf(cmd, sizeof(cmd), "RCPT TO:%s\r\n", e->as.s);
                    else
                        snprintf(cmd, sizeof(cmd), "RCPT TO:<%s>\r\n", e->as.s);
                    ok = smtp_cmd_raw(sock, cmd, 250, &log);
                    if (!ok) mail_err("SMTP: RCPT TO rejected for \"%s\"", keys[ki]);
                }
            } else {
                mail_err("option \"%s\" must be a string or an array of strings", keys[ki]);
                ok = false;
            }
        }
    }
    if (ok) {
        ok = smtp_cmd_raw(sock, "DATA\r\n", 354, &log) &&
             smtp_data(sock, msg.p, msg.n, &log);
    }
    if (ok) smtp_cmd_raw(sock, "QUIT\r\n", 221, &log);   /* best effort */

    if (sock != NV_SOCK_INVALID) nv_close_sock(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    sb_free(&msg); sb_free(&log);
    return nv_bool(I, ok);
}

/* ================= mail_last_error() ================= */

static NVValue *nb_mail_last_error(Interp *I, NVValue **a, int n) {
    (void)a; (void)n;
    return nv_str(I, g_mail_error);
}

/* ================= registration ================= */

void nv_register_mail(Interp *I) {
    nv_reg_native(I, "mail_send",       nb_mail_send,       1, 1);
    nv_reg_native(I, "smtp_send",       nb_smtp_send,       1, 1);
    nv_reg_native(I, "mail_last_error", nb_mail_last_error, 0, 0);
}
