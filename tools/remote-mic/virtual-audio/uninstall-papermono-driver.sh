#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu
TARGET="/Library/Audio/Plug-Ins/HAL/PaperMono.driver"
echo "removing $TARGET (administrator password required)"
sudo rm -rf "$TARGET"
sudo killall -9 coreaudiod 2>/dev/null || true
sleep 2
echo "PaperMono virtual audio device removed."
