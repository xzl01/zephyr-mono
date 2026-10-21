/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_APP_H_
#define PAPERMONO_APP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <papermono/refresh.h>

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

#ifdef __cplusplus
extern "C" {
#endif

enum papermono_app_id {
	PAPERMONO_APP_HOME = 0,
	PAPERMONO_APP_LAUNCHER = 1,
	PAPERMONO_APP_LIBRARY = 2,
	PAPERMONO_APP_REMOTE = 3,
	PAPERMONO_APP_CALENDAR = 4,
	PAPERMONO_APP_TASKS = 5,
	PAPERMONO_APP_NOTES = 6,
	PAPERMONO_APP_INFO = 7,
	PAPERMONO_APP_TOOLS = 8,
	PAPERMONO_APP_POWER = 9,
	PAPERMONO_APP_SETTINGS = 10,
	PAPERMONO_APP_SYSTEM = 11,
	PAPERMONO_APP_CONTENT = 12,
	/* App IDs are persisted in Settings/NVS. Append new values only. */
	PAPERMONO_APP_WEREAD = 13,
	PAPERMONO_APP_COUNT,
};

enum papermono_app_event_type {
	PAPERMONO_APP_EVENT_PREVIOUS,
	PAPERMONO_APP_EVENT_NEXT,
	PAPERMONO_APP_EVENT_UP,
	PAPERMONO_APP_EVENT_DOWN,
	PAPERMONO_APP_EVENT_BACK,
	PAPERMONO_APP_EVENT_HOME,
	PAPERMONO_APP_EVENT_TAP,
	PAPERMONO_APP_EVENT_LONG_PRESS,
	PAPERMONO_APP_EVENT_REFRESH,
	PAPERMONO_APP_EVENT_CONTENT_CHANGED,
	PAPERMONO_APP_EVENT_SLEEP,
};

enum papermono_app_event_source {
	PAPERMONO_APP_EVENT_SOURCE_UNKNOWN = 0,
	PAPERMONO_APP_EVENT_SOURCE_BUTTON,
	PAPERMONO_APP_EVENT_SOURCE_TOUCH,
	PAPERMONO_APP_EVENT_SOURCE_SYSTEM,
};

struct papermono_app_event {
	enum papermono_app_event_type type;
	enum papermono_app_event_source source;
	int16_t x;
	int16_t y;
	uint32_t duration_ms;
	/* Uptime at capture, retained across a display refresh for latency logs. */
	uint32_t queued_at_ms;
};

enum papermono_app_command_type {
	PAPERMONO_APP_COMMAND_NONE,
	PAPERMONO_APP_COMMAND_REDRAW,
	PAPERMONO_APP_COMMAND_OPEN,
	PAPERMONO_APP_COMMAND_BACK,
	PAPERMONO_APP_COMMAND_SLEEP,
	PAPERMONO_APP_COMMAND_SHUTDOWN,
};

struct papermono_app_command {
	enum papermono_app_command_type type;
	enum papermono_app_id target;
	enum papermono_refresh_quality refresh_quality;
};

struct papermono_app_descriptor {
	enum papermono_app_id id;
	const char *title;
	bool visible;
	int (*on_open)(void *context);
	int (*render)(void *context, lv_obj_t *screen);
	int (*on_event)(void *context, const struct papermono_app_event *event,
			struct papermono_app_command *command);
	void (*on_close)(void *context);
	void *context;
};

struct papermono_app_router {
	const struct papermono_app_descriptor *apps;
	size_t app_count;
	enum papermono_app_id current;
	enum papermono_app_id previous;
	bool has_previous;
};

int papermono_app_router_init(struct papermono_app_router *router,
	const struct papermono_app_descriptor *apps, size_t app_count,
	enum papermono_app_id initial);
int papermono_app_router_open(struct papermono_app_router *router,
	enum papermono_app_id target);
int papermono_app_router_back(struct papermono_app_router *router);
int papermono_app_router_render(struct papermono_app_router *router,
	lv_obj_t *screen);
int papermono_app_router_dispatch(struct papermono_app_router *router,
	const struct papermono_app_event *event,
	struct papermono_app_command *command);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_APP_H_ */
