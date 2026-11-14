# Nova

**Nova** is a lightweight, dynamically typed scripting language written from scratch in portable C11 — no third-party dependencies. Its design blends three traditions:

- **PHP**: `$variables`, `->` for members, `=>` in maps and `foreach`, `echo`, `import`, a rich built-in library
- **C**: braces, semicolons, classic operator set, fast native binary
- **Python**: dynamic values, closures, slices, negative indexing, `and/or`-style operand-returning logic, f-string-like interpolation

One file of C compiles to a single fast executable on **Linux, macOS and Windows**.

```
$ nova run examples/fizzbuzz.nv
1
2
Fizz
4
Buzz
...
```

---

## 1. Quick start

### Build (Linux / macOS / BSD)

```sh
make            # produces ./nova
make test       # runs the test suite
make asan-test  # runs the suite under AddressSanitizer + UBSan
```

### Build (Windows)

Use MinGW-w64 or Clang:

```bat
build_windows.bat
```

or manually:

```bat
gcc -O2 -std=c11 -o nova.exe src\value.c src\lexer.c src\parser.c src\interp.c src\builtins.c src\json.c src\pkg.c src\main.c -lm
```

MSVC also works: `cl /O2 /std:c11 /Fe:nova.exe src\value.c src\lexer.c src\parser.c src\interp.c src\builtins.c src\json.c src\pkg.c src\main.c`

### Run

```sh
nova                    # interactive REPL
nova run script.nv      # run a file (args after the file go to $argv)
nova script.nv          # same thing
nova -e 'echo "hi\n";'  # evaluate a snippet
nova pkg help           # package manager (init, install, publish, ...)
nova --version
```

The REPL supports multi-line input (it waits for balanced braces), Ctrl-D to
cancel an unfinished input, and prints the value of the last expression:

```
nova> $x = 21;
nova> $x * 2
= 42
```

---

## 2. The language

### 2.1 Variables and types

Variables start with `$` and are declared by assignment. Everything is
dynamically typed.

```nv
$x = 42;            // int (64-bit)
$y = 3.14159;       // float (double)
$s = "text";        // string (bytes)
$b = true;          // bool: true / false
$n = nil;           // the absence of a value
$a = [1, 2, 3];     // array
$m = ["k" => "v"];  // map (insertion-ordered dictionary)
```

`type($v)` returns one of: `"nil" "bool" "int" "float" "string" "array" "map" "fn" "class" "instance"`.

Arrays and maps are **reference values** — assigning them to another variable
shares the same object. All other values are copied by value.

### 2.2 Statements

Every simple statement ends with `;`. Blocks use braces. `if` accepts a
single statement without braces.

```nv
if ($x > 10) { echo "big\n"; }
elseif ($x > 5) echo "medium\n";
else { echo "small\n"; }

while ($i < 10) { $i++; }

for ($i = 0; $i < 10; $i++) { echo $i; }

foreach ($arr as $v) { ... }                  // arrays and strings
foreach ($map as $k => $v) { ... }            // maps (insertion order)
foreach ($arr as $i => $v) { ... }            // arrays with index

switch ($code) {                 // C/PHP-style switch
    case 200:
        echo "OK\n";
        break;
    case 301:
    case 302:                    // stacked labels share a body
        echo "redirect\n";
        break;
    default:
        echo "unknown\n";
}

break;  continue;  return $v;  throw $value;
```

`switch` evaluates the subject once and jumps to the first `case` label
that `==`-matches it (numbers, strings, any values). Like C, execution
**falls through** to the next body unless you end it with `break` — which
exits the switch, not the surrounding loop. `continue` and `return` inside
a switch behave as in a loop/function.

Comments: `// ...`, `# ...` and `/* ... */`.

### 2.3 Operators (highest to lowest precedence)

| Level | Operators | Notes |
|---|---|---|
| postfix | `() [] -> ++ --` | call, index/slice, member, increment |
| unary | `! - ++ --` | `-2 ** 2` is `-4` (Python rule) |
| power | `**` | right-associative |
| multiplicative | `* / %` | int `/` int truncates (C-style) |
| additive | `+ -` | `+` on strings is an error; use `.` |
| concat | `.` | string concatenation, PHP-style |
| relational | `< <= > >=` | numbers and strings |
| equality | `== != === !==` | `==` deep + numeric cross-type; `===` strict |
| logical | `&& \|\|` | return the operand (Python-style) |
| coalesce | `??` | `$m["k"] ?? "default"` also suppresses missing-key errors |
| ternary | `? :` | |
| assignment | `= += -= *= /= %= .=` | right-associative |

`$x++` / `$x--` work as prefix and postfix, including on array elements and
fields: `$a[-1]++`, `$obj->count++`.

### 2.4 Strings

Double-quoted strings interpolate `$variables` and support escapes
`\n \t \r \\ \" \' \$ \0`. Single-quoted strings are raw (only `\'` and `\\`).

```nv
$name = "Nova";
echo "Hello, $name!\n";
echo 'no interpolation here\n';   // prints literally
```

Strings are immutable and byte-based (UTF-8 safe for storage; `len()` counts
bytes). Indexing gives one-byte strings; `$s[0..3]` slices; negative indexes
count from the end.

### 2.5 Arrays and maps

```nv
$a = [1, 2, 3];
$a[] = 4;                 // append (PHP-style)
$a[0] = 10;               // set
$a[-1];                   // last element
$a[0..2];                 // slice, end exclusive
push($a, 5, 6); pop($a); shift($a); unshift($a, 0);

$m = ["a" => 1, "b" => 2];   // keys: strings and ints
$m["c"] = 3;
$m->d = 4;                    // -> works on maps too
del($m, "a");
keys($m); values($m); has($m, "b");
```

Maps preserve **insertion order** (like PHP arrays). Missing keys/indexes
raise catchable runtime errors — or return `nil` under `??`.

`[1, 2] + [3]` concatenates arrays; `["a"=>1] + ["b"=>2]` merges maps
(right side wins). `==` compares arrays and maps deeply.

### 2.6 Functions and closures

```nv
fn add($a, $b) { return $a + $b; }
add(2, 3);                        // 5

$twice = fn($x) { return $x * 2; };   // anonymous function
$twice(21);                            // 42

fn make_counter($start) {
    $n = $start;
    return fn($by) { $n = $n + $by; return $n; };   // closure
}
$c = make_counter(10);
$c(5);                            // 15
```

- Assignment updates the **nearest** binding in the scope chain, otherwise
  defines locally — so closures can mutate captured variables.
- Function names have no `$` (PHP-style). Calling with the wrong argument
  count is a catchable error.
- Recursion has a depth limit (512) that raises a catchable error.
- Closures created in a loop share the loop's environment (Python-style
  late binding).

### 2.7 Classes

```nv
class Shape {
    fn init($name) { $this->name = $name; }
    fn area() { return 0; }
    fn describe() { return $this->name . " area=" . $this->area(); }
}

class Circle extends Shape {
    fn init($r) {
        parent::init("circle");        // parent constructor
        $this->r = $r;
    }
    fn area() { return 3.14159265 * $this->r * $this->r; }
}

$c = new Circle(2);
echo $c->describe(), "\n";
echo is_a($c, "Shape") ? "yes" : "no", "\n";
```

- Fields come into existence on first `$this->field = ...`.
- Method lookup walks the inheritance chain; `parent::method(args)` starts
  from the parent of the class the current method was declared in.
- Instances are reference values and compare by identity.
- Classes are first-class values: `$cls = Circle; $obj = new $cls(2);` and
  modules export them (`new $mod->Class(...)`).

### 2.8 Error handling

```nv
try {
    throw "anything";               // or throw error("an Error object");
} catch ($e) {
    echo $e->message, "\n";         // Error objects carry message/line/trace
} finally {
    echo "always runs\n";
}
```

Runtime mistakes (undefined variable, bad index, type mismatch, division by
zero, failed builtin argument, call-depth limit, missing module) throw
catchable `Error` instances with `message`, `line` and `trace` fields.
Uncaught errors print a message and stack trace to stderr and exit with
code 1. `assert($cond, $msg?)` throws on falsey input.

### 2.9 Modules

```nv
import "mathx.nv";          // binds $mathx = map of top-level definitions
import "mathx.nv" as $m;    // custom name

$mathx->gcd(48, 18);
new $mathx->Acc();          // module classes
```

A module runs once (results are cached), in its own global scope, and
resolves paths relative to its own file. Every top-level `fn`, `class` and
`$variable` is exported. A module may instead end with `return <value>;`
(PHP include style) to control what it yields. Circular imports are
catchable errors.

`import "name"` (no path) also finds **packages**: first in
`./nova_modules/<name>/` (see section 3), then directly in the registry.
A subpath imports one file from a package: `import "strx/util.nv";`.

### 2.10 Files and I/O

```nv
write_file($path, "text\n");      // true/false
append_file($path, "more\n");
$content = read_file($path);      // string, or nil if missing
file_exists($path); delete_file($path); mkdir($path);
$line = read_line();              // one line from stdin, nil on EOF
```

### 2.11 Predefined variables

| Variable | Content |
|---|---|
| `$argv` | array: script path followed by command-line arguments |
| `$__dir__` | directory of the currently running file |

---

## 3. Packages and the registry

Nova ships with a small package manager. A **package** is a directory with
a `nova.json` manifest:

```json
{
  "name": "strx",
  "version": "1.0.0",
  "entry": "main.nv",
  "description": "String helpers: slug, title, words, shout",
  "dependencies": { "mathx": "^1.0.0" }
}
```

`entry` defaults to `main.nv`. The entry module's top-level definitions are
what the package exports — `import "strx";` gives you `$strx` with every
`fn`, `class` and `$variable` from it (or whatever its entry `return`s).

### 3.1 The registry

The registry is just a directory tree laid out as
`<registry>/<name>/<version>/`. Its location is resolved in this order:

1. the `--registry PATH` flag on any `nova pkg` command
2. the `NOVA_REGISTRY` environment variable
3. the `"registry"` key in `~/.nova/config` (set once with
   `nova pkg registry PATH`)
4. the default: `~/.nova/registry`

### 3.2 Commands

```sh
nova pkg init [dir]               # create nova.json (+ entry main.nv)
nova pkg publish [dir] [--force]  # copy a package into the registry
nova pkg install                  # install every dependency in nova.json
nova pkg install strx mathx@~1.2  # install specific packages
nova pkg remove strx              # uninstall + drop from nova.json
nova pkg list                     # project + installed packages
nova pkg search [query]           # search the registry by name/description
nova pkg registry [path]          # show / set the registry location
```

`nova pkg install` copies packages into `./nova_modules/<name>/` and pins
the resolved version in your project's `nova.json`.

### 3.3 Version constraints

| Constraint | Meaning |
|---|---|
| `*` (or none) | any version — always the newest |
| `^1.2.0` | same major, at least 1.2.0 |
| `~1.2.0` | same major.minor, at least 1.2.0 |
| `1.2.0` | exactly this version |

### 3.4 Typical workflow

```sh
nova pkg init mytool              # or write nova.json by hand
nova pkg publish ../strx          # publish a package you wrote
nova pkg install strx mathx       # fetch dependencies
nova run mytool.nv                # import "strx"; ... just works
```

Even without installing, `import "strx"` falls back to searching the
registry directly, so scripts keep running everywhere the registry is
reachable. Sample packages live in `examples/packages/` (`strx`, `mathx`)
and `examples/packages_demo.nv` shows them in use.

---

## 4. Built-in library (90 functions)

**Output** — `print` `println`

**Types** — `type str int float bool chr ord hex`
`is_nil is_bool is_int is_float is_num is_str is_array is_map is_fn is_class is_instance is_error is_a`

**Math** — `abs min max floor ceil round sqrt pow rand rand_int sum avg number_format`

**Arrays / maps** — `len push pop shift unshift insert remove slice keys values has del
index_of concat reverse sort unique join range map filter reduce`

**Strings** — `split upper lower trim ltrim rtrim starts_with ends_with contains
find replace substr pad_left pad_right repeat`

**Files** — `read_file write_file append_file file_exists delete_file mkdir`

**System** — `now clock_ms read_line get_env error exit assert json_encode json_decode`

**Memory** — `gc_collect gc_stats`

Highlights:

- `sort($arr, cmp?)` — stable merge sort; the optional comparator returns a
  number (`< 0` first) or bool, and can be a closure.
- `map filter reduce` take a closure as their last argument.
- `json_encode($v, pretty?)` serializes nil/bool/int/float/string/array/map;
  `json_decode($text)` parses JSON back into Nova values (objects become
  maps, integral numbers become ints).
- `range($a, $b, $step?)` is inclusive; step defaults to the right direction.

---

## 5. Semantics notes

- **Ints** are 64-bit and wrap on overflow (C behavior). `/` on two ints
  truncates; any float operand gives a float. `%` is int-only.
- **Floats** print compactly (`2.0` prints as `2`); use `number_format` for
  fixed decimals.
- **Truthiness**: `nil`, `false`, `0`, `0.0`, `""`, empty array and empty map
  are falsey; everything else (including `"0"`) is truthy.
- **Equality**: `==` compares numbers across int/float and structures deeply;
  `===` also requires the same type tag. Instances compare by identity.
- **Errors are values**: every runtime error is an `Error` instance, so one
  `try/catch` handles both `throw` and accidents.
- **Memory**: strings, arrays, maps, functions, classes, instances and
  environments are individually reclaimed by a tracing mark-sweep
  collector (see section 6). The parser keeps tokens and the AST in an
  arena that is released when the process exits.

---

## 6. Memory management and the garbage collector

Every runtime value — `NVValue` boxes, string buffers, arrays, hash
tables, environments, closures, classes and instances — is allocated on a
garbage-collected heap and carries a small header (magic, size, mark bit,
type tag). Nova uses **per-object reclamation**: a tracing **mark-sweep**
collector frees each unreachable object individually, so long programs
and long REPL sessions keep memory bounded instead of growing forever.

How it works:

- **Tracing, not counting.** Nova values are shared by pointer everywhere
  (assignment, arguments, containers and closures all alias the same
  object), so the collector marks the live graph from precise roots and
  sweeps everything else. Reference cycles are collected normally.
- **Safe points.** Collection happens only at *statement boundaries* —
  never mid-expression. Values that must survive across statements (loop
  iterables, call frames, callback accumulators, a value flying through a
  `finally` block, ...) are registered on an internal **pin stack**, so
  in-flight temporaries are always protected.
- **Free space reuse.** Freed blocks go into segregated size-class bins
  (16–512 B) plus a first-fit list for larger blocks; adjacent free
  blocks are coalesced on every sweep.
- **Policy.** The first collection runs after ~1 MiB of allocation churn;
  afterwards the trigger adapts to `live_bytes + 1 MiB`, keeping pause
  times proportional to the live set.

From Nova code:

```nova
gc_collect();                    // request a collection at the next
                                 // statement boundary
$s = gc_stats();                 // map: runs, live_bytes, live_objects,
                                 // total_alloc_bytes, total_freed_bytes,
                                 // last_freed_bytes, last_freed_objects
echo $s->live_bytes, "\n";
```

`gc_collect()` never collects *inside* an expression — collections are
only safe where the evaluator's temporaries are pinned, so the builtin
queues the request and the next statement boundary performs it. (The C
API `nv_gc_collect()` is immediate and used by the test suite.)

A reusable demo lives in `examples/gc_demo.nv`; the collector's torture
test is `tests/t15_gc_torture.nv`, which forces collections from inside
user functions called mid-expression and verifies every construct
survives intact.

---

## 7. Grammar sketch

```
program    := stmt*
stmt       := block | ";" | if | while | for | foreach | switch | return | break
            | continue | throw | try | import | echo | fn NAME params block
            | class NAME ("extends" NAME)? "{" fn* "}" | expr ";"
if         := "if" "(" expr ")" stmt ("elseif" ...)* ("else" stmt)?
foreach    := "foreach" "(" expr "as" $var ("=>" $var)? ")" stmt
switch     := "switch" "(" expr ")" "{" branch* "}"
branch     := ("case" expr ":" | "default" ":")+ stmt*
try        := "try" block ("catch" "(" $var ")" block)? ("finally" block)?
import     := "import" STRING ("as" $var)? ";"
echo       := "echo" expr ("," expr)* ";"
expr       := assign
assign     := ternary (("=" | "+=" | ...) assign)?
ternary    := nullish ("?" expr ":" ternary)?
nullish    := or ("??" nullish)?
...        := || && == != === !== < <= > >= . + - * / % ** unary postfix
postfix    := primary ("(" args ")" | "[" idx (".." idx)? "]"
            | "->" NAME | "++" | "--")*
primary    := INT | FLOAT | STRING | $var | "this" | "parent" "::" NAME
            | "new" (NAME | $var ("->" NAME)*) args | "fn" params block
            | "(" expr ")" | "[" elems "]" | "":" | pairs | "true" | "false" | "nil"
```

---

## 8. Project layout

```
nova/
  Makefile              build + test targets (Linux/macOS)
  build_windows.bat     Windows build helper
  src/
    nova.h              shared types and API
    value.c             values, hash tables, equality, display
    gc.c                mark-sweep heap: per-object reclamation
    lexer.c             tokenizer + string interpolation
    parser.c            recursive-descent parser (AST)
    interp.c            evaluator: closures, classes, exceptions, modules
    builtins.c          standard library (90 natives)
    json.c              minimal JSON parser (json_decode, manifests)
    pkg.c               package manager + package-aware import resolution
    main.c              CLI and REPL
  examples/             14 runnable example programs
    packages/           sample packages (strx, mathx)
  tests/                15 output-diff tests + package flow + runner
    lib/                module and config files used by the tests
    expected/           expected outputs
  nova_modules/         installed packages (created by nova pkg install)
```

### Testing

```sh
make test            # ./tests/run_tests.sh with the release build
make asan-test       # same suite under AddressSanitizer + UBSan
```

Each `tests/tNN_*.nv` prints deterministic output that is diffed against
`tests/expected/tNN_*.out`. `tests/pkg_test.sh` drives the full package
workflow (publish, install, constraints, import, remove) against a
throwaway registry; regenerate its golden file with
`PKG_WRITE_EXPECTED=1 sh tests/pkg_test.sh`.

---

## 9. Roadmap ideas

- Regex helpers, date/time formatting, a `net` package
- Bytecode compiler + VM for extra speed
- Optional type annotations with a `--strict` mode
