#!/usr/bin/env python3
"""fake_smtp.py — a one-connection SMTP honeypot for Nova's pentest suite.

Accepts exactly one SMTP session on 127.0.0.1:2525 and logs every command
the client sends to /tmp/nova_smtp_honeypot.log, including the DATA payload.
The pentest suite uses it to prove that CRLF-injected commands never reach
the wire. Run standalone:

    python3 tests/security/fake_smtp.py &   # serves one session, then exits
"""
import socket
import sys

LOG = "/tmp/nova_smtp_honeypot.log"
HOST, PORT = "127.0.0.1", 2525

log = open(LOG, "w")
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((HOST, PORT))
s.listen(1)
print("honeypot listening on %s:%d" % (HOST, PORT), flush=True)


def send(c, line):
    log.write("S: " + line + "\n")
    log.flush()
    c.sendall(line.encode())


c, _ = s.accept()
send(c, "220 honeypot ESMTP\r\n")
buf = b""
data_mode = False
data_buf = b""
while True:
    chunk = c.recv(4096)
    if not chunk:
        break
    if data_mode:
        data_buf += chunk
        if b"\r\n.\r\n" in data_buf:
            log.write("DATA:\n" + data_buf.decode(errors="replace") + "\n")
            log.flush()
            data_mode = False
            data_buf = b""
            send(c, "250 ok\r\n")
        continue
    buf += chunk
    while b"\r\n" in buf:
        line, buf = buf.split(b"\r\n", 1)
        text = line.decode(errors="replace")
        log.write("C: " + text + "\n")
        log.flush()
        u = text.upper()
        if u.startswith("EHLO") or u.startswith("HELO"):
            send(c, "250 honeypot\r\n")
        elif u.startswith("AUTH"):
            send(c, "334 ok\r\n")
        elif u.startswith("MAIL FROM"):
            send(c, "250 ok\r\n")
        elif u.startswith("RCPT TO"):
            send(c, "250 ok\r\n")
        elif u == "DATA":
            data_mode = True
            send(c, "354 go\r\n")
        elif u.startswith("QUIT"):
            send(c, "221 bye\r\n")
            c.close()
            sys.exit(0)
