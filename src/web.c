/*
 * web.c — HTTP / CGI hosting builtins for Nova.
 *
 * These turn any Nova script into a web application that runs under
 * CGI — the protocol supported by cPanel, Plesk, DirectAdmin, CyberPanel,
 * HestiaCP, Apache, nginx+fcgiwrap and practically every other hosting
 * panel or server:
 *
 *   - http_request()        the incoming request as one map
 *   - http_respond()        emit a status, headers and a body
 *   - http_parse_query()    parse "a=1&b=2" into a map
 *   - url_encode/url_decode percent-encoding helpers
 *   - html_escape           make text safe to embed in HTML
 *
 * Zero third-party dependencies: everything rides on the CGI environment
 * (REQUEST_METHOD, QUERY_STRING, HTTP_* ...) that the web server sets up.
 *
 * Outside CGI (e.g. `nova run demo.nv` in a terminal) http_request()
 * returns a simulated GET request so examples can still be executed and
 * previewed from the command line.
 */
#include "nova.h"

#ifdef _WIN32
#include <windows.h>
#else
extern char **environ;
#endif

/* ================= small helpers ================= */

static NVValue *mk_str(Interp *I, const SB *b) {
    return nv_strl(I, b->p, b->n);
}

static void map_set(Interp *I, NVValue *m, const char *key, NVValue *val) {
    nvtab_set(I, m->as.map, key, val);
}

static void map_set_str(Interp *I, NVValue *m, const char *key, const char *s) {
    nvtab_set(I, m->as.map, key, nv_str(I, s));
}

static void map_set_strl(Interp *I, NVValue *m, const char *key, const char *s, size_t n) {
    nvtab_set(I, m->as.map, key, nv_strl(I, s, n));
}

static void map_set_bool(Interp *I, NVValue *m, const char *key, bool b) {
    nvtab_set(I, m->as.map, key, nv_bool(I, b));
}

static const char *get_env_or(const char *name, const char *dflt) {
    const char *v = getenv(name);
    return (v && *v) ? v : dflt;
}

/* ================= percent / url encoding ================= */

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~';
}

/* decode `s[0..n)` into `out`. When plus_space, '+' decodes to ' ' (query
 * strings); when false it stays literal (path segments, cookie names).
 * Invalid escapes are copied through unchanged. */
static void url_decode_sb(SB *out, const char *s, size_t n, bool plus_space) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '%' && i + 2 < n) {
            int hi = hexval(s[i + 1]), lo = hexval(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                sb_putc(out, (char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (plus_space && c == '+') { sb_putc(out, ' '); continue; }
        sb_putc(out, c);
    }
}

static void url_encode_sb(SB *out, const char *s, size_t n) {
    static const char hexd[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (is_unreserved(c)) {
            sb_putc(out, (char)c);
        } else {
            sb_putc(out, '%');
            sb_putc(out, hexd[c >> 4]);
            sb_putc(out, hexd[c & 15]);
        }
    }
}

static NVValue *nb_url_encode(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "url_encode", 0, a)) return NULL;
    SB b; sb_init(&b);
    url_encode_sb(&b, a[0]->as.s, a[0]->slen);
    NVValue *r = mk_str(I, &b);
    sb_free(&b);
    return r;
}

static NVValue *nb_url_decode(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "url_decode", 0, a)) return NULL;
    SB b; sb_init(&b);
    url_decode_sb(&b, a[0]->as.s, a[0]->slen, true);
    NVValue *r = mk_str(I, &b);
    sb_free(&b);
    return r;
}

/* ================= HTML escaping ================= */

static NVValue *nb_html_escape(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "html_escape", 0, a)) return NULL;
    SB b; sb_init(&b);
    const char *s = a[0]->as.s;
    for (size_t i = 0; i < a[0]->slen; i++) {
        switch (s[i]) {
            case '&':  sb_puts(&b, "&amp;");  break;
            case '<':  sb_puts(&b, "&lt;");   break;
            case '>':  sb_puts(&b, "&gt;");   break;
            case '"':  sb_puts(&b, "&quot;"); break;
            case '\'': sb_puts(&b, "&#39;");  break;
            default:   sb_putc(&b, s[i]);     break;
        }
    }
    NVValue *r = mk_str(I, &b);
    sb_free(&b);
    return r;
}

/* ================= application/x-www-form-urlencoded parsing ================= */

/* Parse "k=v&k2=v2" into map `m`, decoding keys and values. */
static void parse_urlencoded(Interp *I, NVValue *m, const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && s[j] != '&') j++;
        size_t eq = i;
        while (eq < j && s[eq] != '=') eq++;
        SB k, v; sb_init(&k); sb_init(&v);
        url_decode_sb(&k, s + i, eq - i, true);
        if (eq < j) url_decode_sb(&v, s + eq + 1, j - eq - 1, true);
        if (k.n > 0) {
            NVValue *ks = mk_str(I, &k);
            NVValue *vs = mk_str(I, &v);
            nvtab_set(I, m->as.map, ks->as.s, vs);
        }
        sb_free(&k); sb_free(&v);
        i = j + 1;
    }
}

static NVValue *nb_http_parse_query(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "http_parse_query", 0, a)) return NULL;
    NVValue *m = nv_map_new(I);
    parse_urlencoded(I, m, a[0]->as.s, a[0]->slen);
    return m;
}

/* ================= header map from CGI environment ================= */

static void header_from_env(Interp *I, NVValue *headers,
                            const char *name, const char *value) {
    /* HTTP_USER_AGENT -> user_agent ; CONTENT_TYPE -> content_type.
     * Underscore keys (PHP $_SERVER style) so Nova code can use
     * $req->headers->user_agent directly. */
    char key[128];
    size_t k = 0;
    const char *src = name;
    if (strncmp(src, "HTTP_", 5) == 0) src += 5;
    for (; *src && k + 1 < sizeof(key); src++) {
        key[k++] = (char)tolower((unsigned char)*src);
    }
    key[k] = '\0';
    if (k == 0) return;
    map_set_str(I, headers, key, value);
}

static NVValue *collect_headers(Interp *I) {
    NVValue *headers = nv_map_new(I);
#ifdef _WIN32
    char *block = (char *)GetEnvironmentStringsA();
    if (block) {
        const char *p = block;
        while (*p) {
            const char *eq = strchr(p, '=');
            if (eq) {
                char name[128];
                size_t nl = (size_t)(eq - p);
                if (nl < sizeof(name)) {
                    memcpy(name, p, nl); name[nl] = '\0';
                    if (strncmp(name, "HTTP_", 5) == 0 ||
                        strcmp(name, "CONTENT_TYPE") == 0 ||
                        strcmp(name, "CONTENT_LENGTH") == 0)
                        header_from_env(I, headers, name, eq + 1);
                }
            }
            p += strlen(p) + 1;
        }
        FreeEnvironmentStringsA(block);
    }
#else
    for (char **e = environ; *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        char name[128];
        size_t nl = (size_t)(eq - *e);
        if (nl >= sizeof(name)) continue;
        memcpy(name, *e, nl); name[nl] = '\0';
        if (strncmp(name, "HTTP_", 5) == 0 ||
            strcmp(name, "CONTENT_TYPE") == 0 ||
            strcmp(name, "CONTENT_LENGTH") == 0)
            header_from_env(I, headers, name, eq + 1);
    }
#endif
    return headers;
}

/* ================= request body ================= */

#define MAX_BODY (32u * 1024u * 1024u)   /* 32 MiB CGI body cap */

static NVValue *read_body(Interp *I) {
    const char *cl = getenv("CONTENT_LENGTH");
    size_t want = 0;
    bool read_all = false;
    if (cl && *cl) {
        long long v = 0;
        for (const char *p = cl; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
        if (v < 0) v = 0;
        if ((unsigned long long)v > MAX_BODY) v = MAX_BODY;
        want = (size_t)v;
    } else {
        const char *ct = getenv("CONTENT_TYPE");
        const char *rm = getenv("REQUEST_METHOD");
        bool bodyful = ct && *ct && rm &&
            (strcmp(rm, "POST") == 0 || strcmp(rm, "PUT") == 0 ||
             strcmp(rm, "PATCH") == 0 || strcmp(rm, "DELETE") == 0);
        if (!bodyful) return nv_str(I, "");
        read_all = true;                    /* read until EOF */
    }
    SB b; sb_init(&b);
    char chunk[8192];
    size_t got = 0;
    for (;;) {
        size_t cap = sizeof(chunk);
        if (!read_all && got + cap > want) cap = want - got;
        if (cap == 0) break;
        size_t r = fread(chunk, 1, cap, stdin);
        if (r == 0) break;
        sb_putsl(&b, chunk, r);
        got += r;
        if (got >= MAX_BODY) break;
    }
    NVValue *r = mk_str(I, &b);
    sb_free(&b);
    return r;
}

/* ================= http_request() ================= */

static NVValue *nb_http_request(Interp *I, NVValue **a, int n) {
    (void)a; (void)n;
    bool cgi = getenv("GATEWAY_INTERFACE") != NULL;

    const char *method = get_env_or("REQUEST_METHOD", "GET");
    const char *uri    = get_env_or("REQUEST_URI", "");
    const char *qenv   = get_env_or("QUERY_STRING", "");

    /* REQUEST_URI splits into path + query; QUERY_STRING env is
     * authoritative when the server provides both */
    SB path, query;
    sb_init(&path); sb_init(&query);
    const char *qm = strchr(uri, '?');
    if (qm) {
        sb_putsl(&path, uri, (size_t)(qm - uri));
        if (!*qenv) sb_puts(&query, qm + 1);
    } else {
        sb_puts(&path, uri);
    }
    if (query.n == 0 && *qenv) sb_puts(&query, qenv);
    if (path.n == 0) sb_puts(&path, get_env_or("PATH_INFO", "/"));
    if (path.n == 0) sb_putc(&path, '/');

    NVValue *req = nv_map_new(I);
    map_set_str(I, req, "method", method);
    map_set_strl(I, req, "path", path.p, path.n);
    map_set_strl(I, req, "query", query.p, query.n);
    map_set_bool(I, req, "cgi", cgi);

    /* body */
    NVValue *body = nv_str(I, "");
    if (cgi) body = read_body(I);
    map_set(I, req, "body", body);

    /* headers */
    NVValue *headers = cgi ? collect_headers(I) : nv_map_new(I);
    if (!cgi) map_set_str(I, headers, "host", "cli");
    map_set(I, req, "headers", headers);

    /* form: query params + urlencoded body */
    NVValue *form = nv_map_new(I);
    parse_urlencoded(I, form, query.p, query.n);
    const char *ctype = getenv("CONTENT_TYPE");
    if (cgi && ctype && strstr(ctype, "application/x-www-form-urlencoded") &&
        body->type == T_STR)
        parse_urlencoded(I, form, body->as.s, body->slen);
    map_set(I, req, "form", form);

    /* cookies */
    NVValue *cookies = nv_map_new(I);
    const char *rawck = get_env_or("HTTP_COOKIE", "");
    if (*rawck) {
        const char *p = rawck;
        while (*p) {
            const char *semi = strchr(p, ';');
            size_t seg = semi ? (size_t)(semi - p) : strlen(p);
            const char *eq = memchr(p, '=', seg);
            if (eq) {
                SB k; sb_init(&k);
                url_decode_sb(&k, p, (size_t)(eq - p), false);
                if (k.n > 0)
                    map_set_strl(I, cookies, k.p, eq + 1,
                                 (size_t)(p + seg - (eq + 1)));
                sb_free(&k);
            }
            if (!semi) break;
            p = semi + 1;
            while (*p == ' ') p++;
        }
    }
    map_set(I, req, "cookies", cookies);

    /* decoded JSON body when content type says so */
    nvtab_set(I, req->as.map, "json", nv_nil());
    if (cgi && ctype && strstr(ctype, "json") && body->type == T_STR && body->slen > 0) {
        char err[128];
        NVValue *j = nv_json_parse(I, body->as.s, body->slen, err, sizeof(err));
        if (j) nvtab_set(I, req->as.map, "json", j);
    }

    /* convenience scalars */
    map_set_str(I, req, "remote_addr", get_env_or("REMOTE_ADDR", ""));
    map_set_str(I, req, "host", get_env_or("HTTP_HOST", get_env_or("SERVER_NAME", "")));
    const char *https = get_env_or("HTTPS", "");
    map_set_str(I, req, "scheme",
                (*https && strcmp(https, "off") != 0) ? "https" : "http");
    const char *script = get_env_or("SCRIPT_NAME", "");
    map_set_strl(I, req, "script_name", script, strlen(script));

    sb_free(&path); sb_free(&query);
    return req;
}

/* ================= http_respond() ================= */

static const char *reason_phrase(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 418: return "I'm a teapot";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default:  return "Response";
    }
}

/* http_respond(status, body [, headers]) — emit a CGI response */
static NVValue *nb_http_respond(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "http_respond", n, 2, 3)) return NULL;
    if (!nv_arg_num(I, "http_respond", 0, a)) return NULL;
    NVValue *body = nv_to_string(I, a[1]);

    long long code = (a[0]->type == T_INT) ? a[0]->as.i : (long long)a[0]->as.f;
    if (code < 100 || code > 999) code = 200;

    bool have_ctype = false;
    if (n == 3 && nv_arg_map(I, "http_respond", 2, a)) {
        NVTable *t = a[2]->as.map;
        for (size_t k = 0; k < t->on; k++) {
            TEntry *e = &t->ents[t->order[k]];
            if (e->state == 1 && e->key && strcmp(e->key, "Content-Type") == 0)
                have_ctype = true;
        }
    }

    printf("Status: %lld %s\r\n", code, reason_phrase((int)code));

    if (n == 3 && a[2]->type == T_MAP) {
        NVTable *t = a[2]->as.map;
        for (size_t k = 0; k < t->on; k++) {
            TEntry *e = &t->ents[t->order[k]];
            if (e->state != 1 || !e->key) continue;
            NVValue *sv = nv_to_string(I, e->val);
            printf("%s: %.*s\r\n", e->key, (int)sv->slen, sv->as.s);
        }
    }
    if (!have_ctype) printf("Content-Type: text/html; charset=utf-8\r\n");
    printf("\r\n");
    if (body->slen > 0 && code != 204 && code != 304)
        fwrite(body->as.s, 1, body->slen, stdout);
    fflush(stdout);
    return nv_nil();
}

/* ================= registration ================= */

void nv_register_web(Interp *I) {
    nv_reg_native(I, "url_encode",       nb_url_encode,       1, 1);
    nv_reg_native(I, "url_decode",       nb_url_decode,       1, 1);
    nv_reg_native(I, "html_escape",      nb_html_escape,      1, 1);
    nv_reg_native(I, "http_parse_query", nb_http_parse_query, 1, 1);
    nv_reg_native(I, "http_request",     nb_http_request,     0, 0);
    nv_reg_native(I, "http_respond",     nb_http_respond,     2, 3);
}
