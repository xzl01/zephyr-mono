# PaperMono C153 SSD1677 HIL findings

> Historical snapshot from 2026-08-26. Paths and the device image below record
> the state at that test date; later cleanup removed the build tree and later
> validation replaced the flashed firmware. Current status is tracked in
> [acceptance.md](acceptance.md).

## Summary

The SSD1677 full-refresh and partial-refresh paths now pass on the tested
M5Stack PaperMono C153. The final default firmware completed a cold-start
monochrome refresh in 4322 ms and then rendered the LVGL page. A separate
finite partial-refresh profile completed ten consecutive partial updates,
performed its scheduled maintenance full refresh, and resumed partial updates.

The system-level failure was an electrically asserted active-low panel RESET.
The M5IOE1 output latch could read high while the corresponding physical input
still read low. The SSD1677 therefore remained in hardware reset, so software
reset and waveform commands could not recover it.

The final driver asserts RESET with a strong low output, releases it by
disabling the M5IOE1 output stage and enabling its internal pull-up, and then
checks the physical input value. This is a verified workaround for this test
unit, not yet a production-qualified hardware fix.

## Test target and final artifact

| Item | Value |
| --- | --- |
| Test date | 2026-08-26 |
| Product | M5Stack PaperMono C153 |
| MCU | ESP32-S3R8 |
| Display | 480 x 800 SSD1677 monochrome e-paper |
| Zephyr | 4.4.1 |
| Host connection | Native USB Serial/JTAG only |
| USB identity | Use the unit's live USB serial and MAC; do not copy identifiers between devices |
| Final profile | Default application, HIL actions disabled |
| Final image at test time | `build-default/zephyr/zephyr.bin` |
| Image size | 959828 bytes |
| SHA-256 | `96505c00d7b44fb69a2d1f6de8b8e0b8fc3de239afe352225fdac4e9b539008f` |

The device was left running that default image after this validation. The
partial-refresh and power profiles remain reproducible from their checked-in
configuration files but are not left on the device.

## Workspace cleanup

At that time, the workspace was reduced from approximately 2.2 GB to 184 MB.
The following were retained during that cleanup:

- the final reproducible `build-default/` tree and its flashable image;
- all source, board, binding, Kconfig, profile, patch, script, and documentation
  files; and
- one canonical copy of the C153 schematic used during the investigation.

The old default/self-test, partial-demo, LoRa, and power-profile build trees
were removed because their checked-in configuration files can reproduce them.
The Zephyr capability cache, Python bytecode cache, duplicate schematic PDF,
and duplicate rendered schematic pages were also removed. This reclaimed about
2.0 GB while preserving the final firmware and every build profile definition.

## Signal mapping and polarity

| Signal | Controller | Pin | Polarity / purpose |
| --- | --- | --- | --- |
| MOSI | ESP32-S3 SPI2 | GPIO14 | SPI mode 0 data |
| SCLK | ESP32-S3 SPI2 | GPIO15 | SPI mode 0 clock |
| CS | ESP32-S3 | GPIO16 | Active low |
| DC | ESP32-S3 | GPIO17 | High for data |
| BUSY | ESP32-S3 | GPIO18 | Active high |
| EPD_EN | M5IOE1 | PIN3 / Zephyr index 2 / bit 2 | Active high panel rail enable |
| RESET | M5IOE1 | PIN5 / Zephyr index 4 / bit 4 | Active low panel reset |

The C153 schematic routes RESET directly from the M5IOE1 PIN5 signal to the
display connector. R61 is an optional, unpopulated BUSY pull-up; it is not in
the RESET path.

## Evidence and root cause

The decisive evidence was the difference between the M5IOE1 output latch and
its physical input sense:

| Phase | OUT | IN | BUSY | Interpretation |
| --- | --- | --- | --- | --- |
| RESET asserted | `0x1224` | `0x1224` | High or not usable | PIN5 / bit 4 is physically low |
| Old push-pull release | `0x1234` | `0x1224` | `1` | Latch is high, but PIN5 / bit 4 remains physically low |
| Final input/pull-up release | `0x1224` | `0x1234` | `0` | Output stage is off and physical RESET is high |

`OUT=0x1224` after the final release is intentional: the old low output latch is
preserved while the pin is in input mode. `IN` bit 4 is the electrical proof of
release. With physical RESET held low, the SSD1677 cannot process `0x12`,
`0x22`, `0x20`, or other SPI recovery commands. The previous permanent BUSY
condition was therefore a consequence of hardware reset, not proof of an
incorrect waveform or BUSY polarity.

The remaining component-level cause has not been isolated. Current evidence
narrows it to the M5IOE1 PIN5 push-pull high path, its mode/drive state, or the
board-level RESET network. The successful weak pull-up makes a fixed hard short
less likely, but this has not been verified with an oscilloscope or across
multiple boards.

## Final implementation

The accepted driver behavior is:

1. Assert RESET by configuring the active-low line as an active output.
2. Hold the physical low level for 10 ms.
3. Release RESET by configuring the line as input with the M5IOE1 pull-up.
4. Wait 10 ms and read the physical input through `gpio_pin_get_dt()`.
5. Return `-EIO` immediately if RESET is still physically low.
6. During a deep recovery, hold RESET, CS, and DC low while the panel rail is
   off to prevent back-powering, wait 15 seconds, restore the rail, and repeat
   the verified reset pulse.

The M5IOE1 driver also now:

- waits 500 microseconds after every I2C register transaction, including a
  retry after bus recovery;
- reads the GPIO output register before masked writes and toggles;
- reads the output register back after each change; and
- updates the cached latch only after the hardware value is verified.

The e-paper state machine keeps a full-refresh baseline, limits consecutive
partial updates to ten, performs a maintenance full refresh at the limit, and
falls back to a full refresh after a failed partial update.

## HIL results

### Final default cold start

```text
SSD1677 lines reset-pulse-complete: EPD_EN=1 RESET(active)=0 BUSY=0 IN=0x1234 OUT=0x1224
PaperMono SSD1677 transport ready: 480x800, SPI 20000000 Hz
Starting SSD1677 monochrome OTP full refresh
SSD1677 full refresh 1 complete in 4322 ms; baseline ready
PaperMono LVGL page rendered
```

Acceptance result: **passed**.

### Interactive application follow-up

The final default image also rendered pages 2 and 3 through the event-driven
application loop. Both page changes used a 739 ms partial refresh. After the
second transition, no additional refresh was logged before the 90-second USB
capture ended. This verifies that the refresh-time input guard removed the
previous FT5336/display-noise feedback loop.

### Partial-refresh state machine

| Check | Observed result |
| --- | --- |
| Consecutive partial updates 1-10 | All passed, 737-738 ms each |
| Scheduled maintenance full refresh | Passed, 4321 ms |
| First partial update after maintenance | Passed, 737 ms |
| Finite HIL completion marker | `Partial-refresh HIL demo complete` |

Acceptance result: **passed**.

### Power-test boundary

The RX8130 timed PMIC shutdown/wake and wake-source decoding passed in an
earlier, separate HIL run. That result proves the power wake path, but the
end-to-end display refresh after RX8130 wake has not been replayed with the
final RESET workaround. It remains a separate acceptance item and must not be
inferred from the final cold-start result.

## USB-only reproduction

This is a historical device-lab procedure, not proof for the current HEAD.
For a fresh workspace, first complete the manifest and patch setup documented
in the repository README, then build the default firmware from the workspace
root:

```sh
env CCACHE_DISABLE=1 west build -p always -d build-default \
  -b m5stack_papermono/esp32s3/procpu zephyr-mono -- \
  -DBOARD_ROOT="$PWD/zephyr-mono" -DDTS_ROOT="$PWD/zephyr-mono"
```

Flash the image after verifying the USB device identity:

```sh
esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 921600 \
  --before default-reset --after no-reset write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 build-default/zephyr/zephyr.bin
```

Exit the ROM loader without requiring a UART adapter:

```sh
esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX \
  --before no-reset --after watchdog-reset --no-stub chip-id
```

Capture the native USB console by stable USB serial number, from the workspace
root:

```sh
python3 zephyr-mono/scripts/papermono_usb_hil_capture.py \
  --serial <USB_SERIAL> --timeout 60 --reconnect-settle 2
```

On this unit, dual-core OpenOCD treated the unused second core as an SMP peer
and could report a stale `_DoubleExceptionVector` or pause CPU0 during USB
re-enumeration. Use single-core mode (`set ESP32_S3_ONLYCPU 1`) when a debugger
is required. Start the application only after passive USB capture is attached,
then stop GDB/OpenOCD while USB is disconnected in deep sleep; leaving OpenOCD
attached can halt the fresh boot before the wake refresh finishes. This is a
host-side HIL procedure, not a firmware runtime dependency.

## Acceptance status

| Requirement | Status |
| --- | --- |
| RESET physical input reads released | Passed |
| BUSY reaches ready after reset | Passed |
| SSD1677 transport initialization | Passed |
| Default monochrome full refresh | Passed |
| LVGL page rendered | Passed |
| Ten consecutive partial updates | Passed |
| Automatic maintenance full refresh | Passed |
| Partial update after maintenance | Passed |
| RX8130 timed shutdown/wake | Passed separately |
| Display refresh after RX8130 wake with final fix | Pending retest |
| ESP timer deep-sleep wake and post-wake display refresh | Passed, 4316 ms full refresh and `Power HIL wake verified` |
| Touch deep-sleep wake and post-wake display refresh | Passed, wake source 1, touch coordinates, 4318 ms full refresh and `Power HIL wake verified` |
| Forced 15-second recovery branch | Pending fault-injection test |
| Multi-board and environmental qualification | Not performed |

The current acceptance statement is therefore:

> The tested C153's Zephyr + LVGL e-paper cold-start, full-refresh,
> partial-refresh, ESP-timer deep-sleep recovery, and touch deep-sleep recovery
> paths are resolved. RX8130 PMIC-shutdown display recovery, 50-cycle touch
> endurance, and production reliability remain separate validation work.

## Production recommendations

- Measure RESET rise time, VIH margin, and noise immunity at the display
  connector; do not qualify the internal weak pull-up from one sample alone.
- Evaluate a defined external RESET pull-up or direct MCU control for a future
  hardware revision after checking SSD1677 leakage and worst-case conditions.
- Add RESET test access and verify the FPC and M5IOE1 PIN5 network during board
  test.
- Make production test read MODE, PU, PD, DRV, IN, and OUT; checking only the
  output latch would miss this failure.
- Repeat cold-start and RTC-wake display tests across board lots, low battery,
  temperature corners, and interrupted full/partial refresh phases.
- Keep recovery bounded and fail fast when physical RESET cannot be released.

## References

- [M5Stack C153 product documentation](https://docs.m5stack.com/en/products/sku/C153)
- [M5Stack PaperMono UserDemo](https://github.com/m5stack/M5PaperMono-UserDemo)
- [M5Stack PaperMono OTP Demo](https://github.com/m5stack/M5PaperMono-OTP-Demo)
- [M5Stack M5IOE1](https://github.com/m5stack/M5IOE1)
- [M5Stack M5Unified](https://github.com/m5stack/M5Unified)
- [SSD1677 data sheet](https://files.waveshare.com/upload/2/2a/SSD1677_1.0.pdf)
