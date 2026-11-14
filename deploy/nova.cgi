#!/bin/sh
# nova.cgi — CGI wrapper that hosts any Nova app on cPanel, Plesk,
# DirectAdmin, CyberPanel, HestiaCP or any other panel that runs CGI
# (Apache with mod_cgi / mod_cgid, nginx + fcgiwrap, lighttpd, ...).
#
# Setup on a typical cPanel account:
#   1. Upload the nova binary and your app, e.g.:
#        ~/nova/nova            the interpreter binary (chmod 755)
#        ~/app/main.nv          your application
#        ~/app/nova_modules/    installed packages (nova pkg install ...)
#   2. Copy this file to ~/public_html/cgi-bin/nova.cgi  (chmod 755)
#   3. Edit NOVA_BIN / NOVA_APP below, or leave them and export
#      NOVA_BIN / NOVA_APP in cPanel's "Environment Variables" UI.
#   4. Visit https://yourdomain.com/cgi-bin/nova.cgi
#
# Pretty URLs (no /cgi-bin/nova.cgi in the address bar): add this to
# public_html/.htaccess — see deploy/.htaccess for a ready-made one.
#
# Everything the app prints through http_respond() becomes the HTTP
# response; anything printed to stderr lands in the panel's error log.

# ---- configure (edit these two lines, or set them as env vars) ----
NOVA_BIN="${NOVA_BIN:-$HOME/nova/nova}"
NOVA_APP="${NOVA_APP:-$HOME/app/main.nv}"

# ---- production behaviour ----
export NOVA_ENV=production
export NOVA_REGISTRY="${NOVA_REGISTRY:-$HOME/app/.registry}"
export NOVA_SENDMAIL="${NOVA_SENDMAIL:-/usr/sbin/sendmail}"

# CGI contract: the request body is already on stdin for POST/PUT;
# the interpreter reads it when CONTENT_LENGTH / CONTENT_TYPE say so.
exec "$NOVA_BIN" run "$NOVA_APP"
