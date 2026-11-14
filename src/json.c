/*
 * json.c — minimal JSON parser for Nova.
 *
 * Parses JSON text into arena-backed Nova values:
 *   object -> map, array -> array, string -> string,
 *   number -> int (when integral) or float, true/false -> bool, null -> nil.
 *
 * Used by the `json_decode()` builtin and by the package manager
 * (`nova.json` manifests, `~/.nova/config`).
 */
#include "nova.h"
#include <errno.h>

#define JSON_MAX_DEPTH 64

typedef struct {
    Interp   *I;
    const char *p, *end;
    char     *err;
    size_t    errsz;
    int       depth;
    int       line;
} JP;

static void jerr(JP *j, const char *fmt, ...) {
    if (j->err[0]) return;                 /* keep the first error */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(j->err, j->errsz, fmt, ap);
    va_end(ap);
}

static void skip_ws(JP *j) {
    while (j->p < j->end) {
        char c = *j->p;
        if (c == '\n') { j->line++; j->p++; }
        else if (c == ' ' || c == '\t' || c == '\r') j->p++;
        else break;
    }
}

static void utf8_emit(SB *b, unsigned cp) {
    if (cp < 0x80) {
        sb_putc(b, (char)cp);
    } else if (cp < 0x800) {
        sb_putc(b, (char)(0xC0 | (cp >> 6)));
        sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_putc(b, (char)(0xE0 | (cp >> 12)));
        sb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_putc(b, (char)(0xF0 | (cp >> 18)));
        sb_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

/* read \uXXXX (returning -1 on error); combines surrogate pairs */
static int parse_unicode_escape(JP *j, SB *b) {
    unsigned cp = 0;
    for (int k = 0; k < 4; k++) {
        if (j->p >= j->end || !isxdigit((unsigned char)*j->p)) {
            jerr(j, "invalid \\u escape at line %d", j->line);
            return -1;
        }
        char c = *j->p++;
        int d = isdigit((unsigned char)c) ? c - '0'
              : (tolower((unsigned char)c) - 'a' + 10);
        cp = cp * 16 + (unsigned)d;
    }
    if (cp >= 0xD800 && cp <= 0xDBFF && j->p + 1 < j->end &&
        j->p[0] == '\\' && j->p[1] == 'u') {
        j->p += 2;
        unsigned lo = 0;
        for (int k = 0; k < 4; k++) {
            if (j->p >= j->end || !isxdigit((unsigned char)*j->p)) {
                jerr(j, "invalid surrogate \\u escape at line %d", j->line);
                return -1;
            }
            char c = *j->p++;
            int d = isdigit((unsigned char)c) ? c - '0'
                  : (tolower((unsigned char)c) - 'a' + 10);
            lo = lo * 16 + (unsigned)d;
        }
        if (lo >= 0xDC00 && lo <= 0xDFFF)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
    }
    return (int)cp;
}

static bool parse_string_raw(JP *j, SB *b) {
    if (j->p >= j->end || *j->p != '"') return false;
    j->p++;
    while (j->p < j->end) {
        char c = *j->p++;
        if (c == '"') return true;
        if (c == '\\') {
            if (j->p >= j->end) { jerr(j, "unterminated escape at line %d", j->line); return false; }
            char e = *j->p++;
            switch (e) {
                case '"':  sb_putc(b, '"');  break;
                case '\\': sb_putc(b, '\\'); break;
                case '/':  sb_putc(b, '/');  break;
                case 'b':  sb_putc(b, '\b'); break;
                case 'f':  sb_putc(b, '\f'); break;
                case 'n':  sb_putc(b, '\n'); break;
                case 'r':  sb_putc(b, '\r'); break;
                case 't':  sb_putc(b, '\t'); break;
                case 'u': {
                    int cp = parse_unicode_escape(j, b);
                    if (cp < 0) return false;
                    utf8_emit(b, (unsigned)cp);
                    break;
                }
                default:
                    jerr(j, "invalid escape '\\%c' at line %d", e, j->line);
                    return false;
            }
            continue;
        }
        if (c == '\n') j->line++;
        sb_putc(b, c);
    }
    jerr(j, "unterminated string at line %d", j->line);
    return false;
}

static NVValue *parse_value(JP *j);

static NVValue *parse_object(JP *j) {
    if (++j->depth > JSON_MAX_DEPTH) {
        jerr(j, "JSON nesting too deep (max %d)", JSON_MAX_DEPTH);
        j->depth--;
        return NULL;
    }
    NVValue *m = nv_map_new(j->I);
    j->p++;                                    /* consume '{' */
    skip_ws(j);
    if (j->p < j->end && *j->p == '}') { j->p++; j->depth--; return m; }
    for (;;) {
        skip_ws(j);
        SB kb; sb_init(&kb);
        if (!parse_string_raw(j, &kb)) { sb_free(&kb); break; }
        skip_ws(j);
        if (j->p >= j->end || *j->p != ':') {
            jerr(j, "expected ':' after object key at line %d", j->line);
            sb_free(&kb);
            break;
        }
        j->p++;
        NVValue *v = parse_value(j);
        if (!v) { sb_free(&kb); break; }
        /* nvtab_set copies the key into a table-owned GC block, so the
         * scratch buffer can be released right after the call */
        nvtab_set(j->I, m->as.map, kb.p ? kb.p : "", v);
        sb_free(&kb);
        skip_ws(j);
        if (j->p < j->end && *j->p == ',') { j->p++; continue; }
        if (j->p < j->end && *j->p == '}') { j->p++; j->depth--; return m; }
        jerr(j, "expected ',' or '}' in object at line %d", j->line);
        break;
    }
    j->depth--;
    return NULL;
}

static NVValue *parse_array(JP *j) {
    if (++j->depth > JSON_MAX_DEPTH) {
        jerr(j, "JSON nesting too deep (max %d)", JSON_MAX_DEPTH);
        j->depth--;
        return NULL;
    }
    NVValue *a = nv_arr_new(j->I);
    j->p++;                                    /* consume '[' */
    skip_ws(j);
    if (j->p < j->end && *j->p == ']') { j->p++; j->depth--; return a; }
    for (;;) {
        NVValue *v = parse_value(j);
        if (!v) break;
        nv_arr_push(j->I, a, v);
        skip_ws(j);
        if (j->p < j->end && *j->p == ',') { j->p++; continue; }
        if (j->p < j->end && *j->p == ']') { j->p++; j->depth--; return a; }
        jerr(j, "expected ',' or ']' in array at line %d", j->line);
        break;
    }
    j->depth--;
    return NULL;
}

static NVValue *parse_number(JP *j) {
    char buf[64];
    size_t k = 0;
    while (j->p < j->end && k < sizeof(buf) - 1) {
        char c = *j->p;
        if (!isdigit((unsigned char)c) && c != '-' && c != '+' &&
            c != '.' && c != 'e' && c != 'E') break;
        buf[k++] = c;
        j->p++;
    }
    buf[k] = '\0';
    if (k == 0) {
        jerr(j, "invalid number at line %d", j->line);
        return NULL;
    }
    bool integral = true;
    for (size_t i = 0; i < k; i++) {
        if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') { integral = false; break; }
    }
    if (integral) {
        errno = 0;
        long long iv = strtoll(buf, NULL, 10);
        if (errno == 0) return nv_int(j->I, iv);
    }
    return nv_float(j->I, strtod(buf, NULL));
}

static NVValue *parse_value(JP *j) {
    skip_ws(j);
    if (j->p >= j->end) {
        jerr(j, "unexpected end of JSON input");
        return NULL;
    }
    char c = *j->p;
    if (c == '{') return parse_object(j);
    if (c == '[') return parse_array(j);
    if (c == '"') {
        SB b; sb_init(&b);
        if (!parse_string_raw(j, &b)) { sb_free(&b); return NULL; }
        NVValue *s = nv_strl(j->I, b.p ? b.p : "", b.n);
        sb_free(&b);
        return s;
    }
    if (c == 't') {
        if (j->end - j->p >= 4 && memcmp(j->p, "true", 4) == 0) { j->p += 4; return nv_bool(j->I, true); }
        jerr(j, "invalid literal at line %d", j->line);
        return NULL;
    }
    if (c == 'f') {
        if (j->end - j->p >= 5 && memcmp(j->p, "false", 5) == 0) { j->p += 5; return nv_bool(j->I, false); }
        jerr(j, "invalid literal at line %d", j->line);
        return NULL;
    }
    if (c == 'n') {
        if (j->end - j->p >= 4 && memcmp(j->p, "null", 4) == 0) { j->p += 4; return nv_nil(); }
        jerr(j, "invalid literal at line %d", j->line);
        return NULL;
    }
    if (c == '-' || c == '+' || isdigit((unsigned char)c)) return parse_number(j);
    jerr(j, "unexpected character '%c' at line %d", isprint((unsigned char)c) ? c : '?', j->line);
    return NULL;
}

NVValue *nv_json_parse(Interp *I, const char *text, size_t len,
                       char *err, size_t errsz) {
    JP j;
    memset(&j, 0, sizeof(j));
    j.I = I;
    j.p = text;
    j.end = text + len;
    j.err = err;
    j.errsz = errsz;
    j.line = 1;
    if (errsz > 0) err[0] = '\0';

    NVValue *v = parse_value(&j);
    if (!v) {
        if (errsz > 0 && err[0] == '\0')
            snprintf(err, errsz, "invalid JSON");
        return NULL;
    }
    skip_ws(&j);
    if (j.p < j.end) {
        snprintf(err, errsz, "trailing characters after JSON value at line %d", j.line);
        return NULL;
    }
    return v;
}
