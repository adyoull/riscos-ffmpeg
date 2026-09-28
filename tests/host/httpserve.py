#!/usr/bin/env python3
"""A small HTTP server for the network tests (tests/host/https.sh, net_test):
serves the files in ROOT with Range requests (which FFmpeg uses to seek;
python's own http.server has none), 404 for anything else, and writes each
request's headers to LOG if given.

  httpserve.py PORT ROOT [LOG]     serve
  httpserve.py PORT --silent       accept connections, never answer
"""
import http.server, os, re, socket, sys

port = int(sys.argv[1])
if sys.argv[2] == "--silent":
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(8)
    conns = []
    while True:
        c, _ = s.accept()
        conns.append(c)            # read nothing, answer nothing
root = sys.argv[2]
log = sys.argv[3] if len(sys.argv) > 3 else None

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def do_GET(self):
        if log:
            with open(log, "a") as f:
                f.write("GET %s\n%s\n" % (self.path, str(self.headers)))
        path = os.path.join(root, os.path.basename(self.path.split("?")[0]))
        if not os.path.isfile(path):
            self.send_error(404)
            return
        data = open(path, "rb").read()
        m = re.match(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
        if m:
            a = int(m.group(1))
            b = int(m.group(2)) if m.group(2) else len(data) - 1
            part = data[a:b + 1]
            self.send_response(206)
            self.send_header("Content-Range", "bytes %d-%d/%d" % (a, a + len(part) - 1, len(data)))
        else:
            part = data
            self.send_response(200)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(len(part)))
        self.end_headers()
        try:
            self.wfile.write(part)
        except (BrokenPipeError, ConnectionResetError):
            pass
    def log_message(self, *a):
        pass

http.server.ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
