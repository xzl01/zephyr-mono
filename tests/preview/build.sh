#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Native 480x800 LVGL I1 preview. No firmware/backend source is modified.
set -euo pipefail
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "${PYTHON:-python3}" "$HERE/scripts/build.py" "$@"
