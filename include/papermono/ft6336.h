/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_FT6336_H_
#define PAPERMONO_FT6336_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct papermono_ft6336_status {
	uint32_t frames;
	uint32_t press_events;
	uint32_t release_events;
	uint32_t read_errors;
	uint32_t invalid_frames;
	uint32_t recoveries;
	uint32_t last_sample_ms;
	uint32_t max_poll_gap_ms;
	int32_t x;
	int32_t y;
	bool online;
	bool pressed;
};

/** Return a lock-free diagnostic snapshot for the board touch controller. */
int papermono_ft6336_get_status(struct papermono_ft6336_status *status);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_FT6336_H_ */
