# SPDX-License-Identifier: GPL-3.0-only
"""Command line interface for the PaperMono C153 BLE voice receiver."""

from __future__ import annotations

import argparse
import asyncio
import logging
import math
import sys
import tempfile
import time
import wave
from array import array
from pathlib import Path

from . import __version__, protocol
from .audio import (
    DEFAULT_RING_MS,
    GAIN_DB_MAX,
    GAIN_DB_MIN,
    SAMPLE_RATE,
    DcBlocker,
    RingBuffer,
    WavWriter,
    clamp_gain_db,
    process_block,
)
from .receiver import (
    AudioDeviceError,
    AudioSink,
    DependencyError,
    ReceiverConfig,
    ReceiverError,
    VoiceReceiver,
    format_device_table,
    load_sounddevice,
)

LOGGER = logging.getLogger("papermono_mic")

_EPILOG = """\
examples:
  %(prog)s --list-devices
  %(prog)s --device "BlackHole 2ch"          # macOS virtual microphone
  %(prog)s --device "CABLE Input"            # Windows VB-CABLE
  %(prog)s --device papermono_mic            # Linux PulseAudio null sink
  %(prog)s --address AA:BB:CC:DD:EE:FF --device 3 --gain-db 6 --wav voice.wav
  %(prog)s --send-mic-open --mic-open-seconds 5 --once   # link self-test

The remote is normally already bonded because it also exposes BLE HID; if the
receiver reports an encryption error, pair it in the OS Bluetooth settings
first. No kernel driver is installed by this tool.
"""


class _ConsoleFormatter(logging.Formatter):
    """Prefix warnings and errors, leave info lines readable."""

    _PREFIXES = {
        "DEBUG": "debug: ",
        "WARNING": "warning: ",
        "ERROR": "error: ",
        "CRITICAL": "error: ",
    }

    def format(self, record: logging.LogRecord) -> str:
        prefix = self._PREFIXES.get(record.levelname, "")
        return prefix + record.getMessage()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="papermono_mic",
        description=(
            "Receive the PaperMono C153 BLE voice stream (ATVV v1.0, ADPCM "
            "16 kHz) and play it into an audio output device so a virtual "
            "loopback device can serve as the system microphone."
        ),
        epilog=_EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--address",
        metavar="ADDR",
        help="connect to this BLE address/UUID instead of scanning by name",
    )
    parser.add_argument(
        "--name",
        metavar="NAME",
        default=protocol.DEFAULT_DEVICE_NAME,
        help=f'device name to scan for (default: "{protocol.DEFAULT_DEVICE_NAME}")',
    )
    parser.add_argument(
        "--device",
        metavar="DEV",
        default=None,
        help=(
            "audio output device: index or name substring "
            '(for example "BlackHole 2ch", "CABLE Input"); default: system '
            "default output"
        ),
    )
    parser.add_argument(
        "--list-devices",
        action="store_true",
        help="list audio devices and exit",
    )
    parser.add_argument(
        "--gain-db",
        type=float,
        default=0.0,
        metavar="DB",
        help=(
            f"output gain in dB, clamped to {GAIN_DB_MIN:+.0f}..{GAIN_DB_MAX:+.0f} "
            "(default: 0)"
        ),
    )
    parser.add_argument(
        "--wav",
        metavar="PATH",
        help=(
            "record the decoded stream (before gain and DC blocking) to a "
            "16 kHz mono WAV file for hardware verification"
        ),
    )
    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="log every control event and per-frame detail",
    )
    parser.add_argument(
        "--once",
        action="store_true",
        help=(
            "connect once and exit after the first voice session or link loss "
            "instead of reconnecting"
        ),
    )
    parser.add_argument(
        "--send-mic-open",
        action="store_true",
        help=(
            "test mode: write MIC_OPEN so the device streams without holding "
            "the touch area, then send MIC_CLOSE after --mic-open-seconds"
        ),
    )
    parser.add_argument(
        "--mic-open-seconds",
        type=float,
        default=10.0,
        metavar="SEC",
        help="duration of the --send-mic-open test session (default: 10)",
    )
    parser.add_argument(
        "--scan-timeout",
        type=float,
        default=8.0,
        metavar="SEC",
        help="BLE scan duration when no --address is given (default: 8)",
    )
    parser.add_argument(
        "--ring-ms",
        type=int,
        default=DEFAULT_RING_MS,
        metavar="MS",
        help=(
            "audio ring buffer length in milliseconds; larger values absorb "
            f"more jitter (default: {DEFAULT_RING_MS})"
        ),
    )
    parser.add_argument(
        "--no-dc-blocker",
        action="store_true",
        help="disable the 40 Hz high-pass DC blocker",
    )
    parser.add_argument(
        "--selftest",
        action="store_true",
        help=(
            "run the offline protocol/audio pipeline self-test (no Bluetooth "
            "or audio hardware needed)"
        ),
    )
    parser.add_argument(
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )
    return parser


def configure_logging(verbose: bool) -> None:
    level = logging.DEBUG if verbose else logging.INFO
    handler = logging.StreamHandler()
    handler.setFormatter(_ConsoleFormatter())
    root = logging.getLogger()
    root.handlers.clear()
    root.addHandler(handler)
    root.setLevel(level)
    LOGGER.setLevel(level)


def _list_devices() -> int:
    try:
        sounddevice = load_sounddevice()
    except DependencyError as exc:
        LOGGER.error("%s", exc)
        return 2
    print("PortAudio devices (* marks the default output):")
    print(format_device_table(sounddevice))
    print()
    print(
        "Select the virtual loopback device with --device \"<substring>\" or "
        "--device <index>, then choose the matching capture side in the target "
        "application."
    )
    return 0


def _snr_db(reference: array, decoded: array) -> float:
    signal = 0.0
    noise = 0.0
    for expected, actual in zip(reference, decoded):
        signal += float(expected) * float(expected)
        error = float(expected) - float(actual)
        noise += error * error
    if noise == 0.0:
        return 200.0
    if signal == 0.0:
        return -200.0
    return 10.0 * math.log10(signal / noise)


def run_selftest() -> int:
    """Offline end-to-end check of the framing, codec and audio helpers."""

    failures = 0
    started = time.monotonic()

    def report(name: str, ok: bool, detail: str = "") -> None:
        nonlocal failures
        if not ok:
            failures += 1
        status = "ok  " if ok else "FAIL"
        print(f"[{status}] {name}{(' — ' + detail) if detail else ''}")

    # 1. Capabilities vector from the protocol document.
    caps = protocol.parse_caps(bytes.fromhex("0B0100020300780000"))
    report(
        "capabilities vector",
        caps.version == "1.0"
        and caps.codec == protocol.CODEC_ADPCM_16K
        and caps.frame_size == 120
        and caps.sample_rate == SAMPLE_RATE
        and caps.is_supported,
        protocol.describe_event(caps),
    )

    # 2. Silence frame: 240 zero samples encode to 120 zero bytes.
    encoder = protocol.AdpcmEncoder()
    silence = bytes(protocol.FRAME_BYTES)
    encoded_silence = encoder.encode([0] * protocol.FRAME_SAMPLES)
    decoder = protocol.AdpcmDecoder()
    decoded_silence = decoder.decode(silence)
    report(
        "silence frame",
        encoded_silence == silence
        and len(decoded_silence) == protocol.FRAME_SAMPLES
        and not any(decoded_silence),
        f"{len(encoded_silence)} encoded bytes -> {len(decoded_silence)} samples",
    )

    # 3. Round trip through the wire format, fed in uneven chunks. Keep the
    #    reference an exact number of 120-byte frames: the accumulator only
    #    emits complete frames by design.
    tone_samples = (int(SAMPLE_RATE * 0.5) // protocol.FRAME_SAMPLES) * protocol.FRAME_SAMPLES
    reference = array(
        "h",
        (
            int(12000.0 * math.sin(2.0 * math.pi * 440.0 * index / SAMPLE_RATE))
            for index in range(tone_samples)
        ),
    )
    encoder = protocol.AdpcmEncoder()
    wire = encoder.encode(reference)
    accumulator = protocol.FrameAccumulator()
    decoder = protocol.AdpcmDecoder()
    decoded = array("h")
    offset = 0
    chunk_sizes = (20, 7, 120, 53, 1, 240, 120)
    chunk_index = 0
    while offset < len(wire):
        size = chunk_sizes[chunk_index % len(chunk_sizes)]
        chunk_index += 1
        chunk = wire[offset:offset + size]
        offset += len(chunk)
        for frame in accumulator.feed(chunk):
            decoded.extend(decoder.decode(frame))
    snr = _snr_db(reference, decoded)
    report(
        "ADPCM round trip (chunked notifications)",
        len(decoded) == len(reference) and snr > 20.0,
        f"{len(decoded)} samples, SNR {snr:.1f} dB",
    )

    # 4. Ring buffer returns silence on underrun and drops the oldest samples.
    ring = RingBuffer(160)
    ring.write(array("h", [1] * 320))
    report(
        "ring buffer overrun",
        ring.overrun_samples == 160 and ring.fill == 160,
        f"dropped {ring.overrun_samples} samples",
    )
    block = ring.read(320)
    report(
        "ring buffer underrun",
        ring.underrun_samples == 160 and not any(block[160:]),
        f"padded {ring.underrun_samples} samples with silence",
    )

    # 5. Gain clamp and DC blocking keep samples in range.
    loud = process_block(
        array("h", [30000, -30000] * 8),
        gain_db=99.0,
        dc_blocker=None,
    )
    report(
        "gain clamp",
        max(loud) == 32767 and min(loud) == -32768,
        f"gain clamped to {clamp_gain_db(99.0):+.0f} dB",
    )
    blocker = DcBlocker()
    biased = array("h", [2000] * 16000)
    blocked = blocker.process(biased)
    report(
        "DC blocker",
        abs(blocked[-1]) < 100,
        f"tail sample {blocked[-1]} (input bias 2000)",
    )

    # 6. WAV capture round trip.
    with tempfile.TemporaryDirectory(prefix="papermono-mic-selftest-") as tmp:
        wav_path = Path(tmp) / "selftest.wav"
        writer = WavWriter(wav_path)
        writer.open()
        writer.write(reference[:1600])
        writer.close()
        with wave.open(str(wav_path), "rb") as handle:
            channels = handle.getnchannels()
            width = handle.getsampwidth()
            rate = handle.getframerate()
            frames = handle.readframes(handle.getnframes())
        report(
            "WAV capture",
            channels == 1
            and width == 2
            and rate == SAMPLE_RATE
            and len(frames) == 1600 * 2,
            f"{len(frames) // 2} samples, {channels}ch/{width * 8}bit/{rate} Hz",
        )

    # 7. No BLE or audio dependency was imported for the offline path.
    report(
        "lazy dependencies",
        "bleak" not in sys.modules and "sounddevice" not in sys.modules,
        "bleak/sounddevice not imported",
    )

    elapsed = time.monotonic() - started
    if failures:
        print(f"selftest FAILED ({failures} check(s)) in {elapsed:.2f} s")
        return 1
    print(f"selftest passed in {elapsed:.2f} s")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    configure_logging(args.verbose)

    if args.list_devices:
        return _list_devices()
    if args.selftest:
        return run_selftest()

    gain_db = clamp_gain_db(args.gain_db)
    if gain_db != float(args.gain_db):
        LOGGER.warning(
            "--gain-db %.1f is outside the supported range; using %.0f dB",
            args.gain_db,
            gain_db,
        )
    if args.mic_open_seconds <= 0:
        parser.error("--mic-open-seconds must be positive")
    if args.ring_ms <= 0:
        parser.error("--ring-ms must be positive")

    config = ReceiverConfig(
        address=args.address,
        name=args.name,
        audio_device=args.device,
        gain_db=gain_db,
        wav_path=args.wav,
        dc_blocker=not args.no_dc_blocker,
        verbose=args.verbose,
        once=args.once,
        send_mic_open=args.send_mic_open,
        mic_open_seconds=args.mic_open_seconds,
        scan_timeout=args.scan_timeout,
        ring_ms=args.ring_ms,
    )

    sink = AudioSink(device=args.device, ring_ms=args.ring_ms)
    wav: WavWriter | None = None
    exit_code = 0
    try:
        if args.wav:
            wav = WavWriter(args.wav, sample_rate=SAMPLE_RATE)
            wav.open()
            LOGGER.info("capturing the decoded stream to %s", args.wav)
        sink.open()
        receiver = VoiceReceiver(config, sink, wav)
        try:
            exit_code = asyncio.run(receiver.run())
        except KeyboardInterrupt:
            LOGGER.info("stopped by user")
            exit_code = 0
        finally:
            stats = receiver.stats
            LOGGER.info(
                "received %d frames (%d sessions, %.1f s of audio, %d gap(s))",
                stats.frames,
                stats.sessions,
                stats.samples / float(SAMPLE_RATE),
                stats.gaps,
            )
            if wav is not None:
                LOGGER.info(
                    "wrote %d samples to %s", wav.samples_written, wav.path
                )
    except KeyboardInterrupt:
        LOGGER.info("stopped by user")
        exit_code = 0
    except (DependencyError, AudioDeviceError, ReceiverError) as exc:
        LOGGER.error("%s", exc)
        exit_code = 2
    finally:
        sink.close()
        if wav is not None:
            wav.close()
    return exit_code


if __name__ == "__main__":  # pragma: no cover - module entry point
    raise SystemExit(main())
