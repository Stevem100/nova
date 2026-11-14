/*
 * main.c — Nova command-line interface and interactive REPL.
 *
 * Usage:
 *   nova                 start the interactive REPL
 *   nova run file.nv     run a script (or just: nova file.nv)
 *   nova -e "code"       evaluate a code snippet
 *   nova pkg <cmd>       package manager (install, publish, search, ...)
 *   nova --version       print version
 *   nova --help          print usage
 */
#include "nova.h"

#define REPL_MAX_LINE 4096

static void print_usage(void) {
    printf(
        "Nova %s — a lightweight scripting language (PHP + C + Python blend)\n"
        "\n"
        "Usage:\n"
        "  nova                    start the interactive REPL\n"
        "  nova run <file.nv>      run a script (args after the file go to $argv)\n"
        "  nova <file.nv>          same as 'nova run'\n"
        "  nova -e \"<code>\"        evaluate a code snippet\n"
        "  nova pkg <command>      package manager: init install remove list search publish registry\n"
        "  nova --version          print the version\n"
        "  nova --help             print this help\n"
        "\n"
        "REPL commands: :quit  :exit  :help\n",
        NOVA_VERSION);
}

static int run_repl(void) {
    Interp I;
    interp_init(&I);
    nv_register_builtins(&I);
    I.repl = true;
    I.globals = env_new(&I, I.builtins_env);

    printf("Nova %s — interactive shell\n", NOVA_VERSION);
    printf("PHP-style syntax, C-style braces, Python-style dynamics.\n");
    printf("Type :help for help, :quit to exit.\n\n");

    char line[REPL_MAX_LINE];
    SB acc;
    sb_init(&acc);

    for (;;) {
        printf("%s", acc.n == 0 ? "nova> " : "....> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            if (acc.n > 0) {          /* Ctrl-D cancels a multi-line input */
                sb_free(&acc);
                sb_init(&acc);
                printf("\n");
                continue;
            }
            printf("\n");
            break;
        }

        /* REPL commands (only at the start of a fresh input) */
        if (acc.n == 0) {
            char *t = line;
            while (*t == ' ' || *t == '\t') t++;
            if (strcmp(t, ":quit\n") == 0 || strcmp(t, ":exit\n") == 0 ||
                strcmp(t, ":q\n") == 0 || strcmp(t, "quit\n") == 0 ||
                strcmp(t, "exit\n") == 0) break;
            if (strcmp(t, ":help\n") == 0 || strcmp(t, "help\n") == 0) {
                printf(
                    "Nova REPL help\n"
                    "  Variables use $:      $x = 42;\n"
                    "  Statements end in ;   echo \"hi\";\n"
                    "  Blocks use braces:    if ($x > 3) { ... }\n"
                    "  Define functions:     fn add($a, $b) { return $a + $b; }\n"
                    "  Multi-line input:     keep typing; the shell waits for balanced braces\n"
                    "  Cancel input:         press Ctrl-D\n"
                    "  Commands:             :quit :help\n");
                continue;
            }
            if (strcmp(t, "\n") == 0 || strcmp(t, "\r\n") == 0) continue;
        }

        sb_puts(&acc, line);

        NvDiag diag;
        Node *prog;
        bool compiled = nv_compile(&I, acc.p, "<repl>", &prog, &diag);
        if (!compiled && !diag.eof) {
            /* REPL convenience: retry with a trailing ';' */
            sb_putc(&acc, ';');
            compiled = nv_compile(&I, acc.p, "<repl>", &prog, &diag);
            if (!compiled) {
                acc.n--;                 /* drop the ';' again */
                acc.p[acc.n] = '\0';
            }
        }
        if (!compiled) {
            if (diag.eof) continue;   /* truncated: wait for more input */
            fprintf(stderr, "syntax error at line %d: %s\n", diag.line, diag.msg);
            sb_free(&acc);
            sb_init(&acc);
            continue;
        }

        /* execute in the persistent global env */
        I.last_expr = NULL;
        Ctrl r = nv_exec_nodes(&I, I.globals, prog->kids, prog->nkids);
        if (r.s == SIG_THROW) {
            nv_report_uncaught(&I, r.v);
        } else if (r.s == SIG_RETURN) {
            NVValue *v = r.v ? r.v : nv_nil();
            NVValue *s = nv_to_string(&I, v);
            printf("=> %.*s\n", (int)s->slen, s->as.s);
        } else if (r.s == SIG_BREAK || r.s == SIG_CONTINUE) {
            fprintf(stderr, "nova: 'break' or 'continue' used outside of a loop\n");
        } else if (I.last_expr && I.last_expr->type != T_NIL) {
            NVValue *s = nv_to_string(&I, I.last_expr);
            printf("= %.*s\n", (int)s->slen, s->as.s);
        }

        sb_free(&acc);
        sb_init(&acc);
        fflush(stdout);
    }

    sb_free(&acc);
    interp_free(&I);
    return 0;
}

static int eval_snippet(const char *code) {
    Interp I;
    interp_init(&I);
    nv_register_builtins(&I);
    I.globals = env_new(&I, I.builtins_env);

    /* $argv for -e mode */
    NVValue *argvv = nv_arr_new(&I);
    nv_arr_push(&I, argvv, nv_str(&I, "-e"));

    Node *prog;
    NvDiag diag;
    if (!nv_compile(&I, code, "<-e>", &prog, &diag)) {
        fprintf(stderr, "nova: syntax error at line %d: %s\n", diag.line, diag.msg);
        interp_free(&I);
        return 1;
    }
    Ctrl r = nv_exec_nodes(&I, I.globals, prog->kids, prog->nkids);
    int code_out = 0;
    if (r.s == SIG_THROW) {
        nv_report_uncaught(&I, r.v);
        code_out = 1;
    } else if (r.s == SIG_RETURN && r.v && r.v->type == T_INT) {
        code_out = (int)r.v->as.i;
    }
    interp_free(&I);
    return code_out;
}

int main(int argc, char **argv) {
    if (argc < 2) return run_repl();

    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("Nova %s\n", NOVA_VERSION);
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage();
        return 0;
    }
    if (strcmp(argv[1], "-e") == 0) {
        if (argc < 3) {
            fprintf(stderr, "nova: -e requires a code argument\n");
            return 2;
        }
        return eval_snippet(argv[2]);
    }
    if (strcmp(argv[1], "pkg") == 0)
        return nv_pkg_main(argc - 1, argv + 1);

    const char *path = argv[1];
    int script_argc = argc - 2;
    char **script_argv = argv + 2;
    if (strcmp(path, "run") == 0) {
        if (argc < 3) {
            fprintf(stderr, "nova: 'run' requires a file name\n");
            return 2;
        }
        path = argv[2];
        script_argc = argc - 3;
        script_argv = argv + 3;
    }

    Interp I;
    interp_init(&I);
    nv_register_builtins(&I);
    int exit_code = 0;
    bool ok = nv_run_file(&I, path, script_argc, script_argv, &exit_code);
    fflush(stdout);
    interp_free(&I);
    return ok ? exit_code : (exit_code ? exit_code : 1);
}
