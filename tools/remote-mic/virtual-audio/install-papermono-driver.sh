#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUNDLE="$HERE/build/PaperMono.driver"
TARGET="/Library/Audio/Plug-Ins/HAL/PaperMono.driver"

if [ ! -d "$BUNDLE" ]; then
	echo "driver not built; running build script..." >&2
	"$HERE/build-papermono-driver.sh"
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/papermono-driver.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
cp -R "$BUNDLE" "$TMP/PaperMono.driver"

echo "installing $TARGET (administrator password required)"
sudo rm -rf "$TARGET"
sudo cp -R "$TMP/PaperMono.driver" "$TARGET"
sudo chown -R root:wheel "$TARGET"
sudo killall -9 coreaudiod 2>/dev/null || true
sleep 2

echo "installed. Audio devices:"
system_profiler SPAudioDataType 2>/dev/null | grep -A3 -i "PaperMono" || true
echo
echo "Use: tools/remote-mic/papermono_mic.py --device \"PaperMono\""
echo "Target applications select the microphone \"PaperMono\"."
