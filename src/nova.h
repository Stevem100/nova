/*
 * nova.h — Nova programming language, shared definitions.
 *
 * Nova is a lightweight, dynamically-typed scripting language whose
 * syntax blends PHP ($variables, ->, =>, echo), C (braces, operators,
 * semicolons) and Python (dynamic values, slices, closures).
 *
 * This interpreter is written from scratch in portable C11 with zero
 * third-party dependencies. It builds on Linux, macOS and Windows.
 */
#ifndef NOVA_H
#define NOVA_H

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NOVA_VERSION   "1.0.0"
#define NOVA_MAX_DEPTH 512        /* max user-function call depth */
#define NOVA_MAX_RANGE 10000000   /* max elements produced by range() */

/* ================= memory management =================
 * Two tiers:
 *
 * 1. Arena (below): parse-time structures only — tokens, AST nodes,
 *    identifier names. Grows monotonically and is released at shutdown.
 *
 * 2. GC heap (gc.c): every runtime value — NVValue boxes, string buffers,
 *    arrays, hash tables, environments, functions, classes and instances.
 *    A tracing mark-sweep collector with precise roots reclaims each
 *    object individually (per-object reclamation). Collection happens
 *    only at statement boundaries (safe points), never mid-expression,
 *    so values held in C locals of the evaluator are always safe; values
 *    that must live across statement boundaries (loop iterables, call
 *    frames, callback results...) are registered on a pin stack.
 */

typedef struct ABlock { struct ABlock *next; size_t used, cap; } ABlock;
typedef struct Arena { ABlock *head; size_t used_total; } Arena;

void  *arena_alloc(Arena *a, size_t n);
char  *arena_strndup(Arena *a, const char *s, size_t n);
char  *arena_strdup(Arena *a, const char *s);
void   arena_free_all(Arena *a);

/* ---- per-object reclamation (mark-sweep GC heap) ---- */
#define NV_MAGIC_ALLOC 0x4E564141u   /* header of a live heap block   */
#define NV_MAGIC_FREE  0x4E564146u   /* header of a freed heap block  */
#define GC_NSIZES      32            /* free bins: 16, 32 ... 512 B   */

typedef struct GCHdr {
    uint32_t magic;                 /* NV_MAGIC_ALLOC or NV_MAGIC_FREE  */
    uint32_t size;                  /* total block size incl. header    */
    uint8_t  mark;                  /* live during current mark phase   */
    uint8_t  otype;                 /* OT_* tag (statistics only)       */
    uint16_t aux;
    uint32_t pad;                   /* keep the header exactly 16 bytes */
} GCHdr;

/* chunk header is a multiple of the block alignment so payloads stay
 * 16-byte aligned (NVValue contains a double) */
typedef struct GCChunk { struct GCChunk *next; size_t size, used; size_t pad; } GCChunk;

typedef struct GCHeap {
    GCChunk       *chunks;          /* malloc'd 1 MiB (or larger) slabs */
    GCHdr         *bins[GC_NSIZES]; /* segregated free lists            */
    GCHdr         *big;             /* first-fit list for >512 B blocks */
    size_t         live_bytes, live_objs;
    size_t         total_alloc_bytes;
    size_t         total_freed_bytes, last_freed_bytes;
    unsigned long  total_freed_objs, last_freed_objs;
    size_t         since_gc, trigger;  /* allocation budget until next GC */
    unsigned long  runs;               /* completed collections           */
} GCHeap;

/* what kind of object a heap block holds (for gc_stats) */
enum { OT_VALUE, OT_STRBUF, OT_ARRAY, OT_ARRITEMS, OT_TABLE, OT_TABLEENTS,
       OT_TABLEORDER, OT_ENV, OT_FN, OT_CLASS, OT_INST, OT_KEY };

/* pin stack tags */
enum { GC_TAG_VALUE = 0, GC_TAG_ENV = 1 };
typedef struct { void *p; int tag; } GCPin;

typedef struct Interp Interp;   /* full definition below */

void   *nv_gc_alloc(Interp *I, size_t n, uint8_t otype);   /* zeroed? no: returns raw block */
void    nv_gc_maybe(Interp *I);                /* safe point: collect when budget exceeded  */
void    nv_gc_collect(Interp *I);              /* force a full collection now               */
size_t  nv_gc_pin(Interp *I, void *p, int tag);/* keep a value/env alive across statements  */
void    nv_gc_unpin(Interp *I, size_t mark);   /* drop everything pinned after `mark`       */
void    nv_gc_repin(Interp *I, size_t mark, void *p); /* update a pin slot in place          */
void    nv_gc_free_all(Interp *I);             /* release the whole heap at shutdown        */

/* ---- scratch string buffer (plain malloc/free; never holds Nova values) ---- */
typedef struct SB { char *p; size_t n, cap; } SB;
void sb_init(SB *b);
void sb_free(SB *b);
void sb_putc(SB *b, char c);
void sb_putsl(SB *b, const char *s, size_t len);
void sb_puts(SB *b, const char *s);
void sb_fmt_float(SB *b, double d);

/* ================= values ================= */

typedef enum {
    T_NIL, T_BOOL, T_INT, T_FLOAT, T_STR, T_ARR, T_MAP, T_FN, T_CLASS, T_INSTANCE
} NType;

typedef struct NVValue  NVValue;
typedef struct NVTable  NVTable;
typedef struct NVArray  NVArray;
typedef struct ObjFn    ObjFn;
typedef struct ObjClass ObjClass;
typedef struct ObjInst  ObjInst;
typedef struct Env      Env;
typedef struct Interp   Interp;
typedef struct Node     Node;

struct NVArray { NVValue **items; size_t n, cap; };

struct NVValue {
    NType type;
    size_t slen;                    /* T_STR: byte length */
    union {
        int      b;                 /* T_BOOL */
        long long i;                /* T_INT */
        double   f;                 /* T_FLOAT */
        char    *s;                 /* T_STR (NUL-terminated for convenience) */
        NVArray *arr;               /* T_ARR */
        NVTable *map;               /* T_MAP */
        ObjFn   *fn;                /* T_FN */
        ObjClass *cls;              /* T_CLASS */
        ObjInst *inst;              /* T_INSTANCE */
    } as;
};

/* ---- insertion-ordered string-keyed hash table (maps, envs, methods) ---- */
typedef struct { const char *key; uint32_t hash; NVValue *val; unsigned char state; } TEntry;
/* state: 0 = empty, 1 = used, 2 = tombstone */
struct NVTable {
    TEntry   *ents;
    size_t    cap, count, tombs;
    size_t   *order;              /* insertion order of entry indices */
    size_t    on, ocap;
};

NVTable *nvtab_new(Interp *I);
bool     nvtab_get(NVTable *t, const char *key, NVValue **out);
void     nvtab_set(Interp *I, NVTable *t, const char *key, NVValue *val);   /* table owns a copy of key */
bool     nvtab_del(NVTable *t, const char *key);
size_t   nvtab_count(NVTable *t);
typedef bool (*NVTabIt)(const char *key, NVValue *val, void *ud);
void     nvtab_foreach(NVTable *t, NVTabIt fn, void *ud);

/* ---- functions, classes, instances ---- */
typedef NVValue *(*NNative)(Interp *I, NVValue **args, int n);

typedef struct { char *text; bool is_var; } SPart;   /* interpolated string piece */

struct ObjFn {
    const char *name;
    bool        native;
    NNative     native_fn;
    int         arity_min, arity_max;   /* arity_max -1 = variadic */
    /* user-defined: */
    const char **pnames; int nparams;
    Node       *body;                   /* N_BLOCK */
    Env        *closure;
    ObjClass   *in_class;               /* class this method was declared in */
};

struct ObjClass {
    const char *name;
    ObjClass   *base;
    NVTable    *methods;                /* name (no $) -> ObjFn */
};

struct ObjInst {
    ObjClass *cls;
    NVTable  *fields;                   /* "$name" -> value */
};

/* ---- environments (scope chain) ---- */
struct Env { NVTable *vars; Env *parent; };
Env  *env_new(Interp *I, Env *parent);
bool  env_get(Env *e, const char *key, NVValue **out);
void  env_define(Interp *I, Env *e, const char *key, NVValue *v);   /* current scope only */
void  env_assign(Interp *I, Env *e, const char *key, NVValue *v);   /* nearest binding, else define here */

/* ---- AST ---- */
typedef enum {
    N_BLOCK, N_EXPRSTMT, N_ASSIGN, N_AUG,
    N_IF, N_WHILE, N_FOR, N_FOREACH, N_BREAK, N_CONTINUE, N_RETURN,
    N_SWITCH, N_CASE,
    N_FN_DECL, N_CLASS_DECL, N_IMPORT, N_THROW, N_TRY, N_ECHO, N_EMPTY,
    N_INT, N_FLOAT, N_STR, N_BOOL, N_NIL, N_VAR, N_ARR, N_MAP, N_ANON,
    N_BIN, N_LOGIC, N_UN, N_POST, N_TERNARY, N_CALL, N_INDEX, N_APPEND,
    N_MEMBER, N_THIS, N_PARENT
} NKind;

typedef enum {
    O_ADD, O_SUB, O_MUL, O_DIV, O_MOD, O_POW, O_CAT,
    O_EQ, O_NE, O_IDE, O_NIDE, O_LT, O_LE, O_GT, O_GE,
    O_AND, O_OR, O_NILCOAL,
    O_NOT, O_NEG, O_INC, O_DEC,
    A_ADD, A_SUB, A_MUL, A_DIV, A_MOD, A_CAT
} Op;

struct Node {
    NKind        kind;
    int          line;
    int          op;
    const char  *name;      /* var/fn/class/member/import path/catch var */
    const char  *name2;     /* import "as" name, foreach value var, base class */
    long long    ival;
    double       fval;
    SPart       *parts; int nparts;      /* N_STR interpolation pieces */
    const char **pnames; int np;         /* function parameter names */
    Node       *a, *b, *c, *d;
    Node       **kids; int nkids, kcap;  /* blocks, args, elements, methods */
};

/* ---- diagnostics (compile time) ---- */
typedef struct { bool ok, eof; char msg[256]; int line; } NvDiag;
void nv_diag_ok(NvDiag *d);
void nv_diag_fail(NvDiag *d, int line, const char *fmt, ...);

/* ================= control flow ================= */
typedef enum { SIG_OK, SIG_RETURN, SIG_BREAK, SIG_CONTINUE, SIG_THROW } Signal;
typedef struct { Signal s; NVValue *v; } Ctrl;

/* ================= interpreter ================= */
struct Interp {
    Arena      mem;                 /* tokens, AST nodes, names */
    GCHeap     heap;                /* all runtime values live here */
    GCPin     *pins; int npins, pin_cap;   /* roots held across statements */
    Env       *builtins_env;        /* native functions live here */
    Env       *globals;             /* main script globals */
    ObjClass  *error_class;
    NVTable   *mod_cache;           /* resolved path -> module value */
    NVTable   *mod_busy;            /* circular-import detection */
    const char **dirs; int ndirs, dircap;      /* import resolution stack */
    const char **fname; int *fline; int depth, fcap;   /* call stack */
    int        cur_line;
    int        soft;               /* >0 while evaluating '??' left side */
    ObjFn     *cur_fn;             /* currently executing user function */
    NVValue   *err_value;           /* set whenever nv_err() returns NULL */
    NVValue   *last_expr;           /* REPL echo of last expression */
    bool       repl;
    int        exit_code;
};

void    interp_init(Interp *I);
void    interp_free(Interp *I);
char   *nv_read_whole_file(const char *path, size_t *out_len);   /* malloc'd, caller frees */
NVValue *nv_err(Interp *I, const char *fmt, ...);            /* always returns NULL */
Ctrl    nv_throw_ctrl(Interp *I);                            /* {SIG_THROW, err_value} */

/* value constructors */
NVValue *nv_int(Interp *I, long long v);
NVValue *nv_float(Interp *I, double v);
NVValue *nv_bool(Interp *I, bool v);
NVValue *nv_nil(void);
NVValue *nv_str(Interp *I, const char *s);
NVValue *nv_strl(Interp *I, const char *s, size_t len);
NVValue *nv_arr_new(Interp *I);
NVValue *nv_map_new(Interp *I);
NVValue *nv_fn_native(Interp *I, const char *name, NNative fn, int amin, int amax);
NVValue *nv_inst_new(Interp *I, ObjClass *cls);
NVValue *nv_new_error(Interp *I, const char *msg);
NVValue *nv_snapshot_trace(Interp *I);
ObjClass *nv_new_class(Interp *I, const char *name, ObjClass *base);

/* value operations */
void      nv_arr_push(Interp *I, NVValue *arrv, NVValue *item);
void      nv_map_set(Interp *I, NVValue *mapv, NVValue *k, NVValue *v);
NVValue  *nv_map_get(NVValue *mapv, NVValue *k);            /* NULL if missing */
NVValue  *nv_to_string(Interp *I, NVValue *v);              /* display form */
const char *nv_type_name(NType t);
bool      nv_truthy(NVValue *v);
bool      nv_equal(NVValue *a, NVValue *b);                 /* == (deep) */
bool      nv_strict_equal(NVValue *a, NVValue *b);          /* === */
bool      nv_num_val(NVValue *v, double *out);              /* int/float -> double */

/* ---- lexer / parser ---- */
typedef enum { TK_INT, TK_FLOAT, TK_STR, TK_VAR, TK_IDENT, TK_KW, TK_OP, TK_EOF } TKind;

typedef enum {
    KW_FN, KW_RETURN, KW_IF, KW_ELSEIF, KW_ELSE, KW_WHILE, KW_FOR, KW_FOREACH,
    KW_AS, KW_BREAK, KW_CONTINUE, KW_CLASS, KW_EXTENDS, KW_NEW, KW_THIS, KW_PARENT,
    KW_THROW, KW_TRY, KW_CATCH, KW_FINALLY, KW_IMPORT, KW_ECHO,
    KW_TRUE, KW_FALSE, KW_NIL,
    KW_SWITCH, KW_CASE, KW_DEFAULT
} NwKw;

typedef struct {
    TKind       kind;
    int         kw;         /* TK_KW */
    const char *lex;        /* lexeme (arena-owned) */
    long long   ival;       /* TK_INT */
    double      fval;       /* TK_FLOAT */
    SPart      *parts; int nparts;   /* TK_STR interpolation pieces */
    int         line;
} Token;

bool  nv_lex_tokens(Interp *I, const char *src, const char *file,
                    Token **out, int *ntok, NvDiag *diag);
Node *nv_parse_tokens(Interp *I, Token *toks, int ntok, NvDiag *diag);
bool  nv_compile(Interp *I, const char *src, const char *file, Node **out_prog, NvDiag *diag);
Ctrl  nv_exec_source(Interp *I, Env *env, const char *src, const char *file, NvDiag *diag);
Ctrl  nv_exec_nodes(Interp *I, Env *env, Node **stmts, int n);
bool  nv_call_value(Interp *I, NVValue *fn, NVValue **args, int n, NVValue **out);
void  nv_report_uncaught(Interp *I, NVValue *thrown);
bool  nv_run_file(Interp *I, const char *path, int argc, char **argv, int *exit_code);
Ctrl  nv_import(Interp *I, Env *importer, const char *path, const char *as_name);
NVValue *nv_find_method(ObjClass *cls, const char *name, ObjClass **def_out);
bool  nv_inst_of(Interp *I, NVValue *v, const char *cname);   /* is_a check */

/* json.c — minimal JSON parser (arena-backed values) */
NVValue *nv_json_parse(Interp *I, const char *text, size_t len,
                       char *err, size_t errsz);       /* NULL on error */

/* pkg.c — package manager (`nova pkg ...`) + import resolution fallback */
int  nv_pkg_main(int argc, char **argv);   /* argv[0] == "pkg"; returns exit code */
bool nv_pkg_resolve(const char *spec, char *out, size_t outsz,
                    char *err, size_t errsz);          /* module spec -> file path */

/* builtins.c */
void nv_register_builtins(Interp *I);

/* arg-check helpers shared by natives (builtins.c) */
bool nv_argc(Interp *I, const char *name, int n, int min, int max);
bool nv_arg_str(Interp *I, const char *name, int i, NVValue **args);
bool nv_arg_int(Interp *I, const char *name, int i, NVValue **args);
bool nv_arg_num(Interp *I, const char *name, int i, NVValue **args);
bool nv_arg_arr(Interp *I, const char *name, int i, NVValue **args);
bool nv_arg_map(Interp *I, const char *name, int i, NVValue **args);
bool nv_arg_fn (Interp *I, const char *name, int i, NVValue **args);

#endif /* NOVA_H */
