/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_UI_H_
#define PAPERMONO_UI_H_

#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Build the home page and perform the initial physical display refresh. */
int papermono_ui_init(const struct device *display);

/** Process button, touch, and periodic refresh events, recovering transient errors. */
int papermono_ui_run(void);
void papermono_ui_request_background_refresh(void);
void papermono_ui_request_content_refresh(void);

/** Update the two labels used by the finite partial-refresh HIL profile. */
int papermono_ui_demo_update(uint32_t update, uint32_t total);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_UI_H_ */
