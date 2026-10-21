#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Shared, non-destructive PaperMono USB reset and boot observation.

Native USB RTS is NOT an EN-pin/whole-board power cycle. Default full recovery
uses RTC watchdog whole-chip reset to refresh boot straps, then observed JTAG
reset run for peripheral/application startup. Neither cycles the always-on PMIC;
no reset can promise that a faulty image will boot.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import termios
import threading
import time

import serial
from serial.tools import list_ports

VID, PID = 0x303A, 0x1001
DEFAULT_SERIAL = "28:84:85:44:6D:A4"
BOOT_MARKERS = ("Booting Zephyr", "TF content store mounted at /SD:",
                "PaperMono LVGL page rendered", "Authenticated BLE small-data sync ready")
HIL_MARKERS = ("Power HIL wake verified", "Power HIL endurance PASS")
FATAL_MARKERS = ("Guru Meditation", "ZEPHYR FATAL ERROR", "ASSERTION FAIL",
                 "Power HIL endurance FAIL")
ANSI = re.compile(r"\x1b\[[0-9;]*m")


class RecoveryError(RuntimeError):
    pass


def normalized_serial(value):
    return value.replace(":", "").upper()


def find_port(serial_number):
    expected = normalized_serial(serial_number)
    matches = [p.device for p in list_ports.comports()
               if p.vid == VID and p.pid == PID
               and normalized_serial(p.serial_number or "") == expected]
    if len(matches) > 1:
        raise RecoveryError(f"Ambiguous USB serial {serial_number}: {matches}")
    return matches[0] if matches else None


class PassiveSerial(serial.Serial):
    """Do not toggle native USB control lines or discard buffered boot output."""
    def _update_dtr_state(self):
        pass

    def _update_rts_state(self):
        pass

    def _reset_input_buffer(self):
        # pyserial calls this unconditionally during open. The caller can still
        # explicitly discard pre-reset data with the base implementation.
        pass


def open_cdc(path):
    handle = PassiveSerial(path, 115200, timeout=0.1, write_timeout=0.1)
    try:
        attrs = termios.tcgetattr(handle.fileno())
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(handle.fileno(), termios.TCSANOW, attrs)
        return handle
    except BaseException:
        handle.close()
        raise


def checked_run(command, timeout):
    try:
        result = subprocess.run(command, capture_output=True, timeout=timeout)
    except (OSError, subprocess.SubprocessError) as error:
        raise RecoveryError(f"Command failed: {error}") from error
    output = (result.stdout + result.stderr).decode("utf-8", "replace")
    if result.returncode:
        raise RecoveryError(f"Command exited {result.returncode}:\n{output}")
    return output


def jtag_command(executable, usb_serial):
    # Serial is interpolated into Tcl, so reject anything other than a MAC.
    digits = normalized_serial(usb_serial)
    if not re.fullmatch(r"[0-9A-F]{12}", digits):
        raise RecoveryError("Expected a 12-hex-digit USB MAC serial number")
    canonical = ":".join(digits[i:i + 2] for i in range(0, 12, 2))
    return [executable, "-f", "interface/esp_usb_jtag.cfg", "-c",
            f"adapter serial {canonical}; set ESP_ONLYCPU 1; "
            "set ESP_FLASH_SIZE 0; set ESP_RTOS none", "-f", "target/esp32s3.cfg",
            "-c", "adapter speed 2000; gdb port disabled; tcl port disabled; "
            "telnet port disabled", "-c", "init; reset run; shutdown"]


def locate_openocd(explicit=None):
    candidate = explicit or os.environ.get("PAPERMONO_OPENOCD") or shutil.which("openocd")
    if not candidate or not Path(candidate).is_file():
        raise RecoveryError("Set --openocd or PAPERMONO_OPENOCD to Espressif OpenOCD")
    return str(Path(candidate).resolve())


def issue_reset(method, serial_number, port, openocd=None, esptool="esptool"):
    if method == "jtag":
        output = checked_run(jtag_command(locate_openocd(openocd), serial_number), 40)
        # OpenOCD can report an examination error yet exit zero. Never call that
        # an issued reset; fresh application markers are still required below.
        if "Error:" in output or "Examination succeed" not in output or "soft reset" not in output:
            raise RecoveryError(f"JTAG reset was not confirmed:\n{output}")
        return output
    if method == "watchdog":
        # One connection: clear forced download first, then RTC whole-chip WDT.
        # Splitting register writes across CLIs would disable WDT on reconnect.
        output = checked_run([esptool, "--chip", "esp32s3", "--port", port,
                              "--before", "usb-reset", "--after", "watchdog-reset",
                              "--no-stub", "write-mem", "0x6000812c", "0", "0x1"], 45)
        if "Hard resetting with a watchdog" not in output:
            raise RecoveryError(f"RTC watchdog reset was not confirmed:\n{output}")
        return output
    raise RecoveryError(f"Unknown reset method: {method}")


def current_boot(text):
    text = ANSI.sub("", text)
    # A second boot supersedes earlier readiness. Never combine two boot logs.
    for marker in ("ESP-ROM:", "*** Booting Zephyr"):
        if marker in text:
            text = marker + text.rsplit(marker, 1)[1]
    return text


def evidence(text, mode):
    text = current_boot(text)
    if any(marker in text for marker in FATAL_MARKERS):
        return False
    if mode == "boot":
        return all(marker in text for marker in BOOT_MARKERS)
    if mode == "power-hil":
        # Previous RTC result is intentionally NOT evidence of this run.
        return any(marker in text for marker in HIL_MARKERS)
    raise ValueError(mode)


def verified_after_reset(text, mode, require_jtag=False):
    if require_jtag:
        marker = "pro cpu reset by JTAG"
        if marker not in text:
            return False
        text = text.rsplit(marker, 1)[1]
    return evidence(text, mode)


class ConsoleCapture:
    """Read before a JTAG reset; reconnect by identity, never by first path."""
    def __init__(self, serial_number, reconnect_settle=0, discard_before_reset=False,
                 log_path=None):
        self.serial_number = serial_number
        self.reconnect_settle = reconnect_settle
        self.discard_before_reset = discard_before_reset
        self.log_path = log_path
        self.data = bytearray()
        self.ready = threading.Event()
        self.stop = threading.Event()
        self.error = None
        self._log = None
        self.thread = threading.Thread(target=self._read, daemon=True)

    def _read(self):
        handle = None
        try:
            if self.log_path:
                # Stream raw bytes so a long run still leaves evidence if the
                # capture process is interrupted or the device hangs.
                path = Path(self.log_path)
                path.parent.mkdir(parents=True, exist_ok=True)
                self._log = path.open("wb", buffering=0)
            while not self.stop.is_set():
                try:
                    if handle is None:
                        path = find_port(self.serial_number)
                        if path is None:
                            self.stop.wait(0.1)
                            continue
                        handle = open_cdc(path)
                        if self.discard_before_reset and not self.ready.is_set():
                            # Only discard before the reset, never after it or on
                            # reconnect. Keep stale buffered readiness out of this run.
                            serial.Serial._reset_input_buffer(handle)
                        print(f"[console] connected {path}", flush=True)
                        self.ready.set()
                    chunk = handle.read(8192)
                    if chunk:
                        self.data.extend(chunk)
                        if self._log is not None:
                            self._log.write(chunk)
                        if len(self.data) > 8 * 1024 * 1024:
                            raise RecoveryError("Capture exceeded 8 MiB; shorten the run")
                except (OSError, serial.SerialException) as error:
                    if handle is not None:
                        handle.close()
                    handle = None
                    print(f"[console] disconnected: {error}", flush=True)
                    # HIL can explicitly opt into its previous observer delay.
                    self.stop.wait(self.reconnect_settle)
        except Exception as error:
            self.error = error
        finally:
            if handle is not None:
                handle.close()
            if self._log is not None:
                self._log.close()
                self._log = None

    def start(self):
        self.thread.start()

    def finish(self):
        self.stop.set()
        self.thread.join(timeout=3)
        if self.thread.is_alive():
            raise RecoveryError("Serial reader failed to stop")
        if self.error:
            raise RecoveryError(str(self.error)) from self.error

    def text(self):
        return self.data.decode("utf-8", "replace")


def capture_and_reset(serial_number, timeout=35, method="full", openocd=None,
                      mode="boot", reset=True, reconnect_settle=0,
                      log_path=None, reset_log=None, esptool="esptool",
                      attempts=1):
    # A whole-chip reset can occasionally land in the early boot defect; retry
    # the reset (not the flash) when the caller allows it.
    for attempt in range(1, max(1, attempts) + 1):
        if attempt > 1:
            print(f"[retry] reset attempt {attempt}/{attempts}", flush=True)
        verified = _capture_and_reset_once(
            serial_number, timeout, method, openocd, mode, reset,
            reconnect_settle, log_path, reset_log, esptool)
        if verified or not reset:
            return verified
    return False


def _capture_and_reset_once(serial_number, timeout, method, openocd,
                            mode, reset, reconnect_settle,
                            log_path, reset_log, esptool):
    deadline = time.monotonic() + timeout
    port = find_port(serial_number) if reset else None
    if reset and port is None:
        raise RecoveryError(f"USB device {serial_number} is not enumerated")
    if reset and method in ("full", "jtag"):
        # Fail before altering the device if the selected debugger is missing.
        openocd = locate_openocd(openocd)
    capture = ConsoleCapture(serial_number, reconnect_settle,
                             discard_before_reset=reset and method in ("jtag", "full"),
                             log_path=log_path)
    reset_output = ""
    verification_start = 0
    try:
        # esptool needs exclusive CDC access. JTAG uses a separate interface,
        # allowing observation to be armed before its final application reset.
        if reset and method in ("watchdog", "full"):
            reset_output = issue_reset("watchdog", serial_number, port, openocd, esptool)
            # RTC WDT resets USB too. Give host enumeration a bounded settling
            # interval; then follow the serial identity, not the old device path.
            time.sleep(0.5)
        capture.start()
        if reset and not capture.ready.wait(5):
            raise RecoveryError("Serial observer could not open the target")
        if reset and method in ("jtag", "full"):
            verification_start = len(capture.data)
            reset_output += issue_reset("jtag", serial_number, port, openocd, esptool)
        if reset:
            print(f"[reset] {method} command completed; verifying fresh boot", flush=True)
        if reset:
            deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if capture.error:
                raise RecoveryError(str(capture.error))
            observed = bytes(capture.data[verification_start:]).decode("utf-8", "replace")
            if verified_after_reset(observed, mode, reset and method in ("jtag", "full")):
                break
            if any(m in current_boot(observed) for m in FATAL_MARKERS):
                break
            time.sleep(0.1)
    except Exception as error:
        reset_output += f"\n[recovery error] {error}\n"
        raise
    finally:
        try:
            if capture.thread.ident is not None:
                capture.finish()
        finally:
            if log_path and not Path(log_path).exists():
                Path(log_path).write_bytes(capture.data)
            if reset_log:
                Path(reset_log).write_text(reset_output, encoding="utf-8")
    text = capture.text()
    print(text, end="" if text.endswith("\n") else "\n", flush=True)
    verified = verified_after_reset(
        bytes(capture.data[verification_start:]).decode("utf-8", "replace"), mode,
        reset and method in ("jtag", "full"))
    print(f"[capture] mode={mode} bytes={len(capture.data)} "
          f"result={'verified' if verified else 'unverified'}", flush=True)
    if not verified:
        print("[diagnosis] No matching fresh completion evidence. This does not "
              "establish a power fault or exhaust recovery methods.", flush=True)
    return verified


def add_reset_arguments(parser):
    parser.add_argument("--serial", default=DEFAULT_SERIAL)
    parser.add_argument("--method", choices=("full", "jtag", "watchdog"), default="full",
                        help="full: RTC whole-chip reset, then observed JTAG reset run")
    parser.add_argument("--openocd", help="Espressif OpenOCD with esp_usb_jtag support")
    parser.add_argument("--esptool", default="esptool")
    parser.add_argument("--timeout", type=float, default=35)
    parser.add_argument("--log", help="Save complete, unfiltered raw console bytes")
    parser.add_argument("--reset-log", help="Save complete reset tool output")
    parser.add_argument("--attempts", type=int, default=1,
                        help="Reset attempts when boot evidence is missing")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    add_reset_arguments(parser)
    args = parser.parse_args()
    if args.timeout <= 0 or args.attempts < 1:
        parser.error("timeout must be positive; attempts must be at least 1")
    try:
        ok = capture_and_reset(args.serial, args.timeout, args.method, args.openocd,
                               log_path=args.log, reset_log=args.reset_log,
                               esptool=args.esptool, attempts=args.attempts)
        return 0 if ok else 2
    except (RecoveryError, OSError) as error:
        print(f"[error] {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
