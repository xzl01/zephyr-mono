/* SPDX-License-Identifier: GPL-3.0-only */

/* Preview shim: numeric stand-ins for the devicetree nodes the UI names.
 * Each distinct id yields a distinct dummy struct device pointer.
 */
#ifndef PAPERMONO_PREVIEW_NODES_H_
#define PAPERMONO_PREVIEW_NODES_H_

#define PAPERMONO_DT_ID_rtc 0
#define PAPERMONO_DT_ID_m5pm1 1
#define PAPERMONO_DT_ID_zephyr_touch 2
#define PAPERMONO_DT_ID_buttons 3
#define PAPERMONO_DT_ID_imu 4
#define PAPERMONO_DT_ID_nfc 5
#define PAPERMONO_DT_ID_lora 6
#define PAPERMONO_DT_ID_microphone 7
#define PAPERMONO_DT_ID_display 8
#define PAPERMONO_DT_ID_sd_slot_control 9

#endif /* PAPERMONO_PREVIEW_NODES_H_ */
