#!/usr/bin/env python3
"""Local mock API for manual/browser QA of the embedded content-center page."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import secrets
import threading


ROOT = Path(__file__).resolve().parents[2]
HTML = (ROOT / "src" / "web" / "content-center.html").read_bytes()
SESSION = "ABCDEF0123456789"
PAIRING_CODE = "01234567"
PAIRING_TICKET = "0011223344556677"
TICKETS = {PAIRING_TICKET}
TICKET_LOCK = threading.Lock()
paired = False


class Handler(BaseHTTPRequestHandler):
    def send(self, status: int, body: bytes, content_type: str = "application/json") -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def authorized(self) -> bool:
        return self.headers.get("X-PaperMono-Session") == SESSION

    def do_GET(self) -> None:
        global paired

        if self.path in ("/", "/content-center.html"):
            return self.send(200, HTML, "text/html; charset=utf-8")
        if (self.path == "/api/v1/state" and not paired and
                self.headers.get("X-PaperMono-Code") == PAIRING_CODE):
            paired = True
            return self.send(200, json.dumps({
                "paired": True, "ticket": PAIRING_TICKET, "state_version": 1
            }).encode())
        if self.path == "/api/v1/status":
            return self.send(200, json.dumps({
                "wifi": {"ap": True, "ap_verified": True, "connected": True,
                         "station_ipv4": "127.0.0.1"}
            }).encode())
        if self.path == "/api/v1/content" and self.authorized():
            return self.send(200, json.dumps([
                {"id": "0123456789abcdef", "title": "西游记", "kind": "txt", "size": 481233},
                {"id": "fedcba9876543210", "title": "一份很长的中文测试便签名称用于验证手机换行",
                 "kind": "note", "size": 8192},
            ], ensure_ascii=False).encode())
        self.send(401 if self.path.startswith("/api/") else 404, b'{"error":"preview only"}')

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(length)
        if self.path == "/api/v1/session":
            ticket = self.headers.get("X-PaperMono-Ticket")
            if ticket:
                with TICKET_LOCK:
                    if ticket not in TICKETS:
                        return self.send(401, b'{"error":"invalid ticket"}')
                    TICKETS.remove(ticket)
                return self.send(200, json.dumps({"session": SESSION}).encode())
            if self.authorized():
                handoff_ticket = secrets.token_hex(8).upper()
                with TICKET_LOCK:
                    TICKETS.add(handoff_ticket)
                return self.send(200, json.dumps({
                    "ticket": handoff_ticket, "expires_in_ms": 60000
                }).encode())
            return self.send(401, b'{"error":"pairing required"}')
        if not self.authorized():
            return self.send(401, b'{"error":"pairing required"}')
        if self.path == "/api/v1/content":
            return self.send(201, b'{"ok":true}')
        if self.path == "/api/v1/wifi":
            return self.send(202, b'{"ok":true}')
        self.send(404, b'{"error":"preview only"}')

    def do_DELETE(self) -> None:
        self.send(200 if self.authorized() else 401,
                  b'{"ok":true}' if self.authorized() else b'{"error":"pairing required"}')

    def log_message(self, format: str, *args: object) -> None:
        return


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", 8766), Handler).serve_forever()
