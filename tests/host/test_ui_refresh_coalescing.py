#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Source-contract checks for the voice refresh coalescing path.

The preview harness cannot drive a push-to-talk press/release without a
Bluetooth stack, so this test pins the two properties that keep one hold from
causing up to four back-to-back panel refreshes:

1. ``queue_voice_refresh()`` must route through the shared pending background
   refresh flag (so the worker's request merges with the touch request) instead
   of enqueuing a separate touch-sourced REFRESH event;
2. the voice-pad press must not start a microphone session while the refresh
   input guard is up.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
UI = ROOT / "src" / "papermono_ui.c"


def main() -> int:
    text = UI.read_text(encoding="utf-8")

    match = re.search(
        r"static void queue_voice_refresh\(void\)\n\{(?P<body>.*?)\n\}",
        text,
        re.S,
    )
    if match is None:
        raise SystemExit("queue_voice_refresh() definition not found")
    body = match.group("body")
    assert "papermono_ui_request_background_refresh()" in body, body
    assert "queue_event(" not in body, body
    assert "PAPERMONO_APP_EVENT_SOURCE_TOUCH" not in body, body

    hold = re.search(r"voice_hold = ([^;]+);", text, re.S)
    if hold is None:
        raise SystemExit("voice_hold assignment not found")
    assert "input_blocked" in hold.group(1), hold.group(1)

    print("voice refresh coalescing and refresh-guard press rejection: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
