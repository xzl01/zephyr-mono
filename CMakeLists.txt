# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(papermono_lvgl)

include(cmake/esp32_ble_compat.cmake)
include(cmake/xtensa_irq_exit_compat.cmake)

if(CONFIG_BT_ESP32 AND (CONFIG_SOC_SERIES_ESP32S3 OR CONFIG_SOC_SERIES_ESP32C3))
  target_sources(app PRIVATE src/papermono_esp_radio_compat.c)
endif()

if(CONFIG_WIFI_ESP32 AND CONFIG_SOC_SERIES_ESP32S3)
  target_sources(app PRIVATE src/papermono_esp_wifi_compat.c)
  target_link_options(app PUBLIC -Wl,--wrap=ap_try_sa_query_process)
endif()

target_sources(app PRIVATE
  src/main.c
  src/papermono_app.c
  src/papermono_interaction.c
  src/papermono_touch_gesture.c
  src/papermono_touchpad.c
  src/papermono_frontlight.c
  src/papermono_status_led.c
  src/papermono_content_store.c
  src/papermono_fs_read.c
  src/papermono_archive_safety.c
  src/papermono_content_validation.c
  src/papermono_display_scheduler.c
  src/papermono_refresh_policy.c
  src/papermono_epub.c
  src/papermono_reader.c
  src/papermono_reader_cache.c
  src/papermono_personal_data.c
  src/papermono_organizer.c
  src/papermono_weread.c
  src/papermono_weread_view.c
  src/papermono_info_provider.c
  src/papermono_text_codec.c
  src/papermono_text_layout.c
  src/papermono_zip_reader.c
  src/generated/papermono_gbk_table.c
  src/generated/papermono_cjk_16.c
  src/generated/papermono_clock_148.c
  src/generated/papermono_reading_24.c
  src/generated/papermono_heading_48.c
  src/generated/papermono_state_40.c
  src/generated/papermono_icon_library.c
  src/generated/papermono_icon_weread.c
  src/generated/papermono_icon_calendar.c
  src/generated/papermono_selftest_epub.c
  src/generated/papermono_icon_tasks.c
  src/generated/papermono_icon_notes.c
  src/generated/papermono_icon_remote.c
  src/generated/papermono_icon_content.c
  src/generated/papermono_icon_info.c
  src/generated/papermono_icon_tools.c
  src/generated/papermono_icon_settings.c
  src/generated/papermono_icon_power.c
  src/generated/papermono_icon_sleep.c
  src/generated/papermono_icon_bookmark.c
  src/generated/papermono_icon_document.c
  src/generated/papermono_icon_wifi.c
  src/generated/papermono_icon_bluetooth.c
  src/generated/papermono_icon_restore.c
  src/generated/papermono_icon_book_small.c
  src/generated/papermono_icon_headphones_small.c
  src/generated/papermono_icon_bookmark_small.c
  src/generated/papermono_icon_settings_small.c
  src/generated/papermono_icon_sun_small.c
  src/generated/papermono_icon_toc_small.c
  src/generated/papermono_icon_return_small.c
  src/generated/papermono_icon_library_small.c
  src/generated/papermono_icon_document_small.c
  src/generated/papermono_icon_info_small.c
  src/generated/papermono_icon_library_focal.c
  src/generated/papermono_icon_weread_focal.c
  src/generated/papermono_icon_calendar_focal.c
  src/generated/papermono_icon_tasks_focal.c
  src/generated/papermono_icon_document_focal.c
  src/generated/papermono_icon_bookmark_focal.c
  src/generated/papermono_icon_wifi_focal.c
  src/papermono_settings.c
  src/papermono_power_state.c
  src/m5ioe1.c
  src/m5ioe1_pwm.c
  src/m5pm1.c
  src/m5pm1_pwm.c
  src/papermono_rgb_led.c
  src/papermono_ssd1677.c
  src/papermono_ui.c
  src/papermono_almanac.c
  src/papermono_quotes.c
  src/papermono_wifi_input.c
)

target_sources_ifdef(CONFIG_PAPERMONO_FT6336 app PRIVATE
  src/papermono_ft6336.c
  src/papermono_touch_transform.c
)

target_sources_ifdef(CONFIG_PAPERMONO_CONTENT_SERVER app PRIVATE
  src/papermono_content_server.c
  src/papermono_http_codec.c
)

target_sources_ifdef(CONFIG_PAPERMONO_TIME_SYNC app PRIVATE
  src/papermono_time_sync.c
)

target_sources_ifdef(CONFIG_PAPERMONO_BLE_HID app PRIVATE
  src/papermono_hid.c
  src/papermono_hid_reports.c
)

target_sources_ifdef(CONFIG_PAPERMONO_RSS app PRIVATE
  src/papermono_rss.c
)

target_sources_ifdef(CONFIG_PAPERMONO_WEATHER app PRIVATE
  src/papermono_weather.c
)

target_sources_ifdef(CONFIG_PAPERMONO_BLE_SYNC app PRIVATE
  src/papermono_sync.c
)

target_sources_ifdef(CONFIG_PAPERMONO_BLE_MIC app PRIVATE
  src/papermono_adpcm.c
  src/papermono_ble_mic.c
)

if(CONFIG_PAPERMONO_CONTENT_SERVER)
  set(papermono_generated_dir ${ZEPHYR_BINARY_DIR}/include/generated)
  generate_inc_file_for_target(app
    src/web/content-center.html
    ${papermono_generated_dir}/papermono_content_center.html.inc
  )
  zephyr_linker_sources(SECTIONS sections-rom.ld)
endif()

target_sources_ifdef(CONFIG_PAPERMONO_DRIVER_SELFTEST app PRIVATE
  src/papermono_driver_selftest.c
)

target_sources_ifdef(CONFIG_PAPERMONO_IP2315 app PRIVATE
  src/papermono_ip2315.c
)

target_sources_ifdef(CONFIG_PAPERMONO_ST25R3916 app PRIVATE
  src/papermono_st25r3916.c
)

target_sources_ifdef(CONFIG_PAPERMONO_PDM_MIC app PRIVATE
  src/papermono_pdm_mic.c
)

target_sources_ifdef(CONFIG_PAPERMONO_RX8130_TIMER app PRIVATE
  src/papermono_rx8130.c
)

target_sources_ifdef(CONFIG_PAPERMONO_POWER_CONTROL app PRIVATE
  src/papermono_power.c
)

zephyr_include_directories(include)
target_include_directories(app PRIVATE include)
