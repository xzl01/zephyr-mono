#!/bin/sh
set -eu

build_dir="$(mktemp -d "${TMPDIR:-/tmp}/papermono-host-tests.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT INT TERM
cc -std=c17 -Wall -Wextra -Werror \
	-Iinclude \
	tests/host/test_text_codec.c \
	src/papermono_text_codec.c \
	src/generated/papermono_gbk_table.c \
	-o "$build_dir/test_text_codec"
"$build_dir/test_text_codec"
cc -std=c17 -Wall -Wextra -Werror \
	-Iinclude \
	tests/host/test_text_layout.c \
	src/papermono_text_layout.c \
	-o "$build_dir/test_text_layout"
"$build_dir/test_text_layout"
cc -std=c17 -Wall -Wextra -Werror \
	-Iinclude \
	tests/host/test_app.c \
	src/papermono_app.c \
	-o "$build_dir/test_app"
"$build_dir/test_app"
cc -std=c17 -Wall -Wextra -Werror \
	-Iinclude \
	tests/host/test_interaction.c \
	src/papermono_interaction.c \
	-o "$build_dir/test_interaction"
"$build_dir/test_interaction"
cc -std=c17 -Wall -Wextra -Werror \
	-Iinclude \
	tests/host/test_touch_gesture.c \
	src/papermono_touch_gesture.c \
	-o "$build_dir/test_touch_gesture"
"$build_dir/test_touch_gesture"
cc -std=c17 -Wall -Wextra -Werror -Iinclude \
	tests/host/test_touchpad.c src/papermono_touchpad.c \
	-o "$build_dir/test_touchpad"
"$build_dir/test_touchpad"
cc -std=c17 -Wall -Wextra -Werror \
	-Iinclude \
	tests/host/test_frontlight.c \
	src/papermono_frontlight.c \
	-o "$build_dir/test_frontlight"
"$build_dir/test_frontlight"
cc -std=c17 -Wall -Wextra -Werror \
	-Iinclude \
	tests/host/test_status_led.c \
	src/papermono_status_led.c \
	-o "$build_dir/test_status_led"
"$build_dir/test_status_led"
cc -std=c17 -Wall -Wextra -Werror \
	-Iinclude \
	tests/host/test_touch_transform.c \
	src/papermono_touch_transform.c \
	-o "$build_dir/test_touch_transform"
"$build_dir/test_touch_transform"
cc -std=c17 -Wall -Wextra -Werror \
	-Iinclude \
	tests/host/test_power_state.c \
	src/papermono_power_state.c \
	-o "$build_dir/test_power_state"
"$build_dir/test_power_state"
cc -std=c17 -Wall -Wextra -Werror \
	-Iinclude tests/host/test_m5pm1_button.c \
	-o "$build_dir/test_m5pm1_button"
"$build_dir/test_m5pm1_button"
cc -std=c17 -Wall -Wextra -Werror \
	-Iinclude \
	tests/host/test_hid_reports.c \
	src/papermono_hid_reports.c \
	-o "$build_dir/test_hid_reports"
"$build_dir/test_hid_reports"
cc -std=c17 -Wall -Wextra -Werror \
	-Itests/host/stubs -Iinclude \
	tests/host/test_settings.c \
	src/papermono_settings.c \
	-o "$build_dir/test_settings"
"$build_dir/test_settings"
cc -std=c17 -Wall -Wextra -Werror \
	-Iinclude \
	tests/host/test_http_codec.c \
	src/papermono_http_codec.c \
	-o "$build_dir/test_http_codec"
"$build_dir/test_http_codec"
cc -std=c17 -Wall -Wextra -Werror \
	-Itests/host/stubs -Iinclude \
	tests/host/test_reader.c \
	src/papermono_reader.c \
	src/papermono_fs_read.c \
	tests/host/stubs/fs_read_weak.c \
	src/papermono_text_codec.c \
	src/papermono_text_layout.c \
	src/generated/papermono_gbk_table.c \
	-o "$build_dir/test_reader"
"$build_dir/test_reader"
cc -std=c17 -Wall -Wextra -Werror \
	-Itests/host/stubs -Iinclude \
	tests/host/test_reader_cache.c \
	src/papermono_reader_cache.c \
	src/papermono_fs_read.c \
	tests/host/stubs/fs_read_weak.c \
	-o "$build_dir/test_reader_cache"
"$build_dir/test_reader_cache"
cc -std=c17 -Wall -Wextra -Werror \
	-Itests/host/stubs -Iinclude \
	tests/host/test_content_store.c \
	src/papermono_content_store.c \
	-o "$build_dir/test_content_store"
"$build_dir/test_content_store"
cc -std=c17 -Wall -Wextra -Werror \
	-Iinclude \
	tests/host/test_personal_data.c \
	src/papermono_personal_data.c \
	-o "$build_dir/test_personal_data"
"$build_dir/test_personal_data"
cc -std=c17 -Wall -Wextra -Werror \
	-Iinclude \
	tests/host/test_weread.c \
	src/papermono_weread_view.c \
	-o "$build_dir/test_weread"
"$build_dir/test_weread"
cc -std=c17 -Wall -Wextra -Werror \
	-Itests/host/stubs -Iinclude \
	tests/host/test_weread_loader.c \
	src/papermono_weread.c \
	src/papermono_fs_read.c \
	tests/host/stubs/fs_read_weak.c \
	src/papermono_personal_data.c \
	src/papermono_text_codec.c \
	src/generated/papermono_gbk_table.c \
	-o "$build_dir/test_weread_loader"
"$build_dir/test_weread_loader"
cc -std=c17 -Wall -Wextra -Werror \
	-Itests/host/stubs -Iinclude \
	tests/host/test_epub.c \
	src/papermono_epub.c \
	src/papermono_fs_read.c \
	tests/host/stubs/fs_read_weak.c \
	src/papermono_text_codec.c \
	src/papermono_text_layout.c \
	src/generated/papermono_gbk_table.c \
	-o "$build_dir/test_epub"
"$build_dir/test_epub"
cc -std=c17 -Wall -Wextra -Werror \
	-Itests/host/stubs -Iinclude \
	tests/host/test_archive_safety.c \
	src/papermono_archive_safety.c \
	src/papermono_text_codec.c \
	src/generated/papermono_gbk_table.c \
	-o "$build_dir/test_archive_safety"
"$build_dir/test_archive_safety"
cc -std=c17 -Wall -Wextra -Werror \
	-Itests/host/stubs -Iinclude \
	tests/host/test_ui_layout.c src/papermono_interaction.c \
	-o "$build_dir/test_ui_layout"
"$build_dir/test_ui_layout"
cc -std=c17 -Wall -Wextra -Werror \
	-Itests/host/stubs -Iinclude \
	tests/host/test_fs_read.c \
	src/papermono_fs_read.c \
	-o "$build_dir/test_fs_read"
"$build_dir/test_fs_read"
cc -std=c17 -Wall -Wextra -Werror \
	-Iinclude tests/host/test_wifi_input.c src/papermono_wifi_input.c \
	-o "$build_dir/test_wifi_input"
"$build_dir/test_wifi_input"
cc -std=c17 -Wall -Wextra -Werror \
	-Itests/host/stubs -Iinclude tests/host/test_info_provider.c \
	-o "$build_dir/test_info_provider"
"$build_dir/test_info_provider"
cc -std=c17 -Wall -Wextra -Werror \
	-Itests/host/stubs -Iinclude tests/host/test_organizer.c \
	src/papermono_personal_data.c -o "$build_dir/test_organizer"
"$build_dir/test_organizer"
python3 tests/host/test_font_coverage.py
python3 tests/host/test_field_notes_assets.py
python3 tests/host/test_content_web.py
python3 tests/host/test_usb_reset.py
python3 tests/host/test_esp_digest.py
python3 tests/host/test_esp_ble_compat.py
python3 tests/host/test_xtensa_irq_exit.py
python3 tests/host/test_hid_lifecycle.py
cc -std=c17 -Wall -Wextra -Werror -Itests/host/stubs \
	tests/host/test_esp_wifi_compat.c src/papermono_esp_wifi_compat.c \
	-o "$build_dir/test_esp_wifi_compat"
"$build_dir/test_esp_wifi_compat"
python3 tests/host/test_wifi_backend.py
