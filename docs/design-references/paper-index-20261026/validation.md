# PaperMono Paper Index — 2026-10-26

Code-led refinement of the existing Field Notes identity. The user requested overall consistency, prioritizing home, applications and lists.

All 56 individual PNGs are real 480×800 native LVGL I1 renders of production UI code, with synthetic content, network, hardware and microphone states. They are not device photographs or physical display acceptance. Source hashes and fixture names are in SOURCES.txt.

The before/ captures are from 5c0174e. Their prior preview configuration omitted MIC and shows three remote tabs; the new default matches the existing four-tab production configuration. Voice is not a new production feature in this change.

Host regressions and all 56 default preview fixtures pass. Firmware builds using an in-project USER_CACHE_DIR, preserving the external SDK sources. Internal DRAM is 338,520 / 399,108 bytes (84.82%, limit 85%). Reducing the fixed line pool from 96 to 64 entries saves 512 bytes of internal DRAM; the measured maximum across all fixtures is 19 lines, and capacity exhaustion is asserted. Removing nine launcher border objects reduces host LVGL used memory from 15,648 to 13,400 bytes on the launcher fixture (64-bit host ABI; not a target heap measurement).

Independent finish review initially returned **fix**. The two material items were obsolete touch-wake copy and the stale design-system geometry. The power and sleep-confirmation views were recaptured over the same files; DESIGN.md and the local, ignored .impeccable/design.json were refreshed by the documenter. The follow-up verdict returned **ship**, covering those two resolved findings only. See review.md for scope and unread optional captures.

A separate no-microphone build also rendered remote-media, remote-mouse and settings successfully. After the final wake-copy correction, all 56 default captures and the production firmware were regenerated. The entire host regression suite had passed before this final copy-only correction.

Reproduce:

```sh
bash tests/host/run.sh
OUT=tmp/style-refresh-20261026/after bash tests/preview/build.sh
PAPERMONO_PREVIEW_BLE_MIC=0 OUT=tmp/style-refresh-20261026/no-mic bash tests/preview/build.sh remote-media remote-mouse settings
# SDK/toolchain location is environment-specific; keep the cache writable inside the project.
west build -b m5stack_papermono/esp32s3/procpu -d build-style-refresh -- -DUSER_CACHE_DIR="$PWD/tmp/style-refresh-20261026/zephyr-cache"
python3 scripts/check_memory_budget.py build-style-refresh/zephyr/zephyr.map
```

This change does not alter BLE transport, touch polling, network retrieval or power/wake configuration. No firmware flash, NVS erase or TF operation was performed. Physical contrast, touch behavior, partial-refresh artifacts and long-run device acceptance remain unverified for this UI revision.
