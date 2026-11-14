/*
 * value.c — arena allocator, scratch buffers, insertion-ordered hash
 * tables, Nova value constructors and core value operations.
 */
#include "nova.h"

/* ================= arena ================= */

static ABlock *arena_new_block(size_t cap) {
    /* block header + payload allocated as one malloc */
    ABlock *b = (ABlock *)malloc(sizeof(ABlock) + cap);
    if (!b) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    b->next = NULL; b->used = 0; b->cap = cap;
    return b;
}

void *arena_alloc(Arena *a, size_t n) {
    n = (n + 15u) & ~(size_t)15u;               /* 16-byte alignment */
    if (!a->head || a->head->cap - a->head->used < n) {
        size_t cap = 1 << 20;                   /* 1 MiB default block */
        while (cap < n + sizeof(ABlock)) cap <<= 1;
        ABlock *b = arena_new_block(cap - sizeof(ABlock));
        b->next = a->head;
        a->head = b;
    }
    void *p = (char *)(a->head + 1) + a->head->used;
    a->head->used += n;
    a->used_total += n;
    return p;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *p = (char *)arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *arena_strdup(Arena *a, const char *s) {
    return arena_strndup(a, s, strlen(s));
}

void arena_free_all(Arena *a) {
    ABlock *b = a->head;
    while (b) { ABlock *nx = b->next; free(b); b = nx; }
    a->head = NULL;
}

/* ================= scratch string buffer ================= */

void sb_init(SB *b) { b->p = NULL; b->n = 0; b->cap = 0; }
void sb_free(SB *b) { free(b->p); sb_init(b); }

static void sb_grow(SB *b, size_t need) {
    if (b->n + need + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->n + need + 1) cap <<= 1;
        char *p = (char *)realloc(b->p, cap);
        if (!p) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        b->p = p; b->cap = cap;
    }
}

void sb_putc(SB *b, char c) { sb_grow(b, 1); b->p[b->n++] = c; b->p[b->n] = '\0'; }
void sb_putsl(SB *b, const char *s, size_t len) {
    if (!len) return;
    sb_grow(b, len);
    memcpy(b->p + b->n, s, len);
    b->n += len;
    b->p[b->n] = '\0';
}
void sb_puts(SB *b, const char *s) { sb_putsl(b, s, strlen(s)); }

void sb_fmt_float(SB *b, double d) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%.15g", d);
    sb_puts(b, tmp);
}

/* ================= hash table ================= */

static uint32_t fnv1a(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h ? h : 1;
}

NVTable *nvtab_new(Interp *I) {
    NVTable *t = (NVTable *)nv_gc_alloc(I, sizeof(NVTable), OT_TABLE);
    memset(t, 0, sizeof(NVTable));                    /* count/tombs/on = 0 */
    t->cap = 8;
    t->ents = (TEntry *)nv_gc_alloc(I, t->cap * sizeof(TEntry), OT_TABLEENTS);
    memset(t->ents, 0, t->cap * sizeof(TEntry));      /* state=empty */
    t->ocap = 8;
    t->order = (size_t *)nv_gc_alloc(I, t->ocap * sizeof(size_t), OT_TABLEORDER);
    return t;
}

static TEntry *nvtab_find(NVTable *t, const char *key, uint32_t h) {
    size_t i = h & (t->cap - 1);
    for (;;) {
        TEntry *e = &t->ents[i];
        if (e->state == 0) return NULL;
        if (e->state == 1 && e->hash == h && strcmp(e->key, key) == 0) return e;
        i = (i + 1) & (t->cap - 1);
    }
}

static void nvtab_rehash(Interp *I, NVTable *t, size_t newcap) {
    TEntry *oe = t->ents; size_t ocap = t->cap;
    size_t *oo = t->order; size_t oon = t->on;
    t->ents = (TEntry *)nv_gc_alloc(I, newcap * sizeof(TEntry), OT_TABLEENTS);
    memset(t->ents, 0, newcap * sizeof(TEntry));      /* state=empty */
    t->cap = newcap;
    t->ocap = oon + 8;
    t->order = (size_t *)nv_gc_alloc(I, t->ocap * sizeof(size_t), OT_TABLEORDER);
    t->on = 0; t->count = 0; t->tombs = 0;
    for (size_t k = 0; k < oon; k++) {
        TEntry *e = &oe[oo[k]];
        if (e->state != 1) continue;
        size_t i = e->hash & (newcap - 1);
        while (t->ents[i].state == 1) i = (i + 1) & (newcap - 1);
        TEntry *ne = &t->ents[i];
        ne->key = e->key; ne->hash = e->hash; ne->val = e->val; ne->state = 1;
        t->order[t->on++] = i;
        t->count++;
    }
    (void)ocap;   /* old ents/order blocks become garbage -> reclaimed by GC */
}

static void nvtab_maybe_grow(Interp *I, NVTable *t) {
    if ((t->count + t->tombs + 1) * 4 >= t->cap * 3) {
        size_t nc = t->cap * 2;
        /* avoid unbounded growth from tombstone churn */
        if (t->count * 8 < t->cap) nc = t->cap;
        nvtab_rehash(I, t, nc);
    }
}

static size_t nvtab_insert_raw(NVTable *t, const char *key, uint32_t h, NVValue *val) {
    size_t i = h & (t->cap - 1);
    while (t->ents[i].state == 1) i = (i + 1) & (t->cap - 1);
    TEntry *e = &t->ents[i];
    bool was_tomb = (e->state == 2);
    e->key = key; e->hash = h; e->val = val; e->state = 1;
    t->count++;
    if (was_tomb) t->tombs--;
    return i;
}

void nvtab_set(Interp *I, NVTable *t, const char *key, NVValue *val) {
    uint32_t h = fnv1a(key);
    TEntry *e = nvtab_find(t, key, h);
    if (e) { e->val = val; return; }
    nvtab_maybe_grow(I, t);
    /* each table owns a private copy of the key (GC-managed block) so a
     * key never aliases a string value's buffer or an arena string */
    size_t klen = strlen(key);
    char *kcopy = (char *)nv_gc_alloc(I, klen + 1, OT_KEY);
    memcpy(kcopy, key, klen + 1);
    size_t idx = nvtab_insert_raw(t, kcopy, h, val);
    if (t->on + 1 > t->ocap) {
        size_t ocap = t->ocap * 2;
        size_t *o = (size_t *)nv_gc_alloc(I, ocap * sizeof(size_t), OT_TABLEORDER);
        memcpy(o, t->order, t->on * sizeof(size_t));
        t->order = o; t->ocap = ocap;
    }
    t->order[t->on++] = idx;
}

bool nvtab_get(NVTable *t, const char *key, NVValue **out) {
    uint32_t h = fnv1a(key);
    TEntry *e = nvtab_find(t, key, h);
    if (e) { *out = e->val; return true; }
    return false;
}

bool nvtab_del(NVTable *t, const char *key) {
    uint32_t h = fnv1a(key);
    TEntry *e = nvtab_find(t, key, h);
    if (!e) return false;
    e->state = 2;
    t->count--; t->tombs++;
    return true;
}

size_t nvtab_count(NVTable *t) { return t->count; }

void nvtab_foreach(NVTable *t, NVTabIt fn, void *ud) {
    for (size_t k = 0; k < t->on; k++) {
        TEntry *e = &t->ents[t->order[k]];
        if (e->state != 1) continue;
        if (!fn(e->key, e->val, ud)) return;
    }
}

/* ================= interp init / free ================= */

void interp_init(Interp *I) {
    memset(I, 0, sizeof(Interp));
    I->heap.trigger = 1u << 20;           /* first GC after ~1 MiB of churn */
    I->mod_cache = nvtab_new(I);
    I->mod_busy  = nvtab_new(I);
    I->builtins_env = env_new(I, NULL);

    /* builtin Error class used by nv_err()/throw */
    I->error_class = nv_new_class(I, "Error", NULL);

    srand((unsigned)(time(NULL) ^ (uintptr_t)I));
}

void interp_free(Interp *I) {
    nv_net_close_all();          /* release any sockets opened via net_connect */
    nv_gc_free_all(I);
    arena_free_all(&I->mem);
    free(I->dirs);
    free(I->fname);
    free(I->fline);
}

/* ================= constructors ================= */

static NVValue nil_singleton = { T_NIL, 0, { 0 } };
static NVValue true_singleton  = { T_BOOL, 0, { .b = 1 } };
static NVValue false_singleton = { T_BOOL, 0, { .b = 0 } };

NVValue *nv_nil(void) { return &nil_singleton; }
NVValue *nv_bool(Interp *I, bool v) { (void)I; return v ? &true_singleton : &false_singleton; }

NVValue *nv_int(Interp *I, long long v) {
    NVValue *r = (NVValue *)nv_gc_alloc(I, sizeof(NVValue), OT_VALUE);
    r->type = T_INT; r->as.i = v;
    return r;
}

NVValue *nv_float(Interp *I, double v) {
    NVValue *r = (NVValue *)nv_gc_alloc(I, sizeof(NVValue), OT_VALUE);
    r->type = T_FLOAT; r->as.f = v;
    return r;
}

NVValue *nv_strl(Interp *I, const char *s, size_t len) {
    NVValue *r = (NVValue *)nv_gc_alloc(I, sizeof(NVValue), OT_VALUE);
    char *buf = (char *)nv_gc_alloc(I, len + 1, OT_STRBUF);
    if (len) memcpy(buf, s, len);
    buf[len] = '\0';
    r->type = T_STR; r->slen = len; r->as.s = buf;
    return r;
}

NVValue *nv_str(Interp *I, const char *s) { return nv_strl(I, s, strlen(s)); }

NVValue *nv_arr_new(Interp *I) {
    NVValue *r = (NVValue *)nv_gc_alloc(I, sizeof(NVValue), OT_VALUE);
    NVArray *a = (NVArray *)nv_gc_alloc(I, sizeof(NVArray), OT_ARRAY);
    memset(a, 0, sizeof(NVArray));
    a->cap = 4;
    a->items = (NVValue **)nv_gc_alloc(I, a->cap * sizeof(NVValue *), OT_ARRITEMS);
    r->type = T_ARR; r->as.arr = a;
    return r;
}

void nv_arr_push(Interp *I, NVValue *arrv, NVValue *item) {
    NVArray *a = arrv->as.arr;
    if (a->n + 1 > a->cap) {
        size_t cap = a->cap * 2;
        NVValue **items = (NVValue **)nv_gc_alloc(I, cap * sizeof(NVValue *), OT_ARRITEMS);
        memcpy(items, a->items, a->n * sizeof(NVValue *));
        a->items = items; a->cap = cap;   /* old item list becomes garbage */
    }
    a->items[a->n++] = item;
}

NVValue *nv_map_new(Interp *I) {
    NVValue *r = (NVValue *)nv_gc_alloc(I, sizeof(NVValue), OT_VALUE);
    r->type = T_MAP; r->as.map = nvtab_new(I);
    return r;
}

ObjClass *nv_new_class(Interp *I, const char *name, ObjClass *base) {
    ObjClass *c = (ObjClass *)nv_gc_alloc(I, sizeof(ObjClass), OT_CLASS);
    c->name = name;
    c->base = base;
    c->methods = nvtab_new(I);
    return c;
}

NVValue *nv_inst_new(Interp *I, ObjClass *cls) {
    NVValue *r = (NVValue *)nv_gc_alloc(I, sizeof(NVValue), OT_VALUE);
    ObjInst *o = (ObjInst *)nv_gc_alloc(I, sizeof(ObjInst), OT_INST);
    o->cls = cls;
    o->fields = nvtab_new(I);
    r->type = T_INSTANCE; r->as.inst = o;
    return r;
}

NVValue *nv_fn_native(Interp *I, const char *name, NNative fn, int amin, int amax) {
    NVValue *r = (NVValue *)nv_gc_alloc(I, sizeof(NVValue), OT_VALUE);
    ObjFn *f = (ObjFn *)nv_gc_alloc(I, sizeof(ObjFn), OT_FN);
    memset(f, 0, sizeof(ObjFn));
    f->name = name; f->native = true; f->native_fn = fn;
    f->arity_min = amin; f->arity_max = amax;
    r->type = T_FN; r->as.fn = f;
    return r;
}

NVValue *nv_new_error(Interp *I, const char *msg) {
    NVValue *inst = nv_inst_new(I, I->error_class);
    NVValue *m = nv_str(I, msg);
    NVValue *line = nv_int(I, I->cur_line);
    nvtab_set(I, inst->as.inst->fields, "$message", m);
    nvtab_set(I, inst->as.inst->fields, "$line", line);
    return inst;
}

NVValue *nv_snapshot_trace(Interp *I) {
    SB b; sb_init(&b);
    for (int k = I->depth - 1; k >= 0 && k >= I->depth - 24; k--) {
        char ln[32];
        snprintf(ln, sizeof(ln), "%d", I->fline[k]);
        sb_puts(&b, "    at ");
        sb_puts(&b, I->fname[k]);
        sb_puts(&b, " (line ");
        sb_puts(&b, ln);
        sb_puts(&b, ")\n");
    }
    NVValue *r = nv_strl(I, b.p ? b.p : "", b.n);
    sb_free(&b);
    return r;
}

NVValue *nv_err(Interp *I, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    NVValue *inst = nv_new_error(I, msg);
    NVValue *tr = nv_snapshot_trace(I);
    if (tr->slen)
        nvtab_set(I, inst->as.inst->fields, "$trace", tr);
    I->err_value = inst;
    return NULL;
}

Ctrl nv_throw_ctrl(Interp *I) {
    Ctrl c = { SIG_THROW, I->err_value };
    I->err_value = NULL;
    return c;
}

/* ================= value operations ================= */

const char *nv_type_name(NType t) {
    switch (t) {
        case T_NIL: return "nil";
        case T_BOOL: return "bool";
        case T_INT: return "int";
        case T_FLOAT: return "float";
        case T_STR: return "string";
        case T_ARR: return "array";
        case T_MAP: return "map";
        case T_FN: return "fn";
        case T_CLASS: return "class";
        case T_INSTANCE: return "instance";
    }
    return "?";
}

bool nv_truthy(NVValue *v) {
    switch (v->type) {
        case T_NIL: return false;
        case T_BOOL: return v->as.b != 0;
        case T_INT: return v->as.i != 0;
        case T_FLOAT: return v->as.f != 0.0;
        case T_STR: return v->slen > 0;
        case T_ARR: return v->as.arr->n > 0;
        case T_MAP: return v->as.map->count > 0;
        default: return true;
    }
}

bool nv_num_val(NVValue *v, double *out) {
    if (v->type == T_INT) { *out = (double)v->as.i; return true; }
    if (v->type == T_FLOAT) { *out = v->as.f; return true; }
    return false;
}

/* quoted string used inside containers: "..." with escapes */
static void quote_str(SB *b, const char *s, size_t len) {
    sb_putc(b, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  sb_puts(b, "\\\""); break;
            case '\\': sb_puts(b, "\\\\"); break;
            case '\n': sb_puts(b, "\\n");  break;
            case '\t': sb_puts(b, "\\t");  break;
            case '\r': sb_puts(b, "\\r");  break;
            default:
                if (c < 0x20) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "\\x%02x", c);
                    sb_puts(b, tmp);
                } else {
                    sb_putc(b, (char)c);
                }
        }
    }
    sb_putc(b, '"');
}

static void value_display(SB *b, NVValue *v, int depth) {
    if (depth > 8) { sb_puts(b, "..."); return; }
    switch (v->type) {
        case T_NIL: sb_puts(b, "nil"); break;
        case T_BOOL: sb_puts(b, v->as.b ? "true" : "false"); break;
        case T_INT: {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%lld", v->as.i);
            sb_puts(b, tmp);
            break;
        }
        case T_FLOAT: sb_fmt_float(b, v->as.f); break;
        case T_STR: sb_putsl(b, v->as.s, v->slen); break;
        case T_ARR: {
            sb_putc(b, '[');
            for (size_t i = 0; i < v->as.arr->n; i++) {
                if (i) sb_puts(b, ", ");
                value_display(b, v->as.arr->items[i], depth + 1);
            }
            sb_putc(b, ']');
            break;
        }
        case T_MAP: {
            sb_putc(b, '{');
            bool first = true;
            for (size_t k = 0; k < v->as.map->on; k++) {
                TEntry *e = &v->as.map->ents[v->as.map->order[k]];
                if (e->state != 1) continue;
                if (!first) sb_puts(b, ", ");
                first = false;
                quote_str(b, e->key, strlen(e->key));
                sb_puts(b, " => ");
                value_display(b, e->val, depth + 1);
            }
            sb_putc(b, '}');
            break;
        }
        case T_FN:
            if (v->as.fn->native) { sb_puts(b, "#<builtin "); sb_puts(b, v->as.fn->name); sb_putc(b, '>'); }
            else { sb_puts(b, "#<fn "); sb_puts(b, v->as.fn->name ? v->as.fn->name : "anon"); sb_putc(b, '>'); }
            break;
        case T_CLASS:
            sb_puts(b, "#<class "); sb_puts(b, v->as.cls->name); sb_putc(b, '>'); break;
        case T_INSTANCE: {
            sb_puts(b, "#<");
            sb_puts(b, v->as.inst->cls->name);
            if (depth <= 4) {
                NVTable *f = v->as.inst->fields;
                for (size_t k = 0; k < f->on; k++) {
                    TEntry *e = &f->ents[f->order[k]];
                    if (e->state != 1) continue;
                    sb_putc(b, ' ');
                    sb_puts(b, e->key + 1);   /* skip '$' */
                    sb_putc(b, '=');
                    value_display(b, e->val, depth + 1);
                }
            }
            sb_putc(b, '>');
            break;
        }
    }
}

NVValue *nv_to_string(Interp *I, NVValue *v) {
    SB b; sb_init(&b);
    value_display(&b, v, 0);
    NVValue *r = nv_strl(I, b.p ? b.p : "", b.n);
    sb_free(&b);
    return r;
}

static bool value_eq(NVValue *a, NVValue *b, int depth) {
    if (a == b) return true;
    if (depth > 20) return false;
    if ((a->type == T_INT || a->type == T_FLOAT) && (b->type == T_INT || b->type == T_FLOAT)) {
        double x, y;
        nv_num_val(a, &x); nv_num_val(b, &y);
        return x == y;
    }
    if (a->type != b->type) return false;
    switch (a->type) {
        case T_NIL: return true;
        case T_BOOL: return a->as.b == b->as.b;
        case T_STR:
            return a->slen == b->slen && memcmp(a->as.s, b->as.s, a->slen) == 0;
        case T_ARR: {
            NVArray *x = a->as.arr, *y = b->as.arr;
            if (x->n != y->n) return false;
            for (size_t i = 0; i < x->n; i++)
                if (!value_eq(x->items[i], y->items[i], depth + 1)) return false;
            return true;
        }
        case T_MAP: {
            NVTable *x = a->as.map, *y = b->as.map;
            if (x->count != y->count) return false;
            for (size_t k = 0; k < x->on; k++) {
                TEntry *e = &x->ents[x->order[k]];
                if (e->state != 1) continue;
                NVValue *bv;
                if (!nvtab_get(y, e->key, &bv)) return false;
                if (!value_eq(e->val, bv, depth + 1)) return false;
            }
            return true;
        }
        default: return false;   /* fn/class/instance compare by identity */
    }
}

bool nv_equal(NVValue *a, NVValue *b) { return value_eq(a, b, 0); }

bool nv_strict_equal(NVValue *a, NVValue *b) {
    if ((a->type == T_INT || a->type == T_FLOAT) && (b->type == T_INT || b->type == T_FLOAT))
        return a->type == b->type && value_eq(a, b, 0);
    if (a->type != b->type) return false;
    return value_eq(a, b, 0);
}

/* ---- map helpers ---- */

void nv_map_set(Interp *I, NVValue *mapv, NVValue *k, NVValue *v) {
    if (k->type != T_STR && k->type != T_INT) {
        nv_err(I, "map keys must be string or int, got %s", nv_type_name(k->type));
        return;
    }
    if (k->type == T_STR) {
        nvtab_set(I, mapv->as.map, k->as.s, v);   /* table copies the key */
    } else {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%lld", k->as.i);
        nvtab_set(I, mapv->as.map, tmp, v);       /* table copies the key */
    }
}

NVValue *nv_map_get(NVValue *mapv, NVValue *k) {
    if (k->type == T_STR) {
        NVValue *out;
        if (nvtab_get(mapv->as.map, k->as.s, &out)) return out;
        return NULL;
    }
    if (k->type == T_INT) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%lld", k->as.i);
        NVValue *out;
        if (nvtab_get(mapv->as.map, tmp, &out)) return out;
        return NULL;
    }
    return NULL;
}

/* ---- environments ---- */

Env *env_new(Interp *I, Env *parent) {
    Env *e = (Env *)nv_gc_alloc(I, sizeof(Env), OT_ENV);
    e->vars = nvtab_new(I);
    e->parent = parent;
    return e;
}

bool env_get(Env *e, const char *key, NVValue **out) {
    for (; e; e = e->parent)
        if (nvtab_get(e->vars, key, out)) return true;
    return false;
}

void env_define(Interp *I, Env *e, const char *key, NVValue *v) {
    nvtab_set(I, e->vars, key, v);
}

void env_assign(Interp *I, Env *e, const char *key, NVValue *v) {
    for (Env *x = e; x; x = x->parent) {
        NVValue *tmp;
        if (nvtab_get(x->vars, key, &tmp)) {
            nvtab_set(I, x->vars, key, v);
            return;
        }
    }
    nvtab_set(I, e->vars, key, v);
}

/* ---- method lookup ---- */

NVValue *nv_find_method(ObjClass *cls, const char *name, ObjClass **def_out) {
    for (ObjClass *c = cls; c; c = c->base) {
        NVValue *m;
        if (nvtab_get(c->methods, name, &m)) {
            if (def_out) *def_out = c;
            return m;
        }
    }
    return NULL;
}

bool nv_inst_of(Interp *I, NVValue *v, const char *cname) {
    (void)I;
    if (v->type == T_INSTANCE) {
        for (ObjClass *c = v->as.inst->cls; c; c = c->base)
            if (strcmp(c->name, cname) == 0) return true;
        return false;
    }
    if (v->type == T_CLASS) {
        for (ObjClass *c = v->as.cls; c; c = c->base)
            if (strcmp(c->name, cname) == 0) return true;
        return false;
    }
    return false;
}
