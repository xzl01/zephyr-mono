# SPDX-License-Identifier: MIT

if(SB_CONFIG_BOOTLOADER_MCUBOOT)
  list(APPEND mcuboot_DTC_OVERLAY_FILE
       "${CMAKE_CURRENT_LIST_DIR}/sysbuild/mcuboot.overlay")
  list(REMOVE_DUPLICATES mcuboot_DTC_OVERLAY_FILE)
  set(mcuboot_DTC_OVERLAY_FILE "${mcuboot_DTC_OVERLAY_FILE}"
      CACHE INTERNAL "PaperMono MCUboot devicetree overlays")
endif()
