#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Entry point for the PaperMono C153 BLE voice receiver.

Run it directly from a checkout:

    python3 tools/remote-mic/papermono_mic.py --help

The heavy dependencies (``bleak`` for Bluetooth LE and ``sounddevice`` for
audio output) are imported lazily, so ``--help`` and ``--selftest`` work
without installing them.
"""

from __future__ import annotations

import sys
from pathlib import Path

# Allow running the script by path without installing the package.
_TOOL_DIR = str(Path(__file__).resolve().parent)
if _TOOL_DIR not in sys.path:
    sys.path.insert(0, _TOOL_DIR)

from remote_mic.cli import main  # noqa: E402  (import after sys.path setup)


if __name__ == "__main__":
    raise SystemExit(main())
