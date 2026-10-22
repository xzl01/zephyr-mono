# SPDX-License-Identifier: GPL-3.0-only
"""Allow ``python3 -m remote_mic`` from the tools/remote-mic directory."""

from __future__ import annotations

from .cli import main

if __name__ == "__main__":
    raise SystemExit(main())
