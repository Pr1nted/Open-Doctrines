#!/usr/bin/env python3
"""Serve the web build and collect the phone's console over the network.

WHY THIS EXISTS. Safari's Web Inspector needs a cable, and the thing we are
trying to catch kills the tab -- so an on-screen console dies with the page
that was printing to it. The log has to leave the device as each line is
written, not be read afterwards.

So the page beacons every console line here and this appends it to a file on
the Mac. When the tab is killed the last line already arrived.

    python3 tools/devlog_server.py [--dir build-web-nonet] [--port 8123]

Then on the phone open  http://<mac-ip>:<port>/OpenDoctrines.html?devlog=1
and watch the file:      tail -f /tmp/od-devlog.txt

The ?devlog=1 is deliberate: without it the page behaves exactly as it ships,
so nothing here can follow the build to a player.
"""

import argparse
import datetime
import http.server
import os
import socketserver
import sys
import urllib.parse

LOG_PATH = "/tmp/od-devlog.txt"


class Handler(http.server.SimpleHTTPRequestHandler):
    def _write_line(self, text, ua=""):
        stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        line = f"{stamp}  {text}"
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        print(line, flush=True)

    def do_POST(self):
        if urllib.parse.urlparse(self.path).path != "/log":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n).decode("utf-8", "replace") if n else ""
        for raw in body.splitlines():
            if raw.strip():
                self._write_line(raw)
        # 204 and no body: the page is mid-crash and must not wait on us.
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.end_headers()

    def end_headers(self):
        # The wasm and .data files are large; without this Safari re-fetches
        # them on every reload, which on a phone is most of the wait.
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def do_GET(self):
        # THE PAGE REQUESTS ARE LOGGED, the assets are not.
        #
        # Suppressing the access log entirely was a mistake: when the phone
        # sent nothing there was no way to tell "it never reached the server"
        # from "it loaded the page but the hook was off". One line per HTML
        # request, with the user agent, answers that immediately.
        path = urllib.parse.urlparse(self.path)
        if path.path.endswith(".html") or path.path in ("/", ""):
            ua = self.headers.get("User-Agent", "?")
            kind = "iPhone" if "iPhone" in ua else ("iPad" if "iPad" in ua else "other")
            on = "devlog=1" in (path.query or "")
            self._write_line(f"[request] {kind} GET {self.path}"
                             f"   hook={'ON' if on else 'OFF -- add ?devlog=1'}")
            self._write_line(f"[request] ua {ua}")
        super().do_GET()

    def log_message(self, fmt, *args):
        pass   # the default access log drowns the thing we are here to read


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="build-web-nonet")
    ap.add_argument("--port", type=int, default=8123)
    a = ap.parse_args()
    os.chdir(a.dir)
    open(LOG_PATH, "w").close()
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("0.0.0.0", a.port), Handler) as srv:
        print(f"serving {a.dir} on :{a.port}, logging to {LOG_PATH}", flush=True)
        srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
