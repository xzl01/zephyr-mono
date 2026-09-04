#!/usr/bin/env python3
"""Capture PaperMono power HIL logs across USB disconnect/reconnect."""

import argparse
import re
import termios
import time

import serial
from serial.tools import list_ports


VID = 0x303A
PID = 0x1001
RECONNECT_SETTLE_SECONDS = 50.0
LOG_FILTER = re.compile(
    r"SSD1677|PaperMono|FT6336|touch|UI |Power HIL|wake|self-test|"
    r"automatic checks|shutdown|RTC|failed|error|complete|m5pm1|marker|"
    r"not ready",
    re.IGNORECASE,
)


class PassiveSerial(serial.Serial):
    """pyserial transport that never changes native USB reset control lines."""

    def _update_dtr_state(self):
        pass

    def _update_rts_state(self):
        pass


def normalized_serial(value):
    return value.replace(":", "").upper()


def find_port(serial_number):
    expected = normalized_serial(serial_number)
    for item in list_ports.comports():
        actual = normalized_serial(item.serial_number or "")
        if item.vid == VID and item.pid == PID and actual == expected:
            return item.device
    return None


def open_cdc(path):
    """Open CDC without writing DTR/RTS at open or close."""
    handle = PassiveSerial()
    handle.port = path
    handle.baudrate = 115200
    handle.timeout = 0.1
    handle.write_timeout = 0.1
    handle.open()
    attributes = termios.tcgetattr(handle.fileno())
    attributes[2] &= ~termios.HUPCL
    termios.tcsetattr(handle.fileno(), termios.TCSANOW, attributes)
    return handle


def open_reset_cdc(path):
    """Open a control-capable handle used only for an explicit reset."""
    handle = serial.Serial()
    handle.port = path
    handle.baudrate = 115200
    handle.timeout = 0.1
    handle.write_timeout = 0.1
    handle.dtr = False
    handle.rts = False
    handle.open()
    attributes = termios.tcgetattr(handle.fileno())
    attributes[2] &= ~termios.HUPCL
    termios.tcsetattr(handle.fileno(), termios.TCSANOW, attributes)
    return handle


def usb_jtag_application_reset(handle):
    """Reset into the flash application without ever asserting the IO0 line."""
    # DTR is the ESP32-S3 native USB download strap. The previous sequence was
    # esptool's *bootloader-entry* reset and deterministically produced
    # boot:0x21 (DOWNLOAD), leaving only the e-paper's retained old page visible.
    handle.dtr = False
    handle.rts = False
    time.sleep(0.1)
    handle.rts = True
    time.sleep(0.2)
    handle.rts = False
    time.sleep(0.2)


def close_cdc(handle):
    if handle is None:
        return
    try:
        handle.close()
    except (OSError, serial.SerialException):
        pass


def capture(serial_number, timeout, reset, reconnect_settle):
    deadline = time.monotonic() + timeout
    handle = None
    pending = ""
    verified = False
    connections = 0
    reset_pending = reset
    reconnect_not_before = 0.0
    settle_announced = False

    try:
        while time.monotonic() < deadline and not verified:
            if handle is None:
                path = find_port(serial_number)
                if path is None:
                    time.sleep(0.1)
                    continue
                if time.monotonic() < reconnect_not_before:
                    if not settle_announced:
                        print(
                            f"[USB CDC reappeared; waiting "
                            f"{reconnect_settle:g} s before open "
                            "so RTC wake verification can persist]",
                            flush=True,
                        )
                        settle_announced = True
                    time.sleep(0.1)
                    continue
                if reset_pending:
                    reset_handle = None
                    try:
                        reset_handle = open_reset_cdc(path)
                        usb_jtag_application_reset(reset_handle)
                    except (OSError, serial.SerialException, termios.error):
                        close_cdc(reset_handle)
                        time.sleep(0.1)
                        continue
                    close_cdc(reset_handle)
                    reset_pending = False
                    print(
                        "[USB-JTAG reset issued; reopening passively]",
                        flush=True,
                    )
                    time.sleep(0.5)
                    continue
                try:
                    handle = open_cdc(path)
                except (OSError, serial.SerialException, termios.error):
                    close_cdc(handle)
                    handle = None
                    time.sleep(0.1)
                    continue
                connections += 1
                settle_announced = False
                print(
                    f"[USB CDC connected #{connections}; HUPCL disabled]",
                    flush=True,
                )

            try:
                raw = handle.read(4096)
                if not raw:
                    continue
                pending += raw.decode("utf-8", errors="replace")
                while "\n" in pending:
                    line, pending = pending.split("\n", 1)
                    if LOG_FILTER.search(line):
                        print(line, flush=True)
                    if (
                        "Power HIL wake verified" in line
                        or "Power HIL endurance PASS" in line
                        or "Previous RTC power HIL result: PASS" in line
                    ):
                        verified = True
            except (OSError, serial.SerialException) as error:
                print(f"[USB CDC disconnected: {error}]", flush=True)
                close_cdc(handle)
                handle = None
                reconnect_not_before = (
                    time.monotonic() + reconnect_settle
                )
                time.sleep(0.1)
    finally:
        close_cdc(handle)

    print(f"[capture result: {'verified' if verified else 'timeout'}]", flush=True)
    return 0 if verified else 2


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--timeout", type=int, default=150)
    parser.add_argument(
        "--reconnect-settle",
        type=float,
        default=RECONNECT_SETTLE_SECONDS,
        help="Seconds to wait before reopening CDC after re-enumeration.",
    )
    parser.add_argument(
        "--reset",
        action="store_true",
        help="Reset into the flash application without asserting the download strap.",
    )
    args = parser.parse_args()
    try:
        result = capture(
            args.serial, args.timeout, args.reset, args.reconnect_settle
        )
    except KeyboardInterrupt:
        print("\n[capture cancelled by operator]", flush=True)
        result = 130
    raise SystemExit(result)


if __name__ == "__main__":
    main()
