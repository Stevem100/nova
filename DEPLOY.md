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

## 6. Security checklist

- Keep the `nova` binary and `.nv` sources **outside** the document root,
  or rely on the `.htaccess` deny rules in `deploy/.htaccess`.
- Never serve `nova_modules/` or `.registry/` — they contain package source.
- `webx->html_escape()` every value you interpolate into markup; the
  framework escapes by default in its helpers, but raw concatenation is
  your responsibility.
- Request bodies are capped at 32 MiB by `http_request()`.
- For uploads over HTTPS on shared hosts, panels terminate TLS at Apache —
  Nova sees `HTTPS: on` via the `scheme` field of the request map.

## 7. Windows servers

The CGI contract is identical on IIS (CGI module) and the interpreter
builds with `build_windows.bat` / MSYS2. `mail_send()` is unavailable on
Windows (no sendmail); use `smtp_send()` against your smarthost instead.
