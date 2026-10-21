# SPDX-License-Identifier: GPL-3.0-only
# hal_espressif 19f979c uses ESP32_BT_CTLR_* in esp_bt.h, but its C3/S3
# controller adapter still uses the older security name to install encryption
# ROM fixes. Scope the alias to that source and retain the configured value.
if(CONFIG_BT_ESP32 AND (CONFIG_SOC_SERIES_ESP32S3 OR CONFIG_SOC_SERIES_ESP32C3))
  set(papermono_esp_bt_adapter
    "${ZEPHYR_HAL_ESPRESSIF_MODULE_DIR}/components/bt/controller/esp32c3/bt.c")
  set_property(SOURCE "${papermono_esp_bt_adapter}" TARGET_DIRECTORY zephyr
    APPEND PROPERTY COMPILE_DEFINITIONS
    "CONFIG_BT_CTRL_BLE_SECURITY_ENABLE=$<BOOL:${CONFIG_ESP32_BT_CTLR_LE_SECURITY_ENABLE}>"
  )
endif()
