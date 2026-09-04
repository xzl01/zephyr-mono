# PaperMono C153 Product

<!-- impeccable:product-schema 1 -->

## Platform

Embedded Zephyr/LVGL interface on a fixed 480 x 800 monochrome e-paper display.

## Users

The primary user carries or places the C153 nearby as a quiet personal information
terminal. Content is prepared on a phone or computer; the device is used for
reading, checking information, lightweight state changes, and remote-control
actions.

## Product Purpose

Turn the M5Stack Paper Mono C153 into a dependable, local-first personal terminal
for books, schedules, tasks, notes, information cards, content transfer, and BLE
HID control. Success means the important path is legible, responsive, recoverable,
and physically verified on the e-paper device.

## Positioning

The product combines a calm retained e-paper surface with local content ownership:
large user content remains on the TF card, settings and small state remain in NVS,
and text authoring happens on a phone or computer rather than through an on-device
keyboard.

## Operating Context

- Portrait 480 x 800 display with touch and physical buttons.
- Monochrome full and partial refresh, where unnecessary large black changes cost
  time, power, and ghosting.
- Chinese-first interface with English, numbers, and common punctuation.
- Wi-Fi content center and BLE HID may run while the UI remains usable.
- Deep sleep retains the last displayed image and restores the active application
  after wake.

## Capabilities and Constraints

- Zephyr, LVGL, Settings/NVS, FATFS, SSD1677, FT6336, Wi-Fi, and BLE remain the
  product stack.
- Applications request refreshes through the display scheduler and background
  threads do not call LVGL directly.
- The device does not provide a soft keyboard or unrestricted text editing.
- Current WeRead support is a credential-free local shelf summary; it does not
  embed account cookies, private APIs, or book contents.
- Fonts must remain one-bit and cover every Chinese character shipped in the UI.
- Build success, flashing, application boot, visible layout, touch behavior, and
  physical hardware validation are separate acceptance claims.

## Brand Commitments

- Product name: PaperMono.
- Default language: Simplified Chinese.
- The new interface is explicitly inspired by hand-drawn monochrome editorial UI
  and uses the ink drop as its design prototype without limiting components to
  circular geometry.
- Paper white is the dominant working surface. Solid black is a compact accent,
  not the default background for navigation or changing information.

## Evidence on Hand

- Zephyr/LVGL source, reproducible firmware builds, and host-side regression tests
  are present in this repository. The current V2 image has not yet completed a
  successful physical boot/display validation.
- The current revision has a host-side one-bit LVGL layout-preview path, but that
  preview is not evidence of the final SSD1677 framebuffer or physical display.
- The design direction is pinned to the user's attached monochrome editorial UI
  reference rather than a temporary local attachment path.
- Real-device display and touch validation remains necessary after each material
  UI or refresh change.

## Product Principles

1. Prioritize readable daily use over hardware-demo density.
2. Keep content local and credentials outside the device unless explicitly
   reviewed.
3. Make touch and physical-button navigation predictable and recoverable.
4. Design for e-paper refresh behavior rather than importing phone UI habits.
5. Treat physical validation as part of completion.
