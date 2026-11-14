/*
 * builtins.c — Nova standard library: 80+ native functions covering
 * output, types, math, arrays, maps, strings, files and system access.
 */
#include "nova.h"

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

/* ================= argument check helpers ================= */

bool nv_argc(Interp *I, const char *name, int n, int min, int max) {
    if (n < min || (max >= 0 && n > max)) {
        if (min == max)
            nv_err(I, "%s() expects %d argument%s, got %d", name, min, min == 1 ? "" : "s", n);
        else if (max < 0)
            nv_err(I, "%s() expects at least %d argument%s, got %d", name, min, min == 1 ? "" : "s", n);
        else
            nv_err(I, "%s() expects %d to %d arguments, got %d", name, min, max, n);
        return false;
    }
    return true;
}

bool nv_arg_str(Interp *I, const char *name, int i, NVValue **args) {
    if (args[i]->type != T_STR) {
        nv_err(I, "%s() argument %d must be a string, got %s", name, i + 1, nv_type_name(args[i]->type));
        return false;
    }
    return true;
}

bool nv_arg_int(Interp *I, const char *name, int i, NVValue **args) {
    if (args[i]->type != T_INT) {
        nv_err(I, "%s() argument %d must be an int, got %s", name, i + 1, nv_type_name(args[i]->type));
        return false;
    }
    return true;
}

bool nv_arg_num(Interp *I, const char *name, int i, NVValue **args) {
    if (args[i]->type != T_INT && args[i]->type != T_FLOAT) {
        nv_err(I, "%s() argument %d must be a number, got %s", name, i + 1, nv_type_name(args[i]->type));
        return false;
    }
    return true;
}

bool nv_arg_arr(Interp *I, const char *name, int i, NVValue **args) {
    if (args[i]->type != T_ARR) {
        nv_err(I, "%s() argument %d must be an array, got %s", name, i + 1, nv_type_name(args[i]->type));
        return false;
    }
    return true;
}

bool nv_arg_map(Interp *I, const char *name, int i, NVValue **args) {
    if (args[i]->type != T_MAP) {
        nv_err(I, "%s() argument %d must be a map, got %s", name, i + 1, nv_type_name(args[i]->type));
        return false;
    }
    return true;
}

bool nv_arg_fn(Interp *I, const char *name, int i, NVValue **args) {
    if (args[i]->type != T_FN) {
        nv_err(I, "%s() argument %d must be a function, got %s", name, i + 1, nv_type_name(args[i]->type));
        return false;
    }
    return true;
}

/* ================= output ================= */

static NVValue *nb_println(Interp *I, NVValue **a, int n) {
    for (int i = 0; i < n; i++) {
        NVValue *s = nv_to_string(I, a[i]);
        fwrite(s->as.s, 1, s->slen, stdout);
    }
    fputc('\n', stdout);
    return nv_nil();
}

static NVValue *nb_print(Interp *I, NVValue **a, int n) {
    for (int i = 0; i < n; i++) {
        NVValue *s = nv_to_string(I, a[i]);
        fwrite(s->as.s, 1, s->slen, stdout);
    }
    return nv_nil();
}

/* ================= types and conversion ================= */

static NVValue *nb_type(Interp *I, NVValue **a, int n) {
    (void)n;
    return nv_str(I, nv_type_name(a[0]->type));
}

static NVValue *nb_str(Interp *I, NVValue **a, int n) {
    (void)n;
    return nv_to_string(I, a[0]);
}

/* PHP-style leading-number parse: "12abc" -> 12, "abc" -> 0 */
static NVValue *nb_int(Interp *I, NVValue **a, int n) {
    (void)n;
    NVValue *v = a[0];
    switch (v->type) {
        case T_INT: return v;
        case T_FLOAT: return nv_int(I, (long long)v->as.f);
        case T_BOOL: return nv_int(I, v->as.b ? 1 : 0);
        case T_NIL: return nv_int(I, 0);
        case T_STR: {
            const char *p = v->as.s;
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            int sign = 1;
            if (*p == '-') { sign = -1; p++; }
            else if (*p == '+') p++;
            if (!isdigit((unsigned char)*p)) return nv_int(I, 0);
            long long r = 0;
            while (isdigit((unsigned char)*p)) { r = r * 10 + (*p - '0'); p++; }
            return nv_int(I, sign * r);
        }
        default:
            nv_err(I, "int() cannot convert %s", nv_type_name(v->type));
            return NULL;
    }
}

static NVValue *nb_float(Interp *I, NVValue **a, int n) {
    (void)n;
    NVValue *v = a[0];
    switch (v->type) {
        case T_INT: return nv_float(I, (double)v->as.i);
        case T_FLOAT: return v;
        case T_BOOL: return nv_float(I, v->as.b ? 1.0 : 0.0);
        case T_NIL: return nv_float(I, 0.0);
        case T_STR: {
            const char *p = v->as.s;
            while (*p == ' ' || *p == '\t') p++;
            char *end;
            double d = strtod(p, &end);
            if (end == p) return nv_float(I, 0.0);
            return nv_float(I, d);
        }
        default:
            nv_err(I, "float() cannot convert %s", nv_type_name(v->type));
            return NULL;
    }
}

static NVValue *nb_bool(Interp *I, NVValue **a, int n) {
    (void)n;
    return nv_bool(I, nv_truthy(a[0]));
}

static NVValue *nb_chr(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_int(I, "chr", 0, a)) return NULL;
    long long c = a[0]->as.i;
    if (c < 0 || c > 255) {
        nv_err(I, "chr() expects a byte in 0..255, got %lld", c);
        return NULL;
    }
    char ch = (char)c;
    return nv_strl(I, &ch, 1);
}

static NVValue *nb_ord(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "ord", 0, a)) return NULL;
    if (a[0]->slen == 0) {
        nv_err(I, "ord() expects a non-empty string");
        return NULL;
    }
    return nv_int(I, (unsigned char)a[0]->as.s[0]);
}

static NVValue *nb_hex(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_int(I, "hex", 0, a)) return NULL;
    char tmp[32];
    if (a[0]->as.i < 0)
        snprintf(tmp, sizeof(tmp), "-0x%llx", (unsigned long long)(-a[0]->as.i));
    else
        snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)a[0]->as.i);
    return nv_str(I, tmp);
}

/* ---- type predicates ---- */

#define PRED(name, test) \
    static NVValue *name(Interp *I, NVValue **a, int n) { \
        (void)n; \
        return nv_bool(I, test); \
    }

PRED(nb_is_nil,      a[0]->type == T_NIL)
PRED(nb_is_bool,     a[0]->type == T_BOOL)
PRED(nb_is_int,      a[0]->type == T_INT)
PRED(nb_is_float,    a[0]->type == T_FLOAT)
PRED(nb_is_num,      a[0]->type == T_INT || a[0]->type == T_FLOAT)
PRED(nb_is_str,      a[0]->type == T_STR)
PRED(nb_is_array,    a[0]->type == T_ARR)
PRED(nb_is_map,      a[0]->type == T_MAP)
PRED(nb_is_fn,       a[0]->type == T_FN)
PRED(nb_is_class,    a[0]->type == T_CLASS)
PRED(nb_is_instance, a[0]->type == T_INSTANCE)
PRED(nb_is_error,    a[0]->type == T_INSTANCE && a[0]->as.inst->cls == I->error_class)

static NVValue *nb_is_a(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "is_a", 1, a)) return NULL;
    if (a[0]->type != T_INSTANCE && a[0]->type != T_CLASS) {
        nv_err(I, "is_a() argument 1 must be an instance or class, got %s", nv_type_name(a[0]->type));
        return NULL;
    }
    return nv_bool(I, nv_inst_of(I, a[0], a[1]->as.s));
}

/* ================= math ================= */

static NVValue *nb_abs(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_num(I, "abs", 0, a)) return NULL;
    if (a[0]->type == T_INT) return nv_int(I, a[0]->as.i < 0 ? -a[0]->as.i : a[0]->as.i);
    return nv_float(I, fabs(a[0]->as.f));
}

static NVValue *minmax(Interp *I, NVValue **a, int n, bool want_max) {
    NVValue **items = a;
    int cnt = n;
    NVValue **arr_items = NULL;
    if (n == 1 && a[0]->type == T_ARR) {
        cnt = (int)a[0]->as.arr->n;
        arr_items = a[0]->as.arr->items;
        items = arr_items;
        if (cnt == 0) {
            nv_err(I, want_max ? "max() of empty array" : "min() of empty array");
            return NULL;
        }
    }
    if (cnt == 0) {
        nv_err(I, want_max ? "max() expects at least one argument" : "min() expects at least one argument");
        return NULL;
    }
    NVValue *best = items[0];
    double bv, cv;
    if (!nv_num_val(best, &bv)) {
        nv_err(I, "%s() expects numbers, got %s", want_max ? "max" : "min", nv_type_name(best->type));
        return NULL;
    }
    for (int i = 1; i < cnt; i++) {
        if (!nv_num_val(items[i], &cv)) {
            nv_err(I, "%s() expects numbers, got %s", want_max ? "max" : "min", nv_type_name(items[i]->type));
            return NULL;
        }
        if (want_max ? cv > bv : cv < bv) { best = items[i]; bv = cv; }
    }
    return best;
}

static NVValue *nb_min(Interp *I, NVValue **a, int n) { return minmax(I, a, n, false); }
static NVValue *nb_max(Interp *I, NVValue **a, int n) { return minmax(I, a, n, true); }

static NVValue *nb_floor(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_num(I, "floor", 0, a)) return NULL;
    double v;
    nv_num_val(a[0], &v);
    return nv_int(I, (long long)floor(v));
}

static NVValue *nb_ceil(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_num(I, "ceil", 0, a)) return NULL;
    double v;
    nv_num_val(a[0], &v);
    return nv_int(I, (long long)ceil(v));
}

static NVValue *nb_round(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_num(I, "round", 0, a)) return NULL;
    double v;
    nv_num_val(a[0], &v);
    return nv_int(I, (long long)llround(v));
}

static NVValue *nb_sqrt(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_num(I, "sqrt", 0, a)) return NULL;
    double v;
    nv_num_val(a[0], &v);
    if (v < 0) {
        nv_err(I, "sqrt() of a negative number: %g", v);
        return NULL;
    }
    return nv_float(I, sqrt(v));
}

static NVValue *nb_pow(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "pow", n, 2, 2)) return NULL;
    if (!nv_arg_num(I, "pow", 0, a) || !nv_arg_num(I, "pow", 1, a)) return NULL;
    double x, y;
    nv_num_val(a[0], &x);
    nv_num_val(a[1], &y);
    return nv_float(I, pow(x, y));
}

static NVValue *nb_rand(Interp *I, NVValue **a, int n) {
    (void)a; (void)n;
    return nv_int(I, rand());
}

static NVValue *nb_rand_int(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "rand_int", n, 2, 2)) return NULL;
    if (!nv_arg_int(I, "rand_int", 0, a) || !nv_arg_int(I, "rand_int", 1, a)) return NULL;
    long long lo = a[0]->as.i, hi = a[1]->as.i;
    if (lo > hi) {
        nv_err(I, "rand_int() lower bound %lld is greater than upper bound %lld", lo, hi);
        return NULL;
    }
    unsigned long long span = (unsigned long long)(hi - lo) + 1;
    unsigned long long r =
        ((unsigned long long)rand() << 33) ^
        ((unsigned long long)rand() << 17) ^
        (unsigned long long)rand();
    return nv_int(I, lo + (long long)(r % span));
}

static NVValue *nb_sum(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_arr(I, "sum", 0, a)) return NULL;
    bool any_float = false;
    double total = 0;
    NVArray *arr = a[0]->as.arr;
    for (size_t i = 0; i < arr->n; i++) {
        double v;
        if (!nv_num_val(arr->items[i], &v)) {
            nv_err(I, "sum() expects an array of numbers, found %s at index %zu", nv_type_name(arr->items[i]->type), i);
            return NULL;
        }
        if (arr->items[i]->type == T_FLOAT) any_float = true;
        total += v;
    }
    return any_float ? nv_float(I, total) : nv_int(I, (long long)total);
}

static NVValue *nb_avg(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_arr(I, "avg", 0, a)) return NULL;
    NVArray *arr = a[0]->as.arr;
    if (arr->n == 0) {
        nv_err(I, "avg() of empty array");
        return NULL;
    }
    double total = 0;
    for (size_t i = 0; i < arr->n; i++) {
        double v;
        if (!nv_num_val(arr->items[i], &v)) {
            nv_err(I, "avg() expects an array of numbers, found %s at index %zu", nv_type_name(arr->items[i]->type), i);
            return NULL;
        }
        total += v;
    }
    return nv_float(I, total / (double)arr->n);
}

static NVValue *nb_number_format(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "number_format", n, 2, 2)) return NULL;
    if (!nv_arg_num(I, "number_format", 0, a) || !nv_arg_int(I, "number_format", 1, a)) return NULL;
    double v;
    nv_num_val(a[0], &v);
    long long d = a[1]->as.i;
    if (d < 0 || d > 30) {
        nv_err(I, "number_format() decimals must be 0..30");
        return NULL;
    }
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%.*f", (int)d, v);
    return nv_str(I, tmp);
}

/* ================= arrays ================= */

static NVValue *nb_len(Interp *I, NVValue **a, int n) {
    (void)n;
    switch (a[0]->type) {
        case T_STR: return nv_int(I, (long long)a[0]->slen);
        case T_ARR: return nv_int(I, (long long)a[0]->as.arr->n);
        case T_MAP: return nv_int(I, (long long)nvtab_count(a[0]->as.map));
        default:
            nv_err(I, "len() expects a string, array or map, got %s", nv_type_name(a[0]->type));
            return NULL;
    }
}

static NVValue *nb_push(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "push", n, 2, -1)) return NULL;
    if (!nv_arg_arr(I, "push", 0, a)) return NULL;
    for (int i = 1; i < n; i++) nv_arr_push(I, a[0], a[i]);
    return a[0];
}

static NVValue *nb_pop(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_arr(I, "pop", 0, a)) return NULL;
    NVArray *arr = a[0]->as.arr;
    if (arr->n == 0) {
        nv_err(I, "pop() of empty array");
        return NULL;
    }
    return arr->items[--arr->n];
}

static NVValue *nb_shift(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_arr(I, "shift", 0, a)) return NULL;
    NVArray *arr = a[0]->as.arr;
    if (arr->n == 0) {
        nv_err(I, "shift() of empty array");
        return NULL;
    }
    NVValue *first = arr->items[0];
    memmove(arr->items, arr->items + 1, (arr->n - 1) * sizeof(NVValue *));
    arr->n--;
    return first;
}

static NVValue *nb_unshift(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "unshift", n, 2, -1)) return NULL;
    if (!nv_arg_arr(I, "unshift", 0, a)) return NULL;
    NVArray *arr = a[0]->as.arr;
    int count = n - 1;
    /* reserve space at front by pushing then rotating */
    for (int i = 0; i < count; i++) nv_arr_push(I, a[0], nv_nil());
    if (arr->n > (size_t)count)
        memmove(arr->items + count, arr->items, (arr->n - count) * sizeof(NVValue *));
    for (int i = 0; i < count; i++) arr->items[i] = a[1 + i];
    return a[0];
}

static NVValue *nb_insert(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "insert", n, 3, 3)) return NULL;
    if (!nv_arg_arr(I, "insert", 0, a) || !nv_arg_int(I, "insert", 1, a)) return NULL;
    NVArray *arr = a[0]->as.arr;
    long long i = a[1]->as.i;
    if (i < 0) i += (long long)arr->n;
    if (i < 0 || (size_t)i > arr->n) {
        nv_err(I, "insert() index %lld out of range (length %zu)", a[1]->as.i, arr->n);
        return NULL;
    }
    nv_arr_push(I, a[0], nv_nil());
    memmove(arr->items + i + 1, arr->items + i, (arr->n - (size_t)i - 1) * sizeof(NVValue *));
    arr->items[i] = a[2];
    return a[0];
}

static NVValue *nb_remove(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "remove", n, 2, 2)) return NULL;
    if (!nv_arg_arr(I, "remove", 0, a) || !nv_arg_int(I, "remove", 1, a)) return NULL;
    NVArray *arr = a[0]->as.arr;
    long long i = a[1]->as.i;
    if (i < 0) i += (long long)arr->n;
    if (i < 0 || (size_t)i >= arr->n) {
        nv_err(I, "remove() index %lld out of range (length %zu)", a[1]->as.i, arr->n);
        return NULL;
    }
    NVValue *removed = arr->items[i];
    memmove(arr->items + i, arr->items + i + 1, (arr->n - (size_t)i - 1) * sizeof(NVValue *));
    arr->n--;
    return removed;
}

static NVValue *nb_slice(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "slice", n, 2, 3)) return NULL;
    if (!nv_arg_int(I, "slice", 1, a)) return NULL;
    long long s = a[1]->as.i;
    if (a[0]->type == T_ARR) {
        long long len = (long long)a[0]->as.arr->n;
        long long e = (n == 3) ? (nv_arg_int(I, "slice", 2, a) ? a[2]->as.i : 0) : len;
        if (s < 0) s += len;
        if (e < 0) e += len;
        if (s < 0) s = 0;
        if (e > len) e = len;
        if (e < s) e = s;
        NVValue *out = nv_arr_new(I);
        for (long long i = s; i < e; i++) nv_arr_push(I, out, a[0]->as.arr->items[i]);
        return out;
    }
    if (a[0]->type == T_STR) {
        long long len = (long long)a[0]->slen;
        long long e = (n == 3) ? (nv_arg_int(I, "slice", 2, a) ? a[2]->as.i : 0) : len;
        if (s < 0) s += len;
        if (e < 0) e += len;
        if (s < 0) s = 0;
        if (e > len) e = len;
        if (e < s) e = s;
        return nv_strl(I, a[0]->as.s + s, (size_t)(e - s));
    }
    nv_err(I, "slice() expects an array or string, got %s", nv_type_name(a[0]->type));
    return NULL;
}

static NVValue *nb_keys(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_map(I, "keys", 0, a)) return NULL;
    NVValue *out = nv_arr_new(I);
    NVTable *t = a[0]->as.map;
    for (size_t k = 0; k < t->on; k++) {
        TEntry *e = &t->ents[t->order[k]];
        if (e->state == 1) nv_arr_push(I, out, nv_str(I, e->key));
    }
    return out;
}

static NVValue *nb_values(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_map(I, "values", 0, a)) return NULL;
    NVValue *out = nv_arr_new(I);
    NVTable *t = a[0]->as.map;
    for (size_t k = 0; k < t->on; k++) {
        TEntry *e = &t->ents[t->order[k]];
        if (e->state == 1) nv_arr_push(I, out, e->val);
    }
    return out;
}

static NVValue *nb_has(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "has", n, 2, 2)) return NULL;
    if (a[0]->type == T_MAP) {
        if (a[1]->type != T_STR && a[1]->type != T_INT) {
            nv_err(I, "has() key must be string or int, got %s", nv_type_name(a[1]->type));
            return NULL;
        }
        return nv_bool(I, nv_map_get(a[0], a[1]) != NULL);
    }
    if (a[0]->type == T_ARR) {
        if (a[1]->type != T_INT) {
            nv_err(I, "has() array index must be an int, got %s", nv_type_name(a[1]->type));
            return NULL;
        }
        long long i = a[1]->as.i;
        if (i < 0) i += (long long)a[0]->as.arr->n;
        return nv_bool(I, i >= 0 && (size_t)i < a[0]->as.arr->n);
    }
    nv_err(I, "has() expects a map or array, got %s", nv_type_name(a[0]->type));
    return NULL;
}

static NVValue *nb_del(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "del", n, 2, 2)) return NULL;
    if (!nv_arg_map(I, "del", 0, a)) return NULL;
    if (a[1]->type != T_STR && a[1]->type != T_INT) {
        nv_err(I, "del() key must be string or int, got %s", nv_type_name(a[1]->type));
        return NULL;
    }
    if (a[1]->type == T_STR)
        return nv_bool(I, nvtab_del(a[0]->as.map, a[1]->as.s));
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%lld", a[1]->as.i);
    return nv_bool(I, nvtab_del(a[0]->as.map, tmp));
}

static NVValue *nb_index_of(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "index_of", n, 2, 2)) return NULL;
    if (a[0]->type == T_ARR) {
        NVArray *arr = a[0]->as.arr;
        for (size_t i = 0; i < arr->n; i++)
            if (nv_equal(arr->items[i], a[1])) return nv_int(I, (long long)i);
        return nv_int(I, -1);
    }
    if (a[0]->type == T_STR) {
        if (!nv_arg_str(I, "index_of", 1, a)) return NULL;
        if (a[1]->slen == 0) return nv_int(I, 0);
        const char *p = strstr(a[0]->as.s, a[1]->as.s);
        if (!p) return nv_int(I, -1);
        return nv_int(I, (long long)(p - a[0]->as.s));
    }
    nv_err(I, "index_of() expects an array or string, got %s", nv_type_name(a[0]->type));
    return NULL;
}

static NVValue *nb_concat(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "concat", n, 1, -1)) return NULL;
    NVValue *out = nv_arr_new(I);
    for (int i = 0; i < n; i++) {
        if (!nv_arg_arr(I, "concat", i, a)) return NULL;
        for (size_t k = 0; k < a[i]->as.arr->n; k++)
            nv_arr_push(I, out, a[i]->as.arr->items[k]);
    }
    return out;
}

static NVValue *nb_reverse(Interp *I, NVValue **a, int n) {
    (void)n;
    if (a[0]->type == T_ARR) {
        NVArray *arr = a[0]->as.arr;
        NVValue *out = nv_arr_new(I);
        for (size_t i = arr->n; i > 0; i--) nv_arr_push(I, out, arr->items[i - 1]);
        return out;
    }
    if (a[0]->type == T_STR) {
        SB b; sb_init(&b);
        for (size_t i = a[0]->slen; i > 0; i--) sb_putc(&b, a[0]->as.s[i - 1]);
        NVValue *r = nv_strl(I, b.p ? b.p : "", b.n);
        sb_free(&b);
        return r;
    }
    nv_err(I, "reverse() expects an array or string, got %s", nv_type_name(a[0]->type));
    return NULL;
}

/* ---- sort with optional comparator (stable merge sort) ---- */

static bool default_less(Interp *I, NVValue *x, NVValue *y, bool *less) {
    double a, b;
    if (nv_num_val(x, &a) && nv_num_val(y, &b)) { *less = a < b; return true; }
    if (x->type == T_STR && y->type == T_STR) {
        size_t m = x->slen < y->slen ? x->slen : y->slen;
        int c = m ? memcmp(x->as.s, y->as.s, m) : 0;
        if (c == 0) c = x->slen < y->slen ? -1 : x->slen > y->slen ? 1 : 0;
        *less = c < 0;
        return true;
    }
    nv_err(I, "sort() cannot compare %s and %s; pass a comparator function", nv_type_name(x->type), nv_type_name(y->type));
    return false;
}

static bool user_less(Interp *I, NVValue *fn, NVValue *x, NVValue *y, bool *less) {
    NVValue *args[2] = { x, y };
    NVValue *r;
    if (!nv_call_value(I, fn, args, 2, &r)) return false;
    if (r->type == T_BOOL) { *less = r->as.b != 0; return true; }
    double d;
    if (nv_num_val(r, &d)) { *less = d < 0; return true; }
    nv_err(I, "sort() comparator must return a number or bool, got %s", nv_type_name(r->type));
    return false;
}

static bool merge_sort(Interp *I, NVValue **items, size_t n, NVValue *cmp, NVValue **tmp) {
    if (n < 2) return true;
    size_t m = n / 2;
    if (!merge_sort(I, items, m, cmp, tmp)) return false;
    if (!merge_sort(I, items + m, n - m, cmp, tmp)) return false;
    size_t i = 0, j = m, k = 0;
    while (i < m && j < n) {
        bool less;
        bool ok = cmp ? user_less(I, cmp, items[i], items[j], &less)
                      : default_less(I, items[i], items[j], &less);
        if (!ok) return false;
        tmp[k++] = less ? items[i++] : items[j++];
    }
    while (i < m) tmp[k++] = items[i++];
    while (j < n) tmp[k++] = items[j++];
    memcpy(items, tmp, n * sizeof(NVValue *));
    return true;
}

static NVValue *nb_sort(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "sort", n, 1, 2)) return NULL;
    if (!nv_arg_arr(I, "sort", 0, a)) return NULL;
    if (n == 2 && !nv_arg_fn(I, "sort", 1, a)) return NULL;
    NVArray *arr = a[0]->as.arr;
    size_t cnt = arr->n;
    if (cnt == 0) return nv_arr_new(I);
    NVValue **items = (NVValue **)malloc(cnt * sizeof(NVValue *));
    NVValue **tmp = (NVValue **)malloc(cnt * sizeof(NVValue *));
    if (!items || !tmp) {
        free(items); free(tmp);
        nv_err(I, "out of memory");
        return NULL;
    }
    memcpy(items, arr->items, cnt * sizeof(NVValue *));
    bool ok = merge_sort(I, items, cnt, n == 2 ? a[1] : NULL, tmp);
    if (!ok) {
        free(items); free(tmp);
        return NULL;
    }
    NVValue *out = nv_arr_new(I);
    for (size_t i = 0; i < cnt; i++) nv_arr_push(I, out, items[i]);
    free(items);
    free(tmp);
    return out;
}

static NVValue *nb_unique(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_arr(I, "unique", 0, a)) return NULL;
    NVArray *arr = a[0]->as.arr;
    NVValue *out = nv_arr_new(I);
    for (size_t i = 0; i < arr->n; i++) {
        bool dup = false;
        for (size_t k = 0; k < out->as.arr->n; k++) {
            if (nv_equal(out->as.arr->items[k], arr->items[i])) { dup = true; break; }
        }
        if (!dup) nv_arr_push(I, out, arr->items[i]);
    }
    return out;
}

static NVValue *nb_join(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "join", n, 2, 2)) return NULL;
    if (!nv_arg_arr(I, "join", 0, a) || !nv_arg_str(I, "join", 1, a)) return NULL;
    SB b; sb_init(&b);
    NVArray *arr = a[0]->as.arr;
    for (size_t i = 0; i < arr->n; i++) {
        if (i) sb_puts(&b, a[1]->as.s);
        NVValue *s = nv_to_string(I, arr->items[i]);
        sb_putsl(&b, s->as.s, s->slen);
    }
    NVValue *r = nv_strl(I, b.p ? b.p : "", b.n);
    sb_free(&b);
    return r;
}

static NVValue *nb_range(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "range", n, 2, 3)) return NULL;
    if (!nv_arg_int(I, "range", 0, a) || !nv_arg_int(I, "range", 1, a)) return NULL;
    long long from = a[0]->as.i, to = a[1]->as.i;
    long long step = 1;
    if (n == 3) {
        if (!nv_arg_int(I, "range", 2, a)) return NULL;
        step = a[2]->as.i;
        if (step == 0) {
            nv_err(I, "range() step cannot be 0");
            return NULL;
        }
        if ((to - from) * step < 0) {
            nv_err(I, "range() step direction does not match from..to");
            return NULL;
        }
    } else if (to < from) {
        step = -1;
    }
    unsigned long long count = step > 0
        ? (unsigned long long)((to - from) / step) + 1
        : (unsigned long long)((from - to) / (-step)) + 1;
    if (count > NOVA_MAX_RANGE) {
        nv_err(I, "range() would produce %llu elements (max %d)", count, NOVA_MAX_RANGE);
        return NULL;
    }
    NVValue *out = nv_arr_new(I);
    for (long long v = from; step > 0 ? v <= to : v >= to; v += step)
        nv_arr_push(I, out, nv_int(I, v));
    return out;
}

/* ---- callbacks: map / filter / reduce ---- */

static NVValue *nb_map(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "map", n, 2, 2)) return NULL;
    if (!nv_arg_arr(I, "map", 0, a) || !nv_arg_fn(I, "map", 1, a)) return NULL;
    NVArray *arr = a[0]->as.arr;
    NVValue *out = nv_arr_new(I);
    size_t mk = nv_gc_pin(I, out, GC_TAG_VALUE);   /* out lives across callbacks */
    for (size_t i = 0; i < arr->n; i++) {
        NVValue *args[1] = { arr->items[i] };
        NVValue *r;
        if (!nv_call_value(I, a[1], args, 1, &r)) { nv_gc_unpin(I, mk); return NULL; }
        nv_arr_push(I, out, r);
    }
    nv_gc_unpin(I, mk);
    return out;
}

static NVValue *nb_filter(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "filter", n, 2, 2)) return NULL;
    if (!nv_arg_arr(I, "filter", 0, a) || !nv_arg_fn(I, "filter", 1, a)) return NULL;
    NVArray *arr = a[0]->as.arr;
    NVValue *out = nv_arr_new(I);
    size_t mk = nv_gc_pin(I, out, GC_TAG_VALUE);   /* out lives across callbacks */
    for (size_t i = 0; i < arr->n; i++) {
        NVValue *args[1] = { arr->items[i] };
        NVValue *r;
        if (!nv_call_value(I, a[1], args, 1, &r)) { nv_gc_unpin(I, mk); return NULL; }
        if (nv_truthy(r)) nv_arr_push(I, out, arr->items[i]);
    }
    nv_gc_unpin(I, mk);
    return out;
}

static NVValue *nb_reduce(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "reduce", n, 3, 3)) return NULL;
    if (!nv_arg_arr(I, "reduce", 0, a) || !nv_arg_fn(I, "reduce", 1, a)) return NULL;
    NVArray *arr = a[0]->as.arr;
    NVValue *acc = a[2];
    size_t mk = nv_gc_pin(I, acc, GC_TAG_VALUE);   /* acc evolves across callbacks */
    for (size_t i = 0; i < arr->n; i++) {
        NVValue *args[2] = { acc, arr->items[i] };
        if (!nv_call_value(I, a[1], args, 2, &acc)) { nv_gc_unpin(I, mk); return NULL; }
        nv_gc_repin(I, mk, acc);
    }
    nv_gc_unpin(I, mk);
    return acc;
}

/* ================= memory management ================= */

static NVValue *nb_gc_collect(Interp *I, NVValue **a, int n) {
    (void)a; (void)n;
    /* Collections are only safe at statement-boundary safe points, where
     * the evaluator's in-flight temporaries are pinned. Called from inside
     * an expression (e.g. by a user function mid-computation), a collection
     * would run while unpinned C locals still hold values, so instead the
     * request is queued and honoured at the next safe point. */
    if (I->heap.since_gc < I->heap.trigger)
        I->heap.since_gc = I->heap.trigger;
    return nv_int(I, 0);
}

static NVValue *nb_gc_stats(Interp *I, NVValue **a, int n) {
    (void)a; (void)n;
    NVValue *m = nv_map_new(I);
    nvtab_set(I, m->as.map, "runs",              nv_int(I, (long long)I->heap.runs));
    nvtab_set(I, m->as.map, "live_bytes",        nv_int(I, (long long)I->heap.live_bytes));
    nvtab_set(I, m->as.map, "live_objects",      nv_int(I, (long long)I->heap.live_objs));
    nvtab_set(I, m->as.map, "total_alloc_bytes", nv_int(I, (long long)I->heap.total_alloc_bytes));
    nvtab_set(I, m->as.map, "total_freed_bytes", nv_int(I, (long long)I->heap.total_freed_bytes));
    nvtab_set(I, m->as.map, "last_freed_bytes",  nv_int(I, (long long)I->heap.last_freed_bytes));
    nvtab_set(I, m->as.map, "last_freed_objects",nv_int(I, (long long)I->heap.last_freed_objs));
    return m;
}

/* ================= strings ================= */

static const char *WS = " \t\n\r\v\f";

static NVValue *nb_split(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "split", n, 2, 2)) return NULL;
    if (!nv_arg_str(I, "split", 0, a) || !nv_arg_str(I, "split", 1, a)) return NULL;
    if (a[1]->slen == 0) {
        nv_err(I, "split() separator cannot be empty");
        return NULL;
    }
    NVValue *out = nv_arr_new(I);
    const char *p = a[0]->as.s;
    const char *sep = a[1]->as.s;
    size_t slen = a[1]->slen;
    for (;;) {
        const char *hit = strstr(p, sep);
        if (!hit) {
            nv_arr_push(I, out, nv_str(I, p));
            break;
        }
        nv_arr_push(I, out, nv_strl(I, p, (size_t)(hit - p)));
        p = hit + slen;
    }
    return out;
}

static NVValue *nb_upper(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "upper", 0, a)) return NULL;
    char *tmp = (char *)malloc(a[0]->slen + 1);
    if (!tmp) { nv_err(I, "out of memory"); return NULL; }
    for (size_t i = 0; i < a[0]->slen; i++) tmp[i] = (char)toupper((unsigned char)a[0]->as.s[i]);
    NVValue *r = nv_strl(I, tmp, a[0]->slen);
    free(tmp);
    return r;
}

static NVValue *nb_lower(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "lower", 0, a)) return NULL;
    char *tmp = (char *)malloc(a[0]->slen + 1);
    if (!tmp) { nv_err(I, "out of memory"); return NULL; }
    for (size_t i = 0; i < a[0]->slen; i++) tmp[i] = (char)tolower((unsigned char)a[0]->as.s[i]);
    NVValue *r = nv_strl(I, tmp, a[0]->slen);
    free(tmp);
    return r;
}

static NVValue *trim_impl(Interp *I, NVValue **a, int n, int mode, const char *fname) {
    (void)n;
    if (!nv_arg_str(I, fname, 0, a)) return NULL;
    const char *p = a[0]->as.s;
    size_t len = a[0]->slen;
    if (mode != 2) {   /* left trim */
        while (len && strchr(WS, p[0]) && p[0] != '\0') { p++; len--; }
    }
    if (mode != 1) {   /* right trim */
        while (len && strchr(WS, p[len - 1])) len--;
    }
    return nv_strl(I, p, len);
}

static NVValue *nb_trim(Interp *I, NVValue **a, int n)  { return trim_impl(I, a, n, 0, "trim"); }
static NVValue *nb_ltrim(Interp *I, NVValue **a, int n) { return trim_impl(I, a, n, 1, "ltrim"); }
static NVValue *nb_rtrim(Interp *I, NVValue **a, int n) { return trim_impl(I, a, n, 2, "rtrim"); }

static NVValue *nb_starts_with(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "starts_with", n, 2, 2)) return NULL;
    if (!nv_arg_str(I, "starts_with", 0, a) || !nv_arg_str(I, "starts_with", 1, a)) return NULL;
    return nv_bool(I, a[0]->slen >= a[1]->slen &&
        (a[1]->slen == 0 || memcmp(a[0]->as.s, a[1]->as.s, a[1]->slen) == 0));
}

static NVValue *nb_ends_with(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "ends_with", n, 2, 2)) return NULL;
    if (!nv_arg_str(I, "ends_with", 0, a) || !nv_arg_str(I, "ends_with", 1, a)) return NULL;
    return nv_bool(I, a[0]->slen >= a[1]->slen &&
        (a[1]->slen == 0 || memcmp(a[0]->as.s + a[0]->slen - a[1]->slen, a[1]->as.s, a[1]->slen) == 0));
}

static NVValue *nb_contains(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "contains", n, 2, 2)) return NULL;
    if (!nv_arg_str(I, "contains", 0, a) || !nv_arg_str(I, "contains", 1, a)) return NULL;
    if (a[1]->slen == 0) return nv_bool(I, true);
    return nv_bool(I, strstr(a[0]->as.s, a[1]->as.s) != NULL);
}

static NVValue *nb_find(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "find", n, 2, 2)) return NULL;
    if (!nv_arg_str(I, "find", 0, a) || !nv_arg_str(I, "find", 1, a)) return NULL;
    if (a[1]->slen == 0) return nv_int(I, 0);
    const char *p = strstr(a[0]->as.s, a[1]->as.s);
    return nv_int(I, p ? (long long)(p - a[0]->as.s) : -1);
}

static NVValue *nb_replace(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "replace", n, 3, 3)) return NULL;
    if (!nv_arg_str(I, "replace", 0, a) || !nv_arg_str(I, "replace", 1, a) || !nv_arg_str(I, "replace", 2, a)) return NULL;
    if (a[1]->slen == 0) {
        nv_err(I, "replace() search string cannot be empty");
        return NULL;
    }
    SB b; sb_init(&b);
    const char *p = a[0]->as.s;
    const char *hit;
    while ((hit = strstr(p, a[1]->as.s)) != NULL) {
        sb_putsl(&b, p, (size_t)(hit - p));
        sb_puts(&b, a[2]->as.s);
        p = hit + a[1]->slen;
    }
    sb_puts(&b, p);
    NVValue *r = nv_strl(I, b.p ? b.p : "", b.n);
    sb_free(&b);
    return r;
}

static NVValue *nb_substr(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "substr", n, 2, 3)) return NULL;
    if (!nv_arg_str(I, "substr", 0, a) || !nv_arg_int(I, "substr", 1, a)) return NULL;
    long long len = (long long)a[0]->slen;
    long long s = a[1]->as.i;
    if (s < 0) s += len;
    if (s < 0) s = 0;
    if (s > len) s = len;
    long long e = len;
    if (n == 3) {
        if (!nv_arg_int(I, "substr", 2, a)) return NULL;
        e = s + a[2]->as.i;
    }
    if (e < s) e = s;
    if (e > len) e = len;
    return nv_strl(I, a[0]->as.s + s, (size_t)(e - s));
}

static NVValue *pad_impl(Interp *I, NVValue **a, int n, bool left, const char *fname) {
    if (left ? !nv_argc(I, fname, n, 2, 3) : !nv_argc(I, fname, n, 2, 3)) return NULL;
    if (!nv_arg_str(I, fname, 0, a) || !nv_arg_int(I, fname, 1, a)) return NULL;
    const char *pad = " ";
    if (n == 3) {
        if (!nv_arg_str(I, fname, 2, a)) return NULL;
        pad = a[2]->as.s;
        if (strlen(pad) == 0) pad = " ";
    }
    long long width = a[1]->as.i;
    size_t cur = a[0]->slen;
    if ((long long)cur >= width) return a[0];
    SB b; sb_init(&b);
    size_t need = (size_t)width - cur;
    size_t plen = strlen(pad);
    while (need > 0) {
        size_t take = need < plen ? need : plen;
        if (left) sb_putsl(&b, pad, take);
        else sb_putsl(&b, pad, take);
        need -= take;
    }
    NVValue *fill = nv_strl(I, b.p ? b.p : "", b.n);
    sb_free(&b);
    if (left) {
        SB out; sb_init(&out);
        sb_putsl(&out, fill->as.s, fill->slen);
        sb_putsl(&out, a[0]->as.s, a[0]->slen);
        NVValue *r = nv_strl(I, out.p ? out.p : "", out.n);
        sb_free(&out);
        return r;
    }
    SB out; sb_init(&out);
    sb_putsl(&out, a[0]->as.s, a[0]->slen);
    sb_putsl(&out, fill->as.s, fill->slen);
    NVValue *r = nv_strl(I, out.p ? out.p : "", out.n);
    sb_free(&out);
    return r;
}

static NVValue *nb_pad_left(Interp *I, NVValue **a, int n)  { return pad_impl(I, a, n, true, "pad_left"); }
static NVValue *nb_pad_right(Interp *I, NVValue **a, int n) { return pad_impl(I, a, n, false, "pad_right"); }

static NVValue *nb_repeat(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "repeat", n, 2, 2)) return NULL;
    if (!nv_arg_str(I, "repeat", 0, a) || !nv_arg_int(I, "repeat", 1, a)) return NULL;
    long long times = a[1]->as.i;
    if (times < 0) times = 0;
    if ((long long)a[0]->slen * times > 50000000) {
        nv_err(I, "repeat() result too large");
        return NULL;
    }
    SB b; sb_init(&b);
    for (long long i = 0; i < times; i++) sb_putsl(&b, a[0]->as.s, a[0]->slen);
    NVValue *r = nv_strl(I, b.p ? b.p : "", b.n);
    sb_free(&b);
    return r;
}

/* ================= files ================= */

static NVValue *nb_read_file(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "read_file", 0, a)) return NULL;
    size_t len;
    char *buf = nv_read_whole_file(a[0]->as.s, &len);
    if (!buf) return nv_nil();
    NVValue *r = nv_strl(I, buf, len);
    free(buf);
    return r;
}

static NVValue *nb_write_file(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "write_file", n, 2, 2)) return NULL;
    if (!nv_arg_str(I, "write_file", 0, a)) return NULL;
    NVValue *data = nv_to_string(I, a[1]);
    FILE *f = fopen(a[0]->as.s, "wb");
    if (!f) return nv_bool(I, false);
    size_t wrote = fwrite(data->as.s, 1, data->slen, f);
    fclose(f);
    return nv_bool(I, wrote == data->slen);
}

static NVValue *nb_append_file(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "append_file", n, 2, 2)) return NULL;
    if (!nv_arg_str(I, "append_file", 0, a)) return NULL;
    NVValue *data = nv_to_string(I, a[1]);
    FILE *f = fopen(a[0]->as.s, "ab");
    if (!f) return nv_bool(I, false);
    size_t wrote = fwrite(data->as.s, 1, data->slen, f);
    fclose(f);
    return nv_bool(I, wrote == data->slen);
}

static NVValue *nb_file_exists(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "file_exists", 0, a)) return NULL;
    FILE *f = fopen(a[0]->as.s, "rb");
    if (!f) return nv_bool(I, false);
    fclose(f);
    return nv_bool(I, true);
}

static NVValue *nb_delete_file(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "delete_file", 0, a)) return NULL;
    return nv_bool(I, remove(a[0]->as.s) == 0);
}

static NVValue *nb_mkdir(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "mkdir", 0, a)) return NULL;
#ifdef _WIN32
    return nv_bool(I, _mkdir(a[0]->as.s) == 0);
#else
    return nv_bool(I, mkdir(a[0]->as.s, 0777) == 0);
#endif
}

/* ================= system ================= */

static NVValue *nb_now(Interp *I, NVValue **a, int n) {
    (void)a; (void)n;
    return nv_float(I, (double)time(NULL));
}

static NVValue *nb_clock_ms(Interp *I, NVValue **a, int n) {
    (void)a; (void)n;
    return nv_float(I, (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC);
}

static NVValue *nb_read_line(Interp *I, NVValue **a, int n) {
    (void)a; (void)n;
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return nv_nil();
    size_t l = strlen(buf);
    while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = '\0';
    return nv_strl(I, buf, l);
}

static NVValue *nb_get_env(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "get_env", 0, a)) return NULL;
    const char *v = getenv(a[0]->as.s);
    return v ? nv_str(I, v) : nv_nil();
}

static NVValue *nb_error(Interp *I, NVValue **a, int n) {
    (void)n;
    if (!nv_arg_str(I, "error", 0, a)) return NULL;
    return nv_new_error(I, a[0]->as.s);
}

static NVValue *nb_exit(Interp *I, NVValue **a, int n) {
    (void)I;
    int code = 0;
    if (n > 0) {
        double d;
        if (nv_num_val(a[0], &d)) code = (int)d;
    }
    fflush(stdout);
    fflush(stderr);
    exit(code);
    return NULL;   /* unreachable */
}

static NVValue *nb_assert(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "assert", n, 1, 2)) return NULL;
    if (nv_truthy(a[0])) return nv_bool(I, true);
    if (n == 2) {
        NVValue *s = nv_to_string(I, a[1]);
        nv_err(I, "assertion failed: %.*s", (int)s->slen, s->as.s);
        return NULL;
    }
    nv_err(I, "assertion failed");
    return NULL;
}

/* ================= json ================= */

static void json_quote(SB *b, const char *s, size_t len) {
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
                    snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    sb_puts(b, tmp);
                } else {
                    sb_putc(b, (char)c);
                }
        }
    }
    sb_putc(b, '"');
}

static bool json_val(Interp *I, SB *b, NVValue *v, int depth, int pretty) {
    if (depth > 32) {
        nv_err(I, "json_encode(): nesting too deep");
        return false;
    }
    char tmp[64];
    switch (v->type) {
        case T_NIL: sb_puts(b, "null"); return true;
        case T_BOOL: sb_puts(b, v->as.b ? "true" : "false"); return true;
        case T_INT:
            snprintf(tmp, sizeof(tmp), "%lld", v->as.i);
            sb_puts(b, tmp);
            return true;
        case T_FLOAT:
            snprintf(tmp, sizeof(tmp), "%.15g", v->as.f);
            sb_puts(b, tmp);
            return true;
        case T_STR:
            json_quote(b, v->as.s, v->slen);
            return true;
        case T_ARR: {
            sb_putc(b, '[');
            NVArray *arr = v->as.arr;
            for (size_t i = 0; i < arr->n; i++) {
                if (i) sb_putc(b, ',');
                if (pretty) { sb_putc(b, '\n'); for (int k = 0; k <= depth; k++) sb_puts(b, "  "); }
                if (!json_val(I, b, arr->items[i], depth + 1, pretty)) return false;
            }
            if (pretty && arr->n) { sb_putc(b, '\n'); for (int k = 0; k < depth; k++) sb_puts(b, "  "); }
            sb_putc(b, ']');
            return true;
        }
        case T_MAP: {
            sb_putc(b, '{');
            NVTable *t = v->as.map;
            size_t emitted = 0;
            for (size_t k = 0; k < t->on; k++) {
                TEntry *e = &t->ents[t->order[k]];
                if (e->state != 1) continue;
                if (emitted) sb_putc(b, ',');
                if (pretty) { sb_putc(b, '\n'); for (int q = 0; q <= depth; q++) sb_puts(b, "  "); }
                json_quote(b, e->key, strlen(e->key));
                sb_putc(b, ':');
                if (pretty) sb_putc(b, ' ');
                if (!json_val(I, b, e->val, depth + 1, pretty)) return false;
                emitted++;
            }
            if (pretty && emitted) { sb_putc(b, '\n'); for (int q = 0; q < depth; q++) sb_puts(b, "  "); }
            sb_putc(b, '}');
            return true;
        }
        default:
            nv_err(I, "json_encode() cannot serialize a value of type %s", nv_type_name(v->type));
            return false;
    }
}

static NVValue *nb_json_encode(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "json_encode", n, 1, 2)) return NULL;
    int pretty = 0;
    if (n == 2) pretty = nv_truthy(a[1]) ? 1 : 0;
    SB b; sb_init(&b);
    if (!json_val(I, &b, a[0], 0, pretty)) {
        sb_free(&b);
        return NULL;
    }
    NVValue *r = nv_strl(I, b.p ? b.p : "", b.n);
    sb_free(&b);
    return r;
}

static NVValue *nb_json_decode(Interp *I, NVValue **a, int n) {
    if (!nv_argc(I, "json_decode", n, 1, 1)) return NULL;
    if (!nv_arg_str(I, "json_decode", 0, a)) return NULL;
    char err[128];
    NVValue *v = nv_json_parse(I, a[0]->as.s, a[0]->slen, err, sizeof(err));
    if (!v) {
        nv_err(I, "json_decode(): %s", err);
        return NULL;
    }
    return v;
}

/* ================= registration ================= */

static void reg(Interp *I, const char *name, NNative fn, int amin, int amax) {
    NVValue *f = nv_fn_native(I, name, fn, amin, amax);
    env_define(I, I->builtins_env, name, f);
}

void nv_register_builtins(Interp *I) {
    /* output */
    reg(I, "println", nb_println, 0, -1);
    reg(I, "print",   nb_print,   0, -1);

    /* types and conversion */
    reg(I, "type",  nb_type,  1, 1);
    reg(I, "str",   nb_str,   1, 1);
    reg(I, "int",   nb_int,   1, 1);
    reg(I, "float", nb_float, 1, 1);
    reg(I, "bool",  nb_bool,  1, 1);
    reg(I, "chr",   nb_chr,   1, 1);
    reg(I, "ord",   nb_ord,   1, 1);
    reg(I, "hex",   nb_hex,   1, 1);

    /* predicates */
    reg(I, "is_nil",      nb_is_nil, 1, 1);
    reg(I, "is_bool",     nb_is_bool, 1, 1);
    reg(I, "is_int",      nb_is_int, 1, 1);
    reg(I, "is_float",    nb_is_float, 1, 1);
    reg(I, "is_num",      nb_is_num, 1, 1);
    reg(I, "is_str",      nb_is_str, 1, 1);
    reg(I, "is_array",    nb_is_array, 1, 1);
    reg(I, "is_map",      nb_is_map, 1, 1);
    reg(I, "is_fn",       nb_is_fn, 1, 1);
    reg(I, "is_class",    nb_is_class, 1, 1);
    reg(I, "is_instance", nb_is_instance, 1, 1);
    reg(I, "is_error",    nb_is_error, 1, 1);
    reg(I, "is_a",        nb_is_a, 2, 2);

    /* math */
    reg(I, "abs",  nb_abs,  1, 1);
    reg(I, "min",  nb_min,  1, -1);
    reg(I, "max",  nb_max,  1, -1);
    reg(I, "floor", nb_floor, 1, 1);
    reg(I, "ceil",  nb_ceil,  1, 1);
    reg(I, "round", nb_round, 1, 1);
    reg(I, "sqrt",  nb_sqrt,  1, 1);
    reg(I, "pow",   nb_pow,   2, 2);
    reg(I, "rand",     nb_rand,    0, 0);
    reg(I, "rand_int", nb_rand_int, 2, 2);
    reg(I, "sum",  nb_sum,  1, 1);
    reg(I, "avg",  nb_avg,  1, 1);
    reg(I, "number_format", nb_number_format, 2, 2);

    /* arrays and maps */
    reg(I, "len", nb_len, 1, 1);
    reg(I, "push",    nb_push,   2, -1);
    reg(I, "pop",     nb_pop,    1, 1);
    reg(I, "shift",   nb_shift,  1, 1);
    reg(I, "unshift", nb_unshift, 2, -1);
    reg(I, "insert",  nb_insert, 3, 3);
    reg(I, "remove",  nb_remove, 2, 2);
    reg(I, "slice",   nb_slice,  2, 3);
    reg(I, "keys",    nb_keys,   1, 1);
    reg(I, "values",  nb_values, 1, 1);
    reg(I, "has",     nb_has,    2, 2);
    reg(I, "del",     nb_del,    2, 2);
    reg(I, "index_of", nb_index_of, 2, 2);
    reg(I, "concat",  nb_concat, 1, -1);
    reg(I, "reverse", nb_reverse, 1, 1);
    reg(I, "sort",    nb_sort,   1, 2);
    reg(I, "unique",  nb_unique, 1, 1);
    reg(I, "join",    nb_join,   2, 2);
    reg(I, "range",   nb_range,  2, 3);
    reg(I, "map",     nb_map,    2, 2);
    reg(I, "filter",  nb_filter, 2, 2);
    reg(I, "reduce",  nb_reduce, 3, 3);

    /* strings */
    reg(I, "split", nb_split, 2, 2);
    reg(I, "upper", nb_upper, 1, 1);
    reg(I, "lower", nb_lower, 1, 1);
    reg(I, "trim",  nb_trim,  1, 1);
    reg(I, "ltrim", nb_ltrim, 1, 1);
    reg(I, "rtrim", nb_rtrim, 1, 1);
    reg(I, "starts_with", nb_starts_with, 2, 2);
    reg(I, "ends_with",   nb_ends_with,   2, 2);
    reg(I, "contains",    nb_contains,    2, 2);
    reg(I, "find",        nb_find,        2, 2);
    reg(I, "replace",     nb_replace,     3, 3);
    reg(I, "substr",      nb_substr,      2, 3);
    reg(I, "pad_left",    nb_pad_left,    2, 3);
    reg(I, "pad_right",   nb_pad_right,   2, 3);
    reg(I, "repeat",      nb_repeat,      2, 2);

    /* files */
    reg(I, "read_file",   nb_read_file,   1, 1);
    reg(I, "write_file",  nb_write_file,  2, 2);
    reg(I, "append_file", nb_append_file, 2, 2);
    reg(I, "file_exists", nb_file_exists, 1, 1);
    reg(I, "delete_file", nb_delete_file, 1, 1);
    reg(I, "mkdir",       nb_mkdir,       1, 1);

    /* system */
    reg(I, "now",       nb_now,       0, 0);
    reg(I, "clock_ms",  nb_clock_ms,  0, 0);
    reg(I, "read_line", nb_read_line, 0, 0);
    reg(I, "get_env",   nb_get_env,   1, 1);
    reg(I, "error",     nb_error,     1, 1);
    reg(I, "exit",      nb_exit,      0, 1);
    reg(I, "assert",    nb_assert,    1, 2);
    reg(I, "json_encode", nb_json_encode, 1, 2);
    reg(I, "gc_collect",  nb_gc_collect,  0, 0);
    reg(I, "gc_stats",    nb_gc_stats,    0, 0);
    reg(I, "json_decode", nb_json_decode, 1, 1);
}
