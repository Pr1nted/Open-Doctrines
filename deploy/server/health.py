#!/usr/bin/env python3
"""The one thing on $PORT: a health check and a keep-awake target.

    health.py <port> <status-file>

Players never come here -- a relayed server is reached through the account
service with the join code alone -- so this answers only the platform:

  GET /healthz   200 while the container is doing its job, whatever stage it
                 is at: waiting for the previous instance to hand the campaign
                 over, loading the world, or serving. That is deliberate. On a
                 redeploy Render starts the new instance and stops the old one
                 only once the new one is healthy; a health check that waited
                 for the game would wait for an old instance that is waiting
                 for it, and the deploy would never finish.
  GET /          a line saying what this is, and the stage.

The stage is whatever entrypoint.sh last wrote to <status-file>. If the server
itself dies, entrypoint.sh exits and the platform restarts the container;
nothing here has to notice.
"""
import http.server
import sys

port, status_file = int(sys.argv[1]), sys.argv[2]


def stage():
    try:
        with open(status_file) as f:
            return f.read().strip()[:200] or "starting"
    except OSError:
        return "starting"


class Handler(http.server.BaseHTTPRequestHandler):
    def _answer(self, body_wanted):
        body = ("ok " + stage() + "\n") if self.path.startswith("/healthz") else (
            "OpenDoctrines dedicated server (" + stage() + ").\n"
            "Join it from the game with its invite code: Multiplayer -> Join.\n")
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if body_wanted:
            self.wfile.write(data)

    def do_GET(self):
        self._answer(True)

    def do_HEAD(self):
        self._answer(False)

    def log_message(self, *args):   # a ping every few minutes is not news
        pass


http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()
