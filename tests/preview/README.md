<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Native I1 preview and Field Notes/Wi-Fi regressions

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
# Four-tab production voice UI is enabled by default. Compare the supported
# no-microphone build (voice fixtures are unavailable in this variant):
PAPERMONO_PREVIEW_BLE_MIC=0 bash tests/preview/build.sh remote-mouse
# Optional dependency / artifact overrides:
LVGL=/path/to/lvgl OUT=/absolute/path/to/artifacts PYTHON=python3 CC=cc \
  bash tests/preview/build.sh
```

PNG files are directly in `tmp/wifi-preview/`; the executable, UI snapshot,
PBMs, temporary host filesystem and content-addressed LVGL object cache are
in its `build/` subdirectory. Each fixture gets a newly created, separate host
filesystem under `build/fs/`, so imported sample books or events cannot leak
into a later empty-state fixture. These small synthetic filesystem snapshots
are retained with the other artifacts. `SOURCES.txt` records
snapshot/config/header/asset hashes and the microphone UI compile option. Existing output is not deleted; only the requested fixture names are
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

The default run also covers the existing Field Notes shell and storage flow:
`home`, `launcher-1`, `launcher-2`, `library`, `weread`, `calendar`, `news`,
`news-detail`, `news-empty`, `news-error`, `article-long`, `tasks`,
`notes`, `info`, `content-center`, `remote`, `remote-pairing`, `remote-passkey`,
`remote-media`, `remote-presentation`, `remote-mouse`, `tools`, `system`,
`system-unformatted`, `power`, `confirm-sleep`, `confirm-shutdown`,
`confirm-ble`, `confirm-reset` and `confirm-format`.
Remote fixtures cover an idle link, pairing in progress, a synthetic six-digit
passkey, and the media, presentation and mouse modes on a secured link. By
default `CONFIG_PAPERMONO_BLE_MIC` is also defined, matching its current
firmware default-y configuration: all remote screenshots then use four tabs.
`PAPERMONO_PREVIEW_BLE_MIC=0` preserves the three-tab variant and excludes voice
fixtures. These render the production page within the same 64 KiB LVGL heap;
they do not exercise a Bluetooth stack, PDM capture, ADPCM encoding or a host
receiver.

Additional fixtures cover the current standby summaries and representative real
portable-model content:

| Name | Synthetic input, rendered and parsed by production code |
| --- | --- |
| `home-populated` | Three TXT files, a loaded real reader page, upcoming ICS events, tasks and notes |
| `library-populated` | Three imported TXT files, including a long Chinese book title |
| `tasks-populated` | Three imported JSON tasks, including a long title and a completed item |
| `notes-populated` | Two imported Markdown notes, including a long title |
| `remote-voice` | Secured host has not subscribed; production waiting guidance |
| `remote-voice-streaming` | Subscribed synthetic voice worker in STREAMING phase |
| `remote-voice-error` | Subscribed synthetic voice worker reports `-EIO` |
| `sleep` | Default quote summary with valid date and synthetic weather |
| `sleep-weather` | Eight moderate synthetic temperature samples |
| `sleep-weather-extreme` | Eight samples spanning −40 to 60 °C for chart containment |
| `sleep-weather-flat` | Eight equal samples for a zero-range chart |
| `sleep-no-weather` | Invalid weather snapshot with hourly summary selected |
| `sleep-no-date` | Unavailable synthetic RTC; no fabricated date |
| `sleep-reading` | Imported TXT and real reader page; active Library captures its latest progress for standby |
| `sleep-reading-home` | Real second reader page, actual Library close and arena erase, then cached page/percent/bar on standby |
| `sleep-reading-rss` | Same close path followed by a full RSS body load into the reused arena; standby retains cached progress |
| `sleep-reading-record` | Same recent-book record without a loaded page; progress remains unknown |
| `sleep-reading-empty` | Reading summary with no books or recent record |
| `sleep-tomorrow` | An imported ICS event on the day after the fixed preview date |
| `sleep-tomorrow-empty` | Tomorrow summary with no event |
| `sleep-blank` | Deliberately blank summary band |

The default build has 73 fixtures; disabling microphone UI leaves 70. Populated
content is authored demo material and is imported through the real content-store
transaction API. Calendar/tasks/notes use the real portable parsers and organizer;
reader progress comes from the real page reader and in-memory Settings shim,
not a hand-written percentage. The existing unqualified `home`, `library`,
`tasks` and `notes` fixtures remain empty states. All host files are confined to
the selected artifact directory; no real TF card, host documents or persistent
settings are touched.

The unformatted-card fixture injects a mount error into the host filesystem shim and reaches the
real portable storage model; it never formats or accesses a host disk.
Confirmation fixtures render the real production page without executing its
destructive action. Dates, network state and measurements remain synthetic.

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
independent of `assert`/`NDEBUG`; there is no screenshot-only bypass. The
Wi-Fi fixtures remain available alongside the neighboring Field Notes pages.

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
- Reading standby fixtures assert the real page number, percentage and progress
  bar after Library closes, including a full RSS body overwriting its old arena.
  Active Library captures fresh progress; a recent-book record without a loaded
  page keeps the unknown-progress hint and omits the bar.

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
- Labels using `LV_LABEL_LONG_CLIP`, including keyboard keys, must fit their
  actual content rectangle with the current font. Truncated captions fail the
  preview instead of silently producing an incomplete key.
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
