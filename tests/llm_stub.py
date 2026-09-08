#!/usr/bin/env python3
"""A stand-in for a local model runner, speaking the chat-completions shape.

Exists so the game's request/response path can be exercised for real -- a real
socket, a real HTTP round trip, a real JSON body -- without a model, a GPU or a
download. It echoes back what it was asked, so the test can prove the letter the
game receives is built from what the model actually said.
"""
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer

class H(BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("content-length", 0))
        raw = self.rfile.read(n).decode("utf-8")
        try:
            req = json.loads(raw)
        except Exception:
            self.send_response(400); self.end_headers(); return

        # Prove the game sent a well-formed conversation.
        msgs = req.get("messages", [])
        roles = ",".join(m.get("role", "?") for m in msgs)
        system = next((m["content"] for m in msgs if m.get("role") == "system"), "")

        reply = ("**Britain:** \"We accept your terms, and we will not forget "
                 "this.\"\n\nroles=" + roles + "\nmodel=" + req.get("model", "?") +
                 "\nsaw_language=" + ("Ukrainian" if "Ukrainian" in system else "?"))
        body = json.dumps({"choices": [{"message": {"role": "assistant",
                                                    "content": reply}}]}).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass

if __name__ == "__main__":
    HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
