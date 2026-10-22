# SPDX-License-Identifier: GPL-3.0-only
"""ATVV v1.0 voice protocol used by the PaperMono C153 BLE remote.

Everything here is pure Python (standard library only). Importing this module
never pulls in ``bleak`` or ``sounddevice``, so the unit tests and the offline
``--selftest`` path work on machines without Bluetooth or PortAudio.

Wire summary (see ``docs/remote-mic-protocol.md`` for the authoritative spec):

* GATT service ``AB5E0001-...`` with Transmit (host -> device, write),
  Audio (device -> host, notify) and Control (device -> host, notify).
* Capabilities: host writes ``0A 01 00 00 03 03`` (GET_CAPS); the device
  notifies ``0B 01 00 02 03 00 78 00 00`` (version 1.0, ADPCM 16 kHz,
  push-to-talk, 120-byte frames).
* Session events: ``04 00 02 <id>`` (AUDIO_START), ``0A 02 <seq> <pred>
  <step>`` (AUDIO_SYNC), ``00 <reason>`` (AUDIO_STOP) and ``0C <err>``
  (MIC_OPEN_ERROR).
* Audio: 120-byte IMA/DVI ADPCM frames, four bits per sample, high nibble
  first, one continuous predictor across frames, 16 kHz mono.
"""

from __future__ import annotations

from array import array
from dataclasses import dataclass
from enum import IntEnum
from typing import Final, Sequence

__all__ = [
    "AUDIO_UUID",
    "CONTROL_UUID",
    "DEFAULT_DEVICE_NAME",
    "FRAME_BYTES",
    "FRAME_MS",
    "FRAME_SAMPLES",
    "MAX_SESSION_SECONDS",
    "SAMPLE_RATE_16K",
    "SAMPLE_RATE_8K",
    "SERVICE_UUID",
    "TX_UUID",
    "AdpcmDecoder",
    "AdpcmEncoder",
    "AudioStart",
    "AudioStop",
    "AudioSync",
    "Caps",
    "ControlEvent",
    "Event",
    "FrameAccumulator",
    "MicOpenError",
    "Opcode",
    "ProtocolError",
    "UnknownEvent",
    "build_get_caps",
    "build_mic_close",
    "build_mic_extend",
    "build_mic_open",
    "codec_name",
    "codec_sample_rate",
    "describe_event",
    "parse_caps",
    "parse_control_event",
]

SERVICE_UUID: Final = "ab5e0001-5a21-4f05-bc7d-af01f617b664"
TX_UUID: Final = "ab5e0002-5a21-4f05-bc7d-af01f617b664"
AUDIO_UUID: Final = "ab5e0003-5a21-4f05-bc7d-af01f617b664"
CONTROL_UUID: Final = "ab5e0004-5a21-4f05-bc7d-af01f617b664"

DEFAULT_DEVICE_NAME: Final = "PaperMono C153"

SAMPLE_RATE_16K: Final = 16000
SAMPLE_RATE_8K: Final = 8000
FRAME_BYTES: Final = 120
FRAME_SAMPLES: Final = FRAME_BYTES * 2
FRAME_MS: Final = 15.0
MAX_SESSION_SECONDS: Final = 60.0

#: Codec identifiers carried in capabilities, AUDIO_START and AUDIO_SYNC.
CODEC_ADPCM_8K: Final = 0x01
CODEC_ADPCM_16K: Final = 0x02

#: ATVV interaction model advertised by the device (push-to-talk).
INTERACTION_PUSH_TO_TALK: Final = 0x03


class Opcode(IntEnum):
    """Host -> device commands written to the Transmit characteristic."""

    GET_CAPS = 0x0A
    MIC_OPEN = 0x0C
    MIC_CLOSE = 0x0D
    MIC_EXTEND = 0x0E


class Event(IntEnum):
    """Device -> host notifications on the Control characteristic."""

    AUDIO_STOP = 0x00
    AUDIO_START = 0x04
    AUDIO_SYNC = 0x0A
    CAPABILITIES = 0x0B
    MIC_OPEN_ERROR = 0x0C


class ProtocolError(ValueError):
    """Raised when a packet cannot be interpreted as a valid ATVV message."""


def codec_sample_rate(codec: int) -> int | None:
    """Return the sample rate for an ATVV codec id, or ``None`` if unknown."""

    return {
        CODEC_ADPCM_16K: SAMPLE_RATE_16K,
        CODEC_ADPCM_8K: SAMPLE_RATE_8K,
    }.get(codec)


def codec_name(codec: int) -> str:
    """Human readable codec name for logs and error messages."""

    return {
        CODEC_ADPCM_16K: "ADPCM 16 kHz",
        CODEC_ADPCM_8K: "ADPCM 8 kHz",
    }.get(codec, f"unknown codec 0x{codec:02x}")


@dataclass(frozen=True)
class Caps:
    """Decoded ``0B`` capabilities event."""

    version_major: int
    version_minor: int
    codec: int
    interaction_model: int
    frame_size: int
    reserved: bytes

    @property
    def version(self) -> str:
        return f"{self.version_major}.{self.version_minor}"

    @property
    def sample_rate(self) -> int | None:
        return codec_sample_rate(self.codec)

    @property
    def frame_samples(self) -> int:
        """Samples per frame: two four-bit samples per encoded byte."""

        return self.frame_size * 2

    @property
    def frame_duration_ms(self) -> float:
        rate = self.sample_rate
        return 0.0 if not rate else self.frame_samples * 1000.0 / rate

    @property
    def is_supported(self) -> bool:
        """True when this receiver can decode the advertised stream."""

        return self.codec in (CODEC_ADPCM_16K, CODEC_ADPCM_8K) and self.frame_size > 0


@dataclass(frozen=True)
class AudioStart:
    """``04 00 <codec> <session-id>``: a new voice session began."""

    reason: int
    codec: int
    session_id: int


@dataclass(frozen=True)
class AudioSync:
    """``0A 02 <seq> <predictor> <step-index>``: decoder resynchronisation.

    The fields describe the state *before* the next encoded frame, so the host
    drops any partial frame and resets its predictor and step index.
    """

    codec: int
    seq: int
    predictor: int
    step_index: int


@dataclass(frozen=True)
class AudioStop:
    """``00 <reason>``: the device ended the current session."""

    reason: int


@dataclass(frozen=True)
class MicOpenError:
    """``0C <error-hi> <error-lo>``: the device refused a session start."""

    code: int

    @property
    def description(self) -> str:
        return {
            0x0001: "link is not encrypted",
            0x0002: "host has not subscribed to Audio and Control",
            0x0003: "device or microphone unavailable",
        }.get(self.code, f"unknown error 0x{self.code:04x}")


@dataclass(frozen=True)
class UnknownEvent:
    """A well-formed notification this receiver does not interpret."""

    opcode: int
    payload: bytes


ControlEvent = (
    Caps | AudioStart | AudioSync | AudioStop | MicOpenError | UnknownEvent
)


def _require(payload: bytes, minimum: int, what: str) -> None:
    if len(payload) < minimum:
        raise ProtocolError(
            f"{what} needs at least {minimum} bytes, got {len(payload)}"
        )


def parse_caps(payload: bytes) -> Caps:
    """Parse a capabilities notification.

    Raises :class:`ProtocolError` for a truncated packet, a wrong opcode or an
    impossible frame size. An unknown codec is *not* an error: the caller can
    still log it, and :attr:`Caps.is_supported` reports the mismatch.
    """

    _require(payload, 9, "capabilities event")
    if payload[0] != Event.CAPABILITIES:
        raise ProtocolError(
            f"expected capabilities opcode 0x0b, got 0x{payload[0]:02x}"
        )
    frame_size = (payload[5] << 8) | payload[6]
    if frame_size == 0:
        raise ProtocolError("capabilities frame size must not be zero")
    if frame_size > 512:
        raise ProtocolError(f"capabilities frame size {frame_size} is implausible")
    return Caps(
        version_major=payload[1],
        version_minor=payload[2],
        codec=payload[3],
        interaction_model=payload[4],
        frame_size=frame_size,
        reserved=bytes(payload[7:9]),
    )


def parse_control_event(payload: bytes) -> ControlEvent:
    """Dispatch one Control notification to a typed event object."""

    if not payload:
        raise ProtocolError("empty control notification")
    opcode = payload[0]
    if opcode == Event.CAPABILITIES:
        return parse_caps(payload)
    if opcode == Event.AUDIO_START:
        _require(payload, 4, "AUDIO_START")
        return AudioStart(
            reason=payload[1], codec=payload[2], session_id=payload[3]
        )
    if opcode == Event.AUDIO_SYNC:
        _require(payload, 7, "AUDIO_SYNC")
        step_index = payload[6]
        if step_index > 88:
            raise ProtocolError(f"AUDIO_SYNC step index {step_index} exceeds 88")
        return AudioSync(
            codec=payload[1],
            seq=(payload[2] << 8) | payload[3],
            predictor=int.from_bytes(payload[4:6], "big", signed=True),
            step_index=step_index,
        )
    if opcode == Event.AUDIO_STOP:
        _require(payload, 2, "AUDIO_STOP")
        return AudioStop(reason=payload[1])
    if opcode == Event.MIC_OPEN_ERROR:
        _require(payload, 3, "MIC_OPEN_ERROR")
        return MicOpenError(code=int.from_bytes(payload[1:3], "big"))
    return UnknownEvent(opcode=opcode, payload=bytes(payload))


def describe_event(event: ControlEvent) -> str:
    """One-line description used by the receiver log."""

    if isinstance(event, Caps):
        return (
            f"capabilities v{event.version} {codec_name(event.codec)} "
            f"frame={event.frame_size}B ({event.frame_duration_ms:.1f} ms) "
            f"interaction=0x{event.interaction_model:02x}"
        )
    if isinstance(event, AudioStart):
        return (
            f"AUDIO_START session={event.session_id} "
            f"{codec_name(event.codec)} reason={event.reason}"
        )
    if isinstance(event, AudioSync):
        return (
            f"AUDIO_SYNC seq={event.seq} predictor={event.predictor} "
            f"step={event.step_index}"
        )
    if isinstance(event, AudioStop):
        return f"AUDIO_STOP reason={event.reason}"
    if isinstance(event, MicOpenError):
        return f"MIC_OPEN_ERROR 0x{event.code:04x} ({event.description})"
    return f"unknown control event 0x{event.opcode:02x} ({len(event.payload)} bytes)"


# --- host commands -------------------------------------------------------


def build_get_caps() -> bytes:
    """``0A 01 00 00 03 03``: request the capabilities event."""

    return bytes((Opcode.GET_CAPS, 0x01, 0x00, 0x00, 0x03, 0x03))


def build_mic_open(codec: int | None = None) -> bytes:
    """``0C 00`` (ATVV v1.0) or ``0C 00 <codec>`` (legacy v0.4 form)."""

    if codec is None:
        return bytes((Opcode.MIC_OPEN, 0x00))
    return bytes((Opcode.MIC_OPEN, 0x00, codec & 0xFF))


def build_mic_close(session_id: int = 0) -> bytes:
    """``0D <session-id>``: end the current session (idempotent)."""

    return bytes((Opcode.MIC_CLOSE, session_id & 0xFF))


def build_mic_extend(session_id: int = 0) -> bytes:
    """``0E <session-id>``: best-effort keep-alive; the 60 s cap still applies."""

    return bytes((Opcode.MIC_EXTEND, session_id & 0xFF))


# --- IMA/DVI ADPCM -------------------------------------------------------

#: IMA/DVI ADPCM quantisation step table (89 entries).
STEP_TABLE: Final = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2494, 2740, 3008, 3307, 3638, 4002,
    4402, 4842, 5327, 5860, 6446, 7091, 7800, 8580, 9438, 10382,
    11420, 12562, 13818, 15200, 16720, 18392, 20231, 22254, 24479,
    26927, 29620, 32767,
)

#: Index adjustment per nibble: the sign bit is masked off first.
INDEX_TABLE: Final = (-1, -1, -1, -1, 2, 4, 6, 8)

STEP_INDEX_MAX: Final = 88
PREDICTOR_MIN: Final = -32768
PREDICTOR_MAX: Final = 32767


def _clamp_step_index(value: int) -> int:
    if value < 0:
        return 0
    if value > STEP_INDEX_MAX:
        return STEP_INDEX_MAX
    return value


def _clamp_predictor(value: int) -> int:
    if value < PREDICTOR_MIN:
        return PREDICTOR_MIN
    if value > PREDICTOR_MAX:
        return PREDICTOR_MAX
    return value


class AdpcmDecoder:
    """Stateful IMA/DVI ADPCM decoder with one predictor across frames.

    Call :meth:`reset` when the stream starts and on every ``AUDIO_SYNC`` with
    the predictor and step index carried by that event. Never reset per frame:
    the device keeps its encoder state continuous across frame boundaries.
    """

    __slots__ = ("predictor", "step_index")

    def __init__(self, predictor: int = 0, step_index: int = 0) -> None:
        self.reset(predictor, step_index)

    def reset(self, predictor: int = 0, step_index: int = 0) -> None:
        self.predictor = _clamp_predictor(int(predictor))
        self.step_index = _clamp_step_index(int(step_index))

    def decode_nibble(self, nibble: int) -> int:
        """Decode one four-bit nibble and advance the state."""

        nibble &= 0x0F
        step = STEP_TABLE[self.step_index]
        difference = step >> 3
        if nibble & 4:
            difference += step
        if nibble & 2:
            difference += step >> 1
        if nibble & 1:
            difference += step >> 2

        if nibble & 8:
            self.predictor -= difference
        else:
            self.predictor += difference
        self.predictor = _clamp_predictor(self.predictor)
        self.step_index = _clamp_step_index(
            self.step_index + INDEX_TABLE[nibble & 7]
        )
        return self.predictor

    def decode(self, data: bytes | bytearray | memoryview) -> array:
        """Decode encoded bytes into an ``array('h')`` of samples.

        Two samples per byte, high nibble first.
        """

        count = len(data)
        # Two samples per encoded byte, two bytes per sample.
        out = array("h", bytes(4 * count))
        index = 0
        for byte in data:
            out[index] = self.decode_nibble(byte >> 4)
            out[index + 1] = self.decode_nibble(byte & 0x0F)
            index += 2
        return out


class AdpcmEncoder:
    """Reference encoder used by the round-trip test and ``--selftest``.

    The device only encodes; this mirror exists so the host can verify its
    decoder against the exact wire format without copying a GPL implementation.
    """

    __slots__ = ("predictor", "step_index")

    def __init__(self, predictor: int = 0, step_index: int = 0) -> None:
        self.reset(predictor, step_index)

    def reset(self, predictor: int = 0, step_index: int = 0) -> None:
        self.predictor = _clamp_predictor(int(predictor))
        self.step_index = _clamp_step_index(int(step_index))

    def encode_nibble(self, sample: int) -> int:
        """Encode one signed 16-bit sample into a four-bit nibble."""

        step = STEP_TABLE[self.step_index]
        difference = int(sample) - self.predictor
        nibble = 0
        if difference < 0:
            nibble = 8
            difference = -difference

        quantized = step >> 3
        if difference >= step:
            nibble |= 4
            difference -= step
            quantized += step
        shifted = step >> 1
        if difference >= shifted:
            nibble |= 2
            difference -= shifted
            quantized += shifted
        shifted >>= 1
        if difference >= shifted:
            nibble |= 1
            quantized += shifted

        if nibble & 8:
            self.predictor -= quantized
        else:
            self.predictor += quantized
        self.predictor = _clamp_predictor(self.predictor)
        self.step_index = _clamp_step_index(
            self.step_index + INDEX_TABLE[nibble & 7]
        )
        return nibble

    def encode(self, samples: Sequence[int]) -> bytes:
        """Encode samples; the count must be even and non-zero."""

        count = len(samples)
        if count == 0:
            return b""
        if count % 2:
            raise ValueError("IMA ADPCM encodes two samples per byte")
        out = bytearray(count // 2)
        for index in range(0, count, 2):
            high = self.encode_nibble(samples[index])
            low = self.encode_nibble(samples[index + 1])
            out[index // 2] = (high << 4) | low
        return bytes(out)


class FrameAccumulator:
    """Reassemble fixed-size ADPCM frames from arbitrary notification chunks.

    The device notifies one 120-byte frame per packet once the ATT MTU is at
    least 123, but falls back to ``MTU - 3`` chunks on smaller links (for
    example 20-byte notifications on a default-MTU connection). Feeding every
    notification through this accumulator keeps decoding independent of how
    the stack fragments the stream.

    :meth:`reset` drops a partial frame; the receiver calls it on
    ``AUDIO_SYNC`` and ``AUDIO_START`` as required by the protocol.
    """

    __slots__ = ("_frame_size", "_buf", "_frames", "_discarded")

    def __init__(self, frame_size: int = FRAME_BYTES) -> None:
        if frame_size <= 0:
            raise ValueError("frame size must be positive")
        self._frame_size = frame_size
        self._buf = bytearray()
        self._frames = 0
        self._discarded = 0

    @property
    def frame_size(self) -> int:
        return self._frame_size

    @property
    def pending(self) -> int:
        """Bytes buffered towards the next complete frame."""

        return len(self._buf)

    @property
    def frames(self) -> int:
        """Total complete frames produced since construction."""

        return self._frames

    @property
    def discarded_bytes(self) -> int:
        """Bytes dropped by :meth:`reset` (partial frames)."""

        return self._discarded

    def reset(self) -> int:
        """Drop any partial frame and return the number of discarded bytes."""

        discarded = len(self._buf)
        self._buf.clear()
        self._discarded += discarded
        return discarded

    def feed(self, chunk: bytes | bytearray | memoryview) -> list[bytes]:
        """Append a notification chunk and return every complete frame."""

        if not chunk:
            return []
        self._buf.extend(chunk)
        size = self._frame_size
        frames: list[bytes] = []
        while len(self._buf) >= size:
            frames.append(bytes(self._buf[:size]))
            del self._buf[:size]
        self._frames += len(frames)
        return frames
