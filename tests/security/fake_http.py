#!/usr/bin/env python3
"""tests/security/fake_http.py — loopback HTTP server for the http_client
transport smoke test (run_pentest.sh section 3b).

Serves fixed routes on 127.0.0.1:18472:
  GET  /ok      -> 200 "pong"                    (Content-Length body)
  GET  /chunked -> 200 "chunky-response"         (Transfer-Encoding: chunked)
  GET  /redir   -> 301 Location: /ok             (redirect chain)
  GET  /big     -> 200 with a 1 MiB body         (body-cap refusal test)
  POST /echo    -> 200 "echo:<body>"             (request bodies)
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _send(self, code, body, ctype="text/plain", extra=None):
        payload = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.path == "/ok":
            self._send(200, "pong")
        elif self.path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            for part in (b"chunky-", b"response"):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        elif self.path == "/redir":
            self._send(301, "moved", extra={"Location": "/ok"})
        elif self.path == "/big":
            self._send(200, "B" * (1024 * 1024))
        else:
            self._send(404, "not found")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(n)
        self._send(200, "echo:" + body.decode("utf-8", "replace"))

if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", 18472), Handler)
    srv.daemon_threads = True          # abandoned sockets must not block others
    srv.serve_forever()
