/*
 * lexer.c — Nova tokenizer.
 *
 * Produces a flat token array. Double-quoted strings are split into
 * interpolation parts (literal chunks and $variable references) at lex
 * time; values are resolved at run time.
 */
#include "nova.h"

static const struct { const char *word; int kw; } KEYWORDS[] = {
    { "fn",       KW_FN       }, { "return",   KW_RETURN   },
    { "if",       KW_IF       }, { "elseif",   KW_ELSEIF   },
    { "else",     KW_ELSE     }, { "while",    KW_WHILE    },
    { "for",      KW_FOR      }, { "foreach",  KW_FOREACH  },
    { "as",       KW_AS       }, { "break",    KW_BREAK    },
    { "continue", KW_CONTINUE }, { "class",   KW_CLASS    },
    { "extends",  KW_EXTENDS  }, { "new",      KW_NEW      },
    { "this",     KW_THIS     }, { "parent",   KW_PARENT   },
    { "throw",    KW_THROW    }, { "try",      KW_TRY      },
    { "catch",    KW_CATCH    }, { "finally",  KW_FINALLY  },
    { "import",   KW_IMPORT   }, { "echo",     KW_ECHO     },
    { "true",     KW_TRUE     }, { "false",    KW_FALSE    },
    { "nil",      KW_NIL      },
    { "switch",   KW_SWITCH   }, { "case",     KW_CASE     },
    { "default",  KW_DEFAULT  },
};

static const char *OPS[] = {
    "===", "!==", "==", "!=", "<=", ">=", "&&", "||", "++", "--",
    "+=", "-=", "*=", "/=", "%=", ".=", "**", "..", "??", "->", "=>", "::",
    "+", "-", "*", "/", "%", ".", "<", ">", "=", "!",
    "(", ")", "[", "]", "{", "}", ",", ";", ":", "?"
};

typedef struct { Token *t; int n, cap; } TokBuf;

static void tb_push(TokBuf *b, Token tok) {
    if (b->n + 1 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->t = (Token *)realloc(b->t, b->cap * sizeof(Token));
        if (!b->t) { fprintf(stderr, "nova: out of memory\n"); exit(1); }
    }
    b->t[b->n++] = tok;
}

static bool is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool is_ident_char(char c)  { return isalnum((unsigned char)c) || c == '_'; }

/* ---------- string scanning ---------- */

static void spart_push(Interp *I, SPart **parts, int *n, int *cap, const char *s, size_t len, bool is_var) {
    if (*n + 1 > *cap) {
        *cap = *cap ? *cap * 2 : 4;
        SPart *p = (SPart *)arena_alloc(&I->mem, (size_t)(*cap) * sizeof(SPart));
        /* copy previous pieces */
        for (int i = 0; i < *n; i++) p[i] = (*parts)[i];
        *parts = p;
    }
    SPart *p = &(*parts)[(*n)++];
    p->is_var = is_var;
    p->text = arena_strndup(&I->mem, s, len);
}

static bool scan_string(Interp *I, const char *src, size_t len, size_t *i, int *line, int quote,
                        Token *out, NvDiag *diag) {
    size_t start = ++*i;                 /* skip opening quote */
    SPart *parts = NULL; int np = 0, cap = 0;
    size_t chunk = start;

    while (*i < len) {
        char c = src[*i];
        if (c == quote) {
            if (*i > chunk)
                spart_push(I, &parts, &np, &cap, src + chunk, *i - chunk, false);
            out->parts = parts; out->nparts = np;
            out->kind = TK_STR;
            *i = *i + 1;
            return true;
        }
        if (c == '\n') (*line)++;

        if (c == '\\' && *i + 1 < len) {
            if (*i > chunk)
                spart_push(I, &parts, &np, &cap, src + chunk, *i - chunk, false);
            char e = src[*i + 1];
            char decoded;
            switch (e) {
                case 'n': decoded = '\n'; break;
                case 't': decoded = '\t'; break;
                case 'r': decoded = '\r'; break;
                case '0': decoded = '\0'; break;
                case '\\': decoded = '\\'; break;
                case '"': decoded = '"'; break;
                case '$': decoded = '$'; break;
                case '\'': decoded = '\''; break;
                default:
                    /* unknown escape: keep the char as-is */
                    decoded = e;
                    break;
            }
            spart_push(I, &parts, &np, &cap, &decoded, 1, false);
            *i += 2;
            chunk = *i;
            continue;
        }

        if (quote == '"' && c == '$' && *i + 1 < len &&
            is_ident_start(src[*i + 1])) {
            if (*i > chunk)
                spart_push(I, &parts, &np, &cap, src + chunk, *i - chunk, false);
            size_t vs = ++*i;                    /* skip '$' */
            while (*i < len && is_ident_char(src[*i])) (*i)++;
            spart_push(I, &parts, &np, &cap, src + vs - 1, *i - vs + 1, true);
            chunk = *i;
            continue;
        }
        (*i)++;
    }
    nv_diag_fail(diag, *line, "unterminated string literal");
    return false;
}

/* ---------- main entry ---------- */

bool nv_lex_tokens(Interp *I, const char *src, const char *file,
                   Token **out, int *ntok, NvDiag *diag) {
    (void)file;
    TokBuf buf; memset(&buf, 0, sizeof(buf));
    size_t i = 0, len = strlen(src);
    int line = 1;

    while (i < len) {
        char c = src[i];

        if (c == '\n') { line++; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }

        /* comments */
        if ((c == '/' && i + 1 < len && src[i + 1] == '/') || c == '#') {
            while (i < len && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < len && src[i + 1] == '*') {
            int sline = line;
            i += 2;
            bool closed = false;
            while (i + 1 < len) {
                if (src[i] == '*' && src[i + 1] == '/') { closed = true; i += 2; break; }
                if (src[i] == '\n') line++;
                i++;
            }
            if (!closed) { nv_diag_fail(diag, sline, "unterminated block comment"); goto fail; }
            continue;
        }

        Token t; memset(&t, 0, sizeof(t));
        t.line = line;

        /* numbers */
        if (isdigit((unsigned char)c)) {
            if (c == '0' && i + 1 < len && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                i += 2;
                unsigned long long hv = 0;
                bool any = false;
                while (i < len && isxdigit((unsigned char)src[i])) {
                    int d = isdigit((unsigned char)src[i]) ? src[i] - '0'
                          : (tolower(src[i]) - 'a' + 10);
                    hv = hv * 16 + (unsigned long long)d;
                    i++; any = true;
                }
                if (!any) { nv_diag_fail(diag, line, "malformed hex literal"); goto fail; }
                t.kind = TK_INT; t.ival = (long long)hv;
                tb_push(&buf, t);
                continue;
            }
            bool is_float = false;
            size_t s0 = i;
            /* accumulate in unsigned (defined wrap) and detect values that
             * cannot fit in a 63-bit integer: those become floats instead of
             * overflowing (a signed overflow here was UB and silently
             * produced garbage numbers). */
            unsigned long long uv = 0;
            bool int_of = false;
            while (i < len && isdigit((unsigned char)src[i])) {
                if (uv > 0x7fffffffffffffffULL / 10) int_of = true;
                uv = uv * 10 + (unsigned long long)(src[i] - '0');
                i++;
            }
            if (i < len && src[i] == '.' && i + 1 < len && isdigit((unsigned char)src[i + 1])) {
                is_float = true;
                i++;
                while (i < len && isdigit((unsigned char)src[i])) i++;
            }
            if (i < len && (src[i] == 'e' || src[i] == 'E')) {
                size_t save = i;
                i++;
                if (i < len && (src[i] == '+' || src[i] == '-')) i++;
                if (i < len && isdigit((unsigned char)src[i])) {
                    is_float = true;
                    while (i < len && isdigit((unsigned char)src[i])) i++;
                } else {
                    i = save;   /* not an exponent */
                }
            }
            if (is_float || int_of) {
                t.kind = TK_FLOAT;
                t.fval = strtod(src + s0, NULL);
            } else {
                t.kind = TK_INT;
                t.ival = (long long)uv;
            }
            tb_push(&buf, t);
            continue;
        }

        /* identifiers / keywords */
        if (is_ident_start(c)) {
            size_t s0 = i;
            while (i < len && is_ident_char(src[i])) i++;
            const char *word = arena_strndup(&I->mem, src + s0, i - s0);
            bool is_kw = false;
            for (size_t k = 0; k < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); k++) {
                if (strcmp(KEYWORDS[k].word, word) == 0) {
                    t.kind = TK_KW; t.kw = KEYWORDS[k].kw; is_kw = true;
                    break;
                }
            }
            if (!is_kw) { t.kind = TK_IDENT; t.lex = word; }
            tb_push(&buf, t);
            continue;
        }

        /* variables: $name */
        if (c == '$') {
            if (i + 1 < len && is_ident_start(src[i + 1])) {
                size_t s0 = i;
                i++;
                while (i < len && is_ident_char(src[i])) i++;
                t.kind = TK_VAR;
                t.lex = arena_strndup(&I->mem, src + s0, i - s0);
                tb_push(&buf, t);
                continue;
            }
            nv_diag_fail(diag, line, "'$' must be followed by a variable name");
            goto fail;
        }

        /* strings */
        if (c == '"' || c == '\'') {
            if (!scan_string(I, src, len, &i, &line, c, &t, diag)) goto fail;
            tb_push(&buf, t);
            continue;
        }

        /* operators (longest match first) */
        {
            bool matched = false;
            for (size_t k = 0; k < sizeof(OPS) / sizeof(OPS[0]); k++) {
                size_t ol = strlen(OPS[k]);
                if (i + ol <= len && memcmp(src + i, OPS[k], ol) == 0) {
                    t.kind = TK_OP;
                    t.lex = OPS[k];
                    i += ol;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                nv_diag_fail(diag, line, "unexpected character '%c' (0x%02x)", isprint((unsigned char)c) ? c : '?', (unsigned char)c);
                goto fail;
            }
            tb_push(&buf, t);
            continue;
        }
    }

    {
        Token eof; memset(&eof, 0, sizeof(eof));
        eof.kind = TK_EOF; eof.line = line;
        tb_push(&buf, eof);
    }

    /* move buffer into the arena */
    size_t bytes = (size_t)buf.n * sizeof(Token);
    Token *toks = (Token *)arena_alloc(&I->mem, bytes ? bytes : 1);
    memcpy(toks, buf.t, bytes);
    free(buf.t);
    *out = toks;
    *ntok = buf.n;
    diag->ok = true;
    diag->eof = false;
    return true;

fail:
    free(buf.t);
    diag->ok = false;
    return false;
}

void nv_diag_ok(NvDiag *d) { d->ok = true; d->eof = false; }

void nv_diag_fail(NvDiag *d, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->msg, sizeof(d->msg), fmt, ap);
    va_end(ap);
    d->line = line;
    d->ok = false;
}
