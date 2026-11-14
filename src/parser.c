/*
 * parser.c — Nova recursive-descent parser.
 *
 * Precedence (loosest to tightest):
 *   = += -= *= /= %= .=   (assignment, right-assoc)
 *   ?:                    (ternary)
 *   ??                    (null coalescing)
 *   ||                    (logical or)
 *   &&
 *   == != === !==
 *   < <= > >=
 *   .                     (string concatenation, PHP-style)
 *   + -
 *   * / %
 *   **                    (power, right-assoc)
 *   unary ! - ++ --
 *   postfix () [] -> ++ --
 */
#include "nova.h"

typedef struct {
    Token  *toks;
    int     n, pos;
    Interp *I;
    NvDiag *diag;
    bool    failed;
    int     depth;      /* recursion guard: nesting level being parsed */
} P;

/* Nesting bound for recursive-descent parsing. The parser runs one C frame
 * stack per nesting level, so unbounded nesting ("((((((...") would exhaust
 * the stack and crash the process — fatal in CGI hosting where a single
 * request can trigger it. 400 levels leaves ample C-stack headroom and far
 * exceeds anything real source code needs. The same bound effectively caps
 * interpreter AST-walk recursion, since AST depth == parse depth. */
#define NOVA_PARSE_MAX_DEPTH 400

static Token *peek(P *p)  { return &p->toks[p->pos]; }
static Token *peek2(P *p) { return &p->toks[p->pos + 1 < p->n ? p->pos + 1 : p->pos]; }
static void adv(P *p) { if (p->toks[p->pos].kind != TK_EOF) p->pos++; }

static bool at_op(P *p, const char *op) {
    Token *t = peek(p);
    return t->kind == TK_OP && strcmp(t->lex, op) == 0;
}
static bool at_kw(P *p, int kw) {
    Token *t = peek(p);
    return t->kind == TK_KW && t->kw == kw;
}
static bool eat_op(P *p, const char *op) { if (at_op(p, op)) { adv(p); return true; } return false; }
static bool eat_kw(P *p, int kw) { if (at_kw(p, kw)) { adv(p); return true; } return false; }

static void pfail(P *p, int line, const char *fmt, ...) {
    if (p->failed) return;
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    nv_diag_fail(p->diag, line, "%s", msg);
    p->failed = true;
}

static Token *expect_op(P *p, const char *op) {
    if (at_op(p, op)) { Token *t = peek(p); adv(p); return t; }
    pfail(p, peek(p)->line, "syntax error: expected '%s', found '%s'",
          op, peek(p)->kind == TK_EOF ? "<end of input>" : peek(p)->lex);
    return NULL;
}

static const char *tok_desc(Token *t) {
    static const char *kw_names[] = {
        "fn", "return", "if", "elseif", "else", "while", "for", "foreach",
        "as", "break", "continue", "class", "extends", "new", "this", "parent",
        "throw", "try", "catch", "finally", "import", "echo",
        "true", "false", "nil",
        "switch", "case", "default"
    };
    switch (t->kind) {
        case TK_EOF: return "<end of input>";
        case TK_INT: case TK_FLOAT: return t->lex ? t->lex : "number";
        case TK_STR: return "string literal";
        case TK_VAR: return t->lex;
        case TK_IDENT: return t->lex;
        case TK_KW: return kw_names[t->kw];
        case TK_OP: return t->lex;
    }
    return "?";
}

/* ---------- node helpers ---------- */

static Node *nnew(P *p, NKind k, int line) {
    Node *n = (Node *)arena_alloc(&p->I->mem, sizeof(Node));
    memset(n, 0, sizeof(Node));      /* all counters/pointers start empty */
    n->kind = k;
    n->line = line;
    return n;
}

static void kid(P *p, Node *n, Node *c) {
    if (n->nkids + 1 > n->kcap) {
        int cap = n->kcap ? n->kcap * 2 : 4;
        Node **ks = (Node **)arena_alloc(&p->I->mem, (size_t)cap * sizeof(Node *));
        for (int i = 0; i < n->nkids; i++) ks[i] = n->kids[i];
        n->kids = ks; n->kcap = cap;
    }
    n->kids[n->nkids++] = c;
}

static bool is_lvalue(Node *n) {
    if (n->kind == N_VAR || n->kind == N_MEMBER || n->kind == N_APPEND) return true;
    if (n->kind == N_INDEX && n->c == NULL) return true;   /* not a slice */
    return false;
}

/* ---------- forward decls ---------- */
static Node *parse_expr(P *p);
static Node *parse_stmt(P *p);

/* ---------- expressions ---------- */

static Node *parse_call_args(P *p, int line, Node *callee, int is_new) {
    Node *n = nnew(p, N_CALL, line);
    n->a = callee;
    n->op = is_new;
    if (!expect_op(p, "(")) return NULL;
    if (!at_op(p, ")")) {
        for (;;) {
            Node *arg = parse_expr(p);
            if (!arg) return NULL;
            kid(p, n, arg);
            if (!eat_op(p, ",")) break;
            if (at_op(p, ")")) break;      /* trailing comma */
        }
    }
    if (!expect_op(p, ")")) return NULL;
    return n;
}

static Node *parse_primary(P *p) {
    Token *t = peek(p);
    int line = t->line;

    if (t->kind == TK_INT) {
        adv(p);
        Node *n = nnew(p, N_INT, line);
        n->ival = t->ival;
        return n;
    }
    if (t->kind == TK_FLOAT) {
        adv(p);
        Node *n = nnew(p, N_FLOAT, line);
        n->fval = t->fval;
        return n;
    }
    if (t->kind == TK_STR) {
        adv(p);
        Node *n = nnew(p, N_STR, line);
        n->parts = t->parts;
        n->nparts = t->nparts;
        return n;
    }
    if (t->kind == TK_VAR) {
        adv(p);
        Node *n = nnew(p, N_VAR, line);
        n->name = t->lex;
        return n;
    }
    if (t->kind == TK_KW) {
        switch (t->kw) {
            case KW_TRUE: case KW_FALSE: {
                adv(p);
                Node *n = nnew(p, N_BOOL, line);
                n->ival = t->kw == KW_TRUE;
                return n;
            }
            case KW_NIL: adv(p); return nnew(p, N_NIL, line);
            case KW_THIS: adv(p); return nnew(p, N_THIS, line);
            case KW_NEW: {
                adv(p);
                Token *cn = peek(p);
                if (cn->kind != TK_IDENT && cn->kind != TK_VAR) {
                    pfail(p, cn->line, "syntax error: 'new' expects a class name or class variable");
                    return NULL;
                }
                adv(p);
                Node *base = nnew(p, N_VAR, line);
                base->name = arena_strndup(&p->I->mem, cn->lex, strlen(cn->lex));
                Node *cls = base;
                /* allow member chains: new $mod->Class(...) */
                while (at_op(p, "->")) {
                    adv(p);
                    Token *mt = peek(p);
                    if (mt->kind != TK_IDENT) {
                        pfail(p, mt->line, "syntax error: '->' expects a member name after 'new'");
                        return NULL;
                    }
                    adv(p);
                    Node *mem = nnew(p, N_MEMBER, line);
                    mem->a = cls;
                    mem->name = mt->lex;
                    cls = mem;
                }
                Node *call = parse_call_args(p, line, cls, 1);
                return call;
            }
            case KW_PARENT: {
                adv(p);
                if (!expect_op(p, "::")) return NULL;
                Token *mn = peek(p);
                if (mn->kind != TK_IDENT) {
                    pfail(p, mn->line, "syntax error: 'parent::' expects a method name");
                    return NULL;
                }
                adv(p);
                Node *n = nnew(p, N_PARENT, line);
                n->name = mn->lex;
                /* the following '(' is consumed by the postfix loop,
                   which wraps this node in a N_CALL with the arguments */
                return n;
            }
            case KW_FN: {
                /* anonymous function: fn ($x) { ... } */
                adv(p);
                Node *n = nnew(p, N_ANON, line);
                if (!expect_op(p, "(")) return NULL;
                /* params */
                const char **names = NULL; int np = 0, cap = 0;
                if (!at_op(p, ")")) {
                    for (;;) {
                        Token *vt = peek(p);
                        if (vt->kind != TK_VAR) {
                            pfail(p, vt->line, "syntax error: parameters must be variables like $x");
                            return NULL;
                        }
                        adv(p);
                        if (np + 1 > cap) {
                            cap = cap ? cap * 2 : 4;
                            const char **nn = (const char **)arena_alloc(&p->I->mem, (size_t)cap * sizeof(char *));
                            for (int i = 0; i < np; i++) nn[i] = names[i];
                            names = nn;
                        }
                        names[np++] = vt->lex;
                        if (!eat_op(p, ",")) break;
                        if (at_op(p, ")")) break;
                    }
                }
                if (!expect_op(p, ")")) return NULL;
                n->pnames = names; n->np = np;
                if (peek(p)->kind != TK_OP || strcmp(peek(p)->lex, "{") != 0) {
                    pfail(p, peek(p)->line, "syntax error: expected '{' for function body");
                    return NULL;
                }
                n->a = parse_stmt(p);
                if (!n->a) return NULL;
                return n;
            }
            default:
                pfail(p, line, "syntax error: unexpected keyword '%s'", tok_desc(t));
                return NULL;
        }
    }
    if (t->kind == TK_OP && strcmp(t->lex, "(") == 0) {
        adv(p);
        Node *e = parse_expr(p);
        if (!e) return NULL;
        if (!expect_op(p, ")")) return NULL;
        return e;
    }
    if (t->kind == TK_OP && strcmp(t->lex, "[") == 0) {
        /* array or map literal */
        adv(p);
        if (eat_op(p, "]")) return nnew(p, N_ARR, line);
        if (at_op(p, ":")) {          /* [:] = empty map */
            adv(p);
            if (!expect_op(p, "]")) return NULL;
            return nnew(p, N_MAP, line);
        }
        Node *first = parse_expr(p);
        if (!first) return NULL;
        if (at_op(p, "=>")) {
            adv(p);
            Node *val = parse_expr(p);
            if (!val) return NULL;
            Node *m = nnew(p, N_MAP, line);
            kid(p, m, first);
            kid(p, m, val);
            while (eat_op(p, ",")) {
                if (at_op(p, "]")) break;
                Node *k = parse_expr(p);
                if (!k) return NULL;
                if (!eat_op(p, "=>")) {
                    pfail(p, peek(p)->line, "syntax error: expected '=>' in map literal");
                    return NULL;
                }
                Node *v = parse_expr(p);
                if (!v) return NULL;
                kid(p, m, k);
                kid(p, m, v);
            }
            if (!expect_op(p, "]")) return NULL;
            return m;
        }
        Node *a = nnew(p, N_ARR, line);
        kid(p, a, first);
        while (eat_op(p, ",")) {
            if (at_op(p, "]")) break;
            Node *el = parse_expr(p);
            if (!el) return NULL;
            kid(p, a, el);
        }
        if (!expect_op(p, "]")) return NULL;
        return a;
    }
    if (t->kind == TK_IDENT) {
        /* a bare identifier is only legal as a function call: add(1, 2) */
        Token *nx = peek2(p);
        if (nx->kind == TK_OP && strcmp(nx->lex, "(") == 0) {
            adv(p);
            Node *vn = nnew(p, N_VAR, line);
            vn->name = arena_strndup(&p->I->mem, t->lex, strlen(t->lex));
            Node *call = parse_call_args(p, line, vn, 0);
            return call;
        }
        pfail(p, line,
              "syntax error: unexpected identifier '%s' (call it as %s(), or variables start with '$')",
              t->lex, t->lex);
        return NULL;
    }
    pfail(p, line, "syntax error: unexpected %s", tok_desc(t));
    return NULL;
}

static Node *parse_postfix(P *p) {
    Node *e = parse_primary(p);
    if (!e) return NULL;
    for (;;) {
        Token *t = peek(p);
        int line = t->line;
        if (t->kind == TK_OP && strcmp(t->lex, "(") == 0) {
            Node *c = parse_call_args(p, line, e, 0);
            if (!c) return NULL;
            e = c;
            continue;
        }
        if (t->kind == TK_OP && strcmp(t->lex, "[") == 0) {
            adv(p);
            if (eat_op(p, "]")) {
                Node *n = nnew(p, N_APPEND, line);
                n->a = e;
                e = n;
                continue;
            }
            Node *idx = parse_expr(p);
            if (!idx) return NULL;
            Node *n = nnew(p, N_INDEX, line);
            n->a = e;
            n->b = idx;
            if (eat_op(p, "..")) {
                Node *end = parse_expr(p);
                if (!end) return NULL;
                n->c = end;
            }
            if (!expect_op(p, "]")) return NULL;
            e = n;
            continue;
        }
        if (t->kind == TK_OP && strcmp(t->lex, "->") == 0) {
            adv(p);
            Token *m = peek(p);
            if (m->kind != TK_IDENT) {
                pfail(p, m->line, "syntax error: '->' expects a member name");
                return NULL;
            }
            adv(p);
            Node *n = nnew(p, N_MEMBER, line);
            n->a = e;
            n->name = m->lex;
            e = n;
            continue;
        }
        if (t->kind == TK_OP && (strcmp(t->lex, "++") == 0 || strcmp(t->lex, "--") == 0)) {
            if (!is_lvalue(e)) {
                pfail(p, line, "syntax error: %s needs a variable, field or index as target", t->lex);
                return NULL;
            }
            adv(p);
            Node *n = nnew(p, N_POST, line);
            n->op = strcmp(t->lex, "++") == 0 ? O_INC : O_DEC;
            n->a = e;
            e = n;
            continue;
        }
        break;
    }
    return e;
}

/* unary: -x !x ++x --x or a power expression; '-' binds looser than '**'
 * (Python-style: -2 ** 2 == -4) */
static Node *parse_pow(P *p);
static Node *parse_uexpr_inner(P *p);

/* guarded wrapper — a chain like ----------------1 recurses here once per '-' */
static Node *parse_uexpr(P *p) {
    if (++p->depth > NOVA_PARSE_MAX_DEPTH) {
        pfail(p, peek(p)->line, "expression nesting too deep (max %d)",
              NOVA_PARSE_MAX_DEPTH);
        p->depth--;
        return NULL;
    }
    Node *n = parse_uexpr_inner(p);
    p->depth--;
    return n;
}

static Node *parse_uexpr_inner(P *p) {
    Token *t = peek(p);
    int line = t->line;
    if (t->kind == TK_OP && strcmp(t->lex, "!") == 0) {
        adv(p);
        Node *e = parse_uexpr(p);
        if (!e) return NULL;
        Node *n = nnew(p, N_UN, line);
        n->op = O_NOT;
        n->a = e;
        return n;
    }
    if (t->kind == TK_OP && strcmp(t->lex, "-") == 0) {
        adv(p);
        Node *e = parse_uexpr(p);
        if (!e) return NULL;
        Node *n = nnew(p, N_UN, line);
        n->op = O_NEG;
        n->a = e;
        return n;
    }
    if (t->kind == TK_OP && strcmp(t->lex, "+") == 0) {
        adv(p);
        return parse_uexpr(p);
    }
    if (t->kind == TK_OP && (strcmp(t->lex, "++") == 0 || strcmp(t->lex, "--") == 0)) {
        adv(p);
        Node *e = parse_uexpr(p);
        if (!e) return NULL;
        if (!is_lvalue(e)) {
            pfail(p, line, "syntax error: %s needs a variable, field or index as target", t->lex);
            return NULL;
        }
        Node *n = nnew(p, N_UN, line);
        n->op = strcmp(t->lex, "++") == 0 ? O_INC : O_DEC;
        n->a = e;
        return n;
    }
    return parse_pow(p);
}

static Node *binnode(P *p, int line, int op, Node *l, Node *r) {
    Node *n = nnew(p, N_BIN, line);
    n->op = op;
    n->a = l;
    n->b = r;
    return n;
}

/* power: base is a postfix expression, exponent is a unary expression.
 * This makes '-2 ** 2' equal -4 (Python semantics) and '2 ** -3' legal.
 * (Recursion 2**2**2**... passes through the guarded parse_uexpr.) */
static Node *parse_pow(P *p) {
    Node *l = parse_postfix(p);
    if (!l) return NULL;
    if (at_op(p, "**")) {
        int line = peek(p)->line;
        adv(p);
        Node *r = parse_uexpr(p);
        if (!r) return NULL;
        return binnode(p, line, O_POW, l, r);
    }
    return l;
}

static Node *parse_mul(P *p) {
    Node *l = parse_uexpr(p);
    if (!l) return NULL;
    for (;;) {
        Token *t = peek(p);
        if (t->kind == TK_OP && (strcmp(t->lex, "*") == 0 || strcmp(t->lex, "/") == 0 || strcmp(t->lex, "%") == 0)) {
            int line = t->line;
            int op = t->lex[0] == '*' ? O_MUL : t->lex[0] == '/' ? O_DIV : O_MOD;
            adv(p);
            Node *r = parse_uexpr(p);
            if (!r) return NULL;
            l = binnode(p, line, op, l, r);
            continue;
        }
        break;
    }
    return l;
}

static Node *parse_add(P *p) {
    Node *l = parse_mul(p);
    if (!l) return NULL;
    for (;;) {
        Token *t = peek(p);
        if (t->kind == TK_OP && (strcmp(t->lex, "+") == 0 || strcmp(t->lex, "-") == 0)) {
            int line = t->line;
            int op = t->lex[0] == '+' ? O_ADD : O_SUB;
            adv(p);
            Node *r = parse_mul(p);
            if (!r) return NULL;
            l = binnode(p, line, op, l, r);
            continue;
        }
        break;
    }
    return l;
}

static Node *parse_concat(P *p) {
    Node *l = parse_add(p);
    if (!l) return NULL;
    while (at_op(p, ".")) {
        int line = peek(p)->line;
        adv(p);
        Node *r = parse_add(p);
        if (!r) return NULL;
        l = binnode(p, line, O_CAT, l, r);
    }
    return l;
}

static Node *parse_rel(P *p) {
    Node *l = parse_concat(p);
    if (!l) return NULL;
    for (;;) {
        Token *t = peek(p);
        if (t->kind == TK_OP &&
            (strcmp(t->lex, "<") == 0 || strcmp(t->lex, "<=") == 0 ||
             strcmp(t->lex, ">") == 0 || strcmp(t->lex, ">=") == 0)) {
            int line = t->line;
            int op = strcmp(t->lex, "<") == 0 ? O_LT : strcmp(t->lex, "<=") == 0 ? O_LE
                   : strcmp(t->lex, ">") == 0 ? O_GT : O_GE;
            adv(p);
            Node *r = parse_concat(p);
            if (!r) return NULL;
            l = binnode(p, line, op, l, r);
            continue;
        }
        break;
    }
    return l;
}

static Node *parse_eq(P *p) {
    Node *l = parse_rel(p);
    if (!l) return NULL;
    for (;;) {
        Token *t = peek(p);
        if (t->kind == TK_OP &&
            (strcmp(t->lex, "==") == 0 || strcmp(t->lex, "!=") == 0 ||
             strcmp(t->lex, "===") == 0 || strcmp(t->lex, "!==") == 0)) {
            int line = t->line;
            int op = strcmp(t->lex, "==") == 0 ? O_EQ : strcmp(t->lex, "!=") == 0 ? O_NE
                   : strcmp(t->lex, "===") == 0 ? O_IDE : O_NIDE;
            adv(p);
            Node *r = parse_rel(p);
            if (!r) return NULL;
            l = binnode(p, line, op, l, r);
            continue;
        }
        break;
    }
    return l;
}

static Node *parse_and(P *p) {
    Node *l = parse_eq(p);
    if (!l) return NULL;
    while (at_op(p, "&&")) {
        int line = peek(p)->line;
        adv(p);
        Node *r = parse_eq(p);
        if (!r) return NULL;
        l = binnode(p, line, O_AND, l, r);
    }
    return l;
}

static Node *parse_or(P *p) {
    Node *l = parse_and(p);
    if (!l) return NULL;
    while (at_op(p, "||")) {
        int line = peek(p)->line;
        adv(p);
        Node *r = parse_and(p);
        if (!r) return NULL;
        l = binnode(p, line, O_OR, l, r);
    }
    return l;
}

static Node *parse_nullish_inner(P *p);

/* guarded wrapper — 'a ?? b ?? c ...' recurses here once per '??' */
static Node *parse_nullish(P *p) {
    if (++p->depth > NOVA_PARSE_MAX_DEPTH) {
        pfail(p, peek(p)->line, "expression nesting too deep (max %d)",
              NOVA_PARSE_MAX_DEPTH);
        p->depth--;
        return NULL;
    }
    Node *n = parse_nullish_inner(p);
    p->depth--;
    return n;
}
static Node *parse_nullish_inner(P *p) {
    Node *l = parse_or(p);
    if (!l) return NULL;
    if (at_op(p, "??")) {
        int line = peek(p)->line;
        adv(p);
        Node *r = parse_nullish(p);
        if (!r) return NULL;
        return binnode(p, line, O_NILCOAL, l, r);
    }
    return l;
}

static Node *parse_ternary_inner(P *p);
static Node *parse_ternary(P *p) {
    if (++p->depth > NOVA_PARSE_MAX_DEPTH) {
        pfail(p, peek(p)->line, "expression nesting too deep (max %d)",
              NOVA_PARSE_MAX_DEPTH);
        p->depth--;
        return NULL;
    }
    Node *n = parse_ternary_inner(p);
    p->depth--;
    return n;
}
static Node *parse_ternary_inner(P *p) {
    Node *c = parse_nullish(p);
    if (!c) return NULL;
    if (at_op(p, "?")) {
        int line = peek(p)->line;
        adv(p);
        Node *a = parse_expr(p);
        if (!a) return NULL;
        if (!expect_op(p, ":")) return NULL;
        Node *b = parse_ternary(p);
        if (!b) return NULL;
        Node *n = nnew(p, N_TERNARY, line);
        n->a = c; n->b = a; n->c = b;
        return n;
    }
    return c;
}

static int assign_op_code(const char *lex) {
    if (strcmp(lex, "=") == 0) return 0;
    if (strcmp(lex, "+=") == 0) return A_ADD;
    if (strcmp(lex, "-=") == 0) return A_SUB;
    if (strcmp(lex, "*=") == 0) return A_MUL;
    if (strcmp(lex, "/=") == 0) return A_DIV;
    if (strcmp(lex, "%=") == 0) return A_MOD;
    if (strcmp(lex, ".=") == 0) return A_CAT;
    return -1;
}

static Node *parse_assign_inner(P *p);

/* guarded wrapper — see NOVA_PARSE_MAX_DEPTH */
static Node *parse_assign(P *p) {
    if (++p->depth > NOVA_PARSE_MAX_DEPTH) {
        pfail(p, peek(p)->line, "expression nesting too deep (max %d)",
              NOVA_PARSE_MAX_DEPTH);
        p->depth--;
        return NULL;
    }
    Node *n = parse_assign_inner(p);
    p->depth--;
    return n;
}

static Node *parse_assign_inner(P *p) {
    Node *l = parse_ternary(p);
    if (!l) return NULL;
    Token *t = peek(p);
    if (t->kind == TK_OP) {
        int code = assign_op_code(t->lex);
        if (code >= 0) {
            int line = t->line;
            if (!is_lvalue(l)) {
                pfail(p, line, "syntax error: invalid assignment target");
                return NULL;
            }
            adv(p);
            Node *r = parse_assign(p);
            if (!r) return NULL;
            Node *n = nnew(p, code == 0 ? N_ASSIGN : N_AUG, line);
            n->op = code;
            n->a = l;
            n->b = r;
            return n;
        }
    }
    return l;
}

static Node *parse_expr(P *p) { return parse_assign(p); }   /* guarded via parse_assign */

/* ---------- statements ---------- */

static void eat_trailing_semi(P *p) { while (eat_op(p, ";")) {} }

static Node *parse_block(P *p) {
    int line = peek(p)->line;
    if (!expect_op(p, "{")) return NULL;
    Node *n = nnew(p, N_BLOCK, line);
    while (!at_op(p, "}") && !p->failed) {
        if (peek(p)->kind == TK_EOF) {
            pfail(p, peek(p)->line, "syntax error: unexpected end of input, missing '}'");
            return NULL;
        }
        Node *s = parse_stmt(p);
        if (!s) return NULL;
        kid(p, n, s);
    }
    if (!expect_op(p, "}")) return NULL;
    return n;
}

static Node *parse_fn_rest(P *p, int line, const char *name) {
    Node *n = nnew(p, name ? N_FN_DECL : N_ANON, line);
    n->name = name;
    if (!expect_op(p, "(")) return NULL;
    const char **names = NULL; int np = 0, cap = 0;
    if (!at_op(p, ")")) {
        for (;;) {
            Token *vt = peek(p);
            if (vt->kind != TK_VAR) {
                pfail(p, vt->line, "syntax error: parameters must be variables like $x");
                return NULL;
            }
            adv(p);
            if (np + 1 > cap) {
                cap = cap ? cap * 2 : 4;
                const char **nn = (const char **)arena_alloc(&p->I->mem, (size_t)cap * sizeof(char *));
                for (int i = 0; i < np; i++) nn[i] = names[i];
                names = nn;
            }
            names[np++] = vt->lex;
            if (!eat_op(p, ",")) break;
            if (at_op(p, ")")) break;
        }
    }
    if (!expect_op(p, ")")) return NULL;
    n->pnames = names;
    n->np = np;
    n->a = parse_block(p);
    if (!n->a) return NULL;
    return n;
}

static Node *parse_if_chain_inner(P *p, int line);

/* guarded wrapper — elseif chains recurse here once per 'elseif' */
static Node *parse_if_chain(P *p, int line) {
    if (++p->depth > NOVA_PARSE_MAX_DEPTH) {
        pfail(p, line, "statement nesting too deep (max %d)",
              NOVA_PARSE_MAX_DEPTH);
        p->depth--;
        return NULL;
    }
    Node *n = parse_if_chain_inner(p, line);
    p->depth--;
    return n;
}

static Node *parse_if_chain_inner(P *p, int line) {
    Node *n = nnew(p, N_IF, line);
    if (!expect_op(p, "(")) return NULL;
    n->a = parse_expr(p);
    if (!n->a) return NULL;
    if (!expect_op(p, ")")) return NULL;
    n->b = parse_stmt(p);
    if (!n->b) return NULL;
    if (at_kw(p, KW_ELSEIF)) {
        int l2 = peek(p)->line;
        adv(p);
        n->c = parse_if_chain(p, l2);
        if (!n->c) return NULL;
    } else if (eat_kw(p, KW_ELSE)) {
        n->c = parse_stmt(p);
        if (!n->c) return NULL;
    }
    return n;
}

static Node *parse_stmt_inner(P *p);

/* guarded wrapper — nested blocks/if/while bodies recurse here once per level */
static Node *parse_stmt(P *p) {
    if (p->failed) return NULL;
    if (++p->depth > NOVA_PARSE_MAX_DEPTH) {
        pfail(p, peek(p)->line, "statement nesting too deep (max %d)",
              NOVA_PARSE_MAX_DEPTH);
        p->depth--;
        return NULL;
    }
    Node *n = parse_stmt_inner(p);
    p->depth--;
    return n;
}

static Node *parse_stmt_inner(P *p) {
    if (p->failed) return NULL;
    Token *t = peek(p);
    int line = t->line;

    if (t->kind == TK_OP && strcmp(t->lex, "{") == 0)
        return parse_block(p);
    if (t->kind == TK_OP && strcmp(t->lex, ";") == 0) {
        adv(p);
        return nnew(p, N_EMPTY, line);
    }
    if (t->kind == TK_EOF) {
        pfail(p, line, "syntax error: unexpected end of input");
        return NULL;
    }

    if (t->kind == TK_KW) {
        switch (t->kw) {
            case KW_IF: {
                adv(p);
                return parse_if_chain(p, line);
            }
            case KW_WHILE: {
                adv(p);
                if (!expect_op(p, "(")) return NULL;
                Node *n = nnew(p, N_WHILE, line);
                n->a = parse_expr(p);
                if (!n->a) return NULL;
                if (!expect_op(p, ")")) return NULL;
                n->b = parse_stmt(p);
                if (!n->b) return NULL;
                return n;
            }
            case KW_FOR: {
                adv(p);
                if (!expect_op(p, "(")) return NULL;
                Node *n = nnew(p, N_FOR, line);
                n->a = at_op(p, ";") ? NULL : parse_expr(p);
                if (p->failed) return NULL;
                if (!expect_op(p, ";")) return NULL;
                n->b = at_op(p, ";") ? NULL : parse_expr(p);
                if (p->failed) return NULL;
                if (!expect_op(p, ";")) return NULL;
                n->c = at_op(p, ")") ? NULL : parse_expr(p);
                if (p->failed) return NULL;
                if (!expect_op(p, ")")) return NULL;
                n->d = parse_stmt(p);
                if (!n->d) return NULL;
                return n;
            }
            case KW_FOREACH: {
                adv(p);
                if (!expect_op(p, "(")) return NULL;
                Node *iter = parse_expr(p);
                if (!iter) return NULL;
                if (!eat_kw(p, KW_AS)) {
                    pfail(p, peek(p)->line, "syntax error: expected 'as' in foreach");
                    return NULL;
                }
                Token *k = peek(p);
                if (k->kind != TK_VAR) {
                    pfail(p, k->line, "syntax error: foreach expects a variable after 'as'");
                    return NULL;
                }
                adv(p);
                Node *n = nnew(p, N_FOREACH, line);
                n->name = k->lex;
                n->a = iter;
                if (eat_op(p, "=>")) {
                    Token *v = peek(p);
                    if (v->kind != TK_VAR) {
                        pfail(p, v->line, "syntax error: foreach expects a value variable after '=>' (map iteration)");
                        return NULL;
                    }
                    adv(p);
                    n->name2 = v->lex;
                }
                if (!expect_op(p, ")")) return NULL;
                n->b = parse_stmt(p);
                if (!n->b) return NULL;
                return n;
            }
            case KW_SWITCH: {
                adv(p);
                if (!expect_op(p, "(")) return NULL;
                Node *n = nnew(p, N_SWITCH, line);
                n->a = parse_expr(p);           /* switch subject */
                if (!n->a) return NULL;
                if (!expect_op(p, ")")) return NULL;
                if (!expect_op(p, "{")) return NULL;

                while (!at_op(p, "}") && !p->failed) {
                    if (peek(p)->kind == TK_EOF) {
                        pfail(p, peek(p)->line, "syntax error: unexpected end of input, missing '}'");
                        return NULL;
                    }
                    /* one branch: one or more stacked case labels, optional default */
                    Node *br = nnew(p, N_CASE, peek(p)->line);
                    bool have_label = false, have_default = false;
                    for (;;) {
                        if (at_kw(p, KW_CASE)) {
                            adv(p);
                            Node *lbl = parse_expr(p);
                            if (!lbl) return NULL;
                            kid(p, br, lbl);
                            if (!expect_op(p, ":")) return NULL;
                            have_label = true;
                        } else if (at_kw(p, KW_DEFAULT)) {
                            adv(p);
                            if (!expect_op(p, ":")) return NULL;
                            have_default = true;   /* br->op == 1 marks the default branch */
                        } else {
                            break;
                        }
                    }
                    if (!have_label && !have_default) {
                        pfail(p, br->line, "syntax error: expected 'case' or 'default' in switch body");
                        return NULL;
                    }
                    if (have_default) br->op = 1;

                    /* statements until the next label or the closing brace */
                    Node *body = nnew(p, N_BLOCK, br->line);
                    while (!at_kw(p, KW_CASE) && !at_kw(p, KW_DEFAULT) &&
                           !at_op(p, "}") && !p->failed) {
                        if (peek(p)->kind == TK_EOF) {
                            pfail(p, peek(p)->line, "syntax error: unexpected end of input, missing '}'");
                            return NULL;
                        }
                        Node *s = parse_stmt(p);
                        if (!s) return NULL;
                        kid(p, body, s);
                    }
                    br->a = body;
                    kid(p, n, br);
                }
                if (!expect_op(p, "}")) return NULL;
                return n;
            }
            case KW_RETURN: {
                adv(p);
                Node *n = nnew(p, N_RETURN, line);
                if (!at_op(p, ";")) {
                    n->a = parse_expr(p);
                    if (!n->a) return NULL;
                }
                if (!expect_op(p, ";")) return NULL;
                return n;
            }
            case KW_BREAK: {
                adv(p);
                if (!expect_op(p, ";")) return NULL;
                return nnew(p, N_BREAK, line);
            }
            case KW_CONTINUE: {
                adv(p);
                if (!expect_op(p, ";")) return NULL;
                return nnew(p, N_CONTINUE, line);
            }
            case KW_THROW: {
                adv(p);
                Node *n = nnew(p, N_THROW, line);
                n->a = parse_expr(p);
                if (!n->a) return NULL;
                if (!expect_op(p, ";")) return NULL;
                return n;
            }
            case KW_TRY: {
                adv(p);
                Node *n = nnew(p, N_TRY, line);
                if (peek(p)->kind != TK_OP || strcmp(peek(p)->lex, "{") != 0) {
                    pfail(p, peek(p)->line, "syntax error: 'try' expects '{'");
                    return NULL;
                }
                n->a = parse_block(p);
                if (!n->a) return NULL;
                if (eat_kw(p, KW_CATCH)) {
                    if (!expect_op(p, "(")) return NULL;
                    Token *cv = peek(p);
                    if (cv->kind != TK_VAR) {
                        pfail(p, cv->line, "syntax error: catch expects a variable like ($e)");
                        return NULL;
                    }
                    adv(p);
                    n->name = cv->lex;
                    if (!expect_op(p, ")")) return NULL;
                    if (peek(p)->kind != TK_OP || strcmp(peek(p)->lex, "{") != 0) {
                        pfail(p, peek(p)->line, "syntax error: 'catch' expects '{'");
                        return NULL;
                    }
                    n->b = parse_block(p);
                    if (!n->b) return NULL;
                }
                if (eat_kw(p, KW_FINALLY)) {
                    if (peek(p)->kind != TK_OP || strcmp(peek(p)->lex, "{") != 0) {
                        pfail(p, peek(p)->line, "syntax error: 'finally' expects '{'");
                        return NULL;
                    }
                    n->c = parse_block(p);
                    if (!n->c) return NULL;
                }
                if (!n->b && !n->c) {
                    pfail(p, line, "syntax error: 'try' requires a 'catch' or 'finally' block");
                    return NULL;
                }
                return n;
            }
            case KW_IMPORT: {
                adv(p);
                Node *n = nnew(p, N_IMPORT, line);
                Token *pt = peek(p);
                if (pt->kind != TK_STR) {
                    pfail(p, pt->line, "syntax error: import expects a string path like \"lib.nv\"");
                    return NULL;
                }
                adv(p);
                if (pt->nparts == 1 && !pt->parts[0].is_var) {
                    n->name = pt->parts[0].text;
                } else {
                    pfail(p, pt->line, "syntax error: import path must be a plain string");
                    return NULL;
                }
                if (eat_kw(p, KW_AS)) {
                    Token *an = peek(p);
                    if (an->kind != TK_VAR) {
                        pfail(p, an->line, "syntax error: import 'as' expects a variable like $name");
                        return NULL;
                    }
                    adv(p);
                    n->name2 = an->lex;
                }
                if (!expect_op(p, ";")) return NULL;
                return n;
            }
            case KW_ECHO: {
                adv(p);
                Node *n = nnew(p, N_ECHO, line);
                for (;;) {
                    Node *e = parse_expr(p);
                    if (!e) return NULL;
                    kid(p, n, e);
                    if (!eat_op(p, ",")) break;
                }
                if (!expect_op(p, ";")) return NULL;
                return n;
            }
            case KW_FN: {
                if (peek2(p)->kind == TK_OP && strcmp(peek2(p)->lex, "(") == 0) {
                    /* anonymous function in statement position: expression */
                    Node *e = parse_expr(p);
                    if (!e) return NULL;
                    if (!expect_op(p, ";")) return NULL;
                    Node *n = nnew(p, N_EXPRSTMT, line);
                    n->a = e;
                    return n;
                }
                adv(p);
                Token *nt = peek(p);
                if (nt->kind != TK_IDENT) {
                    pfail(p, nt->line, "syntax error: 'fn' expects a function name");
                    return NULL;
                }
                adv(p);
                Node *n = parse_fn_rest(p, line, nt->lex);
                if (!n) return NULL;
                eat_trailing_semi(p);
                return n;
            }
            case KW_CLASS: {
                adv(p);
                Token *nt = peek(p);
                if (nt->kind != TK_IDENT) {
                    pfail(p, nt->line, "syntax error: 'class' expects a class name");
                    return NULL;
                }
                adv(p);
                Node *n = nnew(p, N_CLASS_DECL, line);
                n->name = nt->lex;
                if (eat_kw(p, KW_EXTENDS)) {
                    Token *bt = peek(p);
                    if (bt->kind != TK_IDENT) {
                        pfail(p, bt->line, "syntax error: 'extends' expects a class name");
                        return NULL;
                    }
                    adv(p);
                    n->name2 = bt->lex;
                }
                if (!expect_op(p, "{")) return NULL;
                while (!at_op(p, "}")) {
                    if (peek(p)->kind == TK_EOF) {
                        pfail(p, peek(p)->line, "syntax error: unexpected end of input in class body");
                        return NULL;
                    }
                    if (!eat_kw(p, KW_FN)) {
                        pfail(p, peek(p)->line,
                              "syntax error: class members must be methods declared with 'fn'");
                        return NULL;
                    }
                    Token *mt = peek(p);
                    if (mt->kind != TK_IDENT) {
                        pfail(p, mt->line, "syntax error: expected method name after 'fn'");
                        return NULL;
                    }
                    adv(p);
                    Node *m = parse_fn_rest(p, mt->line, mt->lex);
                    if (!m) return NULL;
                    kid(p, n, m);
                    eat_trailing_semi(p);
                }
                adv(p);   /* consume '}' */
                eat_trailing_semi(p);
                return n;
            }
            default:
                break;   /* fall through to expression statement */
        }
    }

    /* expression / assignment statement */
    {
        Node *e = parse_expr(p);
        if (!e) return NULL;
        if (!expect_op(p, ";")) return NULL;
        Node *n = nnew(p, N_EXPRSTMT, line);
        n->a = e;
        return n;
    }
}

Node *nv_parse_tokens(Interp *I, Token *toks, int ntok, NvDiag *diag) {
    /* (parse.depth starts at 0 via the static parser init below) */
    P p;
    memset(&p, 0, sizeof(p));
    p.toks = toks;
    p.n = ntok;
    p.I = I;
    p.diag = diag;

    Node *prog = nnew(&p, N_BLOCK, 1);
    while (toks[p.pos].kind != TK_EOF && !p.failed) {
        Node *s = parse_stmt(&p);
        if (!s) p.failed = true;   /* parse_stmt already filled diag->msg */
        else kid(&p, prog, s);
    }
    if (p.failed) {
        /* mark genuine truncation so the REPL can wait for more input
         * ("expected ';' found EOF" is a missing semicolon, not truncation) */
        if (strstr(diag->msg, "unexpected end of input") || strstr(diag->msg, "missing '}'"))
            diag->eof = true;
        return NULL;
    }
    if (toks[p.pos].kind != TK_EOF) {
        pfail(&p, toks[p.pos].line, "syntax error: unexpected token '%s'", tok_desc(&toks[p.pos]));
        return NULL;
    }
    diag->ok = true;
    diag->eof = false;
    return prog;
}

bool nv_compile(Interp *I, const char *src, const char *file, Node **out_prog, NvDiag *diag) {
    Token *toks = NULL;
    int ntok = 0;
    if (!nv_lex_tokens(I, src, file, &toks, &ntok, diag)) return false;
    Node *prog = nv_parse_tokens(I, toks, ntok, diag);
    if (!prog) return false;
    *out_prog = prog;
    return true;
}
