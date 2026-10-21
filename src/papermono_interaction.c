/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>

#include <papermono/interaction.h>

static uint8_t event_trigger(const struct papermono_app_event *event)
{
	if (event == NULL) {
		return 0U;
	}
	if (event->type == PAPERMONO_APP_EVENT_TAP) {
		return PAPERMONO_INTERACTION_TAP;
	}
	if (event->type == PAPERMONO_APP_EVENT_LONG_PRESS) {
		return PAPERMONO_INTERACTION_LONG_PRESS;
	}
	return 0U;
}

bool papermono_app_event_is_button(const struct papermono_app_event *event)
{
	return event != NULL &&
		(event->source == PAPERMONO_APP_EVENT_SOURCE_BUTTON ||
		 (event->source == PAPERMONO_APP_EVENT_SOURCE_UNKNOWN &&
		  event->x < 0));
}

bool papermono_app_event_is_touch(const struct papermono_app_event *event)
{
	return event != NULL && event->x >= 0 && event->y >= 0 &&
		(event->source == PAPERMONO_APP_EVENT_SOURCE_TOUCH ||
		 event->source == PAPERMONO_APP_EVENT_SOURCE_UNKNOWN);
}

bool papermono_app_event_is_back_navigation(
	const struct papermono_app_event *event)
{
	return event != NULL &&
		(event->type == PAPERMONO_APP_EVENT_BACK ||
		 event->type == PAPERMONO_APP_EVENT_PREVIOUS);
}

static unsigned int navigation_priority(enum papermono_app_event_type type)
{
	return type == PAPERMONO_APP_EVENT_SLEEP ? 3U :
		type == PAPERMONO_APP_EVENT_HOME ? 2U :
		type == PAPERMONO_APP_EVENT_BACK ? 1U : 0U;
}

bool papermono_interaction_defer_event(struct papermono_app_event *pending,
	bool *valid, const struct papermono_app_event *incoming)
{
	if (pending == NULL || valid == NULL || incoming == NULL) {
		return false;
	}
	if (*valid && navigation_priority(incoming->type) <=
	    navigation_priority(pending->type)) {
		return false;
	}
	*pending = *incoming;
	*valid = true;
	return true;
}

bool papermono_interaction_contains(const struct papermono_app_event *event,
	uint8_t triggers, const struct papermono_rect *rect)
{
	if (!papermono_app_event_is_touch(event) || rect == NULL ||
	    rect->width <= 0 || rect->height <= 0 ||
	    (event_trigger(event) & triggers) == 0U) {
		return false;
	}
	return event->x >= rect->x && event->y >= rect->y &&
		event->x < rect->x + rect->width &&
		event->y < rect->y + rect->height;
}

int papermono_interaction_route(const struct papermono_app_event *event,
	const struct papermono_interaction_region *regions, size_t region_count,
	uint16_t *action, int16_t *value)
{
	if (event == NULL || regions == NULL || region_count == 0U ||
	    action == NULL) {
		return -EINVAL;
	}
	for (size_t index = 0U; index < region_count; ++index) {
		if (!papermono_interaction_contains(event, regions[index].triggers,
			&regions[index].rect)) {
			continue;
		}
		*action = regions[index].action;
		if (value != NULL) {
			*value = regions[index].value;
		}
		return 0;
	}
	return -ENOENT;
}

int papermono_interaction_grid_index(const struct papermono_app_event *event,
	uint8_t triggers, const struct papermono_rect *rect, uint8_t columns,
	uint8_t rows, size_t *index)
{
	if (rect == NULL || columns == 0U || rows == 0U || index == NULL ||
	    rect->width < columns || rect->height < rows) {
		return -EINVAL;
	}
	if (!papermono_interaction_contains(event, triggers, rect)) {
		return -ENOENT;
	}
	const int32_t relative_x = event->x - rect->x;
	const int32_t relative_y = event->y - rect->y;
	uint8_t column = (uint8_t)((relative_x * columns) / rect->width);
	uint8_t row = (uint8_t)((relative_y * rows) / rect->height);

	if (column >= columns) {
		column = columns - 1U;
	}
	if (row >= rows) {
		row = rows - 1U;
	}
	*index = (size_t)row * columns + column;
	return 0;
}

static int interaction_grid_bounds(
	const struct papermono_interaction_grid_layout *layout,
	struct papermono_rect *bounds)
{
	int32_t width;
	int32_t height;

	if (layout == NULL || bounds == NULL || layout->cell_width <= 0 ||
	    layout->cell_height <= 0 || layout->column_gap < 0 ||
	    layout->row_gap < 0 || layout->columns == 0U || layout->rows == 0U) {
		return -EINVAL;
	}
	width = (int32_t)layout->cell_width * layout->columns +
		(int32_t)layout->column_gap * (layout->columns - 1U);
	height = (int32_t)layout->cell_height * layout->rows +
		(int32_t)layout->row_gap * (layout->rows - 1U);
	if (width <= 0 || width > INT16_MAX || height <= 0 ||
	    height > INT16_MAX || (int32_t)layout->x + width > INT16_MAX ||
	    (int32_t)layout->y + height > INT16_MAX) {
		return -ERANGE;
	}
	*bounds = (struct papermono_rect) {
		.x = layout->x,
		.y = layout->y,
		.width = (int16_t)width,
		.height = (int16_t)height,
	};
	return 0;
}

int papermono_interaction_grid_cell_rect(
	const struct papermono_interaction_grid_layout *layout, size_t index,
	struct papermono_rect *rect)
{
	struct papermono_rect bounds;
	size_t column;
	size_t row;
	int ret;

	if (rect == NULL) {
		return -EINVAL;
	}
	ret = interaction_grid_bounds(layout, &bounds);
	if (ret < 0) {
		return ret;
	}
	if (index >= (size_t)layout->columns * layout->rows) {
		return -ERANGE;
	}
	column = index % layout->columns;
	row = index / layout->columns;
	*rect = (struct papermono_rect) {
		.x = (int16_t)(layout->x + column *
			(layout->cell_width + layout->column_gap)),
		.y = (int16_t)(layout->y + row *
			(layout->cell_height + layout->row_gap)),
		.width = layout->cell_width,
		.height = layout->cell_height,
	};
	return 0;
}

int papermono_interaction_spaced_grid_index(
	const struct papermono_app_event *event, uint8_t triggers,
	const struct papermono_interaction_grid_layout *layout, size_t *index)
{
	struct papermono_rect bounds;
	struct papermono_rect cell;
	int32_t relative_x;
	int32_t relative_y;
	int32_t column_stride;
	int32_t row_stride;
	size_t column;
	size_t row;
	int ret;

	if (index == NULL) {
		return -EINVAL;
	}
	ret = interaction_grid_bounds(layout, &bounds);
	if (ret < 0) {
		return ret;
	}
	if (!papermono_interaction_contains(event, triggers, &bounds)) {
		return -ENOENT;
	}
	relative_x = event->x - layout->x;
	relative_y = event->y - layout->y;
	column_stride = layout->cell_width + layout->column_gap;
	row_stride = layout->cell_height + layout->row_gap;
	column = (size_t)(relative_x / column_stride);
	row = (size_t)(relative_y / row_stride);
	if (column >= layout->columns || row >= layout->rows ||
	    relative_x % column_stride >= layout->cell_width ||
	    relative_y % row_stride >= layout->cell_height) {
		return -ENOENT;
	}
	*index = row * layout->columns + column;
	ret = papermono_interaction_grid_cell_rect(layout, *index, &cell);
	if (ret < 0 || !papermono_interaction_contains(event, triggers, &cell)) {
		return -ENOENT;
	}
	return 0;
}
