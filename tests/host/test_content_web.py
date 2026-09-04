#!/usr/bin/env python3
"""Static contract checks for the embedded, dependency-free content center."""

from html.parser import HTMLParser
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
HTML_PATH = ROOT / "src" / "web" / "content-center.html"
SERVER_PATH = ROOT / "src" / "papermono_content_server.c"


class ContentCenterParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.ids: list[str] = []
        self.attributes: dict[str, dict[str, str | None]] = {}
        self.external_assets: list[str] = []
        self.in_script = False
        self.scripts: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        values = dict(attrs)
        if "id" in values and values["id"] is not None:
            self.ids.append(values["id"])
            self.attributes[values["id"] or ""] = values
        if tag == "script":
            self.in_script = True
            if values.get("src"):
                self.external_assets.append(values["src"] or "")
        if tag == "link" and values.get("href"):
            self.external_assets.append(values["href"] or "")

    def handle_endtag(self, tag: str) -> None:
        if tag == "script":
            self.in_script = False

    def handle_data(self, data: str) -> None:
        if self.in_script:
            self.scripts.append(data)


html = HTML_PATH.read_text(encoding="utf-8")
server = SERVER_PATH.read_text(encoding="utf-8")
parser = ContentCenterParser()
parser.feed(html)

assert not parser.external_assets, "content center must work without Internet assets"
assert len(parser.ids) == len(set(parser.ids)), "HTML ids must be unique"
required_ids = {
    "pair-panel", "pair-form", "code", "panel", "message", "file", "kind",
    "upload-button", "upload-progress", "content-list", "wifi-form", "ssid", "psk",
}
assert required_ids <= set(parser.ids), "required controls are missing"
code_attributes = parser.attributes["code"]
assert code_attributes.get("maxlength") == "8", "pairing code must be 8 characters"
assert code_attributes.get("pattern") == "[0-9A-HJ-KM-NP-TV-Z]{8}", (
    "pairing input must match the firmware's Crockford base32 alphabet"
)
assert '<option value="weread">' in html, (
    "content kind selector must expose the WeRead catalog"
)

script = "\n".join(parser.scripts)
for contract in (
    'localStorage.setItem(TOKEN_KEY, value)',
    'location.hash.startsWith(prefix)',
    'history.replaceState(null, "", location.pathname + location.search)',
    'location.replace(`http://${address}/#ticket=${encodeURIComponent(handoffTicket)}`)',
    'fetch("/api/v1/session"',
    'request("/api/v1/session", { method: "POST" })',
    '"X-PaperMono-Ticket": ticket',
    'request("/api/v1/status")',
    'request("/api/v1/content")',
    'xhr.open("POST", "/api/v1/content")',
    'request("/api/v1/wifi"',
    'xhr.upload.onprogress',
    'if (lower === "weread-v1.json") return "weread";',
):
    assert contract in script, f"missing web handoff contract: {contract}"

# The session token must never be placed in a URL. A cross-origin handoff may
# carry only a one-time ticket in a fragment, which the LAN page removes before
# redeeming it over a POST body.
for forbidden in (
    'sessionStorage',
    '#session=',
    'URLSearchParams',
    'encodeURIComponent(token)',
    'storeToken(state.session)',
):
    assert forbidden not in script, (
        f"session token must not travel in a URL: found {forbidden!r}"
    )

for server_contract in (
    'HTTP_RESOURCE_DEFINE(session_resource',
    'session_authorized_locked(request)',
    'ticket_issue_locked(now_ms)',
    'ticket_redeem_locked(ticket, now_ms)',
    'strcmp(kind, "weread") == 0',
    'entry->kind == PAPERMONO_CONTENT_WEREAD ? "weread"',
):
    assert server_contract in server, f"missing server ticket contract: {server_contract}"

handoff = re.search(r"#define AP_HANDOFF_DELAY_MS\s+(\d+)U", server)
polling = re.search(r"const deadline = Date\.now\(\) \+ (\d+);", script)
assert handoff and polling, "handoff timing constants must be explicit"
assert int(handoff.group(1)) >= int(polling.group(1)) + 1000, (
    "SoftAP must remain available until browser LAN handoff polling finishes"
)

node = shutil.which("node")
if node:
    with tempfile.NamedTemporaryFile("w", suffix=".js", encoding="utf-8") as source:
        source.write(script)
        source.flush()
        subprocess.run([node, "--check", source.name], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

print("content center web contract: PASS")
