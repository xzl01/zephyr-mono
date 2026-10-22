# SPDX-License-Identifier: GPL-3.0-only
"""BLE -> audio receiver for the PaperMono C153 ATVV voice service.

``bleak`` and ``sounddevice`` are imported lazily inside the functions that
need them, so importing this module (and running ``--help`` or ``--selftest``)
works on a machine with neither dependency installed.

Threading model:

* the asyncio thread owns the bleak client and decodes notifications;
* PortAudio runs :meth:`AudioSink._callback` on its own thread and pulls from a
  bounded ring buffer that returns silence on underrun;
* the optional WAV capture writes on the asyncio thread (480 bytes per 15 ms
  block), so it cannot stall the audio callback.
"""

from __future__ import annotations

import asyncio
import logging
import platform
import time
from array import array
from dataclasses import dataclass
from typing import Any

from . import protocol
from .audio import (
    DEFAULT_RING_MS,
    SAMPLE_RATE,
    DcBlocker,
    RingBuffer,
    WavWriter,
    clamp_gain_db,
    process_block,
)

LOGGER = logging.getLogger("papermono_mic")

INSTALL_HINT = "python3 -m pip install -r tools/remote-mic/requirements.txt"

#: Substrings that identify an ATT "insufficient encryption/authentication"
#: failure across the BlueZ, CoreBluetooth and WinRT bleak backends.
_ENCRYPTION_HINTS = (
    "insufficient encryption",
    "insufficient authentication",
    "insufficient_encryption",
    "authentication",
    "not paired",
    "not bonded",
    "encryption",
    "encrypted",
    "not permitted",
    "not authorized",
    "0x05",
    "0x0f",
    "att error: 5",
    "att error: 15",
)


class ReceiverError(RuntimeError):
    """Base class for receiver failures that should be shown to the user."""


class DependencyError(ReceiverError):
    """A required third-party package or system library is missing."""


class DeviceNotFoundError(ReceiverError):
    """No matching PaperMono remote was found during the scan."""


class AudioDeviceError(ReceiverError):
    """The requested audio output device could not be opened."""


class EncryptionRequiredError(ReceiverError):
    """The ATVV characteristics need a bonded, encrypted link."""


def load_bleak() -> tuple[Any, Any, type[Exception]]:
    """Import bleak on demand and return ``(BleakClient, BleakScanner, BleakError)``."""

    try:
        from bleak import BleakClient, BleakScanner
        from bleak.exc import BleakError
    except ModuleNotFoundError as exc:  # pragma: no cover - dependency guard
        raise DependencyError(
            "the 'bleak' package is required for Bluetooth LE: " + INSTALL_HINT
        ) from exc
    return BleakClient, BleakScanner, BleakError


def load_sounddevice() -> Any:
    """Import sounddevice (PortAudio bindings) on demand."""

    try:
        import sounddevice
    except ModuleNotFoundError as exc:  # pragma: no cover - dependency guard
        raise DependencyError(
            "the 'sounddevice' package is required for audio output: "
            + INSTALL_HINT
        ) from exc
    except OSError as exc:  # pragma: no cover - missing PortAudio library
        raise DependencyError(
            "sounddevice could not load the PortAudio library "
            f"({exc}); install PortAudio (for example 'brew install portaudio') "
            "and retry"
        ) from exc
    return sounddevice


def encryption_guidance() -> str:
    """Explain how to bond the remote on each supported platform."""

    return "\n".join(
        (
            "the ATVV characteristics require an encrypted (bonded) link.",
            "Pair the remote in the operating system first:",
            "  macOS   : System Settings > Bluetooth; the remote is normally",
            "            already bonded because it also exposes BLE HID.",
            "  Windows : Settings > Bluetooth & devices > Add device >",
            "            Bluetooth > 'PaperMono C153'.",
            "  Linux   : bluetoothctl -> agent on, scan on,",
            "            pair <address>, trust <address>.",
            "bleak cannot trigger pairing on every platform, so the OS",
            "Bluetooth settings are the reliable path. Then run this receiver",
            "again.",
        )
    )


def _looks_like_encryption_error(exc: BaseException) -> bool:
    text = str(exc).lower()
    return any(hint in text for hint in _ENCRYPTION_HINTS)


# --- audio output --------------------------------------------------------


def _device_dicts(sounddevice: Any) -> list[dict[str, Any]]:
    return list(sounddevice.query_devices())


def format_device_table(sounddevice: Any) -> str:
    """Render the PortAudio device list with the default output marked."""

    devices = _device_dicts(sounddevice)
    try:
        default_output = sounddevice.default.device[1]
    except (TypeError, IndexError):  # pragma: no cover - unusual backend
        default_output = None
    lines = []
    for index, device in enumerate(devices):
        directions = []
        if device.get("max_output_channels", 0) > 0:
            directions.append("out")
        if device.get("max_input_channels", 0) > 0:
            directions.append("in")
        marker = "*" if index == default_output else " "
        lines.append(
            f"{marker}{index:3d}  {device.get('name', '?')}"
            f"  [{'+'.join(directions) or '-'}]"
            f"  out={device.get('max_output_channels', 0)}ch"
            f"  api={device.get('hostapi', '?')}"
        )
    return "\n".join(lines)


def resolve_output_device(
    sounddevice: Any, spec: str | int | None
) -> tuple[int, str]:
    """Resolve ``--device`` (index, name substring or ``default``).

    Returns ``(index, name)``. Raises :class:`AudioDeviceError` with a helpful
    message when nothing matches, so the user can run ``--list-devices``.
    """

    devices = _device_dicts(sounddevice)
    if not devices:
        raise AudioDeviceError("PortAudio reported no audio devices")
    outputs = [
        (index, device)
        for index, device in enumerate(devices)
        if device.get("max_output_channels", 0) > 0
    ]

    text = "" if spec is None else str(spec).strip()
    if text == "" or text.lower() == "default":
        try:
            index = int(sounddevice.default.device[1])
        except (TypeError, ValueError, IndexError):
            index = -1
        if index < 0 or index >= len(devices):
            if not outputs:
                raise AudioDeviceError(
                    "no output device is available; pass --device or --list-devices"
                )
            index = outputs[0][0]
        if devices[index].get("max_output_channels", 0) <= 0:
            raise AudioDeviceError(
                f"default device {index} ({devices[index].get('name', '?')}) has no "
                "output channels; pass --device or --list-devices"
            )
        return index, str(devices[index].get("name", "?"))

    try:
        index = int(text)
    except ValueError:
        index = None
    if index is not None:
        if index < 0 or index >= len(devices):
            raise AudioDeviceError(
                f"output device index {index} is out of range "
                f"(0..{len(devices) - 1}); use --list-devices"
            )
        if devices[index].get("max_output_channels", 0) <= 0:
            raise AudioDeviceError(
                f"device {index} ({devices[index].get('name', '?')}) has no output "
                "channels; use --list-devices"
            )
        return index, str(devices[index].get("name", "?"))

    needle = text.lower()
    matches = [
        (index, device)
        for index, device in outputs
        if needle in str(device.get("name", "")).lower()
    ]
    if not matches:
        raise AudioDeviceError(
            f"no output device name contains {text!r}; run --list-devices"
        )
    index, device = matches[0]
    if len(matches) > 1:
        LOGGER.warning(
            "%d output devices match %r; using %d (%s). Pass an index to be explicit.",
            len(matches),
            text,
            index,
            device.get("name", "?"),
        )
    return index, str(device.get("name", "?"))


class AudioSink:
    """PortAudio output stream fed from a bounded ring buffer.

    The sink always runs at 16 kHz mono int16. Selecting a virtual loopback
    device (BlackHole 2ch, CABLE Input, a PulseAudio null sink) turns the
    receiver into the "microphone" other applications can record from.
    """

    def __init__(
        self,
        device: str | int | None = None,
        sample_rate: int = SAMPLE_RATE,
        ring_ms: int = DEFAULT_RING_MS,
        block_frames: int = 0,
    ) -> None:
        self._device_spec = device
        self._sample_rate = int(sample_rate)
        self._ring = RingBuffer(max(1, int(self._sample_rate * ring_ms / 1000)))
        self._block_frames = int(block_frames)
        self._sounddevice: Any = None
        self._stream: Any = None
        self.device_index: int | None = None
        self.device_name: str = ""
        self.callback_status_events = 0

    @property
    def sample_rate(self) -> int:
        return self._sample_rate

    @property
    def ring(self) -> RingBuffer:
        return self._ring

    @property
    def underruns(self) -> int:
        return self._ring.underrun_samples

    @property
    def dropped_samples(self) -> int:
        return self._ring.overrun_samples

    def open(self) -> None:
        sounddevice = load_sounddevice()
        self._sounddevice = sounddevice
        index, name = resolve_output_device(sounddevice, self._device_spec)
        self.device_index = index
        self.device_name = name
        try:
            stream = self._open_stream(sounddevice, index)
            stream.start()
        except Exception as exc:
            raise AudioDeviceError(
                f"cannot open output device {index} ({name}): {exc}"
            ) from exc
        self._stream = stream
        LOGGER.info("audio output: [%d] %s @ %d Hz", index, name, self._sample_rate)

    def _open_stream(self, sounddevice: Any, index: int) -> Any:
        kwargs = dict(
            samplerate=self._sample_rate,
            channels=1,
            dtype="int16",
            device=index,
            blocksize=self._block_frames,
            callback=self._callback,
        )
        try:
            return sounddevice.OutputStream(latency="low", **kwargs)
        except Exception:
            # Some virtual devices reject the low-latency request; the ring
            # buffer absorbs the extra jitter either way.
            LOGGER.debug(
                "device %d rejected latency='low'; retrying with the default",
                index,
                exc_info=True,
            )
            return sounddevice.OutputStream(**kwargs)

    def _callback(
        self, outdata: Any, frames: int, _time_info: Any, status: Any
    ) -> None:
        # Never let an exception escape the PortAudio thread: it would abort
        # the stream instead of producing silence for one block.
        try:
            if status:
                self.callback_status_events += 1
                LOGGER.debug("audio callback status: %s", status)
            block = self._ring.read(frames)
            try:
                # NumPy path: outdata has shape (frames, 1); (frames,)
                # broadcasts, so one channel is filled.
                outdata[:] = block
            except (TypeError, ValueError, IndexError):
                # Non-NumPy path: outdata is a plain buffer of int16 samples.
                memoryview(outdata).cast("B")[: len(block) * 2] = block.tobytes()
        except Exception:  # pragma: no cover - defensive audio-thread guard
            LOGGER.debug("audio callback failed", exc_info=True)

    def push(self, samples: array) -> None:
        dropped = self._ring.write(samples)
        if dropped:
            LOGGER.debug("ring buffer dropped %d samples", dropped)

    def close(self) -> None:
        stream, self._stream = self._stream, None
        if stream is None:
            return
        try:
            stream.stop()
        except Exception:  # pragma: no cover - backend teardown
            LOGGER.debug("stopping the audio stream failed", exc_info=True)
        try:
            stream.close()
        except Exception:  # pragma: no cover - backend teardown
            LOGGER.debug("closing the audio stream failed", exc_info=True)


# --- receiver ------------------------------------------------------------


@dataclass
class ReceiverConfig:
    """Everything the receiver needs; filled from the CLI."""

    address: str | None = None
    name: str = protocol.DEFAULT_DEVICE_NAME
    audio_device: str | int | None = None
    gain_db: float = 0.0
    wav_path: str | None = None
    dc_blocker: bool = True
    verbose: bool = False
    once: bool = False
    send_mic_open: bool = False
    mic_open_seconds: float = 10.0
    scan_timeout: float = 8.0
    ring_ms: int = DEFAULT_RING_MS
    reconnect_min_delay: float = 1.0
    reconnect_max_delay: float = 20.0
    stats_interval: float = 5.0


@dataclass
class StreamStats:
    """Counters for logging and the ``--wav`` verification report."""

    frames: int = 0
    encoded_bytes: int = 0
    samples: int = 0
    gaps: int = 0
    peak: int = 0
    sessions: int = 0
    session_id: int = 0
    session_frames: int = 0
    session_samples: int = 0
    session_peak: int = 0
    session_started: float = 0.0
    session_ended: float = 0.0

    def begin_session(self, session_id: int) -> None:
        self.sessions += 1
        self.session_id = session_id
        self.session_frames = 0
        self.session_samples = 0
        self.session_peak = 0
        self.session_started = time.monotonic()
        self.session_ended = 0.0

    def add_frame(self, sample_count: int, peak: int) -> None:
        self.frames += 1
        self.samples += sample_count
        self.session_frames += 1
        self.session_samples += sample_count
        if peak > self.peak:
            self.peak = peak
        if peak > self.session_peak:
            self.session_peak = peak

    @property
    def session_seconds(self) -> float:
        end = self.session_ended or time.monotonic()
        return max(0.0, end - self.session_started) if self.session_started else 0.0

    def session_summary(self) -> str:
        return (
            f"session {self.session_id}: {self.session_frames} frames, "
            f"{self.session_samples / SAMPLE_RATE:.2f} s, "
            f"peak {self.session_peak}, gaps {self.gaps}"
        )


class VoiceReceiver:
    """Reconnect loop, ATVV session handling and audio routing."""

    def __init__(
        self,
        config: ReceiverConfig,
        sink: AudioSink,
        wav: WavWriter | None = None,
    ) -> None:
        self._config = config
        self._sink = sink
        self._wav = wav
        self._gain_db = clamp_gain_db(config.gain_db)
        self._dc_blocker = DcBlocker() if config.dc_blocker else None
        self._decoder = protocol.AdpcmDecoder()
        self._accumulator = protocol.FrameAccumulator()
        self._caps: protocol.Caps | None = None
        self._stats = StreamStats()
        self._expected_seq: int | None = None
        self._stream_supported = True
        self._session_active = False
        self._loop: asyncio.AbstractEventLoop | None = None
        self._client: Any = None
        self._disconnected = asyncio.Event()
        self._session_finished = asyncio.Event()
        self._mic_close_task: asyncio.Task[None] | None = None
        self._last_stats = 0.0

    @property
    def stats(self) -> StreamStats:
        return self._stats

    # -- public API ------------------------------------------------------

    async def run(self) -> int:
        """Connect, stream and reconnect until ``--once`` or Ctrl-C.

        Returns a process exit code: ``0`` for a clean stop, ``1`` when the
        single attempt requested by ``--once`` never connected.
        """

        self._loop = asyncio.get_running_loop()
        delay = max(0.5, float(self._config.reconnect_min_delay))
        attempts = 0
        while True:
            attempts += 1
            LOGGER.info("connection attempt %d", attempts)
            try:
                connected = await self._run_once()
            except asyncio.CancelledError:
                raise
            except DependencyError:
                raise
            except EncryptionRequiredError as exc:
                LOGGER.error("%s", exc)
                connected = False
            except ReceiverError as exc:
                LOGGER.error("%s", exc)
                connected = False
            except Exception as exc:  # bleak backends raise their own errors
                LOGGER.error("connection failed: %s", exc)
                LOGGER.debug("connection failure detail", exc_info=True)
                connected = False

            if self._config.once:
                return 0 if connected else 1

            if connected:
                delay = max(0.5, float(self._config.reconnect_min_delay))
            LOGGER.info("reconnecting in %.0f s (Ctrl-C to stop)", delay)
            await asyncio.sleep(delay)
            delay = min(delay * 2.0, float(self._config.reconnect_max_delay))

    # -- one connection --------------------------------------------------

    async def _run_once(self) -> bool:
        BleakClient, BleakScanner, _BleakError = load_bleak()
        target = await self._find_device(BleakScanner)
        LOGGER.info("connecting to %s", self._describe_target(target))

        self._disconnected.clear()
        self._session_finished.clear()
        client = BleakClient(
            target, disconnected_callback=self._on_disconnected
        )
        self._client = client
        connected = False
        try:
            # bleak >= 1.0 returns None from connect(); success is signalled
            # by is_connected and by exceptions from the backend.
            await client.connect()
            if not getattr(client, "is_connected", True):
                raise ReceiverError(
                    "the BLE stack did not complete the connection; wake the "
                    "remote and retry"
                )
            connected = True
            LOGGER.info("link established")
            await self._try_pair(client)
            self._log_services(client)
            await self._subscribe(client)
            await self._write(client, protocol.build_get_caps(), "GET_CAPS")
            if self._config.send_mic_open:
                await self._write(
                    client, protocol.build_mic_open(), "MIC_OPEN (test mode)"
                )
                self._mic_close_task = asyncio.ensure_future(
                    self._auto_close(client)
                )
                LOGGER.info(
                    "MIC_OPEN sent; the device will stream without the touch area "
                    "for %.1f s",
                    self._config.mic_open_seconds,
                )
            LOGGER.info(
                "waiting for the device to start a voice session "
                "(hold the voice area on the remote)"
            )
            await self._wait_for_end()
        finally:
            self._client = None
            if self._mic_close_task is not None:
                self._mic_close_task.cancel()
                await asyncio.gather(
                    self._mic_close_task, return_exceptions=True
                )
                self._mic_close_task = None
            if self._session_active:
                self._end_session("link closed")
            await self._shutdown(client)
            self._log_stats(force=True)
        return connected

    async def _macos_connected_devices(self) -> list[Any]:
        """Return already-connected peripherals on macOS.

        CoreBluetooth stops advertising a peripheral while the operating system
        holds a connection (the device is also a BLE HID remote), so a normal
        scan cannot see it. ``retrieveConnectedPeripherals`` exposes the same
        CBPeripheral together with bleak's central-manager delegate, which is
        exactly what ``BLEDevice.details`` needs for a direct connection.
        """

        if platform.system() != "Darwin":
            return []
        try:
            from bleak.backends.corebluetooth.scanner import (
                BleakScannerCoreBluetooth,
            )
            from bleak.backends.device import BLEDevice
            from CoreBluetooth import CBUUID
        except Exception:  # pragma: no cover - non-macOS or bleak internals moved
            return []

        try:
            scanner = BleakScannerCoreBluetooth(
                detection_callback=None,
                service_uuids=None,
                scanning_mode="active",
                cb={},
            )
            await scanner._manager.wait_until_ready()
            devices: dict[str, Any] = {}
            for service_uuid in (protocol.SERVICE_UUID, "1812"):
                try:
                    peripherals = (
                        scanner._manager.central_manager
                        .retrieveConnectedPeripheralsWithServices_(
                            [CBUUID.UUIDWithString_(service_uuid)]
                        )
                    )
                except Exception as exc:
                    LOGGER.debug(
                        "retrieveConnectedPeripherals(%s) failed: %s",
                        service_uuid,
                        exc,
                    )
                    continue
                for peripheral in peripherals or ():
                    identifier = str(peripheral.identifier().UUIDString())
                    if identifier in devices:
                        continue
                    name = str(peripheral.name() or "")
                    devices[identifier] = BLEDevice(
                        identifier, name, (peripheral, scanner._manager)
                    )
            if devices:
                LOGGER.info(
                    "macOS already-connected candidates: %s",
                    ", ".join(
                        f"{device.name or 'unnamed'} ({device.address})"
                        for device in devices.values()
                    ),
                )
            return list(devices.values())
        except Exception:
            LOGGER.debug(
                "macOS connected-peripheral lookup failed", exc_info=True
            )
            return []

    async def _find_device(self, scanner_class: Any) -> Any:
        address = (self._config.address or "").strip()
        if address:
            LOGGER.info("using configured address %s", address)
            for device in await self._macos_connected_devices():
                if device.address.lower() == address.lower():
                    LOGGER.info(
                        "reusing the macOS connection to %s",
                        self._describe_target(device),
                    )
                    return device
            return address

        timeout = max(1.0, float(self._config.scan_timeout))
        LOGGER.info("scanning for %r (%.0f s) ...", self._config.name, timeout)
        discovered = await self._scan(scanner_class, timeout)
        if not discovered:
            raise DeviceNotFoundError(
                "no Bluetooth LE devices found; check that Bluetooth is on and "
                "the remote is awake"
            )

        needle = self._config.name.strip().lower()
        named = [
            entry for entry in discovered if self._name_matches(entry, needle)
        ]
        if named:
            named.sort(
                key=lambda entry: getattr(entry[1], "rssi", -127) or -127,
                reverse=True,
            )
            if len(named) > 1:
                LOGGER.warning(
                    "%d devices match %r; using the strongest signal",
                    len(named),
                    self._config.name,
                )
            return named[0][0]

        connected_named = [
            device
            for device in await self._macos_connected_devices()
            if needle and needle in (device.name or "").strip().lower()
        ]
        if connected_named:
            LOGGER.info(
                "using the macOS connection to %s (the peripheral is not advertising)",
                self._describe_target(connected_named[0]),
            )
            return connected_named[0]

        by_service = [
            entry for entry in discovered if self._advertises_atvv(entry[1])
        ]
        if len(by_service) == 1:
            LOGGER.warning(
                "no device named %r; using the only ATVV advertiser %s",
                self._config.name,
                self._describe_target(by_service[0][0]),
            )
            return by_service[0][0]
        if len(by_service) > 1:
            raise DeviceNotFoundError(
                f"found {len(by_service)} ATVV advertisers but none named "
                f"{self._config.name!r}; pass --address or --name"
            )

        connected = await self._macos_connected_devices()
        if len(connected) == 1:
            LOGGER.warning(
                "no advertising match; using the only macOS-connected "
                "candidate %s",
                self._describe_target(connected[0]),
            )
            return connected[0]
        raise DeviceNotFoundError(
            f"no device named {self._config.name!r} and no ATVV advertiser found; "
            "wake the remote, pair it, or pass --address"
        )

    async def _scan(self, scanner_class: Any, timeout: float) -> list[tuple[Any, Any]]:
        """Scan and normalise across bleak versions (``return_adv`` support)."""

        try:
            discovered = await scanner_class.discover(
                timeout=timeout, return_adv=True
            )
        except TypeError:  # bleak < 0.20 has no return_adv keyword
            plain = await scanner_class.discover(timeout=timeout)
            return [(device, None) for device in plain]
        if isinstance(discovered, dict):
            return list(discovered.values())
        return [(device, None) for device in discovered]

    @staticmethod
    def _device_name(device: Any, advertisement: Any) -> str:
        name = getattr(device, "name", None) or ""
        if not name and advertisement is not None:
            name = getattr(advertisement, "local_name", None) or ""
        return str(name)

    def _name_matches(self, entry: tuple[Any, Any], needle: str) -> bool:
        if not needle:
            return False
        name = self._device_name(entry[0], entry[1]).strip().lower()
        return bool(name) and (name == needle or needle in name)

    @staticmethod
    def _advertises_atvv(advertisement: Any) -> bool:
        if advertisement is None:
            return False
        for uuid in getattr(advertisement, "service_uuids", None) or ():
            if str(uuid).lower() == protocol.SERVICE_UUID:
                return True
        return False

    @staticmethod
    def _describe_target(target: Any) -> str:
        if isinstance(target, str):
            return target
        name = getattr(target, "name", None) or "unknown"
        address = getattr(target, "address", None) or "?"
        return f"{name} ({address})"

    async def _try_pair(self, client: Any) -> None:
        """Best-effort pairing, only where bleak can drive it.

        bleak implements ``pair()`` for BlueZ. CoreBluetooth pairs
        automatically when a protected characteristic is accessed, and the
        WinRT path is left to the Windows Bluetooth settings so the receiver
        never opens an unexpected pairing dialog for an already bonded device.
        """

        if platform.system() != "Linux":
            return
        pair = getattr(client, "pair", None)
        if pair is None:
            return
        try:
            await pair()
            LOGGER.info("pairing/encryption requested through BlueZ")
        except NotImplementedError:
            LOGGER.debug("this bleak backend does not expose pairing")
        except Exception as exc:
            LOGGER.warning(
                "pair() failed (%s); pair the device in the OS Bluetooth "
                "settings if the next step reports an encryption error",
                exc,
            )

    def _log_services(self, client: Any) -> None:
        """Warn when the remote does not expose the ATVV service at all."""

        try:
            services = getattr(client, "services", None)
            if services is None:
                return
            uuids = [str(service.uuid).lower() for service in services]
        except Exception:  # pragma: no cover - backend without cached services
            LOGGER.debug("cannot enumerate GATT services", exc_info=True)
            return
        if not uuids:
            return
        LOGGER.debug("GATT services: %s", ", ".join(uuids))
        if protocol.SERVICE_UUID not in uuids:
            LOGGER.warning(
                "the remote does not expose the ATVV service %s; the firmware "
                "may have been built without the voice feature",
                protocol.SERVICE_UUID,
            )

    async def _subscribe(self, client: Any) -> None:
        try:
            await client.start_notify(protocol.AUDIO_UUID, self._on_audio)
            await client.start_notify(protocol.CONTROL_UUID, self._on_control)
        except Exception as exc:
            if _looks_like_encryption_error(exc):
                raise EncryptionRequiredError(encryption_guidance()) from exc
            raise ReceiverError(
                f"cannot subscribe to the ATVV characteristics: {exc}"
            ) from exc
        LOGGER.info("subscribed to Audio and Control notifications")

    async def _write(self, client: Any, payload: bytes, description: str) -> None:
        try:
            await client.write_gatt_char(
                protocol.TX_UUID, payload, response=False
            )
        except Exception as exc:
            if _looks_like_encryption_error(exc):
                raise EncryptionRequiredError(encryption_guidance()) from exc
            LOGGER.debug(
                "write without response failed (%s); retrying with a response",
                exc,
            )
            try:
                await client.write_gatt_char(
                    protocol.TX_UUID, payload, response=True
                )
            except Exception as exc2:
                if _looks_like_encryption_error(exc2):
                    raise EncryptionRequiredError(encryption_guidance()) from exc2
                raise ReceiverError(
                    f"cannot write {description} to the remote: {exc2}"
                ) from exc2
        LOGGER.debug("wrote %s: %s", description, payload.hex(" "))

    async def _wait_for_end(self) -> None:
        waiters: list[asyncio.Future[bool]] = [
            asyncio.ensure_future(self._disconnected.wait())
        ]
        if self._config.once:
            waiters.append(asyncio.ensure_future(self._session_finished.wait()))
        try:
            await asyncio.wait(waiters, return_when=asyncio.FIRST_COMPLETED)
        finally:
            for waiter in waiters:
                waiter.cancel()
            await asyncio.gather(*waiters, return_exceptions=True)

    async def _auto_close(self, client: Any) -> None:
        """``--send-mic-open`` helper: close the synthetic session after N s."""

        try:
            await asyncio.sleep(max(0.0, float(self._config.mic_open_seconds)))
            if self._client is not client:
                return
            if not getattr(client, "is_connected", False):
                return
            await self._write(
                client,
                protocol.build_mic_close(self._stats.session_id),
                "MIC_CLOSE (test mode)",
            )
            LOGGER.info(
                "MIC_CLOSE sent after %.1f s", self._config.mic_open_seconds
            )
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            LOGGER.warning("cannot send MIC_CLOSE: %s", exc)

    async def _shutdown(self, client: Any) -> None:
        try:
            if getattr(client, "is_connected", False):
                for uuid in (protocol.AUDIO_UUID, protocol.CONTROL_UUID):
                    try:
                        await client.stop_notify(uuid)
                    except Exception:
                        LOGGER.debug("stop_notify(%s) failed", uuid)
        finally:
            try:
                await client.disconnect()
            except Exception:
                LOGGER.debug("disconnect failed", exc_info=True)

    def _on_disconnected(self, _client: Any) -> None:
        LOGGER.warning("link lost; waiting for the reconnect loop")
        loop = self._loop
        if loop is not None and not loop.is_closed():
            loop.call_soon_threadsafe(self._disconnected.set)

    # -- notifications ---------------------------------------------------

    def _on_audio(self, _sender: Any, data: bytearray) -> None:
        try:
            frames = self._accumulator.feed(data)
        except Exception:  # pragma: no cover - accumulator is total
            LOGGER.exception("dropping a malformed audio notification")
            return
        if not frames:
            return
        if not self._stream_supported:
            LOGGER.debug("ignoring %d frame(s) from an unsupported codec", len(frames))
            return
        for frame in frames:
            self._decode_frame(frame)
        self._log_stats()

    def _decode_frame(self, frame: bytes) -> None:
        samples = self._decoder.decode(frame)
        if not samples:  # defensive: frame_size is validated by Caps parsing
            return
        peak = max(max(samples), -min(samples))
        self._stats.add_frame(len(samples), peak)
        if self._expected_seq is not None:
            self._expected_seq = (self._expected_seq + 1) & 0xFFFF
        if self._wav is not None:
            self._wav.write(samples)
        processed = process_block(
            samples, gain_db=self._gain_db, dc_blocker=self._dc_blocker
        )
        self._sink.push(processed)

    def _on_control(self, _sender: Any, data: bytearray) -> None:
        raw = bytes(data)
        try:
            event = protocol.parse_control_event(raw)
        except protocol.ProtocolError as exc:
            LOGGER.warning(
                "ignoring malformed control notification %s: %s",
                raw.hex(" "),
                exc,
            )
            return
        LOGGER.debug("control: %s", protocol.describe_event(event))
        self._handle_event(event)

    def _handle_event(self, event: protocol.ControlEvent) -> None:
        if isinstance(event, protocol.Caps):
            self._handle_caps(event)
        elif isinstance(event, protocol.AudioStart):
            self._handle_audio_start(event)
        elif isinstance(event, protocol.AudioSync):
            self._handle_audio_sync(event)
        elif isinstance(event, protocol.AudioStop):
            self._handle_audio_stop(event)
        elif isinstance(event, protocol.MicOpenError):
            LOGGER.error("%s", protocol.describe_event(event))
            if event.code == 0x0001:
                LOGGER.error("%s", encryption_guidance())
        else:
            LOGGER.debug("unhandled control event: %s", protocol.describe_event(event))

    def _handle_caps(self, caps: protocol.Caps) -> None:
        self._caps = caps
        LOGGER.info("%s", protocol.describe_event(caps))
        if not caps.is_supported:
            LOGGER.warning(
                "unsupported stream (%s); audio will be dropped",
                protocol.codec_name(caps.codec),
            )
        self._stream_supported = caps.is_supported
        if caps.frame_size != self._accumulator.frame_size:
            LOGGER.warning(
                "device frame size is %d bytes, not %d; re-framing the stream",
                caps.frame_size,
                self._accumulator.frame_size,
            )
            self._accumulator = protocol.FrameAccumulator(caps.frame_size)
        rate = caps.sample_rate
        if rate and rate != self._sink.sample_rate:
            LOGGER.warning(
                "device streams %d Hz but the audio device runs at %d Hz; "
                "audio will play at the wrong speed (no resampling is applied)",
                rate,
                self._sink.sample_rate,
            )

    def _handle_audio_start(self, event: protocol.AudioStart) -> None:
        self._decoder.reset(0, 0)
        self._accumulator.reset()
        self._expected_seq = 0
        self._session_active = True
        self._session_finished.clear()
        self._stats.begin_session(event.session_id)
        LOGGER.info("%s", protocol.describe_event(event))
        if event.codec != protocol.CODEC_ADPCM_16K:
            LOGGER.warning(
                "session uses %s; expected ADPCM 16 kHz",
                protocol.codec_name(event.codec),
            )

    def _handle_audio_sync(self, event: protocol.AudioSync) -> None:
        dropped = self._accumulator.reset()
        self._decoder.reset(event.predictor, event.step_index)
        if self._expected_seq is not None and event.seq != self._expected_seq:
            missing = (event.seq - self._expected_seq) & 0xFFFF
            self._stats.gaps += 1
            LOGGER.warning(
                "sequence gap: expected %d, device resumes at %d (%d frame(s) lost)",
                self._expected_seq,
                event.seq,
                missing,
            )
        self._expected_seq = event.seq
        LOGGER.debug(
            "control: %s (dropped %d buffered byte(s))",
            protocol.describe_event(event),
            dropped,
        )

    def _handle_audio_stop(self, event: protocol.AudioStop) -> None:
        self._stats.session_ended = time.monotonic()
        LOGGER.info("%s", protocol.describe_event(event))
        self._end_session("device stopped")
        self._session_finished.set()

    def _end_session(self, reason: str) -> None:
        if not self._session_active:
            return
        self._session_active = False
        self._accumulator.reset()
        self._expected_seq = None
        LOGGER.info("%s (%s)", self._stats.session_summary(), reason)
        self._log_stats(force=True)

    def _log_stats(self, force: bool = False) -> None:
        if self._stats.frames == 0:
            return
        now = time.monotonic()
        if not force and (now - self._last_stats) < self._config.stats_interval:
            return
        self._last_stats = now
        LOGGER.info(
            "frames=%d samples=%d (%.1f s) gaps=%d dropped_bytes=%d "
            "underruns=%d peak=%d",
            self._stats.frames,
            self._stats.samples,
            self._stats.samples / float(self._sink.sample_rate),
            self._stats.gaps,
            self._accumulator.discarded_bytes,
            self._sink.underruns,
            self._stats.peak,
        )
