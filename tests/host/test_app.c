/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <papermono/app.h>

struct lifecycle_state {
	int opens;
	int closes;
	int renders;
	int events;
	int open_result;
};

static int on_open(void *context)
{
	struct lifecycle_state *state = context;

	++state->opens;
	return state->open_result;
}

static void on_close(void *context)
{
	struct lifecycle_state *state = context;

	++state->closes;
}

static int render(void *context, lv_obj_t *screen)
{
	struct lifecycle_state *state = context;

	assert(screen != NULL);
	++state->renders;
	return 0;
}

static int on_event(void *context, const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	struct lifecycle_state *state = context;

	++state->events;
	if (event->type == PAPERMONO_APP_EVENT_TAP) {
		command->type = PAPERMONO_APP_COMMAND_REDRAW;
	} else if (event->type == PAPERMONO_APP_EVENT_NEXT) {
		command->type = PAPERMONO_APP_COMMAND_REDRAW;
		command->refresh_quality = PAPERMONO_REFRESH_FAST;
	}
	return 0;
}

int main(void)
{
	struct lifecycle_state home = { 0 };
	struct lifecycle_state library = { 0 };
	struct lifecycle_state failing = { .open_result = -EIO };
	const struct papermono_app_descriptor apps[] = {
		{ PAPERMONO_APP_HOME, "home", true, on_open, render, on_event,
			on_close, &home },
		{ PAPERMONO_APP_LIBRARY, "library", true, on_open, render,
			on_event, on_close, &library },
		{ PAPERMONO_APP_SYSTEM, "failing", true, on_open, render,
			on_event, on_close, &failing },
	};
	struct papermono_app_router router;
	struct papermono_app_command command;
	const struct papermono_app_event tap = {
		.type = PAPERMONO_APP_EVENT_TAP,
	};
	const struct papermono_app_event next = {
		.type = PAPERMONO_APP_EVENT_NEXT,
	};

	/* Restoring a persisted non-home page must run its acquisition lifecycle
	 * before the first render (for example, the library TF scan).
	 */
	assert(papermono_app_router_init(&router, apps,
		sizeof(apps) / sizeof(apps[0]), PAPERMONO_APP_LIBRARY) == 0);
	assert(library.opens == 1);
	assert(router.current == PAPERMONO_APP_LIBRARY);
	assert(papermono_app_router_render(&router, (lv_obj_t *)(uintptr_t)1) == 0);
	assert(library.renders == 1);
	assert(papermono_app_router_dispatch(&router, &tap, &command) == 0);
	assert(library.events == 1);
	assert(command.type == PAPERMONO_APP_COMMAND_REDRAW);
	assert(command.refresh_quality == PAPERMONO_REFRESH_AUTO);
	assert(papermono_app_router_dispatch(&router, &next, &command) == 0);
	assert(command.type == PAPERMONO_APP_COMMAND_REDRAW);
	assert(command.refresh_quality == PAPERMONO_REFRESH_FAST);

	assert(papermono_app_router_open(&router, PAPERMONO_APP_HOME) == 0);
	assert(library.closes == 1);
	assert(home.opens == 1);
	assert(router.current == PAPERMONO_APP_HOME);
	assert(papermono_app_router_back(&router) == 0);
	assert(home.closes == 1);
	assert(library.opens == 2);
	assert(router.current == PAPERMONO_APP_LIBRARY);

	/* Failed page acquisition restores and reopens the previous application. */
	assert(papermono_app_router_open(&router, PAPERMONO_APP_SYSTEM) == -EIO);
	assert(failing.opens == 1);
	assert(router.current == PAPERMONO_APP_LIBRARY);
	assert(library.closes == 2);
	assert(library.opens == 3);

	assert(papermono_app_router_open(&router, PAPERMONO_APP_COUNT) == -ENOENT);
	puts("application router lifecycle: PASS");
	return 0;
}
