#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Unit tests for the receiver audio helpers (ring buffer, DC blocker, WAV).

Pure standard library: no PortAudio device is opened, so the tests run on any
machine, including CI without audio hardware.
"""

from __future__ import annotations

import math
import sys
import tempfile
import unittest
import wave
from array import array
from pathlib import Path

_TOOL_DIR = Path(__file__).resolve().parents[1]
if str(_TOOL_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOL_DIR))

from remote_mic import audio  # noqa: E402


class RingBufferTests(unittest.TestCase):
    def test_round_trip(self) -> None:
        ring = audio.RingBuffer(8)
        self.assertEqual(ring.write(array("h", [1, 2, 3])), 0)
        self.assertEqual(ring.fill, 3)
        self.assertEqual(list(ring.read(3)), [1, 2, 3])
        self.assertEqual(ring.fill, 0)
        self.assertEqual(ring.underrun_samples, 0)
        self.assertEqual(ring.overrun_samples, 0)

    def test_underrun_returns_silence(self) -> None:
        ring = audio.RingBuffer(8)
        ring.write(array("h", [5, 6]))
        block = ring.read(6)
        self.assertEqual(len(block), 6)
        self.assertEqual(list(block[:2]), [5, 6])
        self.assertEqual(list(block[2:]), [0, 0, 0, 0])
        self.assertEqual(ring.underrun_samples, 4)

    def test_overrun_drops_the_oldest_samples(self) -> None:
        ring = audio.RingBuffer(8)
        dropped = ring.write(array("h", range(12)))
        self.assertEqual(dropped, 4)
        self.assertEqual(ring.overrun_samples, 4)
        self.assertEqual(list(ring.read(8)), list(range(4, 12)))

    def test_write_larger_than_capacity_keeps_the_tail(self) -> None:
        ring = audio.RingBuffer(4)
        dropped = ring.write(array("h", range(10)))
        self.assertEqual(dropped, 6)
        self.assertEqual(list(ring.read(4)), [6, 7, 8, 9])

    def test_wraparound_read_write(self) -> None:
        ring = audio.RingBuffer(8)
        ring.write(array("h", [1, 2, 3, 4, 5]))
        self.assertEqual(list(ring.read(3)), [1, 2, 3])
        ring.write(array("h", [6, 7, 8, 9, 10, 11, 12]))
        # Two samples (4, 5) were still buffered, so writing seven more needs
        # one sample of room: the oldest (4) is dropped and the ring wraps.
        self.assertEqual(ring.overrun_samples, 1)
        self.assertEqual(list(ring.read(8)), [5, 6, 7, 8, 9, 10, 11, 12])

    def test_empty_and_zero_reads(self) -> None:
        ring = audio.RingBuffer(4)
        self.assertEqual(ring.write(array("h")), 0)
        self.assertEqual(len(ring.read(0)), 0)

    def test_clear_and_reset_counters(self) -> None:
        ring = audio.RingBuffer(4)
        ring.write(array("h", range(8)))
        ring.read(4)
        ring.clear()
        self.assertEqual(ring.fill, 0)
        self.assertEqual(ring.overrun_samples, 4)
        ring.reset_counters()
        self.assertEqual(ring.overrun_samples, 0)
        self.assertEqual(ring.underrun_samples, 0)

    def test_invalid_capacity(self) -> None:
        with self.assertRaises(ValueError):
            audio.RingBuffer(0)
        with self.assertRaises(ValueError):
            audio.RingBuffer(-1)

    def test_default_ring_holds_400_ms(self) -> None:
        self.assertEqual(
            int(audio.SAMPLE_RATE * audio.DEFAULT_RING_MS / 1000), 6400
        )


class GainTests(unittest.TestCase):
    def test_db_to_linear(self) -> None:
        self.assertAlmostEqual(audio.db_to_linear(0.0), 1.0)
        self.assertAlmostEqual(audio.db_to_linear(20.0), 10.0)
        self.assertAlmostEqual(audio.db_to_linear(-6.0), 0.501187, places=6)

    def test_clamp_gain_db(self) -> None:
        self.assertEqual(audio.clamp_gain_db(0.0), 0.0)
        self.assertEqual(audio.clamp_gain_db(99.0), audio.GAIN_DB_MAX)
        self.assertEqual(audio.clamp_gain_db(-99.0), audio.GAIN_DB_MIN)

    def test_clamp_rejects_non_numbers(self) -> None:
        with self.assertRaises(ValueError):
            audio.clamp_gain_db("loud")
        with self.assertRaises(ValueError):
            audio.clamp_gain_db(float("nan"))

    def test_gain_multiplies(self) -> None:
        out = audio.process_block(array("h", [1000, -1000]), gain_db=20.0)
        self.assertEqual(list(out), [10000, -10000])

    def test_gain_clips_to_int16(self) -> None:
        out = audio.process_block(array("h", [10000, -10000]), gain_db=20.0)
        self.assertEqual(list(out), [32767, -32768])

    def test_unity_gain_returns_the_input_block(self) -> None:
        block = array("h", [1, -2, 3])
        self.assertEqual(list(audio.process_block(block)), [1, -2, 3])


class DcBlockerTests(unittest.TestCase):
    def test_constant_offset_decays_to_zero(self) -> None:
        blocker = audio.DcBlocker()
        block = blocker.process(array("h", [2000] * 16000))
        self.assertLess(abs(block[-1]), 10)
        self.assertGreater(block[0], 1900)
        self.assertLess(abs(block[4000]), abs(block[100]))

    def test_sine_passes_through(self) -> None:
        samples = array(
            "h",
            (
                int(8000 * math.sin(2.0 * math.pi * 440.0 * index / audio.SAMPLE_RATE))
                for index in range(audio.SAMPLE_RATE // 2)
            ),
        )
        blocker = audio.DcBlocker()
        filtered = blocker.process(samples)
        tail = 2000  # skip the filter transient
        rms_in = math.sqrt(
            sum(float(value) ** 2 for value in samples[tail:]) / (len(samples) - tail)
        )
        rms_out = math.sqrt(
            sum(float(value) ** 2 for value in filtered[tail:])
            / (len(filtered) - tail)
        )
        self.assertGreater(rms_out / rms_in, 0.9)
        self.assertLess(rms_out / rms_in, 1.1)

    def test_reset_clears_state(self) -> None:
        blocker = audio.DcBlocker()
        blocker.process(array("h", [2000] * 100))
        blocker.reset()
        self.assertEqual(list(blocker.process(array("h", [0, 0]))), [0, 0])

    def test_coefficient_is_stable(self) -> None:
        blocker = audio.DcBlocker(cutoff_hz=40.0, sample_rate=16000)
        self.assertGreater(blocker.coefficient, 0.9)
        self.assertLess(blocker.coefficient, 1.0)

    def test_invalid_sample_rate(self) -> None:
        with self.assertRaises(ValueError):
            audio.DcBlocker(sample_rate=0)


class WavWriterTests(unittest.TestCase):
    def test_round_trip(self) -> None:
        samples = array("h", (index % 1000 - 500 for index in range(1600)))
        with tempfile.TemporaryDirectory(prefix="papermono-mic-test-") as tmp:
            path = Path(tmp) / "nested" / "capture.wav"
            writer = audio.WavWriter(path)
            writer.open()
            writer.write(samples)
            writer.close()
            self.assertEqual(writer.samples_written, 1600)
            self.assertTrue(path.exists())

            with wave.open(str(path), "rb") as handle:
                self.assertEqual(handle.getnchannels(), 1)
                self.assertEqual(handle.getsampwidth(), 2)
                self.assertEqual(handle.getframerate(), audio.SAMPLE_RATE)
                self.assertEqual(handle.getnframes(), 1600)
                raw = handle.readframes(handle.getnframes())

        stored = array("h")
        stored.frombytes(raw)
        if sys.byteorder != "little":
            stored.byteswap()
        self.assertEqual(list(stored), list(samples))

    def test_close_is_idempotent_and_write_after_close_is_ignored(self) -> None:
        with tempfile.TemporaryDirectory(prefix="papermono-mic-test-") as tmp:
            writer = audio.WavWriter(Path(tmp) / "capture.wav")
            writer.open()
            writer.write(array("h", [1, 2, 3]))
            writer.close()
            writer.close()
            writer.write(array("h", [4, 5, 6]))
            self.assertEqual(writer.samples_written, 3)

    def test_context_manager(self) -> None:
        with tempfile.TemporaryDirectory(prefix="papermono-mic-test-") as tmp:
            path = Path(tmp) / "capture.wav"
            with audio.WavWriter(path) as writer:
                writer.write(array("h", [7, 8]))
            with wave.open(str(path), "rb") as handle:
                self.assertEqual(handle.getnframes(), 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
