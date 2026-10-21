/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Regression: settings setters must serialise their read-modify-write.
 *
 * The UI thread changes preferences while the content server's Wi-Fi
 * workqueue persists credentials. Without a lock the second writer stores a
 * candidate copied before the first writer committed, silently reverting the
 * other field.
 *
 * The fake backend blocks inside the first save so the second setter runs
 * while it is in flight. With serialisation the second setter cannot even
 * copy its candidate until the first has committed, so both values survive.
 */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <zephyr/settings/settings.h>

#include <papermono/settings.h>

static pthread_mutex_t backend_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t saving_started = PTHREAD_COND_INITIALIZER;
static bool hook_armed;
static bool holding_save;

int settings_subsys_init(void)
{
	return 0;
}

int settings_load_subtree(const char *subtree)
{
	(void)subtree;
	return 0;
}

int settings_load_subtree_direct(const char *subtree,
	settings_load_direct_cb callback, void *param)
{
	(void)subtree;
	(void)callback;
	(void)param;
	return 0;
}

int settings_delete(const char *key)
{
	(void)key;
	return 0;
}

int settings_save_one(const char *key, const void *value, size_t length)
{
	(void)key;
	(void)value;
	(void)length;
	pthread_mutex_lock(&backend_lock);
	if (hook_armed && !holding_save) {
		holding_save = true;
		pthread_cond_signal(&saving_started);
		pthread_mutex_unlock(&backend_lock);
		/* Widen the window in which a concurrent writer could interleave. */
		struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000000 };
		nanosleep(&delay, NULL);
		pthread_mutex_lock(&backend_lock);
		holding_save = false;
	}
	pthread_mutex_unlock(&backend_lock);
	return 0;
}

static void *sleep_writer(void *arg)
{
	(void)arg;
	assert(papermono_settings_set_idle_sleep(5U) == 0);
	return NULL;
}

static void *frontlight_writer(void *arg)
{
	(void)arg;
	assert(papermono_settings_set_frontlight(75U) == 0);
	return NULL;
}

int main(void)
{
	pthread_t first, second;

	assert(papermono_settings_init() == 0);
	hook_armed = true;

	assert(pthread_create(&first, NULL, sleep_writer, NULL) == 0);
	pthread_mutex_lock(&backend_lock);
	while (!holding_save) {
		pthread_cond_wait(&saving_started, &backend_lock);
	}
	pthread_mutex_unlock(&backend_lock);

	/* Starts while the first setter is inside settings_save_one(). */
	assert(pthread_create(&second, NULL, frontlight_writer, NULL) == 0);
	assert(pthread_join(second, NULL) == 0);
	assert(pthread_join(first, NULL) == 0);

	const struct papermono_preferences *prefs = papermono_settings_get();

	if (prefs->idle_sleep_minutes != 5U || prefs->frontlight_percent != 75U) {
		fprintf(stderr,
			"lost update: sleep=%u frontlight=%u (expected 5/75)\n",
			prefs->idle_sleep_minutes, prefs->frontlight_percent);
		return 1;
	}
	puts("settings concurrent persistence: PASS");
	return 0;
}
