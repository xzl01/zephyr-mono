/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_INTERACTION_H_
#define PAPERMONO_INTERACTION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <papermono/app.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAPERMONO_INTERACTION_TAP (1U << 0)
#define PAPERMONO_INTERACTION_LONG_PRESS (1U << 1)

struct papermono_rect {
	int16_t x;
	int16_t y;
	int16_t width;
	int16_t height;
};

struct papermono_interaction_region {
	struct papermono_rect rect;
	uint16_t action;
	int16_t value;
	uint8_t triggers;
};

struct papermono_interaction_grid_layout {
	int16_t x;
	int16_t y;
	int16_t cell_width;
	int16_t cell_height;
	int16_t column_gap;
	int16_t row_gap;
	uint8_t columns;
	uint8_t rows;
};

bool papermono_app_event_is_button(const struct papermono_app_event *event);
bool papermono_app_event_is_touch(const struct papermono_app_event *event);
bool papermono_app_event_is_back_navigation(
	const struct papermono_app_event *event);
bool papermono_interaction_contains(const struct papermono_app_event *event,
	uint8_t triggers, const struct papermono_rect *rect);
int papermono_interaction_route(const struct papermono_app_event *event,
	const struct papermono_interaction_region *regions, size_t region_count,
	uint16_t *action, int16_t *value);
int papermono_interaction_grid_index(const struct papermono_app_event *event,
	uint8_t triggers, const struct papermono_rect *rect, uint8_t columns,
	uint8_t rows, size_t *index);
int papermono_interaction_grid_cell_rect(
	const struct papermono_interaction_grid_layout *layout, size_t index,
	struct papermono_rect *rect);
int papermono_interaction_spaced_grid_index(
	const struct papermono_app_event *event, uint8_t triggers,
	const struct papermono_interaction_grid_layout *layout, size_t *index);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_INTERACTION_H_ */
