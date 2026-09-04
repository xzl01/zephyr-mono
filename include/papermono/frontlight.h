/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_FRONTLIGHT_H_
#define PAPERMONO_FRONTLIGHT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Convert a user-facing percentage to the display driver's 0..255 level. */
int papermono_frontlight_output_level(uint8_t percent, uint8_t *level);

/** Select the next fixed 0/25/50/75/100 percent setting. */
uint8_t papermono_frontlight_next(uint8_t percent);

/** Select the previous fixed 0/25/50/75/100 percent setting. */
uint8_t papermono_frontlight_previous(uint8_t percent);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_FRONTLIGHT_H_ */
