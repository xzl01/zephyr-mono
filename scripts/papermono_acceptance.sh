#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
#
# Automated acceptance run for the connected PaperMono C153.
#
# Runs everything that does not need a human (host tests, build, boot check,
# flash, self-test) and then tells you exactly which manual steps remain.
#
# Usage: scripts/papermono_acceptance.sh [build-dir]
#   build-dir defaults to ./build
set -eu

BUILD_DIR="${1:-build}"
SERIAL_NO="${PAPERMONO_USB_SERIAL:-28:84:85:44:6D:A4}"
fail=0

step() {
	printf '\n=== %s ===\n' "$1"
}

ok() {
	printf '  OK   %s\n' "$1"
}

bad() {
	printf '  FAIL %s\n' "$1"
	fail=$((fail + 1))
}

step "1/6 host tests"
if sh tests/host/run.sh >/tmp/papermono_host_tests.log 2>&1; then
	count=$(grep -c PASS /tmp/papermono_host_tests.log || true)
	ok "host suite passed (${count} groups)"
else
	bad "host suite failed, see /tmp/papermono_host_tests.log"
fi

step "2/6 firmware build ($BUILD_DIR)"
if west build -p always -d "$BUILD_DIR" -b m5stack_papermono/esp32s3/procpu . \
	> /tmp/papermono_build.log 2>&1; then
	warnings=$(grep -cE 'warning:|error:' /tmp/papermono_build.log || true)
	if [ "$warnings" -eq 0 ]; then
		ok "build clean, no warnings"
	else
		bad "build has ${warnings} warning/error lines"
	fi
else
	bad "build failed, see /tmp/papermono_build.log"
fi

step "3/6 device console"
if python3 scripts/papermono_usb_recover.py --serial "$SERIAL_NO" --verify-only \
	--log /tmp/papermono_console.raw.log > /tmp/papermono_console.log 2>&1; then
	ok "fresh boot verified (TF/UI/BLE)"
else
	bad "fresh boot unverified; inspect /tmp/papermono_console.log (not a power diagnosis)"
fi

if [ "$fail" -ne 0 ]; then
	printf '\nStopping: fix the failures above before flashing.\n'
	exit 1
fi

step "4/6 flash current build"
if python3 scripts/papermono_usb_recover.py --serial "$SERIAL_NO" \
	--log /tmp/papermono_flash.raw.log "$BUILD_DIR/zephyr/zephyr.bin" \
	> /tmp/papermono_flash.log 2>&1; then
	ok "image hash verified and fresh application boot verified"
else
	bad "flashing failed, see /tmp/papermono_flash.log"
	exit 1
fi

step "5/6 boot log checks"
if python3 scripts/papermono_usb_hil_capture.py --serial "$SERIAL_NO" \
	--mode boot --timeout 35 --reconnect-settle 0 --reset \
	--log /tmp/papermono_boot.raw.log > /tmp/papermono_boot.log 2>&1; then
	if grep -q 'content store mounted at' /tmp/papermono_boot.log &&
	   grep -q 'LVGL page rendered' /tmp/papermono_boot.log; then
		ok "TF mounted and UI rendered"
	else
		bad "boot log missing mount/render markers"
	fi
	if grep -qE '<wrn>|<err>' /tmp/papermono_boot.log; then
		bad "boot log contains warnings/errors:"
		grep -E '<wrn>|<err>' /tmp/papermono_boot.log | sed 's/^/         /'
	else
		ok "boot log has no warnings or errors"
	fi
else
	bad "boot capture failed, see /tmp/papermono_boot.log"
fi

step "6/6 manual steps that still need a human"
cat <<'EOF'
  - 50-cycle deep-sleep/wake endurance:
      west build -p always -d build-wake -b m5stack_papermono/esp32s3/procpu . \
        -- -DEXTRA_CONF_FILE='driver_selftest.conf;power_timer_hil.conf' \
           -DCONFIG_PAPERMONO_POWER_HIL_REPEAT_COUNT=50
      then flash and capture for ~35 minutes; expect
      "Power HIL endurance PASS: 50/50".

  - Content-center HTTP upload (needs a phone/computer that can join
    PaperMono-XXXXXXXX): pair with the on-screen code, upload a .txt and a
    .epub, confirm both appear in the list.

  - On-device TF format walkthrough (needs touch): insert a card with no FAT
    filesystem, open 系统状态, tap 格式化 TF 卡, confirm, verify the page
    reports 已就绪 afterwards.
EOF

printf '\nResult: %s\n' "$([ "$fail" -eq 0 ] && echo 'automated steps passed' || echo 'failures above')"
exit "$fail"
