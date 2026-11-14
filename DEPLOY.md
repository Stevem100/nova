# Hosting Nova applications

Nova apps are a single compiled binary plus your `.nv` files, so they can
be hosted anywhere CGI is available — which is effectively **every** shared
hosting panel on earth — or as a plain process on a VPS.

```
browser ──> Apache/nginx (any panel) ──> nova.cgi ──> nova run app.nv
                                             │
                    http_request()/http_respond() builtins
```

The `web.c` builtins implement the **CGI/1.1** interface: the web server
sets `REQUEST_METHOD`, `QUERY_STRING`, `HTTP_*` … in the environment and
pipes the request body to stdin; `http_request()` parses all of it into one
map and `http_respond()` writes the status line, headers and body to
stdout. That is the entire contract — no plugins, no daemons, no root.

---

## 1. cPanel (and any Apache-based panel)

cPanel ships Apache with `mod_cgid` enabled, so CGI needs no configuration.

1. **Build the interpreter** (locally, or directly on the server via SSH):

   ```sh
   make                # Linux/macOS/BSD, produces ./nova
   make static         # optional: static binary for maximum portability
   ```

2. **Upload** with File Manager or FTP (paths are relative to your home dir):

   ```
   ~/nova/nova                 the interpreter (chmod 755)
   ~/app/main.nv               your application
   ~/app/nova.json             optional project manifest
   ~/app/nova_modules/         installed packages (see below)
   ```

3. **Install your packages** before uploading (or on the server over SSH):

   ```sh
   nova pkg install webx tailwind bootstrap fonts icons mailer
   ```

   This creates `~/app/nova_modules/<name>/` which uploads like any folder.

4. **Copy the wrapper** `deploy/nova.cgi` into `~/public_html/cgi-bin/`,
   edit the two paths at the top if you used different folders, and
   `chmod 755 ~/public_html/cgi-bin/nova.cgi`.

5. **Visit** `https://yourdomain.com/cgi-bin/nova.cgi`.

6. **Pretty URLs**: copy `deploy/.htaccess` into `~/public_html/`.
   `/about`, `/hello?name=Nova`, `/table` … are now routed straight to the
   app. The same file blocks downloads of `.nv` sources, manifests and
   `nova_modules/`.

Works the same on **Plesk, DirectAdmin, CyberPanel, HestiaCP, CloudPanel,
aaPanel** — any panel whose web server runs CGI. Only the web-root path
differs (`~/public_html`, `~/httpdocs`, `~/domains/x/public_html`, ...).

### Logs and debugging

- stderr output (including uncaught Nova errors) goes to the panel's error
  log: cPanel → Metrics → Errors, or `~/logs/`.
- Preview locally first: without `GATEWAY_INTERFACE` set, `http_request()`
  simulates a GET request, so `nova run main.nv` prints the response HTML.

---

## 2. VPS — nginx

Use `fcgiwrap` (CGI-to-FastCGI adapter, packaged everywhere):

```sh
apt install fcgiwrap            # spawns /var/run/fcgiwrap.socket
```

```nginx
location / {
    root /var/www/nova-app;
    try_files $uri @nova;
}
location @nova {
    include fastcgi_params;
    fastcgi_param SCRIPT_FILENAME /var/www/nova-app/main.nv;
    fastcgi_param NOVA_ENV production;
    fastcgi_pass unix:/var/run/fcgiwrap.socket;
}
location ~ ^/(app\.nv|nova_modules/|\.registry/) { deny all; }
```

The app binary must be executable by `www-data`:

```sh
chown www-data:www-data /usr/local/bin/nova /var/www/nova-app/main.nv
chmod 755 /usr/local/bin/nova
```

## 3. VPS — Apache

```apache
<Directory /var/www/nova-app>
    Options +ExecCGI
    AddHandler cgi-script .cgi
    DirectoryIndex index.cgi
    Require all granted
</Directory>
```

with `/var/www/nova-app/index.cgi` being the same wrapper as
`deploy/nova.cgi`, and `a2enmod cgid` first.

## 4. systemd (no CGI, one long-running process)

For very high traffic you can keep the interpreter warm with
`systemd` + a tiny socket forwarder — but for the vast majority of sites
CGI is faster than expected: the Nova binary starts in **~1 ms** (it is a
160 KB static-ish C program with a sub-millisecond boot), so the per-request
cost is negligible compared to PHP-FPM pools.

---

## 5. Environment variables recognised by the interpreter

| Variable        | Meaning                                            | Default          |
|-----------------|----------------------------------------------------|------------------|
| `NOVA_SENDMAIL` | sendmail binary used by `mail_send()`              | `/usr/sbin/sendmail` |
| `NOVA_REGISTRY` | package registry directory for `nova pkg`          | `~/.nova`        |
| `NOVA_ENV`      | free-form marker your app can read via `get_env()` | unset            |

Your app reads any panel-defined variable with `get_env("NAME")` — that is
how cPanel environment variables (database DSNs, API keys) reach Nova code
without hard-coding.

## 6. Packages you will want on the server

The registry ships a complete web stack; install it next to your app
(`NOVA_REGISTRY` pointing at the bundled `.registry/` or your own):

```sh
NOVA_REGISTRY=./.registry nova pkg install webx templates forms validate \
    session csrf flash auth jdb paginate api cors cache logger dotenv ids \
    htmx tailwind bootstrap fonts icons mailer
```

Highlights for hosted apps:

- **`jdb`** — flat-file JSON tables (`storage/data/*.json`), so apps run
  without MySQL. Keep the directory outside `public_html` and back it up
  like any data file.
- **`session` / `cache` / `logger`** — write under `storage/` by default.
  Point them inside your home directory (e.g. `/home/<user>/app-storage`)
  so the web server can write but strangers cannot download the files.
- **`session->gc(dir, ttl)` and `cache->gc(opts)`** — delete expired
  session/cache files. Wire them to a daily cron: cPanel → *Advanced →
  Cron Jobs* → command `cd /home/<user>/app && nova run gc.nv` with a
  `gc.nv` that calls both.
- **`mailer`** — uses sendmail with the same envelope PHP's `mail()` does,
  so it works out of the box on cPanel; or point it at your SMTP relay.
- **`dotenv`** — keep secrets in a `.env` above `public_html`; cPanel
  "Environment Variables" also work (they win over the file).
- **`auth`** — user accounts + login against a JSON file (`auth->attempt`
  stores the session id); pair with `csrf->hidden()` on every form.

## 7. Security checklist

- Keep the `nova` binary and `.nv` sources **outside** the document root,
  or rely on the `.htaccess` deny rules in `deploy/.htaccess`.
- Never serve `nova_modules/` or `.registry/` — they contain package source.
- `webx->html_escape()` every value you interpolate into markup; the
  framework escapes by default in its helpers, but raw concatenation is
  your responsibility.
- Request bodies are capped at 32 MiB by `http_request()`.
- For uploads over HTTPS on shared hosts, panels terminate TLS at Apache —
  Nova sees `HTTPS: on` via the `scheme` field of the request map.

## 8. TLS wrapper interop (outbound HTTPS / authenticated SMTP)

Nova ships no TLS stack on purpose (zero dependencies; see SECURITY.md
NV-13 and the issue #2 decision). The standard pattern on any host is to
let a battle-tested local wrapper terminate TLS, and point Nova's outbound
clients at it over loopback:

**Outbound HTTPS with `http_client` + stunnel** — expose any remote HTTPS
API as a plain-HTTP loopback service:

```
# /etc/stunnel/api.conf
[example-api]
client = yes
accept = 127.0.0.1:8443
connect = api.example.com:443
```

```nova
import "http_client";
// TLS is handled by stunnel; Nova talks plain HTTP to loopback
$res = $http_client->get("http://127.0.0.1:8443/v1/ping", nil);
```

**Authenticated SMTP with msmtp** — configure msmtp as a sendmail-compatible
relay (it speaks TLS + AUTH upstream), then use `mail_send()` unchanged:

```
# ~/.msmtprc  (chmod 600)
defaults
account   default
host      smtp.example.com
port      587
tls       on
tls_starttls on
auth      on
from      app@example.com
user      app@example.com
password  ********
```

Point `sendmail_path`-style configuration at msmtp (`sendmail -t`
compatible), or set the mailer package's transport to `smtp_send` aimed at
`127.0.0.1:25` if you run msmtp as a local smarthost. cPanel users rarely
need any of this: the server MTA already relays through TLS, so
`mail_send()` is TLS-secure end to end without extra setup.

On nginx you get the same effect free: `proxy_pass` in front of Nova lets
you keep API keys in nginx-side headers, or mount a `resolver`-based route
that terminates TLS for upstream calls.

## 9. Windows servers

The CGI contract is identical on IIS (CGI module) and the interpreter
builds with `build_windows.bat` / MSYS2 (or plain `make` under UCRT64).
`mail_send()` is unavailable on Windows (no sendmail); use `smtp_send()`
against your smarthost instead, or the stunnel/msmtp patterns from
section 8 for TLS.
