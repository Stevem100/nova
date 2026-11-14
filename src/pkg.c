/*
 * pkg.c — Nova package manager and package-aware import resolution.
 *
 * A Nova package is a directory containing a `nova.json` manifest:
 *
 *   {
 *     "name": "strx",
 *     "version": "1.0.0",
 *     "entry": "main.nv",
 *     "description": "String helpers",
 *     "dependencies": { "mathx": "^1.0.0" }
 *   }
 *
 * The registry is a plain directory tree laid out as
 * `<registry>/<name>/<version>/...`. Its location is resolved in this
 * order:
 *   1. `--registry PATH` command-line flag
 *   2. `$NOVA_REGISTRY` environment variable
 *   3. the "registry" key in `~/.nova/config`  (set via: nova pkg registry PATH)
 *   4. the default: `~/.nova/registry`
 *
 * `nova pkg install` copies packages from the registry into
 * `./nova_modules/<name>/` and pins the resolved version in the project
 * manifest. At run time, `import "name"` falls back to ./nova_modules and
 * then to the registry, so packages work without a manual install step.
 *
 * CLI: nova pkg init|install|remove|list|search|publish|registry|help
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L     /* strdup, strncasecmp under strict C11 */
#endif
#include "nova.h"
#include <errno.h>
#if !defined(_WIN32)
#include <strings.h>                /* strncasecmp */
#endif
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#define PKG_DIR   "nova_modules"
#define MANIFEST  "nova.json"
#define PATH_MAXBUF 2048

#ifdef _WIN32
#define strncasecmp _strnicmp
#endif

/* ================= tiny string list ================= */

typedef struct { char **items; int n, cap; } StrList;

static void sl_push(StrList *l, const char *s) {
    if (l->n + 1 > l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = (char **)realloc(l->items, (size_t)l->cap * sizeof(char *));
        if (!l->items) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    }
    l->items[l->n++] = strdup(s);
}

static void sl_free(StrList *l) {
    for (int i = 0; i < l->n; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL; l->n = l->cap = 0;
}

static int sl_index(StrList *l, const char *s) {
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->items[i], s) == 0) return i;
    return -1;
}

static void sl_remove_at(StrList *l, int idx) {
    if (idx < 0 || idx >= l->n) return;
    free(l->items[idx]);
    for (int i = idx; i < l->n - 1; i++) l->items[i] = l->items[i + 1];
    l->n--;
}

/* ================= fs helpers ================= */

static bool file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* bounded, warning-free path joins */
static void cat2(char *dst, size_t sz, const char *a, const char *b) {
    size_t o = 0;
    for (const char *p = a; *p && o + 1 < sz; p++) dst[o++] = *p;
    for (const char *p = b; *p && o + 1 < sz; p++) dst[o++] = *p;
    dst[o] = '\0';
}

static void pj2(char *dst, size_t sz, const char *a, const char *b) {
    size_t o = 0;
    for (const char *p = a; *p && o + 1 < sz; p++) dst[o++] = *p;
    if (o > 0 && o + 1 < sz && dst[o - 1] != '/') dst[o++] = '/';
    for (const char *p = b; *p && o + 1 < sz; p++) dst[o++] = *p;
    dst[o] = '\0';
}

static void pj3(char *dst, size_t sz, const char *a, const char *b, const char *c) {
    char tmp[PATH_MAXBUF];
    pj2(tmp, sizeof(tmp), a, b);
    pj2(dst, sz, tmp, c);
}

static bool is_dir(const char *p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static bool mk_dir1(const char *p) {
#ifdef _WIN32
    return _mkdir(p) == 0;
#else
    return mkdir(p, 0777) == 0;
#endif
}

/* mkdir -p */
static bool mkdir_p(const char *path) {
    char tmp[PATH_MAXBUF];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return false;
    memcpy(tmp, path, n + 1);
    /* trim trailing slashes */
    while (n > 1 && (tmp[n - 1] == '/' || tmp[n - 1] == '\\')) tmp[--n] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
            mk_dir1(tmp);                 /* EEXIST is fine */
            *p = c;
        }
    }
    mk_dir1(tmp);
    return is_dir(tmp);
}

static bool copy_file(const char *src, const char *dst) {
    size_t len;
    char *buf = nv_read_whole_file(src, &len);
    if (!buf) return false;
    FILE *f = fopen(dst, "wb");
    if (!f) { free(buf); return false; }
    bool ok = fwrite(buf, 1, len, f) == len;
    fclose(f);
    free(buf);
    return ok;
}

typedef struct { char *name; bool is_dir; } PEnt;
typedef struct { PEnt *items; int n, cap; } PList;

static void plist_free(PList *l) {
    for (int i = 0; i < l->n; i++) free(l->items[i].name);
    free(l->items);
    l->items = NULL; l->n = l->cap = 0;
}

static int plist_cmp(const void *a, const void *b) {
    return strcmp(((const PEnt *)a)->name, ((const PEnt *)b)->name);
}

/* list a directory; returns false if it cannot be opened. entries sorted by name */
static bool dir_scan(const char *dir, PList *out) {
#ifdef _WIN32
    char pat[PATH_MAXBUF];
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (out->n + 1 > out->cap) {
            out->cap = out->cap ? out->cap * 2 : 16;
            out->items = (PEnt *)realloc(out->items, (size_t)out->cap * sizeof(PEnt));
            if (!out->items) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        }
        out->items[out->n].name = strdup(fd.cFileName);
        out->items[out->n].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out->n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char sub[PATH_MAXBUF];
        snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);
        if (out->n + 1 > out->cap) {
            out->cap = out->cap ? out->cap * 2 : 16;
            out->items = (PEnt *)realloc(out->items, (size_t)out->cap * sizeof(PEnt));
            if (!out->items) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        }
        out->items[out->n].name = strdup(e->d_name);
        out->items[out->n].is_dir = is_dir(sub);
        out->n++;
    }
    closedir(d);
#endif
    if (out->n > 1)
        qsort(out->items, (size_t)out->n, sizeof(PEnt), plist_cmp);
    return true;
}

static bool copy_tree(const char *src, const char *dst, bool skip_modules,
                      char *err, size_t errsz) {
    PList ents; memset(&ents, 0, sizeof(ents));
    if (!dir_scan(src, &ents)) {
        snprintf(err, errsz, "cannot read directory '%s'", src);
        return false;
    }
    if (!mkdir_p(dst)) {
        plist_free(&ents);
        snprintf(err, errsz, "cannot create directory '%s'", dst);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < ents.n && ok; i++) {
        if (skip_modules && strcmp(ents.items[i].name, PKG_DIR) == 0) continue;
        char sp[PATH_MAXBUF], dp[PATH_MAXBUF];
        pj2(sp, sizeof(sp), src, ents.items[i].name);
        pj2(dp, sizeof(dp), dst, ents.items[i].name);
        if (ents.items[i].is_dir) {
            ok = copy_tree(sp, dp, skip_modules, err, errsz);
        } else if (!copy_file(sp, dp)) {
            snprintf(err, errsz, "cannot copy %.180s -> %.180s", sp, dp);
            ok = false;
        }
    }
    plist_free(&ents);
    return ok;
}

static bool rm_tree(const char *path) {
    PList ents; memset(&ents, 0, sizeof(ents));
    if (dir_scan(path, &ents)) {
        for (int i = 0; i < ents.n; i++) {
            char sub[PATH_MAXBUF];
            snprintf(sub, sizeof(sub), "%s/%s", path, ents.items[i].name);
            if (ents.items[i].is_dir) rm_tree(sub);
            else remove(sub);
        }
        plist_free(&ents);
    }
#ifdef _WIN32
    return _rmdir(path) == 0;
#else
    return rmdir(path) == 0;
#endif
}

/* ================= version utilities ================= */

/* compare one version segment: numeric when both sides are numeric
 * (missing segment counts as 0), byte-wise otherwise */
static int seg_cmp(const char *a, size_t al, const char *b, size_t bl) {
    bool anum = al > 0, bnum = bl > 0;
    for (size_t i = 0; i < al; i++)
        if (!isdigit((unsigned char)a[i])) { anum = false; break; }
    for (size_t i = 0; i < bl; i++)
        if (!isdigit((unsigned char)b[i])) { bnum = false; break; }
    if ((anum || al == 0) && (bnum || bl == 0)) {
        /* saturating numeric compare: absurdly long segments previously
         * overflowed the signed accumulator (UB) */
        unsigned long long x = 0, y = 0;
        const unsigned long long SAT = ~0ULL;
        for (size_t i = 0; i < al; i++) {
            unsigned d = (unsigned)(a[i] - '0');
            if (x > (SAT - d) / 10) { x = SAT; break; }
            x = x * 10 + d;
        }
        for (size_t i = 0; i < bl; i++) {
            unsigned d = (unsigned)(b[i] - '0');
            if (y > (SAT - d) / 10) { y = SAT; break; }
            y = y * 10 + d;
        }
        return x < y ? -1 : x > y ? 1 : 0;
    }
    size_t m = al < bl ? al : bl;
    int c = m ? memcmp(a, b, m) : 0;
    if (c) return c < 0 ? -1 : 1;
    return al < bl ? -1 : al > bl ? 1 : 0;
}

/* "1.2.10" vs "1.2.9" style comparison; missing segments pad as 0 */
static int version_cmp(const char *a, const char *b) {
    const char *pa = a, *pb = b;
    for (;;) {
        bool ha = *pa != '\0', hb = *pb != '\0';
        if (!ha && !hb) return 0;
        const char *sa = "", *sb = "";
        size_t la = 0, lb = 0;
        if (ha) {
            const char *dot = strchr(pa, '.');
            sa = pa; la = dot ? (size_t)(dot - pa) : strlen(pa);
            pa += la + (dot ? 1 : 0);
        }
        if (hb) {
            const char *dot = strchr(pb, '.');
            sb = pb; lb = dot ? (size_t)(dot - pb) : strlen(pb);
            pb += lb + (dot ? 1 : 0);
        }
        int c = seg_cmp(sa, la, sb, lb);
        if (c) return c;
    }
}

/* supported constraints:
 *   "" or "*" or "latest"   any version
 *   ^1.2.3                  same major, version >= 1.2.3
 *   ~1.2.3                  same major.minor, version >= 1.2.3
 *   1.2.3                   exact (padding-tolerant: 1.2 matches 1.2.0)
 */
static bool version_matches(const char *constraint, const char *ver) {
    if (!constraint || !*constraint ||
        strcmp(constraint, "*") == 0 || strcmp(constraint, "latest") == 0)
        return true;
    const char *c = constraint;
    if (*c == '^' || *c == '~') {
        char mode = *c++;
        if (version_cmp(ver, c) < 0) return false;   /* below the floor */
        /* same major */
        size_t k = 0;
        while (c[k] && c[k] != '.') k++;
        if (strlen(ver) < k || memcmp(ver, c, k) != 0) return false;
        if (ver[k] != '\0' && ver[k] != '.') return false;
        if (mode == '~') {
            /* same minor */
            const char *c2 = c[k] ? c + k + 1 : c + k;
            const char *v2 = strchr(ver, '.');
            v2 = v2 ? v2 + 1 : ver + strlen(ver);
            size_t k2 = 0;
            while (c2[k2] && c2[k2] != '.') k2++;
            if (strlen(v2) < k2 || memcmp(v2, c2, k2) != 0) return false;
            if (v2[k2] != '\0' && v2[k2] != '.') return false;
        }
        return true;
    }
    return version_cmp(c, ver) == 0;
}

static bool valid_name(const char *s) {
    if (!s || !*s) return false;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.'))
            return false;
    }
    return s[0] != '.' && s[0] != '-';
}

/* A version string used as a registry directory name. Restricting to
 * [A-Za-z0-9.+-_] without a leading '.' or '-' keeps "1.0.0-beta" legal
 * while making traversal ("../../x") impossible. */
static bool valid_version(const char *s) {
    if (!s || !*s) return false;
    if (s[0] == '.' || s[0] == '-') return false;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (!(isalnum((unsigned char)c) || c == '.' || c == '-' ||
              c == '+' || c == '_'))
            return false;
    }
    return true;
}

/* A relative path that stays inside the package it belongs to. Rejects
 * absolute paths, Windows-style separators/drives and any ".." component —
 * package manifests ("entry") and import sub-paths both pass through here,
 * and both must never be able to climb out of the package root. */
static bool safe_rel_path(const char *s) {
    if (!s || !*s) return false;
    if (s[0] == '/' || s[0] == '\\') return false;
    if (s[0] && s[1] == ':') return false;             /* C:\ style drives */
    const char *p = s;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' &&
            (p == s || p[-1] == '/' || p[-1] == '\\') &&
            (p[2] == '\0' || p[2] == '/' || p[2] == '\\'))
            return false;                               /* ".." component */
        if (*p == '\\' || *p == '\0') return false;
        p++;
    }
    return true;
}

/* ================= registry location ================= */

static const char *g_reg_override = NULL;    /* --registry flag */

static void home_dir(char *buf, size_t n) {
    const char *h = getenv("HOME");
#ifdef _WIN32
    if (!h || !*h) h = getenv("USERPROFILE");
#endif
    if (!h || !*h) { if (n > 0) buf[0] = '\0'; return; }
    snprintf(buf, n, "%s", h);
}

/* resolve the registry directory; *src (optional) gets "flag"/"NOVA_REGISTRY"/"config"/"default" */
static const char *registry_dir_ex(char *buf, size_t bufsz, const char **src) {
    if (src) *src = "default";
    if (g_reg_override) {
        snprintf(buf, bufsz, "%s", g_reg_override);
        if (src) *src = "flag";
        return buf;
    }
    const char *env = getenv("NOVA_REGISTRY");
    if (env && *env) {
        snprintf(buf, bufsz, "%s", env);
        if (src) *src = "NOVA_REGISTRY";
        return buf;
    }
    char home[512];
    home_dir(home, sizeof(home));
    if (home[0]) {
        char cfgpath[1024];
        pj2(cfgpath, sizeof(cfgpath), home, ".nova/config");
        if (file_exists(cfgpath)) {
            size_t len;
            char *txt = nv_read_whole_file(cfgpath, &len);
            if (txt) {
                Interp I;
                interp_init(&I);
                char jerr[128];
                NVValue *v = nv_json_parse(&I, txt, len, jerr, sizeof(jerr));
                free(txt);
                NVValue *r = NULL;
                if (v && v->type == T_MAP &&
                    nvtab_get(v->as.map, "registry", &r) &&
                    r->type == T_STR && r->as.s[0]) {
                    snprintf(buf, bufsz, "%s", r->as.s);
                    interp_free(&I);
                    if (src) *src = "config";
                    return buf;
                }
                interp_free(&I);
            }
        }
        snprintf(buf, bufsz, "%s/.nova/registry", home);
        return buf;
    }
    snprintf(buf, bufsz, ".nova-registry");
    return buf;
}

static void registry_dir(char *buf, size_t bufsz) {
    registry_dir_ex(buf, bufsz, NULL);
}

/* ================= manifest (nova.json) ================= */

typedef struct {
    char name[128];
    char version[64];
    char entry[128];
    char description[256];
    StrList dep_names;
    StrList dep_versions;
} Manifest;

static void manifest_free(Manifest *m) {
    sl_free(&m->dep_names);
    sl_free(&m->dep_versions);
}

static bool manifest_read(const char *dir, Manifest *m, char *err, size_t errsz) {
    char path[PATH_MAXBUF];
    pj2(path, sizeof(path), dir, MANIFEST);
    if (!file_exists(path)) {
        snprintf(err, errsz, "missing %s in %.430s", MANIFEST, dir);
        return false;
    }
    size_t len;
    char *txt = nv_read_whole_file(path, &len);
    if (!txt) { snprintf(err, errsz, "cannot read %.180s", path); return false; }

    Interp I;
    interp_init(&I);
    char jerr[128];
    NVValue *v = nv_json_parse(&I, txt, len, jerr, sizeof(jerr));
    free(txt);
    if (!v) {
        interp_free(&I);
        snprintf(err, errsz, "%.180s: %s", path, jerr);
        return false;
    }
    if (v->type != T_MAP) {
        interp_free(&I);
        snprintf(err, errsz, "%.180s: top level must be a JSON object", path);
        return false;
    }

    memset(m, 0, sizeof(*m));
    NVTable *t = v->as.map;
    for (size_t k = 0; k < t->on; k++) {
        TEntry *e = &t->ents[t->order[k]];
        if (e->state != 1) continue;
        if (strcmp(e->key, "name") == 0 && e->val->type == T_STR)
            snprintf(m->name, sizeof(m->name), "%s", e->val->as.s);
        else if (strcmp(e->key, "version") == 0 && e->val->type == T_STR)
            snprintf(m->version, sizeof(m->version), "%s", e->val->as.s);
        else if (strcmp(e->key, "entry") == 0 && e->val->type == T_STR)
            snprintf(m->entry, sizeof(m->entry), "%s", e->val->as.s);
        else if (strcmp(e->key, "description") == 0 && e->val->type == T_STR)
            snprintf(m->description, sizeof(m->description), "%s", e->val->as.s);
        else if (strcmp(e->key, "dependencies") == 0 && e->val->type == T_MAP) {
            NVTable *d = e->val->as.map;
            for (size_t j = 0; j < d->on; j++) {
                TEntry *de = &d->ents[d->order[j]];
                if (de->state != 1 || de->val->type != T_STR) continue;
                sl_push(&m->dep_names, de->key);
                sl_push(&m->dep_versions, de->val->as.s);
            }
        }
    }
    interp_free(&I);

    if (!m->name[0]) {
        manifest_free(m);
        snprintf(err, errsz, "%.180s: missing \"name\"", path);
        return false;
    }
    if (!m->entry[0])
        snprintf(m->entry, sizeof(m->entry), "main.nv");
    /* the entry is joined onto the package directory at import time — a
     * crafted manifest ("entry": "../../../evil.nv") could otherwise point
     * outside the package and execute files beyond its root */
    if (!safe_rel_path(m->entry)) {
        manifest_free(m);
        snprintf(err, errsz, "%.180s: \"entry\" must be a relative path "
                             "inside the package (no '..' or absolute paths)", path);
        return false;
    }
    /* dependency names become directories under ./nova_modules when
     * installed — they must never traverse out of the project */
    for (int i = 0; i < m->dep_names.n; i++) {
        if (!valid_name(m->dep_names.items[i])) {
            char bad[128];
            snprintf(bad, sizeof(bad), "%s", m->dep_names.items[i]);
            manifest_free(m);
            snprintf(err, errsz, "%.180s: invalid dependency name '%s'", path, bad);
            return false;
        }
    }
    return true;
}

static void json_escape(const char *s, char *out, size_t outsz) {
    size_t o = 0;
    for (; *s && o + 7 < outsz; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  out[o++] = '\\'; out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
            case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
            case '\t': out[o++] = '\\'; out[o++] = 't';  break;
            default:
                if (c < 0x20) o += (size_t)snprintf(out + o, outsz - o, "\\u%04x", c);
                else out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static bool manifest_write(const char *dir, const Manifest *m) {
    char path[PATH_MAXBUF];
    snprintf(path, sizeof(path), "%s/%s", dir, MANIFEST);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    char esc[512];

    json_escape(m->name, esc, sizeof(esc));
    fprintf(f, "{\n  \"name\": \"%s\",\n", esc);
    json_escape(m->version, esc, sizeof(esc));
    fprintf(f, "  \"version\": \"%s\",\n", esc);
    json_escape(m->entry, esc, sizeof(esc));
    fprintf(f, "  \"entry\": \"%s\",\n", esc);
    json_escape(m->description, esc, sizeof(esc));
    fprintf(f, "  \"description\": \"%s\"", esc);

    if (m->dep_names.n == 0) {
        fprintf(f, ",\n  \"dependencies\": {}\n}\n");
    } else {
        fprintf(f, ",\n  \"dependencies\": {\n");
        for (int i = 0; i < m->dep_names.n; i++) {
            json_escape(m->dep_names.items[i], esc, sizeof(esc));
            fprintf(f, "    \"%s\": ", esc);
            json_escape(m->dep_versions.items[i], esc, sizeof(esc));
            fprintf(f, "\"%s\"%s\n", esc, i + 1 < m->dep_names.n ? "," : "");
        }
        fprintf(f, "  }\n}\n");
    }
    fclose(f);
    return true;
}

static void manifest_init_default(Manifest *m, const char *dir) {
    memset(m, 0, sizeof(*m));
    const char *last = dir;
    for (const char *p = dir; *p; p++)
        if (*p == '/' || *p == '\\') last = p + 1;
    if (!*last || strcmp(last, ".") == 0 || strcmp(last, "..") == 0)
        snprintf(m->name, sizeof(m->name), "app");
    else
        snprintf(m->name, sizeof(m->name), "%s", last);
    snprintf(m->version, sizeof(m->version), "0.1.0");
    snprintf(m->entry, sizeof(m->entry), "main.nv");
}

/* installed version of a package, from ./nova_modules/<name>/nova.json
 * ("" if not installed) */
static void installed_version(const char *name, char *out, size_t outsz) {
    out[0] = '\0';
    char dir[PATH_MAXBUF];
    pj3(dir, sizeof(dir), ".", PKG_DIR, name);
    Manifest m;
    char err[256];
    if (manifest_read(dir, &m, err, sizeof(err))) {
        snprintf(out, outsz, "%s", m.version);
        manifest_free(&m);
    }
}

/* ================= registry lookup ================= */

/* find <registry>/<name>/<version> with the highest version matching
 * the constraint */
static bool registry_best(const char *regdir, const char *name, const char *constraint,
                          char *out, size_t outsz, char *err, size_t errsz) {
    char pkgdir[PATH_MAXBUF];
    pj2(pkgdir, sizeof(pkgdir), regdir, name);
    PList ents; memset(&ents, 0, sizeof(ents));
    if (!dir_scan(pkgdir, &ents)) {
        plist_free(&ents);
        snprintf(err, errsz,
                 "package \"%s\" not found in the registry (%s)", name, regdir);
        return false;
    }
    char best[64] = "";
    for (int i = 0; i < ents.n; i++) {
        if (!ents.items[i].is_dir) continue;
        if (constraint && !version_matches(constraint, ents.items[i].name)) continue;
        if (!best[0] || version_cmp(ents.items[i].name, best) > 0)
            snprintf(best, sizeof(best), "%s", ents.items[i].name);
    }
    plist_free(&ents);
    if (!best[0]) {
        snprintf(err, errsz, "no version of \"%s\" matches '%s' in the registry",
                 name, constraint && *constraint ? constraint : "*");
        return false;
    }
    pj3(out, outsz, regdir, name, best);
    return true;
}

/* ================= import-time package resolution ================= */

/* spec is what appears in `import "..."`: a package name ("strx") or a
 * subpath inside a package ("strx/util.nv"). Resolution order:
 *   1. ./nova_modules/<pkg>[/<sub> or manifest entry]
 *   2. <registry>/<pkg>/<best version>[/<sub> or manifest entry]
 */
bool nv_pkg_resolve(const char *spec, char *out, size_t outsz,
                    char *err, size_t errsz) {
    char pkg[256] = "";
    const char *sub = NULL;
    const char *slash = strchr(spec, '/');
    if (slash) {
        size_t n = (size_t)(slash - spec);
        if (n >= sizeof(pkg)) n = sizeof(pkg) - 1;
        memcpy(pkg, spec, n);
        pkg[n] = '\0';
        sub = slash + 1;
    } else {
        snprintf(pkg, sizeof(pkg), "%s", spec);
    }

    if (!valid_name(pkg)) {
        snprintf(err, errsz, "invalid package name \"%s\" in import", pkg);
        return false;
    }
    /* a sub-path like "util.nv" is fine, "../../../etc/passwd" would climb
     * out of the package root — refuse anything that is not a clean,
     * relative path inside the package */
    if (sub && *sub && !safe_rel_path(sub)) {
        snprintf(err, errsz,
                 "invalid module path \"%s\": sub-paths cannot contain '..' "
                 "or absolute components", spec);
        return false;
    }

    char regdir[PATH_MAXBUF];
    registry_dir(regdir, sizeof(regdir));

    char roots[2][PATH_MAXBUF];
    int nroots = 0;
    snprintf(roots[nroots++], PATH_MAXBUF, "./%s/%s", PKG_DIR, pkg);
    {
        char best[PATH_MAXBUF], berr[256];
        if (registry_best(regdir, pkg, "*", best, sizeof(best), berr, sizeof(berr)))
            snprintf(roots[nroots++], PATH_MAXBUF, "%s", best);
    }

    for (int i = 0; i < nroots; i++) {
        if (!is_dir(roots[i])) continue;
        if (sub && *sub) {
            char cand[PATH_MAXBUF];
            pj2(cand, sizeof(cand), roots[i], sub);
            if (file_exists(cand)) {
                snprintf(out, outsz, "%s", cand);
                return true;
            }
            if (!strstr(sub, ".nv")) {
                char subnv[512];
                cat2(subnv, sizeof(subnv), sub, ".nv");
                pj2(cand, sizeof(cand), roots[i], subnv);
                if (file_exists(cand)) {
                    snprintf(out, outsz, "%s", cand);
                    return true;
                }
            }
        } else {
            Manifest m;
            char merr[256];
            if (manifest_read(roots[i], &m, merr, sizeof(merr))) {
                char cand[PATH_MAXBUF];
                pj2(cand, sizeof(cand), roots[i], m.entry);
                manifest_free(&m);
                if (file_exists(cand)) {
                    snprintf(out, outsz, "%s", cand);
                    return true;
                }
            }
        }
    }
    snprintf(err, errsz,
             "package \"%s\" not found in ./%s or the registry; run: nova pkg install %s",
             pkg, PKG_DIR, pkg);
    return false;
}

/* ================= install ================= */

static bool install_one(const char *name, const char *constraint, Manifest *proj,
                        bool *changed, char *err, size_t errsz) {
    /* defense in depth: names reaching here come either from validated CLI
     * args or from nova.json "dependencies" (manifest_read now rejects
     * traversal names, but never trust two sources with one check) */
    if (!valid_name(name)) {
        snprintf(err, errsz, "invalid package name '%s'", name);
        return false;
    }
    char regdir[PATH_MAXBUF];
    registry_dir(regdir, sizeof(regdir));
    char best[PATH_MAXBUF];
    if (!registry_best(regdir, name, constraint, best, sizeof(best), err, errsz))
        return false;

    const char *ver = strrchr(best, '/');
    ver = ver ? ver + 1 : best;

    char instdir[PATH_MAXBUF];
    snprintf(instdir, sizeof(instdir), "./%s/%s", PKG_DIR, name);

    char curver[64];
    installed_version(name, curver, sizeof(curver));
    if (strcmp(curver, ver) == 0) {
        printf("up to date: %s %s\n", name, ver);
        return true;
    }

    if (!copy_tree(best, instdir, true, err, errsz)) return false;

    int idx = sl_index(&proj->dep_names, name);
    if (idx >= 0) {
        free(proj->dep_versions.items[idx]);
        proj->dep_versions.items[idx] = strdup(ver);
    } else {
        sl_push(&proj->dep_names, name);
        sl_push(&proj->dep_versions, ver);
    }
    *changed = true;
    if (curver[0])
        printf("installed: %s %s (was %s)\n", name, ver, curver);
    else
        printf("installed: %s %s\n", name, ver);
    return true;
}

/* ================= CLI commands ================= */

static int cmd_help(void) {
    printf(
        "Nova package manager\n"
        "\n"
        "Usage: nova pkg <command> [args]\n"
        "\n"
        "Commands:\n"
        "  init [dir]               create a %s manifest (and %s)\n"
        "  install [name[@ver]]...  install packages into ./%s\n"
        "                           (no args: install everything in %s)\n"
        "  remove <name>            remove a package from ./%s and %s\n"
        "  list                     list installed packages\n"
        "  search [query]           search the registry\n"
        "  publish [dir] [--force]  publish a package directory to the registry\n"
        "  registry [path]          show or set the registry path\n"
        "  help                     show this help\n"
        "\n"
        "Registry location: --registry flag, $NOVA_REGISTRY, ~/.nova/config,\n"
        "then ~/.nova/registry by default.\n"
        "Version constraints: * (any), ^1.2.0 (same major), ~1.2.0 (same minor),\n"
        "1.2.0 (exact).\n",
        MANIFEST, "main.nv", PKG_DIR, MANIFEST, PKG_DIR, MANIFEST);
    return 0;
}

static int cmd_init(int argc, char **argv) {
    (void)argc;
    const char *dir = argv[0] ? argv[0] : ".";
    char err[512];
    Manifest m;
    if (manifest_read(dir, &m, err, sizeof(err))) {
        printf("%s already exists (name: %s, version: %s)\n", MANIFEST, m.name, m.version);
        manifest_free(&m);
        return 0;
    }
    manifest_init_default(&m, dir);
    if (strcmp(dir, ".") != 0 && !mkdir_p(dir)) {
        fprintf(stderr, "nova pkg: cannot create directory '%s'\n", dir);
        return 1;
    }
    if (!manifest_write(dir, &m)) {
        fprintf(stderr, "nova pkg: cannot write %s in %s\n", MANIFEST, dir);
        return 1;
    }
    printf("created %s (name: %s, version: %s, entry: %s)\n",
           MANIFEST, m.name, m.version, m.entry);
    char mainf[PATH_MAXBUF];
    snprintf(mainf, sizeof(mainf), "%s/%s", dir, m.entry);
    if (!file_exists(mainf)) {
        FILE *f = fopen(mainf, "wb");
        if (f) {
            fprintf(f, "// %s - created by 'nova pkg init'\n"
                       "echo \"Hello from %s\\n\";\n", m.name, m.name);
            fclose(f);
            printf("created %s\n", m.entry);
        }
    }
    manifest_free(&m);
    return 0;
}

static int cmd_install(int argc, char **argv) {
    char err[512];
    Manifest proj;
    if (!manifest_read(".", &proj, err, sizeof(err))) {
        /* distinguish "no manifest yet" (normal) from a manifest that exists
         * but was rejected (e.g. a traversal dependency name) */
        char path[PATH_MAXBUF];
        pj2(path, sizeof(path), ".", MANIFEST);
        if (file_exists(path))
            fprintf(stderr, "nova pkg: %s\n", err);
        manifest_init_default(&proj, ".");
        if (!file_exists(path))
            printf("no %s found; created one (name: %s)\n", MANIFEST, proj.name);
    }

    bool changed = false;
    int rc = 0;

    if (argc > 0) {
        for (int i = 0; i < argc; i++) {
            char name[128];
            const char *constraint = "*";
            const char *at = strchr(argv[i], '@');
            if (at) {
                size_t n = (size_t)(at - argv[i]);
                if (n >= sizeof(name)) n = sizeof(name) - 1;
                memcpy(name, argv[i], n);
                name[n] = '\0';
                constraint = at + 1;
            } else {
                snprintf(name, sizeof(name), "%s", argv[i]);
            }
            if (!valid_name(name)) {
                fprintf(stderr, "nova pkg: invalid package name '%s'\n", name);
                rc = 1;
                break;
            }
            if (!install_one(name, constraint, &proj, &changed, err, sizeof(err))) {
                fprintf(stderr, "nova pkg: %s\n", err);
                rc = 1;
                break;
            }
        }
    } else if (proj.dep_names.n == 0) {
        printf("nothing to install (no dependencies in %s)\n", MANIFEST);
    } else {
        /* snapshot: install_one() may grow proj->dep_names */
        int n = proj.dep_names.n;
        char **names = (char **)malloc((size_t)n * sizeof(char *));
        char **cons  = (char **)malloc((size_t)n * sizeof(char *));
        if (!names || !cons) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        for (int i = 0; i < n; i++) {
            names[i] = strdup(proj.dep_names.items[i]);
            cons[i]  = strdup(proj.dep_versions.items[i]);
        }
        for (int i = 0; i < n; i++) {
            if (!install_one(names[i], cons[i], &proj, &changed, err, sizeof(err))) {
                fprintf(stderr, "nova pkg: %s\n", err);
                rc = 1;
            }
            free(names[i]);
            free(cons[i]);
        }
        free(names);
        free(cons);
    }

    if (changed && !manifest_write(".", &proj))
        fprintf(stderr, "nova pkg: warning: could not update %s\n", MANIFEST);
    manifest_free(&proj);
    return rc;
}

static int cmd_remove(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: nova pkg remove <name>\n");
        return 2;
    }
    const char *name = argv[0];
    char err[512];
    bool changed = false;
    Manifest proj;
    if (manifest_read(".", &proj, err, sizeof(err))) {
        int idx = sl_index(&proj.dep_names, name);
        if (idx >= 0) {
            sl_remove_at(&proj.dep_names, idx);
            sl_remove_at(&proj.dep_versions, idx);
            changed = true;
        }
        if (changed && !manifest_write(".", &proj))
            fprintf(stderr, "nova pkg: warning: could not update %s\n", MANIFEST);
        manifest_free(&proj);
    }
    char instdir[PATH_MAXBUF];
    snprintf(instdir, sizeof(instdir), "./%s/%s", PKG_DIR, name);
    if (is_dir(instdir)) {
        if (!rm_tree(instdir)) {
            fprintf(stderr, "nova pkg: cannot remove '%s'\n", instdir);
            return 1;
        }
        printf("removed %s (from ./%s%s)\n", name, PKG_DIR,
               changed ? " and dependencies" : "");
        return 0;
    }
    if (changed) {
        printf("removed %s (from dependencies; was not installed)\n", name);
        return 0;
    }
    fprintf(stderr, "nova pkg: package '%s' is not installed and not in %s\n",
            name, MANIFEST);
    return 1;
}

static int cmd_list(int argc, char **argv) {
    (void)argc; (void)argv;
    char err[512];
    Manifest proj;
    if (!manifest_read(".", &proj, err, sizeof(err))) {
        manifest_init_default(&proj, ".");
        printf("%s (no %s)\n", proj.name, MANIFEST);
    } else {
        printf("%s %s\n", proj.name, proj.version);
    }
    printf("packages (./%s):\n", PKG_DIR);

    PList mods; memset(&mods, 0, sizeof(mods));
    char moddir[PATH_MAXBUF];
    snprintf(moddir, sizeof(moddir), "./%s", PKG_DIR);
    dir_scan(moddir, &mods);

    int shown = 0;
    for (int i = 0; i < proj.dep_names.n; i++) {
        const char *name = proj.dep_names.items[i];
        char ver[64];
        installed_version(name, ver, sizeof(ver));
        printf("  %-20s %-10s constraint %s%s\n",
               name, ver[0] ? ver : "-", proj.dep_versions.items[i],
               ver[0] ? "" : "  (not installed)");
        shown++;
        /* don't list it again as unpinned */
        for (int j = 0; j < mods.n; j++)
            if (strcmp(mods.items[j].name, name) == 0) { mods.items[j].name[0] = '\0'; }
    }
    for (int j = 0; j < mods.n; j++) {
        if (!mods.items[j].is_dir || mods.items[j].name[0] == '\0') continue;
        char ver[64];
        installed_version(mods.items[j].name, ver, sizeof(ver));
        printf("  %-20s %-10s (not pinned in %s)\n", mods.items[j].name,
               ver[0] ? ver : "-", MANIFEST);
        shown++;
    }
    if (shown == 0) printf("  (none)\n");
    plist_free(&mods);
    manifest_free(&proj);
    return 0;
}

static int cmd_search(int argc, char **argv) {
    const char *query = argc > 0 ? argv[0] : NULL;
    char regdir[PATH_MAXBUF];
    registry_dir(regdir, sizeof(regdir));

    PList pkgs; memset(&pkgs, 0, sizeof(pkgs));
    if (!dir_scan(regdir, &pkgs)) {
        printf("(registry is empty or missing: %s)\n", regdir);
        return 0;
    }
    int hits = 0;
    for (int i = 0; i < pkgs.n; i++) {
        if (!pkgs.items[i].is_dir) continue;
        char best[PATH_MAXBUF], berr[256];
        if (!registry_best(regdir, pkgs.items[i].name, "*", best, sizeof(best),
                           berr, sizeof(berr)))
            continue;
        Manifest m;
        if (!manifest_read(best, &m, berr, sizeof(berr))) continue;
        bool match = !query;
        if (!match) {
            /* case-insensitive substring match on name and description */
            const char *hay1 = m.name;
            const char *hay2 = m.description;
            size_t ql = strlen(query);
            for (const char *h = hay1; *h && !match; h++)
                if (strncasecmp(h, query, ql) == 0) match = true;
            for (const char *h = hay2; *h && !match; h++)
                if (strncasecmp(h, query, ql) == 0) match = true;
        }
        if (match) {
            printf("%-20s %-10s %s\n", m.name, m.version, m.description);
            hits++;
        }
        manifest_free(&m);
    }
    if (hits == 0) printf("(no packages match \"%s\")\n", query ? query : "");
    plist_free(&pkgs);
    return 0;
}

static int cmd_publish(int argc, char **argv) {
    const char *dir = ".";
    bool force = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) force = true;
        else dir = argv[i];
    }
    char err[512];
    Manifest m;
    if (!manifest_read(dir, &m, err, sizeof(err))) {
        fprintf(stderr, "nova pkg: %s\n", err);
        return 1;
    }
    if (!m.version[0]) {
        fprintf(stderr, "nova pkg: %s is missing \"version\"\n", MANIFEST);
        manifest_free(&m);
        return 1;
    }
    if (!valid_name(m.name)) {
        fprintf(stderr, "nova pkg: invalid package name '%s'\n", m.name);
        manifest_free(&m);
        return 1;
    }
    if (!valid_version(m.version)) {
        fprintf(stderr, "nova pkg: invalid version '%s' "
                        "(allowed: letters, digits, '.', '-', '+', '_')\n",
                m.version);
        manifest_free(&m);
        return 1;
    }
    char regdir[PATH_MAXBUF];
    registry_dir(regdir, sizeof(regdir));
    char target[PATH_MAXBUF];
    pj3(target, sizeof(target), regdir, m.name, m.version);
    if (is_dir(target) && !force) {
        fprintf(stderr, "nova pkg: %s %s is already published (use --force to overwrite)\n",
                m.name, m.version);
        manifest_free(&m);
        return 1;
    }
    if (!copy_tree(dir, target, true, err, sizeof(err))) {
        fprintf(stderr, "nova pkg: %s\n", err);
        manifest_free(&m);
        return 1;
    }
    printf("published %s %s -> %s\n", m.name, m.version, target);
    manifest_free(&m);
    return 0;
}

static int cmd_registry(int argc, char **argv) {
    if (argc == 0) {
        char buf[PATH_MAXBUF];
        const char *src = NULL;
        registry_dir_ex(buf, sizeof(buf), &src);
        printf("registry: %s  (source: %s)\n", buf, src);
        return 0;
    }
    const char *path = argv[0];
    char home[512];
    home_dir(home, sizeof(home));
    if (!home[0]) {
        fprintf(stderr, "nova pkg: cannot determine home directory; "
                        "use the NOVA_REGISTRY environment variable instead\n");
        return 1;
    }
    char novadir[600];
    snprintf(novadir, sizeof(novadir), "%s/.nova", home);
    if (!mkdir_p(novadir)) {
        fprintf(stderr, "nova pkg: cannot create %s\n", novadir);
        return 1;
    }
    char cfgpath[1024];
    snprintf(cfgpath, sizeof(cfgpath), "%s/config", novadir);
    FILE *f = fopen(cfgpath, "wb");
    if (!f) {
        fprintf(stderr, "nova pkg: cannot write %s\n", cfgpath);
        return 1;
    }
    char esc[PATH_MAXBUF];
    json_escape(path, esc, sizeof(esc));
    fprintf(f, "{\n  \"registry\": \"%s\"\n}\n", esc);
    fclose(f);
    printf("registry set to %s  (saved in %s)\n", path, cfgpath);
    return 0;
}

/* ================= entry point ================= */

int nv_pkg_main(int argc, char **argv) {
    /* argv[0] is "pkg"; strip global --registry flags */
    const char **clean = (const char **)malloc((size_t)(argc + 1) * sizeof(char *));
    if (!clean) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    int cn = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--registry") == 0 && i + 1 < argc) {
            g_reg_override = argv[++i];
            continue;
        }
        if (strncmp(argv[i], "--registry=", 11) == 0) {
            g_reg_override = argv[i] + 11;
            continue;
        }
        clean[cn++] = argv[i];
    }
    argc = cn;
    argv = (char **)clean;

    if (argc < 2 || strcmp(argv[1], "help") == 0 ||
        strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        free(clean);
        return cmd_help();
    }
    const char *sub = argv[1];
    int ca = argc - 2;
    char **cv = argv + 2;
    int rc;
    if (strcmp(sub, "init") == 0)            rc = cmd_init(ca, cv);
    else if (strcmp(sub, "install") == 0)    rc = cmd_install(ca, cv);
    else if (strcmp(sub, "remove") == 0)     rc = cmd_remove(ca, cv);
    else if (strcmp(sub, "uninstall") == 0)  rc = cmd_remove(ca, cv);
    else if (strcmp(sub, "list") == 0)       rc = cmd_list(ca, cv);
    else if (strcmp(sub, "ls") == 0)         rc = cmd_list(ca, cv);
    else if (strcmp(sub, "search") == 0)     rc = cmd_search(ca, cv);
    else if (strcmp(sub, "publish") == 0)    rc = cmd_publish(ca, cv);
    else if (strcmp(sub, "registry") == 0)   rc = cmd_registry(ca, cv);
    else {
        fprintf(stderr, "nova pkg: unknown command '%s' (try 'nova pkg help')\n", sub);
        rc = 2;
    }
    free(clean);
    return rc;
}
