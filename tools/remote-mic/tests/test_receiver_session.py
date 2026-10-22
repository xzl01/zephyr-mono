#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Session-level tests for :class:`remote_mic.receiver.VoiceReceiver`.

The receiver's notification handlers are pure state machines: they only need a
sink object, so a real :class:`~remote_mic.receiver.AudioSink` is used without
opening PortAudio. This exercises capability parsing, session start/stop,
``AUDIO_SYNC`` resynchronisation, sequence-gap accounting, chunked
notifications and WAV capture without any Bluetooth or audio hardware.
"""

from __future__ import annotations

import sys
import tempfile
import unittest
import wave
from pathlib import Path

_TOOL_DIR = Path(__file__).resolve().parents[1]
if str(_TOOL_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOL_DIR))

from remote_mic import audio, protocol, receiver  # noqa: E402

CAPS = bytes.fromhex("0B0100020300780000")
AUDIO_START = bytes.fromhex("04000200")
SYNC_ZERO = bytes.fromhex("0A020000000000")
AUDIO_STOP = bytes.fromhex("0000")
SILENT_FRAME = bytes(protocol.FRAME_BYTES)


def _make_receiver(
    *,
    wav: audio.WavWriter | None = None,
    gain_db: float = 0.0,
    dc_blocker: bool = False,
) -> tuple[receiver.VoiceReceiver, receiver.AudioSink]:
    sink = receiver.AudioSink(device=None, sample_rate=16000, ring_ms=100)
    config = receiver.ReceiverConfig(
        gain_db=gain_db, dc_blocker=dc_blocker, stats_interval=3600.0
    )
    return receiver.VoiceReceiver(config, sink, wav), sink


class SessionTests(unittest.TestCase):
    def test_capabilities_event_is_recorded(self) -> None:
        voice, sink = _make_receiver()
        voice._on_control(None, bytearray(CAPS))
        self.assertIsNotNone(voice._caps)
        assert voice._caps is not None
        self.assertEqual(voice._caps.frame_size, 120)
        self.assertTrue(voice._stream_supported)
        self.assertEqual(sink.ring.fill, 0)

    def test_session_start_resets_decoder_and_counters(self) -> None:
        voice, _sink = _make_receiver()
        voice._on_control(None, bytearray(CAPS))
        voice._on_control(None, bytearray(AUDIO_START))
        self.assertTrue(voice._session_active)
        self.assertEqual(voice._stats.sessions, 1)
        self.assertEqual(voice._expected_seq, 0)
        self.assertEqual(voice._decoder.predictor, 0)
        self.assertEqual(voice._decoder.step_index, 0)

    def test_one_frame_arrives_in_six_chunks(self) -> None:
        voice, sink = _make_receiver()
        voice._on_control(None, bytearray(CAPS))
        voice._on_control(None, bytearray(AUDIO_START))
        for offset in range(0, protocol.FRAME_BYTES, 20):
            voice._on_audio(
                None, bytearray(SILENT_FRAME[offset:offset + 20])
            )
        self.assertEqual(voice.stats.frames, 1)
        self.assertEqual(voice.stats.samples, protocol.FRAME_SAMPLES)
        self.assertEqual(voice.stats.gaps, 0)
        self.assertEqual(voice._accumulator.pending, 0)
        self.assertEqual(sink.ring.fill, protocol.FRAME_SAMPLES)

    def test_two_frames_in_one_notification(self) -> None:
        voice, sink = _make_receiver()
        voice._on_control(None, bytearray(AUDIO_START))
        voice._on_audio(None, bytearray(SILENT_FRAME * 2))
        self.assertEqual(voice.stats.frames, 2)
        self.assertEqual(sink.ring.fill, protocol.FRAME_SAMPLES * 2)

    def test_sync_drops_partial_frame_and_resets_decoder(self) -> None:
        voice, sink = _make_receiver()
        voice._on_control(None, bytearray(AUDIO_START))
        voice._on_audio(None, bytearray(SILENT_FRAME[:60]))
        self.assertEqual(voice._accumulator.pending, 60)

        # predictor=1000, step=0, seq=0 (the device restarts frame 0)
        sync = bytes.fromhex("0A02000003E800")
        voice._on_control(None, bytearray(sync))
        self.assertEqual(voice._accumulator.pending, 0)
        self.assertEqual(voice._accumulator.discarded_bytes, 60)
        self.assertEqual(voice._decoder.predictor, 1000)
        self.assertEqual(voice._expected_seq, 0)
        self.assertEqual(voice.stats.gaps, 0)

        voice._on_audio(None, bytearray(SILENT_FRAME))
        self.assertEqual(voice.stats.frames, 1)
        # 0x00 nibbles keep the predictor at 1000, so every sample is 1000.
        block = sink.ring.read(protocol.FRAME_SAMPLES)
        self.assertTrue(all(sample == 1000 for sample in block))

    def test_sequence_gap_is_counted(self) -> None:
        voice, _sink = _make_receiver()
        voice._on_control(None, bytearray(AUDIO_START))
        voice._on_audio(None, bytearray(SILENT_FRAME))
        self.assertEqual(voice._expected_seq, 1)

        # The device skips to seq=3, i.e. two frames were lost.
        voice._on_control(None, bytearray(bytes.fromhex("0A020003000000")))
        self.assertEqual(voice.stats.gaps, 1)
        self.assertEqual(voice._expected_seq, 3)

    def test_audio_stop_ends_the_session(self) -> None:
        voice, _sink = _make_receiver()
        voice._on_control(None, bytearray(AUDIO_START))
        voice._on_audio(None, bytearray(SILENT_FRAME))
        voice._on_control(None, bytearray(AUDIO_STOP))
        self.assertFalse(voice._session_active)
        self.assertIsNone(voice._expected_seq)
        self.assertGreater(voice.stats.session_seconds, 0.0)

    def test_malformed_control_notification_is_ignored(self) -> None:
        voice, _sink = _make_receiver()
        for payload in (b"", b"\x04\x00", b"\x0a\x02\x00", b"\xff\xff"):
            with self.subTest(payload=payload.hex()):
                voice._on_control(None, bytearray(payload))
        self.assertIsNone(voice._caps)
        self.assertEqual(voice.stats.frames, 0)

    def test_mic_open_error_is_logged_not_raised(self) -> None:
        voice, _sink = _make_receiver()
        with self.assertLogs("papermono_mic", level="ERROR") as captured:
            voice._on_control(None, bytearray(bytes.fromhex("0C0001")))
        self.assertTrue(
            any("not encrypted" in line for line in captured.output),
            captured.output,
        )

    def test_unsupported_codec_drops_audio_but_keeps_counting_bytes(self) -> None:
        voice, sink = _make_receiver()
        voice._on_control(None, bytearray(bytes.fromhex("0B0100050300780000")))
        self.assertFalse(voice._stream_supported)
        voice._on_control(None, bytearray(AUDIO_START))
        voice._on_audio(None, bytearray(SILENT_FRAME))
        self.assertEqual(voice.stats.frames, 0)
        self.assertEqual(sink.ring.fill, 0)

    def test_gain_is_applied_before_the_sink_and_clips(self) -> None:
        voice, sink = _make_receiver(gain_db=20.0)
        voice._on_control(None, bytearray(AUDIO_START))
        # 0x77 nibbles decode to a rising signal; +20 dB must clip at 32767.
        voice._on_audio(None, bytearray(bytes([0x77] * protocol.FRAME_BYTES)))
        block = sink.ring.read(protocol.FRAME_SAMPLES)
        self.assertEqual(max(block), 32767)

    def test_dc_blocker_removes_a_constant_offset(self) -> None:
        voice, sink = _make_receiver(dc_blocker=True)
        voice._on_control(None, bytearray(AUDIO_START))
        # A constant predictor of 1000 is a pure DC offset; the high-pass
        # filter must drive it towards zero before the samples reach the sink.
        voice._on_control(None, bytearray(bytes.fromhex("0A02000003E800")))
        voice._on_audio(None, bytearray(bytes(protocol.FRAME_BYTES * 4)))
        block = sink.ring.read(protocol.FRAME_SAMPLES * 4)
        self.assertGreater(block[0], 900)
        self.assertLess(abs(block[-1]), 50)

    def test_wav_capture_records_raw_decoded_samples(self) -> None:
        with tempfile.TemporaryDirectory(prefix="papermono-mic-session-") as tmp:
            path = Path(tmp) / "session.wav"
            with audio.WavWriter(path) as wav:
                voice, _sink = _make_receiver(wav=wav)
                voice._on_control(None, bytearray(AUDIO_START))
                voice._on_audio(None, bytearray(SILENT_FRAME * 2))
                self.assertEqual(wav.samples_written, protocol.FRAME_SAMPLES * 2)
            with wave.open(str(path), "rb") as handle:
                self.assertEqual(handle.getframerate(), 16000)
                self.assertEqual(handle.getnchannels(), 1)
                self.assertEqual(handle.getnframes(), protocol.FRAME_SAMPLES * 2)


class SinkCallbackTests(unittest.TestCase):
    def test_callback_fills_silence_on_underrun(self) -> None:
        sink = receiver.AudioSink(device=None, sample_rate=16000, ring_ms=10)
        out = audio.array("h", bytes(2 * 160))
        sink._callback(out, 160, None, None)
        self.assertEqual(list(out), [0] * 160)
        self.assertGreater(sink.underruns, 0)

    def test_callback_copies_buffered_samples(self) -> None:
        sink = receiver.AudioSink(device=None, sample_rate=16000, ring_ms=10)
        sink.push(audio.array("h", range(160)))
        out = audio.array("h", bytes(2 * 160))
        sink._callback(out, 160, None, None)
        self.assertEqual(list(out), list(range(160)))


if __name__ == "__main__":
    unittest.main(verbosity=2)
