# Nova

**Nova** is a lightweight, dynamically typed scripting language written from scratch in portable C11 — no third-party dependencies. Its design blends three traditions:

- **PHP**: `$variables`, `->` for members, `=>` in maps and `foreach`, `echo`, `import`, a rich built-in library
- **C**: braces, semicolons, classic operator set, fast native binary
- **Python**: dynamic values, closures, slices, negative indexing, `and/or`-style operand-returning logic, f-string-like interpolation

One file of C compiles to a single fast executable on **Linux, macOS and Windows**, and Nova apps **host on cPanel, Plesk, DirectAdmin, CyberPanel, HestiaCP — any CGI-capable panel or server — as well as nginx/Apache VPS setups** (see [DEPLOY.md](DEPLOY.md)). Thirty-eight packages ship in `packages/`: a web micro-framework (webx), a template engine, form builder, validation, sessions, CSRF, flash messages, auth, a flat-file JSON database, pagination, a JSON-API helper, CORS, cache, logging, dotenv, random ids, htmx, Tailwind CSS, Bootstrap, Google Fonts, icons, a mailer, an XSS sanitizer, multipart uploads, a rate limiter, JWT tokens, UUIDs/nanoids, salted password hashing, stateless CAPTCHAs, date helpers, CSV, Markdown, i18n, an event emitter, layered config, RSS/Atom/sitemap feeds, plus the `strx`/`mathx` utilities.

Licensed under the [MIT License](LICENSE).

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
gcc -O2 -std=c11 -o nova.exe src\*.c -lm
```

MSVC also works: `cl /O2 /std:c11 /Fe:nova.exe src\*.c bcrypt.lib`

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
reachable. All bundled packages live in `packages/` and are pre-published
to the sample registry `.registry/`.

### 3.5 Bundled packages

| Package | Version | What it gives you |
|---|---|---|
| `webx` | 1.0.0 | Micro web framework: `run($routes)` dispatcher, response builders (`html`/`json`/`text`/`redirect`), request helpers (`param`/`header`/`cookie`), HTML `layout()`, `static_file()` with traversal protection, 404/405/500 pages |
| `tailwind` | 1.0.0 | Tailwind CSS: Play-CDN `cdn_link()`, `config()` generator, `css_link()` for compiled builds, `btn/card/badge/input_field/page` component snippets |
| `bootstrap` | 1.0.0 | Bootstrap 5.3: `css_link()/js_link()/links()`, `btn/alert/card/badge/navbar/table/row/col` component builders |
| `fonts` | 1.0.0 | Google Fonts: `link()/link_many()` (preconnect + css2 API), presets (`inter()`, `roboto()`, `jetbrains()`…), CSS `stack("system")` values |
| `icons` | 1.0.0 | Font Awesome CDN helper `fa_link()/fa()` **plus 24 offline inline SVG icons** (`svg("mail", 18)` — stroke style, colored via CSS) |
| `mailer` | 1.0.0 | E-mail: `send()` with `dry_run` preview, sendmail/SMTP transports, `html_email()` responsive templates, `validate()`, `template("Hi {{name}}")`, `welcome()` |
| `templates` | 1.0.0 | HTML template engine: `render()` with `{{var|e}}` filters, `{{#if}}/{{else}}`, `{{#each}}` with `{{@index}}`, `render_file()`, `layout()` |
| `validate` | 1.0.0 | Rule-based input validation: `check()` with required/email/int/min/max/in/same/url/slug rules, custom messages, `fails()/passes()/first()` |
| `forms` | 1.0.0 | HTML form builder: `open/close`, `text/email/password/textarea/select/checkbox`, Bootstrap error states (`error_cls/err/errors_summary`), `old()` input, CSRF `token()` field |
| `session` | 1.0.0 | Cookie + JSON-file sessions: `start/put/get/flush/destroy`, `finish()` attaches the Set-Cookie header, expiry, index-based `gc()` for cron |
| `csrf` | 1.0.0 | CSRF protection: per-session `token()`, `hidden()` input, `meta()` tag for JS, constant-shape `check()/verify()`, `rotate()` |
| `flash` | 1.0.0 | One-shot messages riding the session: `put()/get()/render()` as Bootstrap alerts, `peek()/pending()` |
| `auth` | 1.0.0 | User accounts on a JSON file store: `register/login/attempt/logout`, salted iterated password hashes, `user()/check()` |
| `jdb` | 1.0.0 | Flat-file JSON database: `open/insert/update/delete`, `row/first/where/query`, `order/limit`, auto ids, write-through persistence — no MySQL needed on cPanel |
| `paginate` | 1.0.0 | Pagination math: `paginate()` meta (pages/from/to/has_prev), sliding `window()`, Bootstrap `links()` nav bar, `slice_page()` |
| `api` | 1.0.0 | JSON API envelopes: `ok/created/fail/bad_request/unauthorized/unprocessable`, `input()` (JSON-or-form), `bearer()` tokens, `list()` with pagination meta |
| `cors` | 1.0.0 | CORS for CGI APIs: `headers()/attach()` (echoes allowed origins, credentials-aware), `preflight()` 204 responses, `allowed()` |
| `cache` | 1.0.0 | File cache with TTLs: `put/get/exists/forget/flush`, `remember(key, ttl, fn)`, index-based `gc()` for cron |
| `logger` | 1.0.0 | Leveled file logging: `debug/info/warn/error` with JSON context, `iso()` UTC timestamps (pure integer date math), `request()` lines, `tail()` |
| `dotenv` | 1.0.0 | `.env` files: `load()/parse()` (quotes, comments, export prefix), `get()` with environment fallback, `require()`, `dump()` |
| `ids` | 1.0.0 | Random identifiers: `uuid()` (v4 layout), `token()/hex()` API secrets, `otp()` codes, `short()` invite codes, `pick()` |
| `htmx` | 1.0.0 | htmx integration: CDN `script()`, `hx-get/post/put/patch/delete` attribute builders, `poll()`, `boost()`, ready-made `btn/form/loader` |
| `strx` | 1.0.0 | String helpers: `slug`, `title`, `words`, `shout` |
| `mathx` | 1.2.0 | Math helpers: `mean`, `median`, `variance`, `clamp`, `lerp` |
| `sanitize` | 1.0.0 | XSS defense: `field()`/`deep()` HTML escaping, `strip_tags()` (drops script bodies), `url()` scheme allow-list, `js()` escaping, traversal-proof `filename()`, `visible()` control-char cleaner |
| `uploads` | 1.0.0 | multipart/form-data: `parse()` into `{name, filename, type, data}` parts (binary-safe), `part()` lookup, `save()` with sanitized names, size caps and no-clobber suffixes |
| `ratelimit` | 1.0.0 | Fixed-window rate limiting that survives CGI exits (file-backed): `allow(key, limit, windowMs)`, `remaining()` for RateLimit headers |
| `jwt` | 1.0.0 | HS256 JSON Web Tokens: `encode(payload, secret, ttl)` with iat/exp claims, `decode()` with constant-time MAC check and alg=none rejection, `valid()` |
| `uuid` | 1.0.0 | CSPRNG identifiers: RFC-4122 `v4()`, URL-safe `short()`, bias-free `nanoid()` alphabets, `pin()`, `token_hex()` |
| `password` | 1.0.0 | Salted SHA-256 password hashing (`hash()`/`verify()` constant-time, versioned `s1$` format), `strength()` 0-4 meter |
| `captcha` | 1.0.0 | Stateless math CAPTCHAs: `make(secret, ttl)` -> `{question, token}` (HMAC-signed answer), `verify()` with expiry, nothing stored server-side |
| `datetime` | 1.0.0 | UTC date math on unix timestamps: `iso()`, `nice()`, `ago()`, calendar `parts()`, `add_days/hours/minutes`, `is_leap()`, `days_in_month()`, `start_of_day()` |
| `csv` | 1.0.0 | RFC 4180 CSV: `parse()` with quoting/escaped quotes/newlines-in-fields, `build()` with automatic escaping, `parse_dicts()` header-mapped rows |
| `markdown` | 1.0.0 | Mini Markdown -> HTML, XSS-safe (escapes before decorating): headings, bold/italic, inline + fenced code, links with scheme allow-list, lists, quotes, rules |
| `i18n` | 1.0.0 | Translations: `load(locale, strings)`, `locale()`, `t("key", vars)` with `{var}` substitution, `one|many` pluralization, en fallback |
| `events` | 1.0.0 | In-process pub/sub: `on/once/emit/off/fire`, handlers receive an args array, `emit` returns the number fired |
| `config` | 1.0.0 | Layered config (defaults < .env < runtime `set`): `get()`/`get_or()`, case-insensitive keys, `source()` layer probe, `public()` masks secret-ish keys |

Install them all into an app with:

```sh
NOVA_REGISTRY=./.registry nova pkg install webx templates forms validate session csrf flash auth jdb paginate api cors cache logger dotenv ids htmx tailwind bootstrap fonts icons mailer sanitize uploads ratelimit jwt uuid password captcha datetime csv markdown i18n events config feeds
```

`examples/web_demo.nv` combines webx + tailwind + bootstrap + fonts + icons
into a routed CGI application (also previewable from the CLI),
`examples/mail_demo.nv` shows the mailer, and `examples/webapp_demo.nv` runs
a complete blog (jdb posts, login + sessions + CSRF + flash, templates,
pagination) as a scripted user journey — the same handlers that serve HTTP
under CGI.

---

## 4. Built-in library (105 functions)

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

**Web / CGI** — `http_request http_respond http_parse_query url_encode url_decode html_escape`

**Mail** — `mail_send smtp_send mail_last_error`

Highlights:

- `sort($arr, cmp?)` — stable merge sort; the optional comparator returns a
  number (`< 0` first) or bool, and can be a closure.
- `map filter reduce` take a closure as their last argument.
- `json_encode($v, pretty?)` serializes nil/bool/int/float/string/array/map;
  `json_decode($text)` parses JSON back into Nova values (objects become
  maps, integral numbers become ints).
- `range($a, $b, $step?)` is inclusive; step defaults to the right direction.

### 4.1 Web hosting builtins (CGI)

Nova apps run under CGI on cPanel, Plesk, DirectAdmin, CyberPanel,
HestiaCP, Apache or nginx+fcgiwrap — see **section 5** and [DEPLOY.md](DEPLOY.md).

```nova
$req = http_request();
// $req->method "POST", $req->path "/hello", $req->query "name=Ada"
// $req->form {name => "Ada"}      urlencoded query + body, merged
// $req->headers {user_agent => "...", content_type => "..."}
// $req->cookies {sid => "..."}
// $req->json  parsed JSON body when Content-Type says so
// $req->cgi   true under a real web server; CLI previews simulate GET /

http_respond(200, "<h1>Hi</h1>", ["Content-Type" => "text/html; charset=utf-8"]);
```

Outside CGI (`nova run demo.nv`), `http_request()` returns a simulated
`GET /` so examples stay runnable and testable from the terminal.

### 4.2 Mail builtins

```nova
mail_send({from: "site@yourdomain.com", to: "user@example.com",
           subject: "Welcome", text: "Hello!"});       // local sendmail

smtp_send({host: "mx.yourdomain.com", port: 25,
           from: "site@yourdomain.com", to: "user@example.com",
           subject: "Welcome", html: "<b>Hello!</b>"}); // direct SMTP

mail_last_error();   // why the last send returned false
```

Both take one options map (`to`/`cc`/`bcc` accept a string or an array,
`text`+`html` become a multipart/alternative message). `mail_send()` uses
the server's sendmail binary — the same way PHP `mail()` works on cPanel.
`smtp_send()` is plain TCP SMTP (no TLS — point it at a local smarthost or
port 25; TLS would require a third-party crypto library, which Nova refuses
to ship). The `mailer` package wraps both with validation, templates and a
`dry_run` preview mode.

### 4.3 Crypto builtins (new in 1.1.0)

Real, self-contained primitives (FIPS 180-4 SHA-256, RFC 2104 HMAC,
RFC 4648 base64, OS CSPRNG) — the engine behind the `jwt`, `password`,
`uuid` and `captcha` packages:

```nova
sha256("hello");                 // "2cf24dba…b9824"  (64 hex chars)
hmac_sha256("key", "message");   // "203d1e5c…"        (64 hex chars)
base64_encode("Nova rocks");     // "Tm92YSByb2Nrcw=="
base64_decode("Tm92YSByb2Nrcw==");
random_bytes(16);                // 16 cryptographically random bytes
constant_time_equals($a, $b);    // secret comparison without timing leaks
```

`random_bytes()` reads `/dev/urandom` (BCryptGenRandom on Windows) — never
`rand()`. `sha256`/`hmac_sha256` outputs were verified against Python's
`hashlib`/`hmac` as part of the security audit.

---

## 5. Hosting Nova applications (cPanel, panels, VPS)

A Nova web app is one executable + your `.nv` files. The CGI interface is
implemented by `src/web.c`, so hosting needs **no plugins, daemons or
root** — the full walkthroughs for cPanel, Plesk/DirectAdmin/CyberPanel,
nginx+fcgiwrap, Apache and systemd live in **[DEPLOY.md](DEPLOY.md)**, and
a ready-made CGI wrapper plus hardened `.htaccess` ship in `deploy/`:

```
browser ──> Apache/nginx (any panel) ──> cgi-bin/nova.cgi ──> nova run app.nv
                                                 │
                       http_request() / http_respond()
```

Quick start on cPanel: upload `nova` (chmod 755) + your app, copy
`deploy/nova.cgi` into `public_html/cgi-bin/`, copy `deploy/.htaccess`
into `public_html/` (pretty URLs + blocks `.nv`/`nova_modules/` access),
visit `https://yourdomain.com/cgi-bin/nova.cgi`.

### 5.1 Hardening and penetration testing

Nova is tested like a web server, because under CGI it is one. The repo
ships its own bug-bounty harness — PoC regressions for every historical
finding, a mutation fuzzer, and an SMTP honeypot — all run under
AddressSanitizer + UBSan:

```sh
make debug                            # ASan + UBSan build
sh tests/security/run_pentest.sh      # 14 checks, exit 0 = clean
python3 tests/security/fuzz.py --nova ./nova_debug --count 2000
```

The hardening this produced (response-splitting, SMTP/e-mail header
injection, package-manager traversals, parser stack exhaustion, overflow
UB — all fixed and regression-tested) is documented finding-by-finding in
**[SECURITY.md](SECURITY.md)**.

---

## 6. Semantics notes

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

## 7. Memory management and the garbage collector

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

## 8. Grammar sketch

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

## 9. Project layout

```
nova/
  Makefile              build + test + static targets (Linux/macOS)
  build_windows.bat     Windows build helper
  LICENSE               MIT license
  DEPLOY.md             hosting guide: cPanel, panels, nginx, Apache
  nova.json             project manifest used by the package demos
  src/
    nova.h              shared types and API
    value.c             values, hash tables, equality, display
    gc.c                mark-sweep heap: per-object reclamation
    lexer.c             tokenizer + string interpolation
    parser.c            recursive-descent parser (AST)
    interp.c            evaluator: closures, classes, exceptions, modules
    builtins.c          standard library (96 natives)
    web.c               CGI/HTTP hosting builtins (cPanel & friends)
    crypto.c            sha256 / hmac_sha256 / base64 / random_bytes primitives
    mail.c              sendmail + SMTP builtins
    json.c              minimal JSON parser (json_decode, manifests)
    pkg.c               package manager + package-aware import resolution
    main.c              CLI and REPL
  packages/             the 38 bundled packages (sources; published into
                        .registry, see section 3)
  .registry/            sample registry pre-published with all packages
  examples/             16 runnable example programs incl. web_demo,
                        mail_demo, packages_demo (+ static assets)
  deploy/               nova.cgi wrapper, hardened .htaccess, nginx example
  tests/                output-diff tests + package flow + runner
    security/           pentest suite: PoC regressions, fuzzer, SMTP honeypot
  SECURITY.md           security policy + full pentest findings report
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

## 10. Roadmap ideas

- Regex helpers, date/time formatting, a `net` package
- Bytecode compiler + VM for extra speed
- Optional type annotations with a `--strict` mode
