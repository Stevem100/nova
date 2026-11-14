# Tutorial: from zero to a working web app on cPanel

This walkthrough builds and deploys a small but complete guestbook app —
list posts, publish through a protected form, get an e-mail notification —
using only what ships with Nova. Budget: about 30 minutes. Everything here
also works on Plesk, DirectAdmin, CyberPanel, HestiaCP and plain VPSes; the
reference for every panel is [DEPLOY.md](../DEPLOY.md).

What you will use:

| Piece | Role |
| --- | --- |
| `nova` binary | the interpreter (one file, no dependencies) |
| `deploy/nova.cgi` | the CGI wrapper Apache calls per request |
| `deploy/.htaccess` | pretty URLs + blocks downloads of sources/state |
| `webx`, `templates`, `session`, `csrf`, `flash` | routing, HTML, login state, form tokens, notices |
| `validate`, `forms` | input rules and form widgets |
| `jdb`, `queue` | flat-file JSON storage, background jobs |
| `mailer`, `tailwind` | e-mail via local sendmail, styling |

---

## 1. Build the interpreter

On your own machine (Linux/macOS/BSD):

```sh
git clone https://github.com/Stevem100/nova.git
cd nova && make && make test       # 23/23 tests pass
```

Or build directly on the server over SSH — cPanel accounts usually have gcc:

```sh
ssh user@yourdomain.com
git clone https://github.com/Stevem100/nova.git ~/nova-src
cd ~/nova-src && make
```

Building on the server is the most reliable option (right libc, right
endian, no upload hassle). If SSH is disabled on your plan, build locally
and upload the `nova` binary with File Manager — set permissions 755.

## 2. Create the app

Pick a home for the app outside the web root, e.g. `~/app`. A manifest
makes dependency management reproducible:

`~/app/nova.json`

```json
{
  "name": "guestbook",
  "version": "1.0.0",
  "entry": "main.nv",
  "description": "Guestbook with e-mail notification",
  "dependencies": {
    "webx": "1.0.0",
    "templates": "1.0.0",
    "session": "1.0.0",
    "csrf": "1.0.0",
    "flash": "1.0.0",
    "validate": "1.0.0",
    "forms": "1.0.0",
    "jdb": "1.0.0",
    "queue": "1.0.0",
    "mailer": "1.0.0",
    "tailwind": "1.0.0"
  }
}
```

Install the dependencies (locally, or on the server with `NOVA_REGISTRY`
pointing at a registry you trust — pin exact versions, `*` installs warn):

```sh
cd ~/app
NOVA_REGISTRY=~/nova-src/.registry nova pkg install
# -> installed: webx 1.0.0 ... (checksums verified against registry.json)
```

`nova pkg install` verifies each package against the registry's
`registry.json` SHA-256 index; a tampered registry is refused before
anything runs.

## 3. Write the application

One file, `~/app/main.nv`. This is the whole app — routing, templates,
validation, storage, mail:

```php
// main.nv — guestbook: list, post, e-mail notification.
import "webx";
import "templates";
import "session";
import "csrf";
import "flash";
import "validate";
import "forms";
import "jdb";
import "queue";
import "mailer";
import "tailwind";

$DATA  = get_env("HOME") . "/app/data";        // never inside public_html
$POSTS = $jdb->open("posts", $DATA);
$SESS  = ["dir" => $DATA . "/sessions"];
$JOBS  = $DATA . "/queue";

// one-time setup: storage dirs + seed row
if (!file_exists($DATA)) {
    mkdir($DATA);
    mkdir($DATA . "/sessions");
    mkdir($JOBS);
    $jdb->insert($POSTS, ["name" => "Nova", "body" => "First! Welcome.",
                          "created" => int(now())]);
}

$LIST_TPL = "<h2>Guestbook</h2>{{flash}}"
  . "{{#if empty}}<p>Be the first to sign.</p>{{else}}"
  . "{{#each posts}}<article><b>{{name|e}}</b><p>{{body|e}}</p></article>{{/each}}"
  . "{{/if}}"
  . "<h3>Sign the guestbook</h3>"
  . "<form method=\"post\" action=\"/sign\">{{token}}{{errors}}"
  . "<p>{{name_field}}</p><p>{{body_field}}</p>"
  . "<button type=\"submit\">Sign</button></form>";

fn view($rows, $errors, $in, $sess) {
    return $templates->render($LIST_TPL,
        ["posts"   => $rows, "empty" => len($rows) == 0,
         "flash"   => $flash->render($sess, nil),
         "token"   => $csrf->hidden($sess),
         "errors"  => $forms->errors_summary($errors, nil),
         "name_field" => $forms->text("name", old_of($in, "name"), nil),
         "body_field" => $forms->textarea("body", old_of($in, "body"), ["rows" => 4])]);
}

fn old_of($in, $key) {
    if (is_map($in) && has($in, $key)) return $in[$key];
    return "";
}

fn home($req) {
    $sess = $session->start($req, $SESS);
    $rows = $jdb->order($POSTS, "created", true);        // newest first
    $body = view($rows, [:], [:], $sess);
    return $session->finish($sess,
        $webx->html($webx->layout("Guestbook", $body, ["head" => $tailwind->cdn_link()])));
}

fn sign_submit($req) {
    $sess = $session->start($req, $SESS);
    if (!$csrf->verify($sess, $req)) {
        $flash->put($sess, "danger", "Invalid form token, please retry.");
        return $session->finish($sess, $webx->redirect("/"));
    }
    $in = ["name" => $webx->param($req, "name", ""),
           "body" => $webx->param($req, "body", "")];
    $errors = $validate->check($in,
        ["name" => "required|max_len:60", "body" => "required|min_len:3|max_len:500"]);
    if ($validate->fails($errors)) {
        $rows = $jdb->order($POSTS, "created", true);
        return $session->finish($sess,
            $webx->html_status(view($rows, $errors, $in, $sess), 422));
    }
    $jdb->insert($POSTS, ["name" => $in["name"], "body" => $in["body"],
                          "created" => int(now())]);

    // heavy work never runs inside the request: queue it instead
    $queue->enqueue("notify", ["name" => $in["name"], "body" => $in["body"]], $JOBS);

    $flash->put($sess, "success", "Thanks for signing!");
    return $session->finish($sess, $webx->redirect("/"));
}

$routes = [
    "GET /"     => fn($req) { return home($req); },
    "POST /sign" => fn($req) { return sign_submit($req); },
    "*"         => fn($req) { return $webx->error_response(404, "no page at " . $req->path); },
];

$webx->run($routes);
```

A tiny `~/app/worker.nv` drains the queue and sends the notification mail
through the local sendmail (on cPanel this is preconfigured — no SMTP
credentials, no TLS worries; see SECURITY.md NV-13):

```php
// worker.nv — cron worker: notifications + queue maintenance.
import "queue";
import "mailer";

$DATA = get_env("HOME") . "/app/data";
$JOBS = $DATA . "/queue";

while (($job = $queue->pop("notify", $JOBS)) != nil) {
    $sent = $mailer->send([
        "from"    => "guestbook@yourdomain.com",
        "to"      => "you@yourdomain.com",
        "subject" => "New guestbook signature",
        "text"    => $job["payload"]["name"] . " wrote:\n" . $job["payload"]["body"]
    ]);
    if ($sent) $queue->complete("notify", $job, $JOBS);
    else       $queue->fail("notify", $job, 3, $JOBS);   // retry, then dead-letter
}
$queue->purge("notify", "done", $JOBS);
```

`mailer->send()` picks the sendmail transport automatically when no SMTP
`host` is configured — exactly what a shared host wants.

## 4. Test before uploading

Locally (or on the server), simulate a full POST round-trip the same way
the test suite does:

```sh
cd ~/app
GATEWAY_INTERFACE=CGI/1.1 REQUEST_METHOD=POST \
CONTENT_TYPE=application/x-www-form-urlencoded CONTENT_LENGTH=28 \
    sh -c 'printf "name=Ada&body=Hello+nova" | ~/nova-src/nova run main.nv'
```

You should see a `303 See Other` redirect to `/` with a `Set-Cookie`
header — the signature was stored and the job queued. A second GET-style
run prints the entry on the home page. All request parsing, header
sanitizing and body limits are enforced by the same code on every request.

## 5. Upload to cPanel

With File Manager or FTP, lay it out like this (paths relative to your
home directory):

```
~/nova/nova                 interpreter binary            (chmod 755)
~/app/main.nv               the application
~/app/worker.nv             cron worker
~/app/nova.json             manifest
~/app/nova_modules/         installed packages (from step 2)
~/app/data/                 runtime storage               (chmod 755)
~/public_html/cgi-bin/nova.cgi    copied from deploy/nova.cgi  (chmod 755)
~/public_html/.htaccess           copied from deploy/.htaccess
```

Everything under `~/app` lives **outside** `public_html` — only `nova.cgi`
and `.htaccess` are web-reachable, and `.htaccess` additionally denies
`*.nv`, `nova.json`, `*.lock` and `nova_modules/` downloads.

Edit the two paths at the top of `nova.cgi` if your layout differs (or set
`NOVA_BIN` / `NOVA_APP` in cPanel's Environment Variables UI):

```sh
NOVA_BIN="${NOVA_BIN:-$HOME/nova/nova}"
NOVA_APP="${NOVA_APP:-$HOME/app/main.nv}"
```

## 6. Go live

Visit `https://yourdomain.com/` — the `.htaccess` rewrites `/` to the CGI
wrapper, `webx->run($routes)` dispatches on `$req->path`, and you are
serving a dynamic app. The address bar never shows `/cgi-bin/nova.cgi`.

## 7. When something breaks

- **500 error** — the app's stderr lands in cPanel → Metrics → Errors (or
  `~/logs/`). Nova prints the failing line and message.
- **Permission denied** — `chmod 755 nova nova.cgi`; `~/app/data` must be
  writable by the web user (755 usually suffices on cPanel).
- **Blank page / malformed response** — make sure nothing `echo`s outside
  `http_respond()`: under CGI, all output must go through the response
  builders (`$webx->html(...)`, `->redirect(...)`, ...).
- **Mail not arriving** — `NOVA_SENDMAIL` defaults to `/usr/sbin/sendmail`;
  cPanel's sendmail path is `/usr/sbin/sendmail` on virtually every build.
  `mailer->last_error()` tells you what the transport said.
- **Sessions lost between requests** — the sessions directory must be the
  same across requests and writable; it is, as long as `$DATA` does not
  move.

## 8. Production checklist

- [ ] Exact versions pinned in `nova.json` (no `"*"` — installs warn now)
- [ ] `registry.json` present in your registry (checksums verified on install)
- [ ] Storage (`data/`) outside the web root; `.htaccess` denies in place
- [ ] Queue drained by a cron worker (cPanel → Advanced → Cron Jobs)
- [ ] `NOVA_ENV=production` (set by `nova.cgi`) — errors go to logs, not visitors
- [ ] Backups: everything is JSON files — snapshot `~/app/data` in cPanel's
      JetBackup or a nightly `tar` cron

From here: `auth` adds login on top of the same session primitives, the
`db_migrate` package versions your data layout as the app grows, and
DEPLOY.md covers nginx, systemd and Windows server layouts.
