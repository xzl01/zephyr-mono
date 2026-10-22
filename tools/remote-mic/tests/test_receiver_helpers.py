#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Unit tests for the receiver helpers that do not need BLE or audio hardware.

``remote_mic.receiver`` imports bleak and sounddevice lazily, so the module can
be imported here; :class:`FakeSoundDevice` stands in for PortAudio's device
query API.
"""

from __future__ import annotations

import sys
import unittest
from array import array
from pathlib import Path
from types import SimpleNamespace

_TOOL_DIR = Path(__file__).resolve().parents[1]
if str(_TOOL_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOL_DIR))

from remote_mic import protocol, receiver  # noqa: E402


class FakeSoundDevice:
    """Minimal stand-in for the parts of sounddevice the receiver queries."""

    def __init__(self, devices: list[dict], default_output: int = 0) -> None:
        self._devices = devices
        self.default = SimpleNamespace(device=[-1, default_output])

    def query_devices(self) -> list[dict]:
        return self._devices


def _device(
    name: str,
    *,
    outputs: int = 2,
    inputs: int = 0,
    hostapi: int = 0,
) -> dict:
    return {
        "name": name,
        "max_output_channels": outputs,
        "max_input_channels": inputs,
        "hostapi": hostapi,
    }


class DeviceResolutionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.sounddevice = FakeSoundDevice(
            [
                _device("MacBook Pro Speakers", outputs=2),
                _device("BlackHole 2ch", outputs=2),
                _device("BlackHole 16ch", outputs=16),
                _device("Built-in Microphone", outputs=0, inputs=2),
                _device("CABLE Input (VB-Audio Virtual Cable)", outputs=2, hostapi=1),
            ],
            default_output=0,
        )

    def test_default_device(self) -> None:
        self.assertEqual(
            receiver.resolve_output_device(self.sounddevice, None),
            (0, "MacBook Pro Speakers"),
        )
        self.assertEqual(
            receiver.resolve_output_device(self.sounddevice, "default"),
            (0, "MacBook Pro Speakers"),
        )

    def test_index_selection(self) -> None:
        self.assertEqual(
            receiver.resolve_output_device(self.sounddevice, "1"),
            (1, "BlackHole 2ch"),
        )
        self.assertEqual(
            receiver.resolve_output_device(self.sounddevice, 4),
            (4, "CABLE Input (VB-Audio Virtual Cable)"),
        )

    def test_substring_selection_is_case_insensitive(self) -> None:
        self.assertEqual(
            receiver.resolve_output_device(self.sounddevice, "blackhole 2CH"),
            (1, "BlackHole 2ch"),
        )
        self.assertEqual(
            receiver.resolve_output_device(self.sounddevice, "cable input"),
            (4, "CABLE Input (VB-Audio Virtual Cable)"),
        )

    def test_multiple_matches_pick_the_first(self) -> None:
        self.assertEqual(
            receiver.resolve_output_device(self.sounddevice, "blackhole"),
            (1, "BlackHole 2ch"),
        )

    def test_unknown_name_lists_the_remedy(self) -> None:
        with self.assertRaises(receiver.AudioDeviceError) as context:
            receiver.resolve_output_device(self.sounddevice, "snd-aloop")
        self.assertIn("--list-devices", str(context.exception))

    def test_index_out_of_range(self) -> None:
        with self.assertRaises(receiver.AudioDeviceError):
            receiver.resolve_output_device(self.sounddevice, "99")

    def test_input_only_device_is_rejected(self) -> None:
        with self.assertRaises(receiver.AudioDeviceError) as context:
            receiver.resolve_output_device(self.sounddevice, "3")
        self.assertIn("no output channels", str(context.exception))

    def test_default_without_output_channels_is_rejected(self) -> None:
        sounddevice = FakeSoundDevice(
            [_device("Microphone", outputs=0, inputs=1)], default_output=0
        )
        with self.assertRaises(receiver.AudioDeviceError):
            receiver.resolve_output_device(sounddevice, None)

    def test_no_devices(self) -> None:
        with self.assertRaises(receiver.AudioDeviceError):
            receiver.resolve_output_device(FakeSoundDevice([]), None)

    def test_device_table_marks_the_default_output(self) -> None:
        table = receiver.format_device_table(self.sounddevice)
        lines = table.splitlines()
        self.assertEqual(len(lines), 5)
        self.assertTrue(lines[0].startswith("*"))
        self.assertIn("[out]", lines[1])
        self.assertIn("[in]", lines[3])
        self.assertIn("BlackHole 2ch", table)


class EncryptionDetectionTests(unittest.TestCase):
    def test_encryption_failures_are_recognised(self) -> None:
        for message in (
            "Insufficient Encryption",
            "Protocol Error 0x05: Insufficient Authentication",
            "Not Permitted",
            "Characteristic requires an encrypted link",
            "not paired",
        ):
            with self.subTest(message=message):
                self.assertTrue(receiver._looks_like_encryption_error(Exception(message)))

    def test_unrelated_failures_are_not_encryption_errors(self) -> None:
        for message in (
            "device not found",
            "operation already in progress",
            "connection timed out",
        ):
            with self.subTest(message=message):
                self.assertFalse(
                    receiver._looks_like_encryption_error(Exception(message))
                )

    def test_guidance_covers_all_platforms_and_pairing(self) -> None:
        text = receiver.encryption_guidance()
        for fragment in ("macOS", "Windows", "Linux", "pair", "Bluetooth"):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, text)


class ConfigurationTests(unittest.TestCase):
    def test_defaults_match_the_protocol_document(self) -> None:
        config = receiver.ReceiverConfig()
        self.assertEqual(config.name, protocol.DEFAULT_DEVICE_NAME)
        self.assertEqual(config.name, "PaperMono C153")
        self.assertIsNone(config.address)
        self.assertIsNone(config.audio_device)
        self.assertEqual(config.gain_db, 0.0)
        self.assertFalse(config.once)
        self.assertFalse(config.send_mic_open)
        self.assertTrue(config.dc_blocker)
        self.assertEqual(config.ring_ms, 400)

    def test_stream_stats_summary(self) -> None:
        stats = receiver.StreamStats()
        stats.begin_session(3)
        stats.add_frame(240, 12000)
        stats.add_frame(240, 9000)
        stats.session_ended = stats.session_started + 0.03
        self.assertEqual(stats.frames, 2)
        self.assertEqual(stats.samples, 480)
        self.assertEqual(stats.peak, 12000)
        self.assertAlmostEqual(stats.session_seconds, 0.03, places=6)
        summary = stats.session_summary()
        self.assertIn("session 3", summary)
        self.assertIn("2 frames", summary)


class SinkTests(unittest.TestCase):
    def test_ring_buffer_capacity_follows_ring_ms(self) -> None:
        sink = receiver.AudioSink(device=None, sample_rate=16000, ring_ms=250)
        self.assertEqual(sink.ring.capacity, 4000)
        self.assertEqual(sink.underruns, 0)
        self.assertEqual(sink.dropped_samples, 0)

    def test_push_and_underrun_counters(self) -> None:
        sink = receiver.AudioSink(device=None, sample_rate=16000, ring_ms=10)
        sink.push(array("h", [1, 2, 3]))
        self.assertEqual(sink.ring.fill, 3)
        sink.ring.read(160)
        self.assertGreater(sink.underruns, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
