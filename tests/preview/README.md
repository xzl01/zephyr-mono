<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Native I1 preview and Wi-Fi interaction regression

Minimal host preview adopted from `tmp/field-notes/preview`; that older tool is
left untouched. This tool compiles a snapshot of the current production UI,
its generated assets and portable modules against **real LVGL**, not a mock
renderer. All maintained preview source is under this directory; artifacts
are written to `tmp/wifi-preview` by default.

## Run

From the repository root, with a C17 compiler, Python 3 + Pillow and an LVGL 9.x
source checkout (normally the workspace's `../modules/lib/gui/lvgl`):

```sh
bash tests/preview/build.sh
# Or a subset:
bash tests/preview/build.sh settings wifi-list wifi-symbols wifi-63chars
# Optional dependency / artifact overrides:
LVGL=/path/to/lvgl OUT=/absolute/path/to/artifacts PYTHON=python3 CC=cc \
  bash tests/preview/build.sh
```

PNG files are directly in `tmp/wifi-preview/`; the executable, UI snapshot,
PBMs, temporary host filesystem and content-addressed LVGL object cache are
in its `build/` subdirectory. `SOURCES.txt` records snapshot/config/header/asset
hashes. Existing output is not deleted; only the requested fixture names are
replaced. Run one build per output directory at a time. A changed shared UI
at the end of a build prints a reminder to rebuild.

## Fixtures

| Name | Synthetic state rendered by the real UI |
| --- | --- |
| `settings` | Production settings router/render path |
| `wifi-scan` | Scan pending, no saved network |
| `wifi-list` | First four of six networks: WPA2, open, unsupported WPA3, hidden |
| `wifi-list-2` | Second page, including a maximum-length SSID |
| `wifi-password` | Masked password, lower-case keyboard |
| `wifi-symbols` | Symbol keyboard and visible **demo-only** password |
| `wifi-63chars` | Visible 63-byte demo password, rejected 64th byte, length error |
| `wifi-connecting` | Saved network connection pending |
| `wifi-failure` | Failed saved network connection |
| `wifi-scan-failure` | Scan error |
| `wifi-empty` | Completed empty scan |
| `wifi-forget` | Destructive-action confirmation |

Test/fixture glue is appended to the byte-for-byte source snapshot in the same
translation unit. It directly calls production `settings_event`, `wifi_event`,
`process_event`, `render_current` and the router's registered `wifi_close`
(`on_close`) hook. It does **not** replace/rename production handlers or
renderers. Changes to private UI state may require updating the test glue.
Backend stubs include the current public headers so signature drift fails the
build. No host radio is accessed and no real credentials are used or saved.

## Production event assertions

Every preview process initializes the real portable modules/router, runs the
assertion runner **before** selecting its screenshot fixture, then resets test
state and renders that fixture. A failed assertion prints its condition and
snapshot line and exits **1** without writing the requested PBM. The runner is
independent of `assert`/`NDEBUG`; there is no screenshot-only bypass. The same
12 fixtures above remain available, without new demo screens or renderer polish.

Coverage uses production hit rectangles and actual keyboard hit testing:

- Settings Wi-Fi entry opens the Wi-Fi app, scans once, preserves synthetic
  saved SSID/configuration and never calls reset/forget. Selecting the saved
  SSID enters an empty masked keyboard rather than reconnecting automatically.
- Background `REFRESH` and `CONTENT_CHANGED` events preserve an in-progress
  draft, including visible display storage, even when the injected connection
  state and scan generation change.
- A seven-character password receives injected `-EINVAL` and retains its draft;
  an injected `-EBUSY` likewise retains it. An accepted eight-character demo
  request erases the **entire** input model and display buffer. The keyboard
  overflow latch rejects connect before even calling the backend API.
- BACK cancellation and HOME routing erase the draft and display buffer;
  HOME exercises the actual registered `on_close` hook and clears router history.
- Request-busy, scan-busy and connecting list states ignore scan/reconnect/forget,
  pagination and network-selection taps with no command/backend calls. An
  unconfigured state does not reconnect or enter forget confirmation.
- Forget opens confirmation without calling the backend; cancel does not call
  it. Only confirm calls it, with both injected rejection and acceptance checked.
- Paging/layer changes advance the input epoch and mark a layout transition;
  failed display refresh leaves the guard set and successful refresh releases
  it. `process_event` drops a stale-epoch touch but accepts a current-epoch key.

The stub records **API calls separately from accepted synthetic requests** and
supports return/state injection. It matches demo SSID/password arguments during
the call without retaining another password copy. In production, password length
validation lives in `papermono_content_server_connect_wifi`, **not** `wifi_event`:
short-password coverage injects that API's `-EINVAL` and verifies UI handling;
it does not claim that the UI avoids the API call or that backend validation,
credential persistence or a real connection was exercised. Neither acceptance
nor forget success runs a radio/NVS worker or mutates saved credentials.

## Verification and limits

- 480×800 `LV_COLOR_FORMAT_I1` display; full native monochrome flush, with the
  8-byte indexed palette removed before PBM conversion. No RGB screenshot or
  down-conversion/dithering is involved. PNG mode must be `1`.
- LVGL builtin heap is fixed at **64 KiB**; each process reports use/peak/free
  and checks allocator integrity. The separate 48,008-byte I1 draw buffer and
  host bookkeeping are **not** part of that heap; this is not a claim that the
  whole host process fits in 64 KiB.
- Each fixture runs in a fresh process, fails on interaction assertions,
  rendering errors/no flush, then checks dimensions, 1-bit format and
  nonblank/non-solid ink coverage. Reported heap peak includes the assertion
  renders as well as the final fixture.
- This is a host event regression plus visual smoke test, not Wi-Fi integration,
  provisioning/security, asynchronous worker, physical input or panel-refresh
  verification. `CONFIG_PAPERMONO_INTERACTIVE_UI` is not enabled in this build:
  the physical-input queue, `submit_input_event` transition gate, queue overflow
  production and touch-capture timing remain untested. Display return injection
  tests guard bookkeeping, not actual e-paper completion. Hardware, network, RTC
  and settings persistence are host shims. Portable UI dependencies are real;
  old Field Notes verify-control assertions are not carried forward.
- Former unresolved `preview_verify_field_notes` is removed; the power shim
  implements the current `papermono_power_input_wake_ready` API.

All preview source in this directory is GPL-3.0-only. LVGL remains an external
dependency under its own license.
