/*
 * crypto.c — security primitives for Nova.
 *
 * Pure C11 implementations of the primitives web applications need most:
 *
 *   sha256(data)                  -> lowercase hex digest (SHA-256, FIPS 180-4)
 *   hmac_sha256(key, data)        -> lowercase hex MAC (RFC 2104)
 *   base64_encode(data)           -> base64 text (RFC 4648, with padding)
 *   base64_decode(text)           -> raw bytes as a Nova string (errors throw)
 *   random_bytes(n)               -> n cryptographically random bytes
 *                                    (/dev/urandom, or BCryptGenRandom on
 *                                    Windows; libc rand() is NEVER used)
 *   constant_time_equals(a, b)    -> bool, compares in time independent of
 *                                    content (for token/MAC checks)
 *
 * Everything is self-contained and constant-shape: no lookup tables with
 * secret-dependent indices, no early exits on mismatched MACs.
 */
#include "nova.h"

#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
/* MinGW's headers do not always pull ntstatus.h for STATUS_SUCCESS */
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif
#else
#include <unistd.h>
#include <fcntl.h>
#endif

/* ================= SHA-256 (FIPS 180-4) ================= */

typedef struct {
    unsigned h[8];
    unsigned long long len;
    unsigned char buf[64];
    size_t buflen;
} Sha256;

static unsigned ror(unsigned x, int r) { return (x >> r) | (x << (32 - r)); }

static void sha256_init(Sha256 *c) {
    static const unsigned H0[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(c->h, H0, sizeof(H0));
    c->len = 0;
    c->buflen = 0;
}

static void sha256_block(Sha256 *c, const unsigned char *p) {
    static const unsigned K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    unsigned w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned)p[4 * i] << 24) | ((unsigned)p[4 * i + 1] << 16) |
               ((unsigned)p[4 * i + 2] << 8) | (unsigned)p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        unsigned s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    unsigned a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4];
    unsigned f = c->h[5], g = c->h[6], h = c->h[7], cc = c->h[2];
    for (int i = 0; i < 64; i++) {
        unsigned S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        unsigned ch = (e & f) ^ ((~e) & g);
        unsigned t1 = h + S1 + ch + K[i] + w[i];
        unsigned S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        unsigned mj = (a & b) ^ (a & cc) ^ (b & cc);
        unsigned t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_update(Sha256 *c, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    c->len += n;
    while (n > 0) {
        size_t take = 64 - c->buflen;
        if (take > n) take = n;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        n -= take;
        if (c->buflen == 64) {
            sha256_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

static void sha256_final(Sha256 *c, unsigned char out[32]) {
    unsigned long long bits = c->len * 8;   /* capture before padding */
    unsigned char pad = 0x80;
    sha256_update(c, &pad, 1);
    unsigned char z = 0;
    while (c->buflen != 56) sha256_update(c, &z, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_update(c, lenb, 8);              /* final block; c->len is now stale */
    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (unsigned char)(c->h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(c->h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(c->h[i] >> 8);
        out[4 * i + 3] = (unsigned char)(c->h[i]);
    }
}

/* one-shot */
static void sha256(const void *data, size_t n, unsigned char out[32]) {
    Sha256 c;
    sha256_init(&c);
    sha256_update(&c, data, n);
    sha256_final(&c, out);
}

/* Streaming SHA-256 for the package manager's registry checksums (pkg.c).
 * The heap context keeps the Sha256 layout private to this file:
 *   void *ctx = nv_sha256_new();
 *   nv_sha256_feed(ctx, data, n);   (any number of times)
 *   nv_sha256_hex(ctx, hex64);      (finalizes AND frees ctx) */
void *nv_sha256_new(void) {
    Sha256 *c = (Sha256 *)malloc(sizeof(Sha256));
    if (c) sha256_init(c);
    return c;
}

void nv_sha256_feed(void *ctx, const void *data, size_t n) {
    if (ctx) sha256_update((Sha256 *)ctx, data, n);
}

void nv_sha256_hex(void *ctx, char out[65]) {
    if (!ctx) { out[0] = '\0'; return; }
    unsigned char dig[32];
    sha256_final((Sha256 *)ctx, dig);
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = hexd[dig[i] >> 4];
        out[i * 2 + 1] = hexd[dig[i] & 15];
    }
    out[64] = '\0';
    free(ctx);
}

/* ================= hex helper ================= */

static NVValue *bytes_to_hex(Interp *I, const unsigned char *b, size_t n) {
    static const char hexd[] = "0123456789abcdef";
    SB s;
    sb_init(&s);
    for (size_t i = 0; i < n; i++) {
        sb_putc(&s, hexd[b[i] >> 4]);
        sb_putc(&s, hexd[b[i] & 15]);
    }
    NVValue *r = nv_strl(I, s.p, s.n);
    sb_free(&s);
    return r;
}

/* ================= HMAC-SHA256 (RFC 2104) ================= */

static void hmac_sha256(const unsigned char *key, size_t klen,
                        const void *msg, size_t mlen,
                        unsigned char out[32]) {
    unsigned char k[64], ip[64], op[64], khash[32], mac[32];
    memset(k, 0, sizeof(k));
    if (klen > 64)
        sha256(key, klen, khash);
    else
        memcpy(k, key, klen);
    const unsigned char *kk = (klen > 64) ? khash : k;
    for (int i = 0; i < 64; i++) {
        ip[i] = kk[i] ^ 0x36;
        op[i] = kk[i] ^ 0x5c;
    }
    Sha256 inner;
    sha256_init(&inner);
    sha256_update(&inner, ip, 64);
    sha256_update(&inner, msg, mlen);
    unsigned char ih[32];
    sha256_final(&inner, ih);
    Sha256 outer;
    sha256_init(&outer);
    sha256_update(&outer, op, 64);
    sha256_update(&outer, ih, 32);
    sha256_final(&outer, mac);
    memcpy(out, mac, 32);
}

/* ================= base64 (RFC 4648) ================= */

static NVValue *nb_base64_encode(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "base64_encode", 0, a)) return NULL;
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *in = (const unsigned char *)a[0]->as.s;
    size_t len = a[0]->slen;
    SB out;
    sb_init(&out);
    for (size_t i = 0; i < len; i += 3) {
        unsigned b0 = in[i];
        unsigned b1 = (i + 1 < len) ? in[i + 1] : 0;
        unsigned b2 = (i + 2 < len) ? in[i + 2] : 0;
        sb_putc(&out, tbl[b0 >> 2]);
        sb_putc(&out, tbl[((b0 & 3) << 4) | (b1 >> 4)]);
        sb_putc(&out, (i + 1 < len) ? tbl[((b1 & 15) << 2) | (b2 >> 6)] : '=');
        sb_putc(&out, (i + 2 < len) ? tbl[b2 & 63] : '=');
    }
    NVValue *r = nv_strl(I, out.p, out.n);
    sb_free(&out);
    return r;
}

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static NVValue *nb_base64_decode(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "base64_decode", 0, a)) return NULL;
    const char *s = a[0]->as.s;
    size_t len = a[0]->slen;
    SB out;
    sb_init(&out);
    unsigned acc = 0;
    int bits = 0;
    bool bad = false;
    for (size_t i = 0; i < len && !bad; i++) {
        char c = s[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int v = b64_val(c);
        if (v < 0) { bad = true; break; }
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            sb_putc(&out, (char)((acc >> bits) & 0xff));
        }
    }
    if (bad) {
        sb_free(&out);
        nv_err(I, "base64_decode: invalid character in input");
        return NULL;
    }
    NVValue *r = nv_strl(I, out.p, out.n);
    sb_free(&out);
    return r;
}

/* ================= random_bytes ================= */

static NVValue *nb_random_bytes(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_int(I, "random_bytes", 0, a)) return NULL;
    long long want = a[0]->as.i;
    if (want < 0) {
        nv_err(I, "random_bytes() count %lld must be >= 0", want);
        return NULL;
    }
    if (want > 65536) {
        nv_err(I, "random_bytes() count %lld exceeds the 65536 cap", want);
        return NULL;
    }
    size_t need = (size_t)want;
    SB out;
    sb_init(&out);
    unsigned char chunk[4096];
    size_t got = 0;
    while (got < need) {
        size_t take = need - got;
        if (take > sizeof(chunk)) take = sizeof(chunk);
#ifdef _WIN32
        if (BCryptGenRandom(NULL, (PUCHAR)chunk, (ULONG)take,
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != STATUS_SUCCESS) {
            sb_free(&out);
            nv_err(I, "random_bytes: BCryptGenRandom failed");
            return NULL;
        }
#else
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0) {
            sb_free(&out);
            nv_err(I, "random_bytes: cannot open /dev/urandom");
            return NULL;
        }
        size_t filled = 0;
        while (filled < take) {
            ssize_t r = read(fd, chunk + filled, take - filled);
            if (r <= 0) {
                close(fd);
                sb_free(&out);
                nv_err(I, "random_bytes: read from /dev/urandom failed");
                return NULL;
            }
            filled += (size_t)r;
        }
        close(fd);
#endif
        sb_putsl(&out, (const char *)chunk, take);
        got += take;
    }
    return nv_strl(I, out.p, out.n);
}

/* ================= constant_time_equals ================= */

static NVValue *nb_constant_time_equals(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "constant_time_equals", 0, a) ||
        !nv_arg_str(I, "constant_time_equals", 1, a)) return NULL;
    const unsigned char *x = (const unsigned char *)a[0]->as.s;
    const unsigned char *y = (const unsigned char *)a[1]->as.s;
    size_t xl = a[0]->slen, yl = a[1]->slen;
    size_t m = xl > yl ? xl : yl;
    unsigned char diff = (xl == yl) ? 0 : 1;
    for (size_t i = 0; i < m; i++) {
        unsigned char xv = i < xl ? x[i] : 0;
        unsigned char yv = i < yl ? y[i] : 0;
        diff |= (unsigned char)(xv ^ yv);
    }
    return nv_bool(I, diff == 0);
}

/* ================= registration ================= */

static NVValue *nb_sha256(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "sha256", 0, a)) return NULL;
    unsigned char d[32];
    sha256(a[0]->as.s, a[0]->slen, d);
    return bytes_to_hex(I, d, 32);
}

static NVValue *nb_hmac_sha256(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "hmac_sha256", 0, a) ||
        !nv_arg_str(I, "hmac_sha256", 1, a)) return NULL;
    unsigned char d[32];
    hmac_sha256((const unsigned char *)a[0]->as.s, a[0]->slen,
                a[1]->as.s, a[1]->slen, d);
    return bytes_to_hex(I, d, 32);
}

void nv_register_crypto(Interp *I) {
    nv_reg_native(I, "sha256",               nb_sha256,               1, 1);
    nv_reg_native(I, "hmac_sha256",          nb_hmac_sha256,          2, 2);
    nv_reg_native(I, "base64_encode",        nb_base64_encode,        1, 1);
    nv_reg_native(I, "base64_decode",        nb_base64_decode,        1, 1);
    nv_reg_native(I, "random_bytes",         nb_random_bytes,         1, 1);
    nv_reg_native(I, "constant_time_equals", nb_constant_time_equals, 2, 2);
}
