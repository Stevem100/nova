/*
 * interp.c — Nova tree-walking evaluator.
 *
 * Every eval/exec function returns a Ctrl {Signal, value}. Signals carry
 * return/break/continue/throw through the C call stack without setjmp,
 * which keeps the interpreter portable (Linux, macOS, Windows/MSVC).
 */
#include "nova.h"

static Ctrl ev(Interp *I, Env *env, Node *n);
static Ctrl ex(Interp *I, Env *env, Node *n);
static Ctrl call_fn(Interp *I, NVValue *fnv, NVValue **args, int n, NVValue *self);

static Ctrl c_ok(NVValue *v)   { Ctrl c = { SIG_OK, v }; return c; }
static Ctrl c_none(void)       { Ctrl c = { SIG_OK, NULL }; return c; }

char *nv_read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

/* ---------- call stack frames ---------- */

static void frame_push(Interp *I, const char *name, int line) {
    if (I->depth + 1 > I->fcap) {
        int cap = I->fcap ? I->fcap * 2 : 32;
        I->fname = (const char **)realloc(I->fname, (size_t)cap * sizeof(char *));
        I->fline = (int *)realloc(I->fline, (size_t)cap * sizeof(int));
        if (!I->fname || !I->fline) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        I->fcap = cap;
    }
    I->fname[I->depth] = name;
    I->fline[I->depth] = line;
    I->depth++;
}

static void frame_pop(Interp *I) { if (I->depth > 0) I->depth--; }

/* ---------- file helpers ---------- */

static const char *dir_of(Interp *I, const char *path) {
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (!slash) return arena_strdup(&I->mem, ".");
    return arena_strndup(&I->mem, path, (size_t)(slash - path));
}

static const char *path_join(Interp *I, const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    char *p = (char *)arena_alloc(&I->mem, dl + 1 + nl + 1);
    memcpy(p, dir, dl);
    p[dl] = '/';
    memcpy(p + dl + 1, name, nl);
    p[dl + 1 + nl] = '\0';
    return p;
}

static void dir_push(Interp *I, const char *dir) {
    if (I->ndirs + 1 > I->dircap) {
        int cap = I->dircap ? I->dircap * 2 : 8;
        I->dirs = (const char **)realloc(I->dirs, (size_t)cap * sizeof(char *));
        if (!I->dirs) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
        I->dircap = cap;
    }
    I->dirs[I->ndirs++] = dir;
}

static void dir_pop(Interp *I) { if (I->ndirs > 0) I->ndirs--; }

/* ---------- interpolated strings ---------- */

static Ctrl eval_interp(Interp *I, Env *env, Node *n) {
    SB b; sb_init(&b);
    for (int i = 0; i < n->nparts; i++) {
        SPart *p = &n->parts[i];
        if (!p->is_var) {
            sb_puts(&b, p->text);
            continue;
        }
        NVValue *v;
        if (!env_get(env, p->text, &v)) {
            if (I->soft > 0) continue;   /* missing var -> nothing */
            sb_free(&b);
            nv_err(I, "undefined variable %s in string interpolation", p->text);
            return nv_throw_ctrl(I);
        }
        NVValue *s = nv_to_string(I, v);
        sb_putsl(&b, s->as.s, s->slen);
    }
    NVValue *r = nv_strl(I, b.p ? b.p : "", b.n);
    sb_free(&b);
    return c_ok(r);
}

/* ---------- member / index access ---------- */

/* instance fields are stored under "$name" keys (distinguishes them from methods) */
static const char *dollar_name(Interp *I, const char *name) {
    size_t n = strlen(name);
    char *p = (char *)arena_alloc(&I->mem, n + 2);
    p[0] = '$';
    memcpy(p + 1, name, n);
    p[n + 1] = '\0';
    return p;
}

static Ctrl soft_nil(Interp *I) {
    I->err_value = NULL;
    return c_ok(nv_nil());
}

static Ctrl member_read(Interp *I, NVValue *obj, const char *name) {
    if (obj->type == T_INSTANCE) {
        ObjInst *o = obj->as.inst;
        NVValue *v;
        char keybuf[256];
        snprintf(keybuf, sizeof(keybuf), "$%s", name);
        if (nvtab_get(o->fields, keybuf, &v)) return c_ok(v);
        v = nv_find_method(o->cls, name, NULL);
        if (v) return c_ok(v);
        if (I->soft > 0) return soft_nil(I);
        nv_err(I, "unknown property '%s' on class %s", name, o->cls->name);
        return nv_throw_ctrl(I);
    }
    if (obj->type == T_MAP) {
        NVValue keyv; memset(&keyv, 0, sizeof(keyv));
        keyv.type = T_STR; keyv.as.s = (char *)name; keyv.slen = strlen(name);
        NVValue *k = nv_map_get(obj, &keyv);
        if (k) return c_ok(k);
        if (I->soft > 0) return soft_nil(I);
        nv_err(I, "undefined map member '%s'", name);
        return nv_throw_ctrl(I);
    }
    if (I->soft > 0) return soft_nil(I);
    nv_err(I, "cannot access member '%s' on a value of type %s", name, nv_type_name(obj->type));
    return nv_throw_ctrl(I);
}

static Ctrl member_store(Interp *I, NVValue *obj, const char *name, NVValue *v) {
    if (obj->type == T_INSTANCE) {
        nvtab_set(I, obj->as.inst->fields, dollar_name(I, name), v);
        return c_ok(v);
    }
    if (obj->type == T_MAP) {
        NVValue keyv; memset(&keyv, 0, sizeof(keyv));
        keyv.type = T_STR; keyv.as.s = (char *)name; keyv.slen = strlen(name);
        nv_map_set(I, obj, &keyv, v);
        if (I->err_value) return nv_throw_ctrl(I);
        return c_ok(v);
    }
    nv_err(I, "cannot set member '%s' on a value of type %s", name, nv_type_name(obj->type));
    return nv_throw_ctrl(I);
}

static Ctrl idx_read(Interp *I, NVValue *obj, NVValue *idx) {
    if (obj->type == T_ARR) {
        if (idx->type != T_INT) {
            if (I->soft > 0) return soft_nil(I);
            nv_err(I, "array index must be int, got %s", nv_type_name(idx->type));
            return nv_throw_ctrl(I);
        }
        NVArray *a = obj->as.arr;
        long long i = idx->as.i;
        if (i < 0) i += (long long)a->n;
        if (i < 0 || (size_t)i >= a->n) {
            if (I->soft > 0) return soft_nil(I);
            nv_err(I, "array index %lld out of range (length %zu)", idx->as.i, a->n);
            return nv_throw_ctrl(I);
        }
        return c_ok(a->items[i]);
    }
    if (obj->type == T_STR) {
        if (idx->type != T_INT) {
            if (I->soft > 0) return soft_nil(I);
            nv_err(I, "string index must be int, got %s", nv_type_name(idx->type));
            return nv_throw_ctrl(I);
        }
        long long i = idx->as.i;
        if (i < 0) i += (long long)obj->slen;
        if (i < 0 || (size_t)i >= obj->slen) {
            if (I->soft > 0) return soft_nil(I);
            nv_err(I, "string index %lld out of range (length %zu)", idx->as.i, obj->slen);
            return nv_throw_ctrl(I);
        }
        return c_ok(nv_strl(I, obj->as.s + i, 1));
    }
    if (obj->type == T_MAP) {
        if (idx->type != T_STR && idx->type != T_INT) {
            if (I->soft > 0) return soft_nil(I);
            nv_err(I, "map key must be string or int, got %s", nv_type_name(idx->type));
            return nv_throw_ctrl(I);
        }
        NVValue *v = nv_map_get(obj, idx);
        if (v) return c_ok(v);
        if (I->soft > 0) return soft_nil(I);
        if (idx->type == T_STR)
            nv_err(I, "undefined map key \"%s\"", idx->as.s);
        else
            nv_err(I, "undefined map key %lld", idx->as.i);
        return nv_throw_ctrl(I);
    }
    if (I->soft > 0) return soft_nil(I);
    nv_err(I, "cannot index a value of type %s with []", nv_type_name(obj->type));
    return nv_throw_ctrl(I);
}

static Ctrl idx_store(Interp *I, NVValue *obj, NVValue *idx, NVValue *v) {
    if (obj->type == T_ARR) {
        if (idx->type != T_INT) {
            nv_err(I, "array index must be int, got %s", nv_type_name(idx->type));
            return nv_throw_ctrl(I);
        }
        NVArray *a = obj->as.arr;
        long long i = idx->as.i;
        if (i < 0) i += (long long)a->n;
        if (i < 0 || (size_t)i >= a->n) {
            nv_err(I, "array index %lld out of range (length %zu)", idx->as.i, a->n);
            return nv_throw_ctrl(I);
        }
        a->items[i] = v;
        return c_ok(v);
    }
    if (obj->type == T_MAP) {
        nv_map_set(I, obj, idx, v);
        if (I->err_value) return nv_throw_ctrl(I);
        return c_ok(v);
    }
    if (obj->type == T_STR) {
        nv_err(I, "strings are immutable; use concat or replace() to build new strings");
        return nv_throw_ctrl(I);
    }
    nv_err(I, "cannot index-assign a value of type %s", nv_type_name(obj->type));
    return nv_throw_ctrl(I);
}

/* slice: obj[start..end], Python-style clamping, end exclusive */
static Ctrl eval_slice(Interp *I, NVValue *obj, NVValue *sv, NVValue *ev_) {
    if (sv->type != T_INT || ev_->type != T_INT) {
        nv_err(I, "slice bounds must be ints");
        return nv_throw_ctrl(I);
    }
    long long s = sv->as.i, e = ev_->as.i;
    if (obj->type == T_ARR) {
        long long n = (long long)obj->as.arr->n;
        if (s < 0) s += n;
        if (e < 0) e += n;
        if (s < 0) s = 0;
        if (e > n) e = n;
        if (e < s) e = s;
        NVValue *out = nv_arr_new(I);
        for (long long i = s; i < e; i++)
            nv_arr_push(I, out, obj->as.arr->items[i]);
        return c_ok(out);
    }
    if (obj->type == T_STR) {
        long long n = (long long)obj->slen;
        if (s < 0) s += n;
        if (e < 0) e += n;
        if (s < 0) s = 0;
        if (e > n) e = n;
        if (e < s) e = s;
        return c_ok(nv_strl(I, obj->as.s + s, (size_t)(e - s)));
    }
    nv_err(I, "cannot slice a value of type %s", nv_type_name(obj->type));
    return nv_throw_ctrl(I);
}

/* ---------- operators ---------- */

static NVValue *arr_concat(Interp *I, NVValue *l, NVValue *r) {
    NVValue *out = nv_arr_new(I);
    for (size_t i = 0; i < l->as.arr->n; i++) nv_arr_push(I, out, l->as.arr->items[i]);
    for (size_t i = 0; i < r->as.arr->n; i++) nv_arr_push(I, out, r->as.arr->items[i]);
    return out;
}

static NVValue *map_merge(Interp *I, NVValue *l, NVValue *r) {
    NVValue *out = nv_map_new(I);
    for (size_t k = 0; k < l->as.map->on; k++) {
        TEntry *e = &l->as.map->ents[l->as.map->order[k]];
        if (e->state == 1) nvtab_set(I, out->as.map, e->key, e->val);
    }
    for (size_t k = 0; k < r->as.map->on; k++) {
        TEntry *e = &r->as.map->ents[r->as.map->order[k]];
        if (e->state == 1) nvtab_set(I, out->as.map, e->key, e->val);
    }
    return out;
}

static Ctrl arith(Interp *I, int op, NVValue *l, NVValue *r) {
    bool li = l->type == T_INT, ri = r->type == T_INT;
    double lf, rf;
    switch (op) {
        case O_ADD:
            if (l->type == T_ARR && r->type == T_ARR) return c_ok(arr_concat(I, l, r));
            if (l->type == T_MAP && r->type == T_MAP) return c_ok(map_merge(I, l, r));
            if (li && ri) return c_ok(nv_int(I, l->as.i + r->as.i));
            if (nv_num_val(l, &lf) && nv_num_val(r, &rf)) return c_ok(nv_float(I, lf + rf));
            if (l->type == T_STR || r->type == T_STR) {
                nv_err(I, "cannot add %s and %s; use '.' to join strings", nv_type_name(l->type), nv_type_name(r->type));
                return nv_throw_ctrl(I);
            }
            nv_err(I, "cannot add %s and %s", nv_type_name(l->type), nv_type_name(r->type));
            return nv_throw_ctrl(I);
        case O_SUB:
            if (li && ri) return c_ok(nv_int(I, l->as.i - r->as.i));
            if (nv_num_val(l, &lf) && nv_num_val(r, &rf)) return c_ok(nv_float(I, lf - rf));
            nv_err(I, "cannot subtract %s from %s", nv_type_name(r->type), nv_type_name(l->type));
            return nv_throw_ctrl(I);
        case O_MUL:
            if (l->type == T_STR && r->type == T_INT) {
                if (r->as.i < 0) r->as.i = 0;
                if ((long long)l->slen * r->as.i > 50000000) {
                    nv_err(I, "string repeat result too large");
                    return nv_throw_ctrl(I);
                }
                SB b; sb_init(&b);
                for (long long k = 0; k < r->as.i; k++) sb_putsl(&b, l->as.s, l->slen);
                NVValue *out = nv_strl(I, b.p ? b.p : "", b.n);
                sb_free(&b);
                return c_ok(out);
            }
            if (li && ri) return c_ok(nv_int(I, l->as.i * r->as.i));
            if (nv_num_val(l, &lf) && nv_num_val(r, &rf)) return c_ok(nv_float(I, lf * rf));
            nv_err(I, "cannot multiply %s and %s", nv_type_name(l->type), nv_type_name(r->type));
            return nv_throw_ctrl(I);
        case O_DIV:
            if (li && ri) {
                if (r->as.i == 0) {
                    nv_err(I, "division by zero");
                    return nv_throw_ctrl(I);
                }
                return c_ok(nv_int(I, l->as.i / r->as.i));
            }
            if (nv_num_val(l, &lf) && nv_num_val(r, &rf)) {
                if (rf == 0.0) {
                    nv_err(I, "division by zero");
                    return nv_throw_ctrl(I);
                }
                return c_ok(nv_float(I, lf / rf));
            }
            nv_err(I, "cannot divide %s by %s", nv_type_name(l->type), nv_type_name(r->type));
            return nv_throw_ctrl(I);
        case O_MOD:
            if (li && ri) {
                if (r->as.i == 0) {
                    nv_err(I, "modulo by zero");
                    return nv_throw_ctrl(I);
                }
                return c_ok(nv_int(I, l->as.i % r->as.i));
            }
            nv_err(I, "modulo expects ints, got %s and %s", nv_type_name(l->type), nv_type_name(r->type));
            return nv_throw_ctrl(I);
        case O_POW:
            if (li && ri && r->as.i >= 0 && r->as.i <= 62) {
                long long base = l->as.i, res = 1;
                bool overflow = false;
                for (long long k = 0; k < r->as.i; k++) {
                    if (res > (1LL << 62) / (base ? (base < 0 ? -base : base) : 1)) { overflow = true; break; }
                    res *= base;
                }
                if (!overflow) return c_ok(nv_int(I, res));
            }
            if (nv_num_val(l, &lf) && nv_num_val(r, &rf)) return c_ok(nv_float(I, pow(lf, rf)));
            nv_err(I, "cannot raise %s to %s", nv_type_name(l->type), nv_type_name(r->type));
            return nv_throw_ctrl(I);
        case O_CAT: {
            NVValue *ls = nv_to_string(I, l);
            NVValue *rs = nv_to_string(I, r);
            SB b; sb_init(&b);
            sb_putsl(&b, ls->as.s, ls->slen);
            sb_putsl(&b, rs->as.s, rs->slen);
            NVValue *out = nv_strl(I, b.p ? b.p : "", b.n);
            sb_free(&b);
            return c_ok(out);
        }
    }
    nv_err(I, "internal error: bad arithmetic op %d", op);
    return nv_throw_ctrl(I);
}

static Ctrl compare(Interp *I, int op, NVValue *l, NVValue *r) {
    double lf, rf;
    bool both_num = nv_num_val(l, &lf) && nv_num_val(r, &rf);
    bool lt, eq;
    if (both_num) {
        lt = lf < rf;
        eq = lf == rf;
    } else if (l->type == T_STR && r->type == T_STR) {
        size_t m = l->slen < r->slen ? l->slen : r->slen;
        int c = m ? memcmp(l->as.s, r->as.s, m) : 0;
        if (c == 0) c = l->slen < r->slen ? -1 : l->slen > r->slen ? 1 : 0;
        lt = c < 0;
        eq = c == 0;
    } else if (l->type == T_ARR && r->type == T_ARR && (op == O_LT || op == O_GT)) {
        /* arrays: < means "shorter", > means "longer" */
        lt = l->as.arr->n < r->as.arr->n;
        eq = l->as.arr->n == r->as.arr->n;
    } else {
        nv_err(I, "cannot compare %s and %s with < <= > >=", nv_type_name(l->type), nv_type_name(r->type));
        return nv_throw_ctrl(I);
    }
    bool res = false;
    switch (op) {
        case O_LT: res = lt; break;
        case O_LE: res = lt || eq; break;
        case O_GT: res = !lt && !eq; break;
        case O_GE: res = !lt; break;
    }
    return c_ok(nv_bool(I, res));
}

static Ctrl incdec_num(Interp *I, NVValue *cur, int op) {
    if (cur->type == T_INT) return c_ok(nv_int(I, cur->as.i + (op == O_INC ? 1 : -1)));
    if (cur->type == T_FLOAT) return c_ok(nv_float(I, cur->as.f + (op == O_INC ? 1.0 : -1.0)));
    nv_err(I, "cannot apply ++/-- to a value of type %s", nv_type_name(cur->type));
    return nv_throw_ctrl(I);
}

static Ctrl apply_assign_op(Interp *I, int op, NVValue *cur, NVValue *rhs) {
    switch (op) {
        case A_ADD: return arith(I, O_ADD, cur, rhs);
        case A_SUB: return arith(I, O_SUB, cur, rhs);
        case A_MUL: return arith(I, O_MUL, cur, rhs);
        case A_DIV: return arith(I, O_DIV, cur, rhs);
        case A_MOD: return arith(I, O_MOD, cur, rhs);
        case A_CAT: return arith(I, O_CAT, cur, rhs);
    }
    nv_err(I, "internal error: bad assign op %d", op);
    return nv_throw_ctrl(I);
}
/* ---------- function calls ---------- */

static bool check_arity(Interp *I, ObjFn *f, int n) {
    if (n < f->arity_min || (f->arity_max >= 0 && n > f->arity_max)) {
        if (f->arity_min == f->arity_max)
            nv_err(I, "%s() expects %d argument%s, got %d",
                   f->name ? f->name : "<anon>", f->arity_min,
                   f->arity_min == 1 ? "" : "s", n);
        else if (f->arity_max < 0)
            nv_err(I, "%s() expects at least %d argument%s, got %d",
                   f->name ? f->name : "<anon>", f->arity_min,
                   f->arity_min == 1 ? "" : "s", n);
        else
            nv_err(I, "%s() expects %d to %d arguments, got %d",
                   f->name ? f->name : "<anon>", f->arity_min, f->arity_max, n);
        return false;
    }
    return true;
}

static Ctrl call_fn(Interp *I, NVValue *fnv, NVValue **args, int n, NVValue *self) {
    ObjFn *f = fnv->as.fn;

    if (f->native) {
        if (!check_arity(I, f, n)) return nv_throw_ctrl(I);
        /* native calls do not bind arguments into an environment, so the
         * arguments' only references live in the (malloc'd) args array;
         * pin them for the duration — natives that run user callbacks
         * (map/filter/reduce/sort) reach statement boundaries. */
        size_t mk = nv_gc_pin(I, NULL, GC_TAG_VALUE);
        for (int i = 0; i < n; i++) nv_gc_pin(I, args[i], GC_TAG_VALUE);
        NVValue *r = f->native_fn(I, args, n);
        nv_gc_unpin(I, mk);
        if (!r) return nv_throw_ctrl(I);
        return c_ok(r);
    }

    if (!check_arity(I, f, n)) return nv_throw_ctrl(I);
    if (I->depth >= NOVA_MAX_DEPTH) {
        nv_err(I, "maximum call depth (%d) exceeded; runaway recursion?", NOVA_MAX_DEPTH);
        return nv_throw_ctrl(I);
    }

    Env *cenv = env_new(I, f->closure);
    if (self) env_define(I, cenv, "$this", self);
    for (int i = 0; i < f->nparams; i++)
        env_define(I, cenv, f->pnames[i], args[i]);

    /* the callee environment and function value are only referenced by C
     * locals while the body runs; pin them across its statements */
    size_t mk = nv_gc_pin(I, cenv, GC_TAG_ENV);
    nv_gc_pin(I, fnv, GC_TAG_VALUE);

    ObjFn *saved_fn = I->cur_fn;
    int saved_line = I->cur_line;
    I->cur_fn = f;
    /* frame_push/frame_pop own the depth counter (one frame per user call) */
    frame_push(I, f->name ? f->name : "<anon>", saved_line);
    Ctrl r = ex(I, cenv, f->body);
    frame_pop(I);
    nv_gc_unpin(I, mk);
    I->cur_fn = saved_fn;
    I->cur_line = saved_line;

    if (r.s == SIG_OK) return c_ok(nv_nil());
    if (r.s == SIG_RETURN) return c_ok(r.v ? r.v : nv_nil());
    if (r.s == SIG_BREAK || r.s == SIG_CONTINUE) {
        nv_err(I, "'%s' used outside of a loop",
               r.s == SIG_BREAK ? "break" : "continue");
        return nv_throw_ctrl(I);
    }
    return r;   /* SIG_THROW propagates */
}

bool nv_call_value(Interp *I, NVValue *fn, NVValue **args, int n, NVValue **out) {
    if (fn->type != T_FN) {
        nv_err(I, "expected a function, got %s", nv_type_name(fn->type));
        return false;
    }
    Ctrl c = call_fn(I, fn, args, n, NULL);
    if (c.s == SIG_THROW) {
        I->err_value = c.v;
        return false;
    }
    *out = c.v ? c.v : nv_nil();
    return true;
}

/* call a method on an instance, searching from class `start` (NULL = full chain) */
static Ctrl call_method_from(Interp *I, NVValue *obj, ObjClass *start,
                             const char *name, NVValue **args, int n) {
    ObjClass *def = NULL;
    NVValue *m = nv_find_method(start, name, &def);
    if (!m) {
        nv_err(I, "call to undefined method %s->%s()", start->name, name);
        return nv_throw_ctrl(I);
    }
    (void)def;
    return call_fn(I, m, args, n, obj);
}

/* ---------- expression evaluation ---------- */

static Ctrl eval_args(Interp *I, Env *env, Node *call, NVValue ***out_args, int *out_n,
                      size_t *out_pin) {
    int n = call->nkids;
    NVValue **args = NULL;
    size_t mk = nv_gc_pin(I, NULL, GC_TAG_VALUE);
    if (n > 0) {
        args = (NVValue **)malloc((size_t)n * sizeof(NVValue *));
        if (!args) { nv_err(I, "out of memory"); return nv_throw_ctrl(I); }
        for (int i = 0; i < n; i++) {
            Ctrl c = ev(I, env, call->kids[i]);
            if (c.s != SIG_OK) { free(args); nv_gc_unpin(I, mk); return c; }
            args[i] = c.v;
            /* earlier arguments live only in this malloc'd array while
             * later ones evaluate (their evaluation may cross GC safe
             * points through user function calls) */
            nv_gc_pin(I, c.v, GC_TAG_VALUE);
        }
    }
    *out_args = args;
    *out_n = n;
    *out_pin = mk;
    return c_none();
}

static Ctrl eval_call(Interp *I, Env *env, Node *n) {
    /* constructor: new Point(...) / new $clsvar(...) / new $mod->Class(...) */
    if (n->op == 1) {
        Ctrl cc = ev(I, env, n->a);
        if (cc.s != SIG_OK) return cc;
        if (cc.v->type != T_CLASS) {
            nv_err(I, "'new' expects a class, got %s", nv_type_name(cc.v->type));
            return nv_throw_ctrl(I);
        }
        NVValue **args = NULL; int an = 0; size_t mk = 0;
        /* keep the class value (and thus the ObjClass chain) alive while
         * arguments and the constructor body evaluate */
        size_t cls_mk = nv_gc_pin(I, cc.v, GC_TAG_VALUE);
        Ctrl ac = eval_args(I, env, n, &args, &an, &mk);
        if (ac.s != SIG_OK) { nv_gc_unpin(I, cls_mk); return ac; }
        ObjClass *cls = cc.v->as.cls;
        NVValue *inst = nv_inst_new(I, cls);
        NVValue *init = nv_find_method(cls, "init", NULL);
        Ctrl r = c_ok(inst);
        if (init) {
            r = call_fn(I, init, args, an, inst);
            if (r.s == SIG_OK) r = c_ok(inst);   /* constructor always yields the instance */
            else if (r.s == SIG_RETURN) r = c_ok(inst);
        }
        free(args);
        nv_gc_unpin(I, mk);
        nv_gc_unpin(I, cls_mk);
        return r;
    }

    Node *callee = n->a;

    /* plain function call: name(...) or $fnv(...) */
    if (callee->kind == N_VAR) {
        NVValue *fv;
        if (!env_get(env, callee->name, &fv)) {
            if (callee->name[0] == '$')
                nv_err(I, "undefined variable %s", callee->name);
            else
                nv_err(I, "call to undefined function %s()", callee->name);
            return nv_throw_ctrl(I);
        }
        NVValue **args = NULL; int an = 0; size_t mk = 0;
        Ctrl ac = eval_args(I, env, n, &args, &an, &mk);
        if (ac.s != SIG_OK) return ac;
        if (fv->type != T_FN) {
            free(args);
            nv_gc_unpin(I, mk);
            if (fv->type == T_CLASS) {
                nv_err(I, "class '%s' cannot be called directly; use new %s(...)", callee->name, callee->name);
                return nv_throw_ctrl(I);
            }
            nv_err(I, "value of type %s is not callable", nv_type_name(fv->type));
            return nv_throw_ctrl(I);
        }
        Ctrl r = call_fn(I, fv, args, an, NULL);
        free(args);
        nv_gc_unpin(I, mk);
        return r;
    }

    /* method call: $obj->name(...) */
    if (callee->kind == N_MEMBER) {
        Ctrl oc = ev(I, env, callee->a);
        if (oc.s != SIG_OK) return oc;
        NVValue *obj = oc.v;
        /* the receiver lives in a C local while arguments evaluate */
        size_t obj_mk = nv_gc_pin(I, obj, GC_TAG_VALUE);
        NVValue **args = NULL; int an = 0; size_t mk = 0;
        Ctrl ac = eval_args(I, env, n, &args, &an, &mk);
        if (ac.s != SIG_OK) { nv_gc_unpin(I, obj_mk); return ac; }
        Ctrl r;
        if (obj->type == T_INSTANCE) {
            r = call_method_from(I, obj, obj->as.inst->cls, callee->name, args, an);
        } else if (obj->type == T_MAP) {
            NVValue keyv; memset(&keyv, 0, sizeof(keyv));
            keyv.type = T_STR; keyv.as.s = (char *)callee->name; keyv.slen = strlen(callee->name);
            NVValue *m = nv_map_get(obj, &keyv);
            if (!m) {
                free(args);
                nv_gc_unpin(I, mk);
                nv_gc_unpin(I, obj_mk);
                nv_err(I, "unknown map member '%s'", callee->name);
                return nv_throw_ctrl(I);
            }
            if (m->type != T_FN) {
                free(args);
                nv_gc_unpin(I, mk);
                nv_gc_unpin(I, obj_mk);
                nv_err(I, "map member '%s' is not a function", callee->name);
                return nv_throw_ctrl(I);
            }
            r = call_fn(I, m, args, an, NULL);
        } else {
            nv_err(I, "cannot call method '%s' on a value of type %s", callee->name, nv_type_name(obj->type));
            r = nv_throw_ctrl(I);
        }
        free(args);
        nv_gc_unpin(I, mk);
        nv_gc_unpin(I, obj_mk);
        return r;
    }

    /* parent::method(...) */
    if (callee->kind == N_PARENT) {
        ObjFn *cur = I->cur_fn;
        if (!cur || !cur->in_class || !cur->in_class->base) {
            nv_err(I, "'parent::%s()' used outside a class method (or the class has no parent)", callee->name);
            return nv_throw_ctrl(I);
        }
        NVValue *selfv;
        if (!env_get(env, "$this", &selfv)) {
            nv_err(I, "'parent::%s()' called without $this", callee->name);
            return nv_throw_ctrl(I);
        }
        NVValue **args = NULL; int an = 0; size_t mk = 0;
        Ctrl ac = eval_args(I, env, n, &args, &an, &mk);
        if (ac.s != SIG_OK) return ac;
        Ctrl r = call_method_from(I, selfv, cur->in_class->base, callee->name, args, an);
        free(args);
        nv_gc_unpin(I, mk);
        return r;
    }

    /* any other expression evaluating to a function */
    Ctrl fc = ev(I, env, callee);
    if (fc.s != SIG_OK) return fc;
    if (fc.v->type != T_FN) {
        nv_err(I, "value of type %s is not callable", nv_type_name(fc.v->type));
        return nv_throw_ctrl(I);
    }
    NVValue **args = NULL; int an = 0; size_t mk = 0;
    /* the callee value lives only here while arguments evaluate */
    size_t fn_mk = nv_gc_pin(I, fc.v, GC_TAG_VALUE);
    Ctrl ac = eval_args(I, env, n, &args, &an, &mk);
    if (ac.s != SIG_OK) { nv_gc_unpin(I, fn_mk); return ac; }
    Ctrl r = call_fn(I, fc.v, args, an, NULL);
    free(args);
    nv_gc_unpin(I, mk);
    nv_gc_unpin(I, fn_mk);
    return r;
}

/* ---------- assignment ---------- */

static Ctrl eval_incdec(Interp *I, Env *env, Node *target, int op, bool prefix) {
    if (target->kind == N_VAR) {
        NVValue *cur;
        if (!env_get(env, target->name, &cur)) {
            nv_err(I, "undefined variable %s", target->name);
            return nv_throw_ctrl(I);
        }
        Ctrl nc = incdec_num(I, cur, op);
        if (nc.s != SIG_OK) return nc;
        env_assign(I, env, target->name, nc.v);
        return c_ok(prefix ? nc.v : cur);
    }
    if (target->kind == N_MEMBER) {
        Ctrl oc = ev(I, env, target->a);
        if (oc.s != SIG_OK) return oc;
        Ctrl cc = member_read(I, oc.v, target->name);
        if (cc.s != SIG_OK) return cc;
        Ctrl nc = incdec_num(I, cc.v, op);
        if (nc.s != SIG_OK) return nc;
        Ctrl sc = member_store(I, oc.v, target->name, nc.v);
        if (sc.s != SIG_OK) return sc;
        return c_ok(prefix ? nc.v : cc.v);
    }
    if (target->kind == N_INDEX) {
        Ctrl oc = ev(I, env, target->a);
        if (oc.s != SIG_OK) return oc;
        Ctrl kc = ev(I, env, target->b);
        if (kc.s != SIG_OK) return kc;
        int saved = I->soft; I->soft = 0;
        Ctrl cc = idx_read(I, oc.v, kc.v);
        I->soft = saved;
        if (cc.s != SIG_OK) return cc;
        Ctrl nc = incdec_num(I, cc.v, op);
        if (nc.s != SIG_OK) return nc;
        Ctrl sc = idx_store(I, oc.v, kc.v, nc.v);
        if (sc.s != SIG_OK) return sc;
        return c_ok(prefix ? nc.v : cc.v);
    }
    nv_err(I, "internal error: bad ++/-- target");
    return nv_throw_ctrl(I);
}

static Ctrl eval_assign(Interp *I, Env *env, Node *n) {
    Node *target = n->a;
    bool aug = n->kind == N_AUG;

    if (aug && target->kind == N_APPEND) {
        nv_err(I, "cannot use compound assignment with [] append");
        return nv_throw_ctrl(I);
    }

    if (!aug && target->kind == N_APPEND) {
        Ctrl vc = ev(I, env, n->b);
        if (vc.s != SIG_OK) return vc;
        Ctrl oc = ev(I, env, target->a);
        if (oc.s != SIG_OK) return oc;
        if (oc.v->type != T_ARR) {
            nv_err(I, "[] append expects an array, got %s", nv_type_name(oc.v->type));
            return nv_throw_ctrl(I);
        }
        /* the pushed value lives in a C local while the target evaluates */
        size_t mk = nv_gc_pin(I, vc.v, GC_TAG_VALUE);
        nv_arr_push(I, oc.v, vc.v);
        nv_gc_unpin(I, mk);
        return c_ok(vc.v);
    }

    if (aug && target->kind == N_VAR) {
        NVValue *cur;
        if (!env_get(env, target->name, &cur)) {
            nv_err(I, "undefined variable %s", target->name);
            return nv_throw_ctrl(I);
        }
        Ctrl vc = ev(I, env, n->b);
        if (vc.s != SIG_OK) return vc;
        Ctrl rc = apply_assign_op(I, n->op, cur, vc.v);
        if (rc.s != SIG_OK) return rc;
        env_assign(I, env, target->name, rc.v);
        return rc;
    }

    if (!aug && target->kind == N_VAR) {
        Ctrl vc = ev(I, env, n->b);
        if (vc.s != SIG_OK) return vc;
        env_assign(I, env, target->name, vc.v);
        return vc;
    }

    if (target->kind == N_MEMBER) {
        Ctrl oc = ev(I, env, target->a);
        if (oc.s != SIG_OK) return oc;
        if (aug) {
            Ctrl cc = member_read(I, oc.v, target->name);
            if (cc.s != SIG_OK) return cc;
            size_t mk = nv_gc_pin(I, oc.v, GC_TAG_VALUE);
            nv_gc_pin(I, cc.v, GC_TAG_VALUE);
            Ctrl vc = ev(I, env, n->b);
            Ctrl rc;
            if (vc.s != SIG_OK) { rc = vc; }
            else {
                rc = apply_assign_op(I, n->op, cc.v, vc.v);
                if (rc.s == SIG_OK) {
                    Ctrl sc = member_store(I, oc.v, target->name, rc.v);
                    if (sc.s != SIG_OK) rc = sc;
                }
            }
            nv_gc_unpin(I, mk);
            return rc;
        }
        Ctrl vc = ev(I, env, n->b);
        if (vc.s != SIG_OK) return vc;
        size_t mk = nv_gc_pin(I, vc.v, GC_TAG_VALUE);
        Ctrl sc = member_store(I, oc.v, target->name, vc.v);
        nv_gc_unpin(I, mk);
        if (sc.s != SIG_OK) return sc;
        return vc;
    }

    if (target->kind == N_INDEX) {
        Ctrl oc = ev(I, env, target->a);
        if (oc.s != SIG_OK) return oc;
        Ctrl kc = ev(I, env, target->b);
        if (kc.s != SIG_OK) return kc;
        if (aug) {
            int saved = I->soft; I->soft = 0;
            Ctrl cc = idx_read(I, oc.v, kc.v);
            I->soft = saved;
            if (cc.s != SIG_OK) return cc;
            size_t mk = nv_gc_pin(I, oc.v, GC_TAG_VALUE);
            nv_gc_pin(I, kc.v, GC_TAG_VALUE);
            nv_gc_pin(I, cc.v, GC_TAG_VALUE);
            Ctrl vc = ev(I, env, n->b);
            Ctrl rc;
            if (vc.s != SIG_OK) { rc = vc; }
            else {
                rc = apply_assign_op(I, n->op, cc.v, vc.v);
                if (rc.s == SIG_OK) {
                    Ctrl sc = idx_store(I, oc.v, kc.v, rc.v);
                    if (sc.s != SIG_OK) rc = sc;
                }
            }
            nv_gc_unpin(I, mk);
            return rc;
        }
        Ctrl vc = ev(I, env, n->b);
        if (vc.s != SIG_OK) return vc;
        size_t mk = nv_gc_pin(I, vc.v, GC_TAG_VALUE);
        nv_gc_pin(I, oc.v, GC_TAG_VALUE);
        nv_gc_pin(I, kc.v, GC_TAG_VALUE);
        Ctrl sc = idx_store(I, oc.v, kc.v, vc.v);
        nv_gc_unpin(I, mk);
        if (sc.s != SIG_OK) return sc;
        return vc;
    }

    nv_err(I, "internal error: bad assignment target");
    return nv_throw_ctrl(I);
}

/* ---------- expression dispatcher ---------- */

static Ctrl ev(Interp *I, Env *env, Node *n) {
    I->cur_line = n->line;
    switch (n->kind) {
        case N_INT:    return c_ok(nv_int(I, n->ival));
        case N_FLOAT:  return c_ok(nv_float(I, n->fval));
        case N_BOOL:   return c_ok(nv_bool(I, n->ival != 0));
        case N_NIL:    return c_ok(nv_nil());
        case N_STR:    return eval_interp(I, env, n);
        case N_VAR: {
            NVValue *v;
            if (env_get(env, n->name, &v)) return c_ok(v);
            if (I->soft > 0) return c_ok(nv_nil());
            nv_err(I, "undefined variable %s", n->name);
            return nv_throw_ctrl(I);
        }
        case N_THIS: {
            NVValue *v;
            if (env_get(env, "$this", &v)) return c_ok(v);
            nv_err(I, "'$this' used outside of a class method");
            return nv_throw_ctrl(I);
        }
        case N_ARR: {
            NVValue *a = nv_arr_new(I);
            size_t mk = nv_gc_pin(I, a, GC_TAG_VALUE);
            for (int i = 0; i < n->nkids; i++) {
                Ctrl c = ev(I, env, n->kids[i]);
                if (c.s != SIG_OK) { nv_gc_unpin(I, mk); return c; }
                nv_arr_push(I, a, c.v);
            }
            nv_gc_unpin(I, mk);
            return c_ok(a);
        }
        case N_MAP: {
            NVValue *m = nv_map_new(I);
            size_t mk = nv_gc_pin(I, m, GC_TAG_VALUE);
            for (int i = 0; i + 1 < n->nkids; i += 2) {
                Ctrl kc = ev(I, env, n->kids[i]);
                if (kc.s != SIG_OK) { nv_gc_unpin(I, mk); return kc; }
                /* the key value lives in a C local while the value
                 * evaluates (and may call user functions) */
                nv_gc_pin(I, kc.v, GC_TAG_VALUE);
                Ctrl vc = ev(I, env, n->kids[i + 1]);
                if (vc.s != SIG_OK) { nv_gc_unpin(I, mk); return vc; }
                nv_map_set(I, m, kc.v, vc.v);
                if (I->err_value) { nv_gc_unpin(I, mk); return nv_throw_ctrl(I); }
            }
            nv_gc_unpin(I, mk);
            return c_ok(m);
        }
        case N_ANON: {
            NVValue *fv = (NVValue *)nv_gc_alloc(I, sizeof(NVValue), OT_VALUE);
            ObjFn *f = (ObjFn *)nv_gc_alloc(I, sizeof(ObjFn), OT_FN);
            memset(f, 0, sizeof(ObjFn));
            f->name = NULL;
            f->native = false;
            f->arity_min = n->np;
            f->arity_max = n->np;
            f->pnames = n->pnames;
            f->nparams = n->np;
            f->body = n->a;
            f->closure = env;
            fv->type = T_FN;
            fv->as.fn = f;
            return c_ok(fv);
        }
        case N_BIN: {
            int op = n->op;
            if (op == O_NILCOAL) {
                int saved = I->soft;
                I->soft++;
                Ctrl l = ev(I, env, n->a);
                I->soft = saved;
                if (l.s != SIG_OK) return l;
                if (l.v->type != T_NIL) return l;
                return ev(I, env, n->b);
            }
            if (op == O_AND || op == O_OR) {
                Ctrl l = ev(I, env, n->a);
                if (l.s != SIG_OK) return l;
                if (op == O_OR && nv_truthy(l.v)) return l;
                if (op == O_AND && !nv_truthy(l.v)) return l;
                return ev(I, env, n->b);
            }
            Ctrl l = ev(I, env, n->a);
            if (l.s != SIG_OK) return l;
            /* the left operand lives in a C local while the right one
             * evaluates (its evaluation may call user functions whose
             * bodies cross GC safe points) */
            size_t mk = nv_gc_pin(I, l.v, GC_TAG_VALUE);
            Ctrl r = ev(I, env, n->b);
            if (r.s != SIG_OK) { nv_gc_unpin(I, mk); return r; }
            nv_gc_unpin(I, mk);
            switch (op) {
                case O_EQ:   return c_ok(nv_bool(I, nv_equal(l.v, r.v)));
                case O_NE:   return c_ok(nv_bool(I, !nv_equal(l.v, r.v)));
                case O_IDE:  return c_ok(nv_bool(I, nv_strict_equal(l.v, r.v)));
                case O_NIDE: return c_ok(nv_bool(I, !nv_strict_equal(l.v, r.v)));
                case O_LT: case O_LE: case O_GT: case O_GE:
                    return compare(I, op, l.v, r.v);
                default:
                    return arith(I, op, l.v, r.v);
            }
        }
        case N_UN: {
            if (n->op == O_NOT) {
                Ctrl c = ev(I, env, n->a);
                if (c.s != SIG_OK) return c;
                return c_ok(nv_bool(I, !nv_truthy(c.v)));
            }
            if (n->op == O_NEG) {
                Ctrl c = ev(I, env, n->a);
                if (c.s != SIG_OK) return c;
                if (c.v->type == T_INT) return c_ok(nv_int(I, -c.v->as.i));
                if (c.v->type == T_FLOAT) return c_ok(nv_float(I, -c.v->as.f));
                nv_err(I, "cannot negate a value of type %s", nv_type_name(c.v->type));
                return nv_throw_ctrl(I);
            }
            /* prefix ++ / -- */
            return eval_incdec(I, env, n->a, n->op, true);
        }
        case N_POST:
            return eval_incdec(I, env, n->a, n->op, false);
        case N_TERNARY: {
            Ctrl c = ev(I, env, n->a);
            if (c.s != SIG_OK) return c;
            return nv_truthy(c.v) ? ev(I, env, n->b) : ev(I, env, n->c);
        }
        case N_CALL:
            return eval_call(I, env, n);
        case N_INDEX: {
            Ctrl oc = ev(I, env, n->a);
            if (oc.s != SIG_OK) return oc;
            Ctrl kc = ev(I, env, n->b);
            if (kc.s != SIG_OK) return kc;
            if (n->c) {
                Ctrl ec = ev(I, env, n->c);
                if (ec.s != SIG_OK) return ec;
                return eval_slice(I, oc.v, kc.v, ec.v);
            }
            return idx_read(I, oc.v, kc.v);
        }
        case N_MEMBER: {
            Ctrl oc = ev(I, env, n->a);
            if (oc.s != SIG_OK) return oc;
            return member_read(I, oc.v, n->name);
        }
        case N_ASSIGN:
        case N_AUG:
            return eval_assign(I, env, n);
        case N_APPEND:
            nv_err(I, "[] append can only be used as an assignment target");
            return nv_throw_ctrl(I);
        default:
            nv_err(I, "internal error: unexpected expression node %d", (int)n->kind);
            return nv_throw_ctrl(I);
    }
}

/* ---------- function / class declaration helpers ---------- */

static NVValue *make_user_fn(Interp *I, Node *n, const char *name, Env *env, ObjClass *in_class) {
    NVValue *fv = (NVValue *)nv_gc_alloc(I, sizeof(NVValue), OT_VALUE);
    ObjFn *f = (ObjFn *)nv_gc_alloc(I, sizeof(ObjFn), OT_FN);
    memset(f, 0, sizeof(ObjFn));
    f->name = name;
    f->native = false;
    f->arity_min = n->np;
    f->arity_max = n->np;
    f->pnames = n->pnames;
    f->nparams = n->np;
    f->body = n->a;
    f->closure = env;
    f->in_class = in_class;
    fv->type = T_FN;
    fv->as.fn = f;
    return fv;
}

static Ctrl declare_class(Interp *I, Env *env, Node *n) {
    ObjClass *base = NULL;
    if (n->name2) {
        NVValue *bv;
        if (!env_get(env, n->name2, &bv)) {
            nv_err(I, "undefined class '%s' in 'extends'", n->name2);
            return nv_throw_ctrl(I);
        }
        if (bv->type != T_CLASS) {
            nv_err(I, "'%s' is not a class", n->name2);
            return nv_throw_ctrl(I);
        }
        base = bv->as.cls;
    }
    ObjClass *cls = nv_new_class(I, n->name, base);
    for (int i = 0; i < n->nkids; i++) {
        Node *m = n->kids[i];
        NVValue *fv = make_user_fn(I, m, m->name, env, cls);
        nvtab_set(I, cls->methods, m->name, fv);
    }
    NVValue *cv = (NVValue *)nv_gc_alloc(I, sizeof(NVValue), OT_VALUE);
    cv->type = T_CLASS;
    cv->as.cls = cls;
    env_define(I, env, n->name, cv);
    return c_none();
}

/* ---------- module import ---------- */

static const char *default_module_name(Interp *I, const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    char *p = (char *)arena_alloc(&I->mem, n + 2);
    p[0] = '$';
    for (size_t i = 0; i < n; i++) {
        char c = base[i];
        p[i + 1] = (isalnum((unsigned char)c) || c == '_') ? c : '_';
    }
    p[n + 1] = '\0';
    return p;
}

Ctrl nv_import(Interp *I, Env *importer, const char *path, const char *as_name) {
    const char *resolved = NULL;

    if (I->ndirs > 0) {
        const char *cand = path_join(I, I->dirs[I->ndirs - 1], path);
        FILE *f = fopen(cand, "rb");
        if (f) { fclose(f); resolved = cand; }
    }
    if (!resolved) {
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); resolved = arena_strdup(&I->mem, path); }
    }
    if (!resolved) {
        /* last resort: package resolution (./nova_modules/<pkg>, then the
         * registry configured via NOVA_REGISTRY / `nova pkg registry`) */
        char pbuf[1024], perr[256];
        if (nv_pkg_resolve(path, pbuf, sizeof(pbuf), perr, sizeof(perr)))
            resolved = arena_strdup(&I->mem, pbuf);
        else {
            nv_err(I, "cannot open module \"%s\" (%s)", path, perr);
            return nv_throw_ctrl(I);
        }
    }

    NVValue *hit;
    if (nvtab_get(I->mod_cache, resolved, &hit)) {
        env_assign(I, importer, as_name ? as_name : default_module_name(I, path), hit);
        return c_ok(hit);
    }
    if (nvtab_get(I->mod_busy, resolved, &hit)) {
        nv_err(I, "circular import of \"%s\"", path);
        return nv_throw_ctrl(I);
    }
    nvtab_set(I, I->mod_busy, resolved, nv_nil());

    size_t len;
    char *src = nv_read_whole_file(resolved, &len);
    if (!src) {
        nvtab_del(I->mod_busy, resolved);
        nv_err(I, "cannot open module \"%s\"", path);
        return nv_throw_ctrl(I);
    }
    Node *prog;
    NvDiag diag;
    if (!nv_compile(I, src, resolved, &prog, &diag)) {
        free(src);
        nvtab_del(I->mod_busy, resolved);
        nv_err(I, "syntax error in module \"%s\" at line %d: %s", path, diag.line, diag.msg);
        return nv_throw_ctrl(I);
    }
    free(src);

    Env *menv = env_new(I, I->builtins_env);
    NVValue *gargv;
    if (env_get(I->globals, "$argv", &gargv))
        env_define(I, menv, "$argv", gargv);
    env_define(I, menv, "$__dir__", nv_str(I, dir_of(I, resolved)));

    dir_push(I, dir_of(I, resolved));
    /* the module's environment is only referenced by this C local while
     * its top-level statements run; pin it across them */
    size_t mk = nv_gc_pin(I, menv, GC_TAG_ENV);
    Ctrl r = nv_exec_nodes(I, menv, prog->kids, prog->nkids);
    nv_gc_unpin(I, mk);
    dir_pop(I);
    nvtab_del(I->mod_busy, resolved);

    if (r.s == SIG_THROW) return r;
    if (r.s == SIG_BREAK || r.s == SIG_CONTINUE) {
        nv_err(I, "'break' or 'continue' escaped module \"%s\"", path);
        return nv_throw_ctrl(I);
    }

    NVValue *modv;
    if (r.s == SIG_RETURN) {
        modv = r.v ? r.v : nv_nil();
    } else {
        /* default: export every top-level definition as a map.
         * fn/class names are stored without '$', variables with '$'. */
        modv = nv_map_new(I);
        NVTable *t = menv->vars;
        for (size_t k = 0; k < t->on; k++) {
            TEntry *e = &t->ents[t->order[k]];
            if (e->state != 1) continue;
            if (strcmp(e->key, "$argv") == 0 || strcmp(e->key, "$__dir__") == 0) continue;
            const char *name = (e->key[0] == '$') ? e->key + 1 : e->key;
            nvtab_set(I, modv->as.map, name, e->val);
        }
    }
    nvtab_set(I, I->mod_cache, resolved, modv);
    env_assign(I, importer, as_name ? as_name : default_module_name(I, path), modv);
    return c_ok(modv);
}

/* ---------- uncaught error reporting ---------- */

void nv_report_uncaught(Interp *I, NVValue *v) {
    fflush(stdout);
    if (v && v->type == T_INSTANCE && v->as.inst->cls == I->error_class) {
        NVValue *msg = NULL, *tr = NULL;
        nvtab_get(v->as.inst->fields, "$message", &msg);
        nvtab_get(v->as.inst->fields, "$trace", &tr);
        fprintf(stderr, "Uncaught error: %s\n", msg && msg->type == T_STR ? msg->as.s : "(unknown)");
        if (tr && tr->type == T_STR && tr->slen)
            fprintf(stderr, "%s", tr->as.s);
    } else if (v) {
        NVValue *s = nv_to_string(I, v);
        fprintf(stderr, "Uncaught error: %.*s\n", (int)s->slen, s->as.s);
    }
}

/* ---------- statement execution ---------- */

static Ctrl ex(Interp *I, Env *env, Node *n) {
    I->cur_line = n->line;
    switch (n->kind) {
        case N_BLOCK:
            for (int i = 0; i < n->nkids; i++) {
                nv_gc_maybe(I);            /* safe point between statements */
                Ctrl c = ex(I, env, n->kids[i]);
                if (c.s != SIG_OK) return c;
            }
            return c_none();

        case N_EMPTY:
            return c_none();

        case N_EXPRSTMT: {
            Ctrl c = ev(I, env, n->a);
            if (c.s != SIG_OK) return c;
            if (I->repl && n->a->kind != N_ASSIGN && n->a->kind != N_AUG)
                I->last_expr = c.v;
            return c_none();
        }

        case N_IF: {
            Ctrl c = ev(I, env, n->a);
            if (c.s != SIG_OK) return c;
            if (nv_truthy(c.v)) return ex(I, env, n->b);
            if (n->c) return ex(I, env, n->c);
            return c_none();
        }

        case N_WHILE: {
            for (;;) {
                nv_gc_maybe(I);            /* safe point per iteration */
                Ctrl c = ev(I, env, n->a);
                if (c.s != SIG_OK) return c;
                if (!nv_truthy(c.v)) return c_none();
                Ctrl b = ex(I, env, n->b);
                if (b.s == SIG_BREAK) return c_none();
                if (b.s == SIG_THROW || b.s == SIG_RETURN) return b;
                /* SIG_OK or SIG_CONTINUE -> keep looping */
            }
        }

        case N_FOR: {
            if (n->a) {
                Ctrl c = ev(I, env, n->a);   /* init is an expression (assignment) */
                if (c.s != SIG_OK) return c;
            }
            for (;;) {
                nv_gc_maybe(I);            /* safe point per iteration */
                if (n->b) {
                    Ctrl c = ev(I, env, n->b);
                    if (c.s != SIG_OK) return c;
                    if (!nv_truthy(c.v)) return c_none();
                }
                Ctrl b = ex(I, env, n->d);
                if (b.s == SIG_BREAK) return c_none();
                if (b.s == SIG_THROW || b.s == SIG_RETURN) return b;
                if (n->c) {
                    Ctrl p = ev(I, env, n->c);
                    if (p.s != SIG_OK) return p;
                }
            }
        }

        case N_FOREACH: {
            Ctrl ic = ev(I, env, n->a);
            if (ic.s != SIG_OK) return ic;
            NVValue *iter = ic.v;

            size_t count = 0;
            NVValue **items = NULL;
            const char **keys = NULL;
            bool map_mode = n->name2 != NULL;
            bool is_str = false;
            bool arr_keyed = false;

            if (iter->type == T_ARR) {
                count = iter->as.arr->n;
                items = (NVValue **)malloc(count * sizeof(NVValue *));
                if (!items) { nv_err(I, "out of memory"); return nv_throw_ctrl(I); }
                memcpy(items, iter->as.arr->items, count * sizeof(NVValue *));
                arr_keyed = true;
            } else if (iter->type == T_MAP) {
                NVTable *t = iter->as.map;
                items = (NVValue **)malloc(t->on * sizeof(NVValue *));
                keys = (const char **)malloc(t->on * sizeof(char *));
                if (!items || !keys) { free(items); free(keys); nv_err(I, "out of memory"); return nv_throw_ctrl(I); }
                for (size_t k = 0; k < t->on; k++) {
                    TEntry *e = &t->ents[t->order[k]];
                    if (e->state != 1) continue;
                    keys[count] = e->key;
                    items[count] = e->val;
                    count++;
                }
            } else if (iter->type == T_STR) {
                is_str = true;
                count = iter->slen;
            } else {
                nv_err(I, "foreach expects an array, map or string, got %s", nv_type_name(iter->type));
                return nv_throw_ctrl(I);
            }

            /* the iterable, its snapshot key strings (owned by its table)
             * and the loop environment must survive the body's statements */
            size_t mk = nv_gc_pin(I, iter, GC_TAG_VALUE);
            Env *lenv = env_new(I, env);
            nv_gc_pin(I, lenv, GC_TAG_ENV);
            Ctrl r = c_none();
            bool stop = false;
            for (size_t i = 0; i < count && !stop; i++) {
                nv_gc_maybe(I);            /* safe point per iteration */
                if (is_str) {
                    env_define(I, lenv, n->name, nv_strl(I, iter->as.s + i, 1));
                } else if (map_mode) {
                    if (arr_keyed) {   /* foreach ($arr as $i => $v) gives the index */
                        env_define(I, lenv, n->name, nv_int(I, (long long)i));
                        env_define(I, lenv, n->name2, items[i]);
                    } else {
                        env_define(I, lenv, n->name, nv_str(I, keys[i]));
                        env_define(I, lenv, n->name2, items[i]);
                    }
                } else {
                    env_define(I, lenv, n->name, items[i]);
                }
                Ctrl b = ex(I, lenv, n->b);
                if (b.s == SIG_BREAK || b.s == SIG_THROW || b.s == SIG_RETURN) {
                    r = (b.s == SIG_BREAK) ? c_none() : b;
                    stop = true;
                }
            }
            nv_gc_unpin(I, mk);
            free(items);
            free(keys);
            return r;
        }

        case N_BREAK:    { Ctrl c = { SIG_BREAK, NULL }; return c; }
        case N_CONTINUE: { Ctrl c = { SIG_CONTINUE, NULL }; return c; }

        case N_SWITCH: {
            /* C-style switch: subject evaluated once, first matching branch
             * (or default) starts executing, bodies fall through until a
             * 'break' (consumed here), 'return', 'continue' or 'throw'. */
            Ctrl sc = ev(I, env, n->a);            /* switch subject */
            if (sc.s != SIG_OK) return sc;

            /* the subject must survive label matching (labels may call user
             * functions, whose bodies cross GC safe points) and the branch
             * bodies that follow */
            size_t mk = nv_gc_pin(I, sc.v, GC_TAG_VALUE);

            int start = -1, dflt = -1;
            for (int i = 0; i < n->nkids && start < 0; i++) {
                Node *br = n->kids[i];
                if (br->op == 1 && dflt < 0) dflt = i;
                for (int j = 0; j < br->nkids; j++) {
                    Ctrl lc = ev(I, env, br->kids[j]);   /* case label */
                    if (lc.s != SIG_OK) { nv_gc_unpin(I, mk); return lc; }
                    if (nv_equal(sc.v, lc.v)) { start = i; break; }
                }
            }
            if (start < 0) start = dflt;
            if (start < 0) { nv_gc_unpin(I, mk); return c_none(); }  /* no match, no default */

            for (int i = start; i < n->nkids; i++) {
                Ctrl bc = ex(I, env, n->kids[i]->a);   /* branch body */
                if (bc.s == SIG_OK) continue;          /* fall through */
                if (bc.s == SIG_BREAK) { nv_gc_unpin(I, mk); return c_none(); }
                nv_gc_unpin(I, mk);
                return bc;                             /* return/continue/throw propagate */
            }
            nv_gc_unpin(I, mk);
            return c_none();
        }

        case N_RETURN: {
            Ctrl c = { SIG_RETURN, NULL };
            if (n->a) {
                Ctrl v = ev(I, env, n->a);
                if (v.s != SIG_OK) return v;
                c.v = v.v;
            }
            return c;
        }

        case N_THROW: {
            Ctrl v = ev(I, env, n->a);
            if (v.s != SIG_OK) return v;
            /* attach a call-stack snapshot to Error objects that lack one */
            if (v.v->type == T_INSTANCE && v.v->as.inst->cls == I->error_class) {
                NVValue *tr;
                if (!nvtab_get(v.v->as.inst->fields, "$trace", &tr))
                    nvtab_set(I, v.v->as.inst->fields, "$trace", nv_snapshot_trace(I));
            }
            Ctrl c = { SIG_THROW, v.v };
            return c;
        }

        case N_TRY: {
            Ctrl r = ex(I, env, n->a);
            if (r.s == SIG_THROW && n->b) {
                /* the thrown value and the catch scope must survive the
                 * catch block's statements */
                size_t mk = nv_gc_pin(I, r.v ? r.v : nv_nil(), GC_TAG_VALUE);
                Env *cenv = env_new(I, env);
                nv_gc_pin(I, cenv, GC_TAG_ENV);
                env_define(I, cenv, n->name, r.v ? r.v : nv_nil());
                r = ex(I, cenv, n->b);
                nv_gc_unpin(I, mk);
            }
            if (n->c) {
                /* a value flying through finally (throw/return) must survive
                 * the finally block's statements */
                size_t mkf = (r.s != SIG_OK && r.v)
                    ? nv_gc_pin(I, r.v, GC_TAG_VALUE) : (size_t)-1;
                Ctrl f = ex(I, env, n->c);
                if (mkf != (size_t)-1) nv_gc_unpin(I, mkf);
                if (f.s != SIG_OK) return f;
            }
            return r;
        }

        case N_ECHO: {
            for (int i = 0; i < n->nkids; i++) {
                Ctrl c = ev(I, env, n->kids[i]);
                if (c.s != SIG_OK) return c;
                NVValue *s = nv_to_string(I, c.v);
                fwrite(s->as.s, 1, s->slen, stdout);
            }
            return c_none();
        }

        case N_IMPORT:
            return nv_import(I, env, n->name, n->name2);

        case N_FN_DECL: {
            NVValue *fv = make_user_fn(I, n, n->name, env, NULL);
            env_define(I, env, n->name, fv);
            return c_none();
        }

        case N_CLASS_DECL:
            return declare_class(I, env, n);

        default:
            nv_err(I, "internal error: unexpected statement node %d", (int)n->kind);
            return nv_throw_ctrl(I);
    }
}

/* ---------- public execution entry points ---------- */

Ctrl nv_exec_nodes(Interp *I, Env *env, Node **stmts, int n) {
    for (int i = 0; i < n; i++) {
        nv_gc_maybe(I);                /* GC safe point between statements */
        Ctrl c = ex(I, env, stmts[i]);
        if (c.s != SIG_OK) return c;
    }
    return c_none();
}

Ctrl nv_exec_source(Interp *I, Env *env, const char *src, const char *file, NvDiag *diag) {
    Node *prog;
    Ctrl none = { SIG_OK, NULL };
    if (!nv_compile(I, src, file, &prog, diag)) return none;
    return nv_exec_nodes(I, env, prog->kids, prog->nkids);
}

bool nv_run_file(Interp *I, const char *path, int argc, char **argv, int *exit_code) {
    size_t len;
    char *src = nv_read_whole_file(path, &len);
    if (!src) {
        fprintf(stderr, "nova: cannot open file '%s'\n", path);
        return false;
    }
    Node *prog;
    NvDiag diag;
    if (!nv_compile(I, src, path, &prog, &diag)) {
        fprintf(stderr, "%s:%d: %s\n", path, diag.line, diag.msg);
        free(src);
        return false;
    }
    free(src);

    I->globals = env_new(I, I->builtins_env);
    NVValue *argvv = nv_arr_new(I);
    nv_arr_push(I, argvv, nv_str(I, path));
    for (int i = 0; i < argc; i++) nv_arr_push(I, argvv, nv_str(I, argv[i]));
    env_define(I, I->globals, "$argv", argvv);
    env_define(I, I->globals, "$__dir__", nv_str(I, dir_of(I, path)));

    dir_push(I, dir_of(I, path));
    Ctrl r = nv_exec_nodes(I, I->globals, prog->kids, prog->nkids);
    dir_pop(I);

    if (r.s == SIG_THROW) {
        nv_report_uncaught(I, r.v);
        if (exit_code) *exit_code = 1;
        return false;
    }
    if (r.s == SIG_RETURN) {
        if (exit_code) *exit_code = (r.v && r.v->type == T_INT) ? (int)r.v->as.i : 0;
        return true;
    }
    if (r.s == SIG_BREAK || r.s == SIG_CONTINUE) {
        fprintf(stderr, "nova: 'break' or 'continue' used outside of a loop\n");
        if (exit_code) *exit_code = 1;
        return false;
    }
    if (exit_code) *exit_code = 0;
    return true;
}
