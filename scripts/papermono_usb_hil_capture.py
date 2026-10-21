#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Capture complete PaperMono logs; distinguish normal boot from power HIL.

--reset defaults to RTC whole-chip reset followed by observed USB-JTAG reset run.
Power HIL's old 50-second observer delay is available explicitly, never default.
"""

import argparse
import sys

from papermono_usb_reset import (PassiveSerial, RecoveryError, add_reset_arguments,
                                 capture_and_reset, find_port, normalized_serial,
                                 open_cdc)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    add_reset_arguments(parser)
    parser.set_defaults(timeout=150)
    parser.add_argument("--mode", choices=("boot", "power-hil"), default="power-hil")
    parser.add_argument("--reset", action="store_true")
    parser.add_argument("--reconnect-settle", type=float, default=0,
                        help="Explicit observer delay after reconnect (may lose boot output)")
    args = parser.parse_args()
    if args.timeout <= 0 or args.reconnect_settle < 0:
        parser.error("timeout must be positive; reconnect-settle must be nonnegative")
    try:
        ok = capture_and_reset(args.serial, args.timeout, args.method, args.openocd,
                               mode=args.mode, reset=args.reset,
                               reconnect_settle=args.reconnect_settle,
                               log_path=args.log, reset_log=args.reset_log,
                               esptool=args.esptool, attempts=args.attempts)
        return 0 if ok else 2
    except KeyboardInterrupt:
        print("[capture cancelled]", file=sys.stderr)
        return 130
    except (RecoveryError, OSError) as error:
        print(f"[error] {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
