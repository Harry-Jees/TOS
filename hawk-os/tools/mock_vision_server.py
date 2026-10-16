"""A tiny OpenAI-compatible mock, used to verify Hawk's goal loop offline.

It replays a scripted sequence of replies (one per /chat/completions call) and
records whether each request really carried a screenshot, so the plan ->
execute -> verify loop can be tested without a paid vision model.

    python tools/mock_vision_server.py <port> <reply1> <reply2> ...
"""
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

REPLIES = []
STATE = {"calls": 0, "with_image": 0}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8", "replace")
        i = STATE["calls"]
        STATE["calls"] += 1
        if "data:image/png;base64," in body:
            STATE["with_image"] += 1
        reply = REPLIES[i] if i < len(REPLIES) else "done"
        payload = json.dumps(
            {"choices": [{"message": {"role": "assistant", "content": reply}}]}
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)
        with open("mock_state.json", "w") as f:
            json.dump(STATE, f)


if __name__ == "__main__":
    port = int(sys.argv[1])
    REPLIES.extend(sys.argv[2:])
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
