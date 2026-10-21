#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Hardware-free regression coverage for reset identity, commands and evidence."""
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import papermono_usb_reset as reset


def port(device, serial_number, vid=reset.VID, pid=reset.PID):
    return SimpleNamespace(device=device, serial_number=serial_number, vid=vid, pid=pid)


def boot():
    return "ESP-ROM:esp32s3\n*** Booting Zephyr OS\n" + "\n".join(reset.BOOT_MARKERS[1:])


class ResetTests(unittest.TestCase):
    def test_identity_is_bound_to_port_not_first_device(self):
        with patch.object(reset.list_ports, "comports", return_value=[
            port("wrong", "AA:BB:CC:DD:EE:FF"),
            port("wanted", "288485446da4"),
            port("wrong-vendor", reset.DEFAULT_SERIAL, vid=1),
        ]):
            self.assertEqual(reset.find_port(reset.DEFAULT_SERIAL), "wanted")

    def test_missing_and_ambiguous_identity(self):
        with patch.object(reset.list_ports, "comports", return_value=[]):
            self.assertIsNone(reset.find_port(reset.DEFAULT_SERIAL))
        with patch.object(reset.list_ports, "comports", return_value=[
            port("one", reset.DEFAULT_SERIAL), port("two", reset.DEFAULT_SERIAL)]):
            with self.assertRaises(reset.RecoveryError):
                reset.find_port(reset.DEFAULT_SERIAL)

    def test_nonzero_exit_never_accepted_for_success_words(self):
        with patch.object(reset.subprocess, "run", return_value=SimpleNamespace(
                returncode=2, stdout=b"MAC Connected Hash of data verified", stderr=b"fatal")):
            with self.assertRaises(reset.RecoveryError):
                reset.checked_run(["fake"], 1)

    def test_command_not_found_and_timeout_fail(self):
        for error in (FileNotFoundError("missing"), subprocess.TimeoutExpired("fake", 1)):
            with self.subTest(error=error), patch.object(reset.subprocess, "run", side_effect=error):
                with self.assertRaises(reset.RecoveryError):
                    reset.checked_run(["fake"], 1)

    def test_jtag_identity_single_core_and_no_flash_or_rtos_probe(self):
        cmd = reset.jtag_command("openocd", "288485446da4")
        joined = " ".join(cmd)
        for expected in ("adapter serial 28:84:85:44:6D:A4", "ESP_ONLYCPU 1",
                         "ESP_FLASH_SIZE 0", "ESP_RTOS none", "init; reset run; shutdown"):
            self.assertIn(expected, joined)
        self.assertNotIn("write-flash", joined)
        self.assertNotIn("erase", joined)
        with self.assertRaises(reset.RecoveryError):
            reset.jtag_command("openocd", "x; shutdown")

    def test_jtag_zero_exit_with_examination_error_fails(self):
        with patch.object(reset, "locate_openocd", return_value="openocd"), \
             patch.object(reset, "checked_run", return_value="Examination succeed\nError: bad\nsoft reset"):
            with self.assertRaises(reset.RecoveryError):
                reset.issue_reset("jtag", reset.DEFAULT_SERIAL, "fake")

    def test_watchdog_clear_and_reset_in_one_process(self):
        with patch.object(reset, "checked_run", return_value="Hard resetting with a watchdog...") as run:
            reset.issue_reset("watchdog", reset.DEFAULT_SERIAL, "fake")
        cmd = run.call_args.args[0]
        self.assertIn("watchdog-reset", cmd)
        self.assertEqual(cmd[-4:], ["write-mem", "0x6000812c", "0", "0x1"])
        self.assertNotIn("write-flash", cmd)
        self.assertNotIn("erase-flash", cmd)
        self.assertEqual(run.call_count, 1)

    def test_boot_requires_all_fresh_markers_not_arbitrary_driver_text(self):
        self.assertTrue(reset.evidence(boot(), "boot"))
        self.assertFalse(reset.evidence("m5pm1 SSD1677 failed", "boot"))
        self.assertFalse(reset.evidence(boot().replace(reset.BOOT_MARKERS[2], ""), "boot"))
        self.assertFalse(reset.evidence(boot() + "\nESP-ROM:second boot\n", "boot"))
        self.assertFalse(reset.evidence(boot() + "\n*** Booting Zephyr OS\n", "boot"))

    def test_hil_not_confused_with_boot_or_previous_rtc_pass(self):
        self.assertFalse(reset.evidence(boot(), "power-hil"))
        self.assertFalse(reset.evidence("Previous RTC power HIL result: PASS", "power-hil"))
        self.assertTrue(reset.evidence("Power HIL endurance PASS: 50/50", "power-hil"))
        self.assertFalse(reset.evidence("Power HIL endurance PASS\nZEPHYR FATAL ERROR", "power-hil"))

    def test_full_raw_ansi_utf8_and_trailing_line_kept(self):
        text = boot() + "\n\x1b[31m<wrn> 中文\x1b[0m"
        self.assertTrue(reset.evidence(text, "boot"))
        capture = reset.ConsoleCapture(reset.DEFAULT_SERIAL)
        capture.data.extend(text.encode("utf-8"))
        self.assertEqual(capture.text(), text)
        self.assertIn("<wrn>", capture.text())

    def test_jtag_observer_armed_before_reset(self):
        events = []
        captures = []
        def issue(*args):
            events.append("reset")
            captures[0].data.extend(("pro cpu reset by JTAG\n" + boot()).encode())
            return "ok"
        class FakeCapture:
            def __init__(self, *args, **kwargs):
                self.thread = SimpleNamespace(ident=1)
                self.ready = SimpleNamespace(wait=lambda timeout: True)
                self.error = None
                self.data = bytearray()
                captures.append(self)
            def start(self): events.append("observer-start")
            def finish(self): events.append("observer-stop")
            def text(self): return self.data.decode()
        with patch.object(reset, "find_port", return_value="fake"), \
             patch.object(reset, "ConsoleCapture", FakeCapture), \
             patch.object(reset, "locate_openocd", return_value="openocd"), \
             patch.object(reset, "issue_reset", side_effect=issue), \
             redirect_stdout(StringIO()):
            self.assertTrue(reset.capture_and_reset(reset.DEFAULT_SERIAL, timeout=.1, method="jtag"))
        self.assertEqual(events, ["observer-start", "reset", "observer-stop"])

    def test_jtag_requires_markers_after_its_own_boot_epoch(self):
        self.assertFalse(reset.verified_after_reset(boot(), "boot", True))
        self.assertFalse(reset.verified_after_reset(boot() + "\npro cpu reset by JTAG\n", "boot", True))
        self.assertTrue(reset.verified_after_reset("pro cpu reset by JTAG\n" + boot(), "boot", True))

    def test_watchdog_fallback_not_accepted_as_full_reset(self):
        with patch.object(reset, "checked_run", return_value="Hard resetting via RTS pin..."):
            with self.assertRaises(reset.RecoveryError):
                reset.issue_reset("watchdog", reset.DEFAULT_SERIAL, "fake")

    def test_hil_can_wait_for_initially_absent_device_with_full_deadline(self):
        clock = [0.0]
        captures = []
        class DelayedCapture:
            def __init__(self, *args, **kwargs):
                self.thread = SimpleNamespace(ident=1)
                self.ready = SimpleNamespace(wait=lambda timeout: self.fail_wait())
                self.error = None
                self.data = bytearray()
                captures.append(self)
            def fail_wait(self):
                raise AssertionError("Passive HIL must not enforce a five-second initial wait")
            def start(self): pass
            def finish(self): pass
            def text(self): return self.data.decode()
        def tick(seconds):
            clock[0] += seconds
            if clock[0] >= 10:
                captures[0].data[:] = b"Power HIL endurance PASS: 50/50"
        with patch.object(reset, "find_port", return_value=None) as find, \
             patch.object(reset, "ConsoleCapture", DelayedCapture), \
             patch.object(reset.time, "monotonic", side_effect=lambda: clock[0]), \
             patch.object(reset.time, "sleep", side_effect=tick), \
             redirect_stdout(StringIO()):
            self.assertTrue(reset.capture_and_reset(reset.DEFAULT_SERIAL, timeout=20,
                                                    mode="power-hil", reset=False))
            find.assert_not_called()
        self.assertGreaterEqual(clock[0], 10)

    def test_passive_open_does_not_flush_or_toggle(self):
        # These overrides are intentionally no-ops. pyserial's open otherwise
        # calls _reset_input_buffer and discards buffered boot bytes.
        handle = object.__new__(reset.PassiveSerial)
        self.assertIsNone(handle._reset_input_buffer())
        self.assertIsNone(handle._update_dtr_state())
        self.assertIsNone(handle._update_rts_state())
        # Do not invoke pyserial destructor on an intentionally uninitialized object.
        handle.is_open = False


if __name__ == "__main__":
    unittest.main()
