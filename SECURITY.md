# Security

Nova ships with an in-repo **bug-bounty / penetration-testing program**: the
interpreter's own attack surface is tested by [`tests/security/run_pentest.sh`](tests/security/run_pentest.sh)
on every release, under AddressSanitizer + UBSan. This page documents the
methodology, every finding from the latest audit round, and how to report
what you find.

```sh
make debug                            # ASan + UBSan build (./nova_debug)
sh tests/security/run_pentest.sh      # full pentest suite, exit 0 = clean
python3 tests/security/fuzz.py --nova ./nova_debug --count 2000
```

---

## 1. Scope and threat model

| Surface | Attacker | Examples |
| --- | --- | --- |
| **CGI hosting** (cPanel & panels) | any HTTP visitor | query strings, form bodies, cookies, absurd `CONTENT_LENGTH`, header injection |
| **Package system** | a cloned repo's `nova.json`, a published package, an `import "pkg/sub.nv"` spec | traversal names, traversal versions, traversal entries |
| **Mail builtins** | form input relayed into `mail_send` / `smtp_send` | CRLF in subject/addresses |
| **Interpreter core** | whoever can feed source code (`-e`, files, fuzzing) | malformed syntax, deep nesting, huge literals, hostile JSON |
| **Packages** | end users of an app | XSS via templates, unsafe uploads, weak session tokens |

Out of scope / documented limitations (see section 5).

## 2. Methodology

1. **Recon** — read every C source with the CGI/package/mail attack model in
   mind (`src/web.c`, `src/mail.c`, `src/pkg.c`, `src/lexer.c`, `src/parser.c`,
   `src/json.c`, `src/interp.c`, `src/builtins.c`, `src/crypto.c`).
2. **PoC first** — every suspicion was turned into a runnable exploit
   **before** fixing, against an ASan/UBSan build; each PoC became a
   permanent regression test in the pentest suite.
3. **Fuzzing** — mutation fuzzer over Nova sources (structural bombs: deep
   nesting, 40-digit literals, NUL/invalid-UTF-8 injection, truncated
   strings) and JSON documents, under ASan; 400 inputs per run, seed pinned
   for reproducibility.
4. **Live protocol testing** — a local SMTP honeypot (`tests/security/fake_smtp.py`)
   captured the raw wire to prove injected commands do/don't reach it; CGI
   responses captured with forged environments.
5. **Static analysis** — `gcc -fanalyzer` over all sources (clean at 0
   warnings) plus UBSan as a runtime oracle for signed overflow / UB.

## 3. Findings and remediation (audit round 1, Nov 2026)

All issues below were found, exploited, fixed, and are now regression-tested.

| ID | Severity | Area | Finding | Status |
| --- | --- | --- | --- | --- |
| NV-01 | **Critical** | parser | **Stack exhaustion via nested expressions** — `((((((…` (300k parens) segfaults the process; in CGI hosting a single crafted source = remote DoS. Same crash via unary chains `----------1`, `??` chains, and block nesting. | **Fixed** — recursion guard `NOVA_PARSE_MAX_DEPTH` (400) on every recursive entry point; clean "nesting too deep" syntax error. |
| NV-02 | **High** | web.c | **HTTP response splitting** — `http_respond(302, "", {"Location": $user})` with `$user = "a\r\nSet-Cookie: pwned=1"` emitted forged headers (cookie forcing, cache poisoning). | **Fixed** — header names and values are CR/LF/NUL-stripped before printing. |
| NV-03 | **High** | mail.c | **SMTP command injection** — CRLF in `from`/`to`/`cc`/`bcc` injected `RCPT TO:<victim>` commands on the wire (proven against a live honeypot); the sendmail path allowed e-mail header injection (`Bcc:` forging via subject). | **Fixed** — addresses containing CR/LF are rejected with an error; all header fields (From/To/Cc/Bcc/Subject/Reply-To/custom) are sanitized. |
| NV-04 | **High** | pkg.c | **Registry escape via `nova pkg publish`** — a manifest with `"version": "../../escaped"` wrote the package tree **outside the registry directory** (arbitrary-write primitive relative to the registry root). | **Fixed** — versions must match `[A-Za-z0-9._+-]+` and cannot start with `.`/`-`. |
| NV-05 | **High** | pkg.c | **Import sub-path traversal** — `import "pkg/../../../outside/steal.nv"` executed code outside the package/registry root (proven: leaked a file outside the registry). | **Fixed** — import sub-paths are validated (`safe_rel_path`: no `..`, no absolute, no backslash). |
| NV-06 | Medium | pkg.c | **Install target traversal** — `nova.json` dependency names were not validated, so `"dependencies": {"../../outside/pwned": "*"}` made `nova pkg install` build a write path outside the project. | **Fixed** — dependency names validated at manifest parse *and* again in `install_one` (defense in depth). |
| NV-07 | Medium | pkg.c | **Manifest entry traversal** — a package's `"entry": "../../../evil.nv"` pointed imports outside the package root. | **Fixed** — `entry` must be a clean relative path. |
| NV-08 | Low | lexer.c | **Signed integer overflow** on huge int literals (`999…9`, 26 digits) — UB, silently produced garbage numbers. | **Fixed** — literals that cannot fit int64 become floats; UBSan-clean. |
| NV-09 | Low | web.c | **`CONTENT_LENGTH` overflow** — absurd CGI lengths hit signed overflow (UB) before clamping. | **Fixed** — saturating unsigned parse capped at the 32 MiB body limit. |
| NV-10 | Low | pkg.c | **Version-segment overflow** in the version comparator (`99999999999999999999.0` vs `8888…`) — UB. | **Fixed** — saturating numeric comparison. |
| NV-11 | Low | lexer.c | **Quadratic blowup** — `scan_string` called `strlen(src)` per character (O(n²) CPU DoS on giant string literals). | **Fixed** — length hoisted, linear scan. |
| NV-12 | Low | builtins.c | **NUL-truncating string ops** — `split`/`replace`/`index_of`/`contains`/`find` used `strstr`, so binary payloads (multipart uploads with NUL bytes) were silently cut at the first NUL. | **Fixed** — length-aware `sfind` everywhere; binary-safe string ops. |
| NV-13 | Info | mail.c | SMTP transport has no TLS; `AUTH LOGIN` credentials ride base64 (= plaintext) unless confined to localhost/a smarthost. | **Documented** — use a local sendmail relay on panels; `smtp_send` is for trusted networks. |

**Verified fixed behaviours** (all covered by `tests/security/run_pentest.sh`):

- 300k-deep parens/unary/`??`/blocks → `expression nesting too deep (max 400)`, exit code 1, no crash.
- Injected `Set-Cookie`/`X-Pwned` no longer start their own header line.
- SMTP honeypot logs show the connection refused before `MAIL FROM`; no injected `RCPT TO`.
- Traversal imports/publishes/installs fail with explicit error messages; no file leaves its root.
- 200 mutated sources + 200 mutated JSON documents: 0 crashes, 0 hangs under ASan.
- `gcc -fanalyzer`: 0 warnings across all sources.

## 4. Defense measures that were already in place

- Runtime call-depth cap (`NOVA_MAX_DEPTH`, 512) stops runaway Nova recursion.
- JSON parser is depth-limited (64) and uses `errno`-checked `strtoll`.
- Circular imports are detected (`circular import of "…"`).
- CGI body capped at 32 MiB; `random_bytes()` reads `/dev/urandom`
  (BCryptGenRandom on Windows) — `rand()` is never used for tokens.
- Crypto primitives (`sha256`, `hmac_sha256`, `base64_*`,
  `constant_time_equals`) are self-contained, reference-verified against
  Python's `hashlib`/`hmac`, and free of secret-dependent branches.

## 5. Secure-use notes for applications

- Always pass user input through the **`sanitize`** package (`field`, `deep`,
  `url`, `js`, `strip_tags`) before it reaches a template; the **`markdown`**
  package escapes *before* decorating and is safe by construction.
- Never write uploads with raw filenames — the **`uploads`** package
  sanitizes names, refuses size overruns and never leaves the target dir.
- Prefer `password->hash/verify` (salted, constant-time) over home-grown
  hashing; compare secrets only via `constant_time_equals`.
- `smtp_send` without TLS is for trusted networks only (NV-13); on cPanel
  use `mail_send` (local sendmail).
- Set a `NOVA_REGISTRY` you control; treat third-party packages as code you
  run with your own privileges — pin versions in `nova.json`.

## 6. Reporting a vulnerability

Open a GitHub issue titled `[security] …` **or** a pull request with a
regression test. Please include a minimal PoC (a `.nv` file, a command line,
or a registry layout) — the suite's PoC-first format is the template. Fixes
land with a new row in section 3, credited unless you prefer otherwise.
