#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Unit tests for the ATVV framing and IMA/DVI ADPCM decoder.

Pure standard library: no Bluetooth or audio hardware is required. The
expected ADPCM values are hand-derived from the IMA/DVI algorithm (step table
and index table), not copied from another implementation; each vector
documents the arithmetic so a reviewer can re-check it.
"""

from __future__ import annotations

import sys
import unittest
from array import array
from pathlib import Path

_TOOL_DIR = Path(__file__).resolve().parents[1]
if str(_TOOL_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOL_DIR))

from remote_mic import protocol  # noqa: E402


class CapsTests(unittest.TestCase):
    def test_documented_capabilities_vector(self) -> None:
        caps = protocol.parse_caps(bytes.fromhex("0B0100020300780000"))
        self.assertEqual(caps.version_major, 1)
        self.assertEqual(caps.version_minor, 0)
        self.assertEqual(caps.version, "1.0")
        self.assertEqual(caps.codec, protocol.CODEC_ADPCM_16K)
        self.assertEqual(caps.interaction_model, protocol.INTERACTION_PUSH_TO_TALK)
        self.assertEqual(caps.frame_size, protocol.FRAME_BYTES)
        self.assertEqual(caps.frame_samples, 240)
        self.assertEqual(caps.sample_rate, 16000)
        self.assertAlmostEqual(caps.frame_duration_ms, 15.0, places=3)
        self.assertEqual(caps.reserved, b"\x00\x00")
        self.assertTrue(caps.is_supported)

    def test_8k_codec_is_supported_but_uses_a_different_rate(self) -> None:
        caps = protocol.parse_caps(bytes.fromhex("0B0100010300780000"))
        self.assertEqual(caps.sample_rate, 8000)
        self.assertEqual(caps.frame_samples, 240)
        self.assertAlmostEqual(caps.frame_duration_ms, 30.0, places=3)
        self.assertTrue(caps.is_supported)

    def test_unknown_codec_is_reported_but_not_fatal(self) -> None:
        caps = protocol.parse_caps(bytes.fromhex("0B0100050300780000"))
        self.assertFalse(caps.is_supported)
        self.assertIsNone(caps.sample_rate)
        self.assertEqual(protocol.codec_name(0x05), "unknown codec 0x05")

    def test_capabilities_are_truncated(self) -> None:
        with self.assertRaises(protocol.ProtocolError):
            protocol.parse_caps(bytes.fromhex("0B01000203007800"))

    def test_capabilities_wrong_opcode(self) -> None:
        with self.assertRaises(protocol.ProtocolError):
            protocol.parse_caps(bytes.fromhex("0A0100020300780000"))

    def test_capabilities_zero_frame_size(self) -> None:
        with self.assertRaises(protocol.ProtocolError):
            protocol.parse_caps(bytes.fromhex("0B0100020300000000"))

    def test_capabilities_implausible_frame_size(self) -> None:
        with self.assertRaises(protocol.ProtocolError):
            protocol.parse_caps(bytes.fromhex("0B0100020304000000"))


class ControlEventTests(unittest.TestCase):
    def test_audio_start(self) -> None:
        event = protocol.parse_control_event(bytes.fromhex("04000200"))
        self.assertIsInstance(event, protocol.AudioStart)
        assert isinstance(event, protocol.AudioStart)
        self.assertEqual(event.reason, 0)
        self.assertEqual(event.codec, protocol.CODEC_ADPCM_16K)
        self.assertEqual(event.session_id, 0)

    def test_audio_start_with_incrementing_session(self) -> None:
        event = protocol.parse_control_event(bytes.fromhex("04000207"))
        assert isinstance(event, protocol.AudioStart)
        self.assertEqual(event.session_id, 7)

    def test_audio_sync_positive_predictor(self) -> None:
        event = protocol.parse_control_event(bytes.fromhex("0A02003204D005"))
        assert isinstance(event, protocol.AudioSync)
        self.assertEqual(event.codec, protocol.CODEC_ADPCM_16K)
        self.assertEqual(event.seq, 50)
        self.assertEqual(event.predictor, 1232)
        self.assertEqual(event.step_index, 5)

    def test_audio_sync_negative_predictor_is_signed(self) -> None:
        event = protocol.parse_control_event(bytes.fromhex("0A020001FC1800"))
        assert isinstance(event, protocol.AudioSync)
        self.assertEqual(event.seq, 1)
        self.assertEqual(event.predictor, -1000)
        self.assertEqual(event.step_index, 0)

    def test_audio_sync_accepts_maximum_step_index(self) -> None:
        event = protocol.parse_control_event(bytes.fromhex("0A020000000058"))
        assert isinstance(event, protocol.AudioSync)
        self.assertEqual(event.step_index, 88)

    def test_audio_sync_rejects_out_of_range_step(self) -> None:
        with self.assertRaises(protocol.ProtocolError):
            protocol.parse_control_event(bytes.fromhex("0A020000000059"))

    def test_audio_stop(self) -> None:
        event = protocol.parse_control_event(bytes.fromhex("0000"))
        self.assertIsInstance(event, protocol.AudioStop)
        assert isinstance(event, protocol.AudioStop)
        self.assertEqual(event.reason, 0)

    def test_mic_open_error(self) -> None:
        event = protocol.parse_control_event(bytes.fromhex("0C0001"))
        assert isinstance(event, protocol.MicOpenError)
        self.assertEqual(event.code, 0x0001)
        self.assertEqual(event.description, "link is not encrypted")

        event = protocol.parse_control_event(bytes.fromhex("0C0002"))
        assert isinstance(event, protocol.MicOpenError)
        self.assertEqual(
            event.description,
            "host has not subscribed to Audio and Control",
        )

        event = protocol.parse_control_event(bytes.fromhex("0C0003"))
        assert isinstance(event, protocol.MicOpenError)
        self.assertEqual(event.code, 0x0003)
        self.assertEqual(event.description, "device or microphone unavailable")

    def test_unknown_opcode_is_preserved(self) -> None:
        event = protocol.parse_control_event(bytes.fromhex("7F0102"))
        self.assertIsInstance(event, protocol.UnknownEvent)
        assert isinstance(event, protocol.UnknownEvent)
        self.assertEqual(event.opcode, 0x7F)
        self.assertEqual(event.payload, bytes.fromhex("7F0102"))

    def test_truncated_events_are_rejected(self) -> None:
        for payload in (
            b"",
            bytes.fromhex("0400"),
            bytes.fromhex("040002"),
            bytes.fromhex("0A0200000000"),
            bytes.fromhex("00"),
            bytes.fromhex("0C00"),
        ):
            with self.subTest(payload=payload.hex()):
                with self.assertRaises(protocol.ProtocolError):
                    protocol.parse_control_event(payload)

    def test_describe_event_mentions_the_important_fields(self) -> None:
        caps = protocol.parse_caps(bytes.fromhex("0B0100020300780000"))
        self.assertIn("v1.0", protocol.describe_event(caps))
        self.assertIn("120B", protocol.describe_event(caps))
        sync = protocol.parse_control_event(bytes.fromhex("0A02003204D005"))
        text = protocol.describe_event(sync)
        self.assertIn("seq=50", text)
        self.assertIn("predictor=1232", text)
        self.assertIn("step=5", text)


class CommandTests(unittest.TestCase):
    def test_get_caps(self) -> None:
        self.assertEqual(protocol.build_get_caps(), bytes.fromhex("0A0100000303"))

    def test_mic_open_v1_and_legacy(self) -> None:
        self.assertEqual(protocol.build_mic_open(), bytes.fromhex("0C00"))
        self.assertEqual(protocol.build_mic_open(0x02), bytes.fromhex("0C0002"))

    def test_mic_close_and_extend(self) -> None:
        self.assertEqual(protocol.build_mic_close(3), bytes.fromhex("0D03"))
        self.assertEqual(protocol.build_mic_extend(3), bytes.fromhex("0E03"))
        self.assertEqual(protocol.build_mic_close(0x1FF), bytes.fromhex("0DFF"))


class AdpcmDecoderTests(unittest.TestCase):
    """Vectors derived by hand from the IMA/DVI algorithm.

    For a nibble ``n`` with step ``S = STEP_TABLE[step_index]`` the decoder
    adds ``S>>3`` plus ``S``, ``S>>1`` and ``S>>2`` for bits 4, 2 and 1, then
    applies the sign bit and the index adjustment ``INDEX_TABLE[n & 7]``.
    """

    def test_silence_nibbles_decode_to_zero(self) -> None:
        decoder = protocol.AdpcmDecoder()
        samples = decoder.decode(bytes(protocol.FRAME_BYTES))
        self.assertEqual(len(samples), protocol.FRAME_SAMPLES)
        self.assertEqual(samples, array("h", bytes(protocol.FRAME_SAMPLES * 2)))

    def test_high_nibble_first_and_state_continuity(self) -> None:
        # predictor=1000, step_index=0 (S=7).
        # 0x70: high nibble 7 -> 0+7+3+1 = 11 -> 1011, index 8 (S=16);
        #       low nibble 0 -> 2 -> 1013, index 7.
        # 0x07: high nibble 0 -> 0 -> 1000, index stays 0 (clamped);
        #       low nibble 7 -> 11 -> 1011, index 8.
        decoder = protocol.AdpcmDecoder(1000, 0)
        self.assertEqual(list(decoder.decode(b"\x70")), [1011, 1013])
        decoder.reset(1000, 0)
        self.assertEqual(list(decoder.decode(b"\x07")), [1000, 1011])

    def test_small_nibbles_from_zero(self) -> None:
        # S=7: nibble 1 -> 0+0+0+1 = 1, index 0 (7-1 clamped to 0);
        # nibble 7 -> 0+7+3+1 = 11 -> 12, index 8.
        decoder = protocol.AdpcmDecoder()
        self.assertEqual(list(decoder.decode(b"\x17")), [1, 12])

    def test_negative_nibbles(self) -> None:
        # S=7: nibble F -> -(0+7+3+1) = -11, index 8 (S=16);
        #       nibble 8 -> -(16>>3) = -2 -> -13, index 7.
        decoder = protocol.AdpcmDecoder()
        self.assertEqual(list(decoder.decode(b"\xF8")), [-11, -13])

    def test_predictor_and_step_index_clamp(self) -> None:
        # S=32767: nibble 7 -> 4095+32767+16383+8191 = 61436, so 32000
        # saturates at 32767 and the index saturates at 88.
        decoder = protocol.AdpcmDecoder(32000, 88)
        self.assertEqual(list(decoder.decode(b"\x77")), [32767, 32767])
        self.assertEqual(decoder.step_index, 88)

        # S=32767, nibble F -> -(4095+32767+16383+8191) = -61436, so -32000
        # saturates at -32768; nibble 8 then subtracts 32767>>3 = 4095 and
        # stays saturated.
        decoder = protocol.AdpcmDecoder(-32000, 88)
        self.assertEqual(list(decoder.decode(b"\xF8")), [-32768, -32768])

    def test_reset_replaces_state(self) -> None:
        decoder = protocol.AdpcmDecoder()
        decoder.decode(bytes(protocol.FRAME_BYTES))
        decoder.reset(1000, 0)
        self.assertEqual(decoder.predictor, 1000)
        self.assertEqual(decoder.step_index, 0)
        self.assertEqual(list(decoder.decode(b"\x00")), [1000, 1000])

    def test_state_carries_across_frames(self) -> None:
        decoder = protocol.AdpcmDecoder()
        first = decoder.decode(bytes([0x77] * protocol.FRAME_BYTES))
        second = decoder.decode(bytes([0x77] * protocol.FRAME_BYTES))
        # A per-frame reset would make both frames identical.
        self.assertNotEqual(list(first), list(second))
        self.assertEqual(len(first) + len(second), protocol.FRAME_SAMPLES * 2)

    def test_decode_empty_input(self) -> None:
        self.assertEqual(list(protocol.AdpcmDecoder().decode(b"")), [])


class AdpcmRoundTripTests(unittest.TestCase):
    @staticmethod
    def _sine(count: int, period: int = 32, amplitude: int = 12000) -> array:
        import math

        return array(
            "h",
            (
                int(round(amplitude * math.sin(2.0 * math.pi * (index % period) / period)))
                for index in range(count)
            ),
        )

    def test_silence_encodes_to_zero_bytes(self) -> None:
        encoder = protocol.AdpcmEncoder()
        encoded = encoder.encode([0] * protocol.FRAME_SAMPLES)
        self.assertEqual(encoded, bytes(protocol.FRAME_BYTES))

    def test_round_trip_signal_to_noise(self) -> None:
        import math

        reference = self._sine(protocol.FRAME_SAMPLES * 4)
        encoder = protocol.AdpcmEncoder()
        encoded = encoder.encode(reference)
        self.assertEqual(len(encoded), len(reference) // 2)

        decoder = protocol.AdpcmDecoder()
        decoded = decoder.decode(encoded)
        self.assertEqual(len(decoded), len(reference))

        signal = sum(float(sample) ** 2 for sample in reference)
        noise = sum(
            (float(expected) - float(actual)) ** 2
            for expected, actual in zip(reference, decoded)
        )
        self.assertGreater(noise, 0.0)
        snr = 10.0 * math.log10(signal / noise)
        self.assertGreater(snr, 20.0)

    def test_encoder_rejects_odd_sample_count(self) -> None:
        with self.assertRaises(ValueError):
            protocol.AdpcmEncoder().encode([0, 1, 2])

    def test_encoder_emits_high_nibble_first(self) -> None:
        # A large positive first sample and a large negative second sample:
        # the first encoded nibble must be positive (bit 3 clear) and the
        # second negative (bit 3 set).
        encoded = protocol.AdpcmEncoder().encode([12000, -12000])
        self.assertEqual(len(encoded), 1)
        self.assertEqual(encoded[0] >> 4 & 0x8, 0)
        self.assertEqual(encoded[0] & 0x8, 0x8)


class FrameAccumulatorTests(unittest.TestCase):
    def test_single_complete_frame(self) -> None:
        accumulator = protocol.FrameAccumulator()
        frame = bytes(range(120))
        self.assertEqual(accumulator.feed(frame), [frame])
        self.assertEqual(accumulator.pending, 0)
        self.assertEqual(accumulator.frames, 1)

    def test_chunked_notifications_reassemble_one_frame(self) -> None:
        accumulator = protocol.FrameAccumulator()
        frame = bytes(range(120))
        produced: list[bytes] = []
        for offset in range(0, 120, 20):
            produced.extend(accumulator.feed(frame[offset:offset + 20]))
        self.assertEqual(produced, [frame])
        self.assertEqual(accumulator.frames, 1)
        self.assertEqual(accumulator.pending, 0)

    def test_uneven_chunks_across_frame_boundaries(self) -> None:
        accumulator = protocol.FrameAccumulator()
        stream = bytes((index * 7) & 0xFF for index in range(120 * 3))
        produced: list[bytes] = []
        for offset in range(0, len(stream), 37):
            produced.extend(accumulator.feed(stream[offset:offset + 37]))
        self.assertEqual(produced, [stream[0:120], stream[120:240], stream[240:360]])

    def test_multiple_frames_in_one_notification(self) -> None:
        accumulator = protocol.FrameAccumulator()
        stream = bytes(240)
        produced = accumulator.feed(stream)
        self.assertEqual(len(produced), 2)
        self.assertEqual(accumulator.frames, 2)

    def test_partial_frame_is_held_then_completed(self) -> None:
        accumulator = protocol.FrameAccumulator()
        self.assertEqual(accumulator.feed(bytes(60)), [])
        self.assertEqual(accumulator.pending, 60)
        produced = accumulator.feed(bytes(60))
        self.assertEqual(len(produced), 1)
        self.assertEqual(accumulator.pending, 0)

    def test_reset_drops_partial_frame(self) -> None:
        accumulator = protocol.FrameAccumulator()
        accumulator.feed(bytes(100))
        self.assertEqual(accumulator.reset(), 100)
        self.assertEqual(accumulator.pending, 0)
        self.assertEqual(accumulator.discarded_bytes, 100)
        self.assertEqual(accumulator.feed(bytes(60)), [])
        self.assertEqual(accumulator.reset(), 60)
        self.assertEqual(accumulator.discarded_bytes, 160)

    def test_reset_keeps_completed_frame_count(self) -> None:
        accumulator = protocol.FrameAccumulator()
        accumulator.feed(bytes(120))
        accumulator.feed(bytes(30))
        accumulator.reset()
        self.assertEqual(accumulator.frames, 1)
        self.assertEqual(accumulator.discarded_bytes, 30)

    def test_custom_frame_size(self) -> None:
        accumulator = protocol.FrameAccumulator(40)
        self.assertEqual(accumulator.feed(bytes(40)), [bytes(40)])

    def test_invalid_frame_size(self) -> None:
        with self.assertRaises(ValueError):
            protocol.FrameAccumulator(0)
        with self.assertRaises(ValueError):
            protocol.FrameAccumulator(-120)

    def test_empty_and_memoryview_input(self) -> None:
        accumulator = protocol.FrameAccumulator()
        self.assertEqual(accumulator.feed(b""), [])
        frame = bytes(range(120))
        self.assertEqual(accumulator.feed(memoryview(frame)), [frame])

    def test_large_notification(self) -> None:
        accumulator = protocol.FrameAccumulator()
        stream = bytes(120 * 10)
        self.assertEqual(len(accumulator.feed(stream)), 10)
        self.assertEqual(accumulator.frames, 10)


class ProtocolConstantsTests(unittest.TestCase):
    def test_uuids_match_the_protocol_document(self) -> None:
        self.assertEqual(
            protocol.SERVICE_UUID, "ab5e0001-5a21-4f05-bc7d-af01f617b664"
        )
        self.assertEqual(protocol.TX_UUID, "ab5e0002-5a21-4f05-bc7d-af01f617b664")
        self.assertEqual(protocol.AUDIO_UUID, "ab5e0003-5a21-4f05-bc7d-af01f617b664")
        self.assertEqual(
            protocol.CONTROL_UUID, "ab5e0004-5a21-4f05-bc7d-af01f617b664"
        )

    def test_frame_geometry(self) -> None:
        self.assertEqual(protocol.FRAME_BYTES, 120)
        self.assertEqual(protocol.FRAME_SAMPLES, 240)
        self.assertAlmostEqual(protocol.FRAME_MS, 15.0)
        self.assertEqual(protocol.DEFAULT_DEVICE_NAME, "PaperMono C153")


if __name__ == "__main__":
    unittest.main(verbosity=2)
