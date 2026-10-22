# SPDX-License-Identifier: GPL-3.0-only
"""Audio plumbing for the PaperMono receiver (standard library only).

The receiver decodes 240 samples every 15 ms on the asyncio/BLE thread while
PortAudio pulls blocks on its own callback thread. A bounded ring buffer
absorbs the jitter; on underrun the buffer returns silence instead of blocking
or emitting stale samples, which keeps the virtual microphone alive when the
link hiccups or the device stops between sessions.

Nothing here imports ``sounddevice``: :class:`RingBuffer`, :class:`DcBlocker`,
:func:`process_block` and :class:`WavWriter` are exercised by unit tests
without audio hardware.
"""

from __future__ import annotations

import math
import sys
import threading
import wave
from array import array
from pathlib import Path

__all__ = [
    "SAMPLE_RATE",
    "DEFAULT_RING_MS",
    "GAIN_DB_MIN",
    "GAIN_DB_MAX",
    "DcBlocker",
    "RingBuffer",
    "WavWriter",
    "clamp_gain_db",
    "db_to_linear",
    "process_block",
]

SAMPLE_RATE = 16000
DEFAULT_RING_MS = 400

#: Gain is clamped so a mistyped ``--gain-db`` cannot destroy the stream.
GAIN_DB_MIN = -24.0
GAIN_DB_MAX = 24.0

INT16_MIN = -32768
INT16_MAX = 32767


def clamp_gain_db(gain_db: float) -> float:
    """Clamp a requested gain into the supported range."""

    try:
        value = float(gain_db)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"gain must be a number, got {gain_db!r}") from exc
    if math.isnan(value):
        raise ValueError("gain must not be NaN")
    return max(GAIN_DB_MIN, min(GAIN_DB_MAX, value))


def db_to_linear(gain_db: float) -> float:
    """Convert decibels to a linear multiplier."""

    return 10.0 ** (float(gain_db) / 20.0)


def _clip16(value: float) -> int:
    if value >= INT16_MAX:
        return INT16_MAX
    if value <= INT16_MIN:
        return INT16_MIN
    return int(round(value))


class RingBuffer:
    """Bounded single-producer/single-consumer buffer of int16 samples.

    :meth:`read` always returns exactly ``count`` samples: missing input is
    replaced by silence and counted in :attr:`underrun_samples`. :meth:`write`
    drops the oldest samples when the buffer is full and counts them in
    :attr:`overrun_samples`; it never blocks the BLE thread.
    """

    def __init__(self, capacity_samples: int) -> None:
        if capacity_samples <= 0:
            raise ValueError("ring capacity must be positive")
        self._capacity = int(capacity_samples)
        self._buf = array("h", bytes(2 * self._capacity))
        self._read_index = 0
        self._fill = 0
        self._lock = threading.Lock()
        self._underrun = 0
        self._overrun = 0

    @property
    def capacity(self) -> int:
        return self._capacity

    @property
    def fill(self) -> int:
        with self._lock:
            return self._fill

    @property
    def underrun_samples(self) -> int:
        with self._lock:
            return self._underrun

    @property
    def overrun_samples(self) -> int:
        with self._lock:
            return self._overrun

    def reset_counters(self) -> None:
        with self._lock:
            self._underrun = 0
            self._overrun = 0

    def clear(self) -> None:
        with self._lock:
            self._read_index = 0
            self._fill = 0

    def write(self, samples: array | bytes | bytearray | list[int]) -> int:
        """Write samples, dropping the oldest ones if needed.

        Returns the number of samples that were dropped to make room.
        """

        if not isinstance(samples, array):
            values = array("h", samples)
        else:
            values = samples
        count = len(values)
        if count == 0:
            return 0

        with self._lock:
            dropped = 0
            if count > self._capacity:
                extra = count - self._capacity
                values = values[extra:]
                count = self._capacity
                dropped += extra
                self._overrun += extra

            free = self._capacity - self._fill
            if count > free:
                shrink = count - free
                self._read_index = (self._read_index + shrink) % self._capacity
                self._fill -= shrink
                dropped += shrink
                self._overrun += shrink

            write_at = (self._read_index + self._fill) % self._capacity
            first = min(count, self._capacity - write_at)
            self._buf[write_at:write_at + first] = values[:first]
            if count > first:
                self._buf[:count - first] = values[first:]
            self._fill += count
            return dropped

    def read(self, count: int) -> array:
        """Read exactly ``count`` samples, padded with silence."""

        if count <= 0:
            return array("h")
        out = array("h", bytes(2 * count))
        with self._lock:
            available = min(count, self._fill)
            if available:
                first = min(available, self._capacity - self._read_index)
                out[:first] = self._buf[
                    self._read_index:self._read_index + first
                ]
                if available > first:
                    out[first:available] = self._buf[:available - first]
                self._read_index = (self._read_index + available) % self._capacity
                self._fill -= available
            if available < count:
                self._underrun += count - available
        return out


class DcBlocker:
    """One-pole high-pass filter that removes the ADPCM DC bias.

    ``y[n] = x[n] - x[n-1] + R * y[n-1]`` with ``R`` derived from the cutoff
    frequency. The decoder can leave a small DC offset after a lost frame, and
    some voice front ends treat that as a constant bias.
    """

    def __init__(self, cutoff_hz: float = 40.0, sample_rate: int = SAMPLE_RATE) -> None:
        if sample_rate <= 0:
            raise ValueError("sample rate must be positive")
        coefficient = 1.0 - (2.0 * math.pi * float(cutoff_hz) / float(sample_rate))
        self._r = max(0.0, min(0.9999, coefficient))
        self._previous_input = 0.0
        self._previous_output = 0.0

    @property
    def coefficient(self) -> float:
        return self._r

    def reset(self) -> None:
        self._previous_input = 0.0
        self._previous_output = 0.0

    def process(self, samples: array | list[int]) -> array:
        """Filter samples in place into a new ``array('h')``."""

        out = array("h", bytes(2 * len(samples)))
        previous_input = self._previous_input
        previous_output = self._previous_output
        r = self._r
        for index, sample in enumerate(samples):
            value = float(sample)
            output = value - previous_input + r * previous_output
            previous_input = value
            previous_output = output
            out[index] = _clip16(output)
        self._previous_input = previous_input
        self._previous_output = previous_output
        return out


def process_block(
    samples: array,
    *,
    gain_db: float = 0.0,
    dc_blocker: DcBlocker | None = None,
) -> array:
    """Apply the optional DC blocker and gain to one decoded block.

    The DC blocker runs first so gain cannot amplify a constant offset.
    ``gain_db`` is clamped by :func:`clamp_gain_db` and every sample is clipped
    to the signed 16-bit range.
    """

    block = samples
    if dc_blocker is not None:
        block = dc_blocker.process(block)

    gain = db_to_linear(clamp_gain_db(gain_db))
    if abs(gain - 1.0) < 1e-9:
        return block

    out = array("h", bytes(2 * len(block)))
    for index, sample in enumerate(block):
        out[index] = _clip16(sample * gain)
    return out


class WavWriter:
    """Mono 16-bit WAV capture used for hardware verification.

    Writing happens on the BLE thread in 15 ms blocks (480 bytes), so a plain
    buffered file object is enough; the lock only protects the close path.
    """

    def __init__(self, path: str | Path, sample_rate: int = SAMPLE_RATE) -> None:
        self._path = Path(path)
        self._sample_rate = int(sample_rate)
        self._wave: wave.Wave_write | None = None
        self._lock = threading.Lock()
        self._samples = 0

    @property
    def path(self) -> Path:
        return self._path

    @property
    def samples_written(self) -> int:
        return self._samples

    def open(self) -> None:
        self._path.parent.mkdir(parents=True, exist_ok=True)
        handle = wave.open(str(self._path), "wb")
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(self._sample_rate)
        self._wave = handle

    def write(self, samples: array) -> None:
        with self._lock:
            if self._wave is None:
                return
            block = samples
            if sys.byteorder != "little":
                block = array("h", samples)
                block.byteswap()
            self._wave.writeframes(block.tobytes())
            self._samples += len(samples)

    def close(self) -> None:
        with self._lock:
            if self._wave is not None:
                self._wave.close()
                self._wave = None

    def __enter__(self) -> "WavWriter":
        self.open()
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()
