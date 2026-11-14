# Nova roadmap

Living document; the GitHub issue tracker is where each item is discussed.
Shipped items link the release that delivered them.

## Shipped — v1.2.0 (2026-11)

- **Outbound TCP + `http_client`** — `net_connect/net_send/net_recv/
  net_recv_line/net_close` builtins (bounded: 64 sockets, 10 s connect and
  15 s I/O timeouts, 64 KiB per recv) plus the zero-dependency HTTP/1.1
  client package (GET/POST/PUT/DELETE, JSON bodies, chunked decoding,
  opt-in redirects, response caps). Transport is exercised against a
  loopback server in the pentest suite; the pure parts in `t24`.
- **Package round 3** — `http_client`, `image` (PPM P3/P6 + 24-bit BMP,
  resize/thumbnail, pure Nova over `chr()`/`ord()`), `view_components`
  (composable HTML fragments over `templates` conventions), `logrotate`
  (size generations or daily files, keep-N pruning, no directory listing
  needed). Registry now ships 44 packages, indexed with per-version
  SHA-256 in `.registry/registry.json`.
- **GC cycle reclamation, proven** — the v1.1.0 mark-sweep collector
  already frees unreachable cycles; `tests/t23_gc_cycles.nv` demonstrates
  it and the README documents the design (the old "refcounting, cycles
  leak" text described the pre-1.1 engine).
- **TLS decision (issue #2)** — closed as "scope statement": Nova stays
  zero-dependency and ships no TLS stack; `smtp_send` is localhost/smarthost
  only, panels use `mail_send` (server MTA handles TLS), and DEPLOY.md §8
  documents the stunnel/msmtp wrapper-interop patterns for outbound HTTPS.
- **CI matrix** — Linux (build + 25-test suite + ASan/UBSan pentest with
  fuzzing), macOS (build + suite), Windows (UCRT64 build + smoke) on every
  push/PR; green on all three.
- **Registry index + checksums** — `nova pkg index` writes
  `.registry/registry.json` with a canonical SHA-256 per package-version;
  `nova pkg install` verifies against it and refuses tampered trees.
- **Docs** — end-to-end cPanel tutorial (`docs/tutorial-cpanel.md`),
  network builtins section in the README.

## Candidates for v1.3 (unpicked, in rough priority order)

1. **Performance baseline, then a bytecode VM** — add `bench/` with
   arithmetic/string/collection/OOP/JSON micro-benchmarks and a
   `bench.sh` runner first; only then decide between a faster tree-walker
   (direct threading, opcode caching) and a full compiler + VM.
2. **admin-scaffold** — CRUD table generator for `webx` + `jdb` apps
   (the last big unclaimed item from the package wishlist).
3. **stdlib gaps surfaced by real apps** — richer date *parsing* (ISO-8601
   in), glob/list-dir primitives (would simplify `logrotate`), a decision
   on regex (keep string ops only vs. a small backtracking engine).
4. **oauth-client** — still blocked on TLS; the stunnel pattern in
   DEPLOY.md §8 is the documented workaround meanwhile.
5. **More examples** — a full mini-blog under `examples/` using webx + jdb
   + templates + queue (the current `webapp_demo.nv` covers most of this
   already; the gap is a packaged, deployable tree).
6. **Hosted registry index** — serve `registry.json` over HTTP so
   `nova pkg install` can resolve dependencies remotely (needs the TLS
   story first for anything public).

## Principles that gate everything above

- Zero third-party dependencies in the interpreter; packages are pure
  Nova + builtins only.
- Every security-relevant change lands with a pentest-suite case.
- Shared-hosting first: file-based storage, CGI-safe (no daemons, no
  background workers, bounded memory), graceful degradation documented.
