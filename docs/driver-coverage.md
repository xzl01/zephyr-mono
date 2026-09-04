# PaperMono C153 driver coverage and HIL status

This matrix separates implementation, build validation, controller-level HIL,
and tests that require an external accessory or a deliberate power/RF action.
Rows retain the strongest dated evidence from earlier candidate images; they do
not imply that the current HEAD passed the same physical checks. The current V2
image was written and verified but has not yet produced a new display frame due
to an Xtensa startup exception under investigation.

| Function | Zephyr interface / extension | Current validation |
| --- | --- | --- |
| SSD1677 monochrome | `display` full refresh | Cold-start board HIL passed with physical RESET-release verification; final full refresh completed in 4322 ms and LVGL rendered. See [SSD1677 HIL findings](ssd1677-hil.md); display refresh after RX8130 wake with the final fix remains a separate retest |
| SSD1677 partial | `display` plus automatic baseline/maintenance | Board HIL passed: ten updates at 737-738 ms, maintenance full refresh at 4321 ms, then the next partial at 737 ms |
| SSD1677 four gray | `papermono_ssd1677_refresh_gray4()` | Controller HIL passed with white/light/dark/black quadrants |
| Frontlight | `display_set_brightness()` via M5PM1 PWM | Board HIL passed |
| Touch | C153 FT6336 dedicated 8 ms polling input + GPIO4 wake | Physical coordinates and EXT0 touch wake passed on 2026-08-28. The 2026-08-31 reliability candidate also passed FT6336 press/release, UI queue/process, refresh-time gesture retention and repeated 737-738 ms partial refresh on hardware. The current V2 image still requires a fresh physical interaction pass |
| Side buttons | Zephyr `gpio-keys` input | Both physical buttons passed |
| RGB LED | Zephyr LED API; centralized storage/Wi-Fi/BLE/activity status policy; M5PM1 red plus M5IOE1 PWM green/blue | Raw channel Board HIL passed on an earlier candidate. Priority/color policy host tests and default build pass; physical confirmation of current V2 boot, transfer/error states and deep-sleep-off behavior still requires replay |
| Buzzer | Zephyr PWM, ESP32-S3 LEDC GPIO42 | 2 kHz tone HIL passed |
| M5IOE1 | GPIO/IRQ/PWM, ADC, temperature, raw registers, I2C sleep/wake | Identity, ADC, temperature, GPIO/PWM/IRQ and sleep/wake HIL passed. Firmware-emulated I2C reads now preserve the official 500 us pointer-latch interval and bounded recovery; post-deep-sleep parent readiness passed timer HIL |
| M5PM1 | GPIO/IRQ/PWM, power rails, voltage, watchdog, battery LVP, RTC RAM, wake control | Identity, voltage, GPIO/PWM/IRQ, sleep/wake and RTC RAM HIL passed. Firmware-emulated I2C pacing/recovery passed timer wake; watchdog and battery-LVP APIs remain intentionally excluded from the non-destructive automatic self-test |
| BMI270 | Zephyr sensor and motion trigger | Accelerometer, gyroscope and motion-trigger setup HIL passed |
| RX8130 | Zephyr RTC plus countdown-timer extension | Time/alarm HIL passed; countdown timer powered the board back up through M5PM1 G0 and the boot wake source was decoded correctly |
| Battery/USB | M5PM1 voltage and power-source APIs | VBAT/VIN/5V telemetry HIL passed |
| IP2315 charger | Zephyr charger API through gated I2C address `0x75` | Driver matches the M5Unified gated access sequence; this board did not ACK the optional register interface, so status falls back to M5PM1 telemetry and the self-test reports a warning |
| microSD | Zephyr SDHC/SDMMC, 4-bit bus, slot detect/power | Detect HIL passed again with `inserted=0`; empty-slot handling now avoids starting SDHC. No card was inserted, so media read/write and hot removal remain accessory-required |
| PDM microphone | Board capture API over Zephyr I2S/DMA | 16 kHz PCM capture HIL passed |
| Wi-Fi | Zephyr ESP32 STA/AP, DHCP server and HTTP server | Mac HIL passed SoftAP discovery (802.11b/g/n, channel 6, WPA2), association, DHCP lease from `192.168.4.1`, public status/index HTTP 200, unauthorized content HTTP 401, and one Wi-Fi off/on rejoin. Authenticated upload/delete still requires a writable TF card; phone-browser validation remains pending |
| BLE HID | Zephyr HIDS/BAS/DIS, SMP and Settings bonds | Controller, advertising setup and composite service initialization passed; real macOS pairing and all media/presentation/mouse reports remain required |
| BLE sync | Authenticated custom GATT service | Service and worker initialization passed; real client START/DATA/COMMIT, CRC, disconnect abort and overlay import remain required |
| SX1262 LoRa | Zephyr LoRa API | Configure-only HIL passed at 868 MHz; finite RX and explicit TX profiles build; no peer receive and no RF transmit HIL performed |
| ST25R3916 NFC | Board NFC-A/B/F/V polling API | Identity and RF configuration HIL passed for all four modes with legal no-tag timeouts; physical tags remain accessory-required |
| ESP32-S3 power | Timer/touch deep sleep | Both profiles passed end to end on 2026-08-28. Timer: wake source 2 and 4316 ms full refresh. Touch: wake source 1, coordinates and 4318 ms full refresh. Both restored the LVGL page and logged the explicit verification marker |
| M5PM1 power | Button/RX8130/BMI270 shutdown wake | All three default-off profiles build. RX8130 shutdown/wake and wake-source decoding passed; the post-wake display recovery retest is pending. Button and BMI270 wake remain interactive |

## Test profiles

The base self-test is enabled only by `driver_selftest.conf`. It exercises all
non-destructive paths and never transmits LoRa data or enters a power state.
Every automatic check logs its subsystem name and result so a failure does not
collapse into an opaque final error code.

LoRa and power profiles must be combined with the base self-test, for example:

```sh
west build -p always -d build-power-timer \
  -b m5stack_papermono/esp32s3/procpu . -- \
  -DEXTRA_CONF_FILE='driver_selftest.conf;power_timer_hil.conf'
```

The following tests remain intentionally external or interactive:

- M5PM1 watchdog reset and battery-LVP cutoff are default-off destructive HIL;
  API presence is not evidence that either action passed on this board.
- NFC tag UID/ATQA/SAK/PUPI/IDm/DSFID decoding with matching physical tags.
- microSD media initialization and read/write with a card inserted.
- LoRa over-the-air receive with a peer, and transmit only in a legal setup.
- Post-RX8130-wake SSD1677 refresh with the final RESET workaround.
- PMIC power-button wake and BMI270 motion wake.
- IP2315 register status on hardware where the gated charger interface ACKs.
- Authenticated HTTP upload/delete from a phone and Mac with a writable TF card; Mac SoftAP discovery, DHCP, public HTTP and unauthorized rejection have passed.
- BLE passkey pairing, bond restore and all composite HID reports on a real Mac.
- BLE small-data transfer with a real GATT client.

## Build validation snapshot

On 2026-08-28, the default, timer HIL, and touch HIL profiles were rebuilt after
the firmware-emulated I2C pacing fix; all eighteen host-test groups passed.
On 2026-08-26, partial-demo, LoRa RX/TX, and M5PM1 button/RTC/IMU shutdown
profiles built successfully with Zephyr 4.4.1. The final Kconfig output was checked to ensure each explicit
HIL build actually selected its requested action, while the default build kept
self-test, periodic partial refresh, RF action, PM, and `POWEROFF` disabled.
