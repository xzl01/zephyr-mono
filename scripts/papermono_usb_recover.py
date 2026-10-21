#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reset and verify PaperMono without asking for manual power switching.

Default: RTC watchdog whole-chip reset, then Espressif USB-JTAG reset run with
console capture armed BEFORE the final application reset.
An image argument explicitly authorizes writing that image (its target flash
sectors are erased by write-flash). No whole-flash erase or NVS clear is used.
Without an image this tool only resets/observes; --verify-only is an alias.
"""

import argparse
from pathlib import Path
import sys

from papermono_usb_reset import (RecoveryError, add_reset_arguments,
                                 capture_and_reset, checked_run, find_port)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", nargs="?", help="Explicitly flash zephyr.bin at 0x0")
    parser.add_argument("--verify-only", action="store_true", help="Reset/verify; never flash")
    parser.add_argument("--release-io0", action="store_true",
                        help="Legacy alias: reset using the selected method, not manual lines")
    add_reset_arguments(parser)
    args = parser.parse_args()
    if args.timeout <= 0 or args.attempts < 1:
        parser.error("timeout must be positive; attempts must be at least 1")
    if args.verify_only and args.image:
        parser.error("--verify-only cannot be combined with an image")
    try:
        port = find_port(args.serial)
        if port is None:
            raise RecoveryError(f"USB device {args.serial} is not enumerated")
        if args.image:
            image = Path(args.image).resolve(strict=True)
            if not image.is_file():
                raise RecoveryError("Image must be a regular file")
            # Leave ROM ready. Never add a second hidden RTS reset after flash.
            output = checked_run([args.esptool, "--chip", "esp32s3", "--port", port,
                                  "--baud", "460800", "--before", "usb-reset",
                                  "--after", "no-reset", "write-flash", "--flash-mode", "dio",
                                  "--flash-size", "16MB", "--flash-freq", "80m", "0x0", str(image)], 300)
            print(output, end="", flush=True)
            if "Hash of data verified" not in output:
                raise RecoveryError("Flasher exited zero without hash verification evidence")
            print("[flash] verified; application startup remains to be checked", flush=True)
        ok = capture_and_reset(args.serial, args.timeout, args.method, args.openocd,
                               log_path=args.log, reset_log=args.reset_log,
                               esptool=args.esptool, attempts=args.attempts)
        return 0 if ok else 2
    except (RecoveryError, OSError) as error:
        print(f"[error] {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
