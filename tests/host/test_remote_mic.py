#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Host-side checks for the tools/remote-mic BLE voice receiver.

This script is the repository entry point (the same shape as the other
``tests/host/test_*.py`` scripts). It runs:

1. the receiver's pure-Python unit suite from ``tools/remote-mic/tests``;
2. an import contract check: importing the protocol, audio, receiver and CLI
   modules must not import ``bleak`` or ``sounddevice``;
3. the dependency guards, so a missing package produces an actionable error
   instead of a traceback;
4. ``papermono_mic.py --help`` and ``--selftest`` as subprocesses.

No Bluetooth or audio hardware is touched, so this runs on any machine with
Python 3.9+.

Usage:
    python3 tests/host/test_remote_mic.py

    # unit suite only, from the repository root:
    python3 -m unittest discover -s tools/remote-mic/tests -v
"""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL_DIR = ROOT / "tools" / "remote-mic"
CLI = TOOL_DIR / "papermono_mic.py"

REQUIRED_HELP_OPTIONS = (
    "--address",
    "--name",
    "--device",
    "--list-devices",
    "--gain-db",
    "--wav",
    "--verbose",
    "--once",
    "--send-mic-open",
)

# Import every module and fail if a hardware dependency leaked in.
_IMPORT_PROBE = """
import sys
sys.path.insert(0, sys.argv[1])
import remote_mic
import remote_mic.protocol
import remote_mic.audio
import remote_mic.receiver
import remote_mic.cli

leaked = [name for name in ("bleak", "sounddevice") if name in sys.modules]
if leaked:
    print("hardware dependency imported at module import time: " + ", ".join(leaked))
    raise SystemExit(1)
print("ok")
"""


class Result:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def check(self, name: str, ok: bool, detail: str = "") -> None:
        print(f"[{'ok  ' if ok else 'FAIL'}] {name}{(' — ' + detail) if detail else ''}")
        if not ok:
            self.failures.append(name)

    def summary(self) -> int:
        if self.failures:
            print(f"FAILED: {len(self.failures)} check(s): {', '.join(self.failures)}")
            return 1
        print("all remote-mic host checks passed")
        return 0


def _run(args: list[str], timeout: float = 120.0) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args,
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def check_unit_suite(result: Result) -> None:
    loader = unittest.TestLoader()
    suite = loader.discover(str(TOOL_DIR / "tests"), pattern="test_*.py")
    count = suite.countTestCases()
    outcome = unittest.TextTestRunner(
        stream=sys.stdout, verbosity=2, buffer=False
    ).run(suite)
    result.check(
        "unit suite",
        outcome.wasSuccessful() and count > 0,
        f"{count} test(s), {len(outcome.failures)} failure(s), "
        f"{len(outcome.errors)} error(s)",
    )


def check_import_contract(result: Result) -> None:
    completed = _run([sys.executable, "-c", _IMPORT_PROBE, str(TOOL_DIR)])
    result.check(
        "no bleak/sounddevice import at module import time",
        completed.returncode == 0 and "ok" in completed.stdout,
        completed.stdout.strip() or completed.stderr.strip(),
    )


def check_dependency_guards(result: Result) -> None:
    sys.path.insert(0, str(TOOL_DIR))
    try:
        from remote_mic import receiver
    finally:
        sys.path.remove(str(TOOL_DIR))

    if importlib.util.find_spec("bleak") is None:
        try:
            receiver.load_bleak()
        except receiver.DependencyError as exc:
            result.check(
                "missing bleak produces a DependencyError",
                "bleak" in str(exc) and "requirements" in str(exc),
                str(exc).splitlines()[0],
            )
        else:
            result.check("missing bleak produces a DependencyError", False, "no error")
    else:
        result.check(
            "missing bleak produces a DependencyError",
            True,
            "bleak installed; guard not exercised",
        )

    if importlib.util.find_spec("sounddevice") is None:
        try:
            receiver.load_sounddevice()
        except receiver.DependencyError as exc:
            result.check(
                "missing sounddevice produces a DependencyError",
                "sounddevice" in str(exc),
                str(exc).splitlines()[0],
            )
        else:
            result.check(
                "missing sounddevice produces a DependencyError", False, "no error"
            )
    else:
        result.check(
            "missing sounddevice produces a DependencyError",
            True,
            "sounddevice installed; guard not exercised",
        )


def check_cli_help(result: Result) -> None:
    completed = _run([sys.executable, str(CLI), "--help"])
    text = completed.stdout
    missing = [option for option in REQUIRED_HELP_OPTIONS if option not in text]
    result.check(
        "papermono_mic.py --help",
        completed.returncode == 0 and not missing,
        "exit 0" if not missing else f"missing options: {', '.join(missing)}",
    )
    result.check(
        "--help documents the virtual microphone workflow",
        "BlackHole" in text and "CABLE Input" in text,
    )


def check_selftest(result: Result) -> None:
    completed = _run([sys.executable, str(CLI), "--selftest"])
    ok = completed.returncode == 0 and "selftest passed" in completed.stdout
    result.check(
        "papermono_mic.py --selftest",
        ok,
        completed.stdout.strip().splitlines()[-1]
        if completed.stdout.strip()
        else completed.stderr.strip(),
    )


def check_device_listing_without_audio_stack(result: Result) -> None:
    if importlib.util.find_spec("sounddevice") is not None:
        result.check(
            "--list-devices without sounddevice reports the dependency",
            True,
            "sounddevice installed; guard not exercised",
        )
        return
    completed = _run([sys.executable, str(CLI), "--list-devices"])
    text = completed.stdout + completed.stderr
    result.check(
        "--list-devices without sounddevice reports the dependency",
        completed.returncode == 2 and "sounddevice" in text,
        f"exit {completed.returncode}",
    )


def main() -> int:
    print(f"repository root: {ROOT}")
    print(f"receiver: {TOOL_DIR.relative_to(ROOT)}")
    print(f"python: {sys.version.split()[0]}")
    print()

    result = Result()
    check_unit_suite(result)
    check_import_contract(result)
    check_dependency_guards(result)
    check_cli_help(result)
    check_selftest(result)
    check_device_listing_without_audio_stack(result)
    return result.summary()


if __name__ == "__main__":
    raise SystemExit(main())
