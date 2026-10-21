/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>

#include <papermono/app.h>

static const struct papermono_app_descriptor *find_app(
	const struct papermono_app_router *router, enum papermono_app_id id)
{
	for (size_t index = 0U; index < router->app_count; ++index) {
		if (router->apps[index].id == id) {
			return &router->apps[index];
		}
	}
	return NULL;
}

int papermono_app_router_init(struct papermono_app_router *router,
	const struct papermono_app_descriptor *apps, size_t app_count,
	enum papermono_app_id initial)
{
	const struct papermono_app_descriptor *app;
	int ret;

	if (router == NULL || apps == NULL || app_count == 0U) {
		return -EINVAL;
	}

	*router = (struct papermono_app_router) {
		.apps = apps,
		.app_count = app_count,
		.current = initial,
	};
	app = find_app(router, initial);
	if (app == NULL) {
		return -ENOENT;
	}
	if (app->on_open != NULL) {
		ret = app->on_open(app->context);
		if (ret < 0) {
			return ret;
		}
	}
	return 0;
}

static const struct papermono_app_descriptor *papermono_app_router_current(
	const struct papermono_app_router *router)
{
	if (router == NULL) {
		return NULL;
	}
	return find_app(router, router->current);
}

int papermono_app_router_open(struct papermono_app_router *router,
	enum papermono_app_id target)
{
	const struct papermono_app_descriptor *current;
	const struct papermono_app_descriptor *next;
	const enum papermono_app_id old_previous = router == NULL ?
		PAPERMONO_APP_HOME : router->previous;
	const bool old_has_previous = router != NULL && router->has_previous;
	const enum papermono_app_id old_current = router == NULL ?
		PAPERMONO_APP_HOME : router->current;
	int ret = 0;

	if (router == NULL) {
		return -EINVAL;
	}
	next = find_app(router, target);
	if (next == NULL) {
		return -ENOENT;
	}
	if (target == router->current) {
		return 0;
	}

	current = papermono_app_router_current(router);
	if (current != NULL && current->on_close != NULL) {
		current->on_close(current->context);
	}
	router->previous = router->current;
	router->has_previous = true;
	router->current = target;
	if (next->on_open != NULL) {
		ret = next->on_open(next->context);
	}
	if (ret < 0) {
		router->current = old_current;
		router->previous = old_previous;
		router->has_previous = old_has_previous;
		if (current != NULL && current->on_open != NULL) {
			(void)current->on_open(current->context);
		}
	}
	return ret;
}

int papermono_app_router_back(struct papermono_app_router *router)
{
	const struct papermono_app_descriptor *current;
	const struct papermono_app_descriptor *next;
	const enum papermono_app_id old_current = router == NULL ?
		PAPERMONO_APP_HOME : router->current;
	const enum papermono_app_id old_previous = router == NULL ?
		PAPERMONO_APP_HOME : router->previous;
	const bool old_has_previous = router != NULL && router->has_previous;
	enum papermono_app_id target;
	int ret = 0;

	if (router == NULL) {
		return -EINVAL;
	}
	target = router->has_previous ? router->previous : PAPERMONO_APP_HOME;
	next = find_app(router, target);
	if (next == NULL) {
		return -ENOENT;
	}
	if (target == router->current) {
		router->has_previous = false;
		return 0;
	}

	current = papermono_app_router_current(router);
	if (current != NULL && current->on_close != NULL) {
		current->on_close(current->context);
	}
	router->current = target;
	router->has_previous = false;
	if (next->on_open != NULL) {
		ret = next->on_open(next->context);
	}
	if (ret < 0) {
		router->current = old_current;
		router->previous = old_previous;
		router->has_previous = old_has_previous;
		if (current != NULL && current->on_open != NULL) {
			(void)current->on_open(current->context);
		}
	}
	return ret;
}

int papermono_app_router_render(struct papermono_app_router *router,
	lv_obj_t *screen)
{
	const struct papermono_app_descriptor *app =
		papermono_app_router_current(router);

	if (app == NULL || app->render == NULL || screen == NULL) {
		return -EINVAL;
	}
	return app->render(app->context, screen);
}

int papermono_app_router_dispatch(struct papermono_app_router *router,
	const struct papermono_app_event *event,
	struct papermono_app_command *command)
{
	const struct papermono_app_descriptor *app;

	if (router == NULL || event == NULL || command == NULL) {
		return -EINVAL;
	}
	*command = (struct papermono_app_command) {
		.type = PAPERMONO_APP_COMMAND_NONE,
		.refresh_quality = PAPERMONO_REFRESH_AUTO,
	};
	app = papermono_app_router_current(router);
	if (app == NULL || app->on_event == NULL) {
		return 0;
	}
	return app->on_event(app->context, event, command);
}
