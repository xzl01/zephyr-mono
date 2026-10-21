/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <papermono/content_store.h>
#include <papermono/fs_read.h>
#include <papermono/organizer.h>

LOG_MODULE_REGISTER(papermono_organizer, LOG_LEVEL_INF);

#define ORGANIZER_OVERLAY_VERSION 1U
#define TASK_OVERLAY_MAX PAPERMONO_TASK_MAX
#define NOTE_OVERLAY_MAX PAPERMONO_NOTE_MAX

#if defined(CONFIG_ESP_SPIRAM)
#define PAPERMONO_ORGANIZER_BSS __attribute__((section(".ext_ram.bss")))
#else
#define PAPERMONO_ORGANIZER_BSS
#endif

struct task_overlay {
	uint64_t key;
	uint8_t done;
	uint8_t valid;
};

struct note_overlay {
	uint64_t key;
	uint8_t favorite;
	uint8_t read;
};

struct organizer_overlay {
	uint16_t version;
	uint16_t task_count;
	uint16_t note_count;
	uint16_t reserved;
	struct task_overlay tasks[TASK_OVERLAY_MAX];
	struct note_overlay notes[NOTE_OVERLAY_MAX];
};

struct organizer_data {
	struct papermono_calendar calendar;
	struct papermono_task_list task_list;
	struct papermono_note notes[PAPERMONO_NOTE_MAX];
	size_t note_count;
	int last_error;
};

static struct organizer_data organizer PAPERMONO_ORGANIZER_BSS;
static struct organizer_data organizer_staging PAPERMONO_ORGANIZER_BSS;
static char parse_buffer[PAPERMONO_METADATA_FILE_LIMIT + 1U]
	PAPERMONO_ORGANIZER_BSS;
static struct organizer_overlay overlay;
static bool initialized;
K_MUTEX_DEFINE(organizer_lock);

static struct papermono_state_overlay overlay_import
	PAPERMONO_ORGANIZER_BSS;

static void copy_cstring(char *destination, size_t capacity,
	const char *source)
{
	if (capacity == 0U) {
		return;
	}
	const size_t length = strnlen(source, capacity - 1U);
	memcpy(destination, source, length);
	destination[length] = '\0';
}

static uint64_t stable_key(const char *text)
{
	uint64_t hash = UINT64_C(14695981039346656037);

	while (*text != '\0') {
		hash = (hash ^ (uint8_t)*text++) * UINT64_C(1099511628211);
	}
	return hash;
}

static int load_overlay(const char *key, size_t length,
	settings_read_cb read_cb, void *cb_arg, void *parameter)
{
	ARG_UNUSED(parameter);
	if (key[0] != '\0' || length != sizeof(overlay)) {
		return 0;
	}
	if (read_cb(cb_arg, &overlay, sizeof(overlay)) != sizeof(overlay) ||
	    overlay.version != ORGANIZER_OVERLAY_VERSION ||
	    overlay.task_count > TASK_OVERLAY_MAX ||
	    overlay.note_count > NOTE_OVERLAY_MAX) {
		memset(&overlay, 0, sizeof(overlay));
	}
	return 0;
}

static int save_overlay(void)
{
	overlay.version = ORGANIZER_OVERLAY_VERSION;
	return settings_save_one("papermono_state/overlay", &overlay,
		sizeof(overlay));
}

static int read_entry(const struct papermono_content_entry *entry,
	size_t *length)
{
	struct fs_file_t file;
	ssize_t count;
	int ret;

	if (entry->size > PAPERMONO_METADATA_FILE_LIMIT) {
		return -EFBIG;
	}
	ret = papermono_content_store_open(entry, &file);
	if (ret < 0) {
		return ret;
	}
	count = papermono_fs_read_chunked(&file, parse_buffer, (size_t)entry->size);
	(void)fs_close(&file);
	if (count < 0) {
		return (int)count;
	}
	if ((uint64_t)count != entry->size) {
		return -EIO;
	}
	parse_buffer[count] = '\0';
	*length = (size_t)count;
	return 0;
}

static int load_content(const struct papermono_content_entry *entry,
	void *context)
{
	struct organizer_data *data = context;
	size_t length;
	int ret;

	if (entry->kind != PAPERMONO_CONTENT_ICS &&
	    entry->kind != PAPERMONO_CONTENT_TASKS &&
	    entry->kind != PAPERMONO_CONTENT_NOTE) {
		return 0;
	}
	ret = read_entry(entry, &length);
	if (ret < 0) {
		data->last_error = ret;
		return 0;
	}
	if (entry->kind == PAPERMONO_CONTENT_ICS) {
		ret = papermono_parse_ics(parse_buffer, length, &data->calendar,
			data->calendar.count > 0U);
	} else if (entry->kind == PAPERMONO_CONTENT_TASKS) {
		ret = papermono_parse_tasks_json(parse_buffer, length,
			&data->task_list);
	} else if (data->note_count < PAPERMONO_NOTE_MAX) {
		struct papermono_note *note = &data->notes[data->note_count];

		ret = papermono_parse_markdown(parse_buffer, length, note);
		(void)snprintf(note->id, sizeof(note->id), "%016llx",
			(unsigned long long)stable_key(entry->path));
		if (ret == 0 && (note->title[0] == '\0' ||
		    strcmp(note->title, "空便签") == 0)) {
			copy_cstring(note->title, sizeof(note->title), entry->title);
		}
		if (ret == 0) {
			++data->note_count;
		}
	} else {
		ret = -ENOSPC;
	}
	if (ret < 0 && data->last_error == 0) {
		data->last_error = ret;
	}
	return 0;
}

static int compare_time(const struct papermono_calendar_time *left,
	const struct papermono_calendar_time *right)
{
	if (left->year != right->year) {
		return left->year < right->year ? -1 : 1;
	}
	if (left->month != right->month) {
		return left->month < right->month ? -1 : 1;
	}
	if (left->day != right->day) {
		return left->day < right->day ? -1 : 1;
	}
	if (left->hour != right->hour) {
		return left->hour < right->hour ? -1 : 1;
	}
	if (left->minute != right->minute) {
		return left->minute < right->minute ? -1 : 1;
	}
	return left->second == right->second ? 0 :
		left->second < right->second ? -1 : 1;
}

static void sort_events(struct papermono_calendar *calendar)
{
	for (size_t index = 1U; index < calendar->count; ++index) {
		struct papermono_calendar_event event = calendar->events[index];
		size_t target = index;

		while (target > 0U && compare_time(&event.start,
			&calendar->events[target - 1U].start) < 0) {
			calendar->events[target] = calendar->events[target - 1U];
			--target;
		}
		calendar->events[target] = event;
	}
}

int papermono_organizer_reload(void)
{
	memset(&organizer_staging, 0, sizeof(organizer_staging));
	int ret = papermono_content_store_scan_all(load_content,
		&organizer_staging, NULL);

	if (ret < 0 && ret != -ENODEV) {
		organizer_staging.last_error = ret;
	}
	sort_events(&organizer_staging.calendar);
	k_mutex_lock(&organizer_lock, K_FOREVER);
	organizer = organizer_staging;
	k_mutex_unlock(&organizer_lock);
	LOG_INF("Organizer loaded: events=%zu tasks=%zu notes=%zu error=%d",
		organizer_staging.calendar.count,
		organizer_staging.task_list.count, organizer_staging.note_count,
		organizer_staging.last_error);
	return ret < 0 ? ret : organizer_staging.last_error;
}

int papermono_organizer_init(void)
{
	int ret;

	if (initialized) {
		return 0;
	}
	memset(&overlay, 0, sizeof(overlay));
	ret = settings_load_subtree_direct("papermono_state/overlay",
		load_overlay, NULL);
	if (ret < 0) {
		LOG_WRN("Organizer overlay unavailable (%d)", ret);
	}
	initialized = true;
	return papermono_organizer_reload();
}

size_t papermono_organizer_calendar_count(void)
{
	size_t count;
	k_mutex_lock(&organizer_lock, K_FOREVER);
	count = organizer.calendar.count;
	k_mutex_unlock(&organizer_lock);
	return count;
}

int papermono_organizer_get_event(size_t index,
	struct papermono_calendar_event *event)
{
	if (event == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&organizer_lock, K_FOREVER);
	if (index >= organizer.calendar.count) {
		k_mutex_unlock(&organizer_lock);
		return -ENOENT;
	}
	*event = organizer.calendar.events[index];
	k_mutex_unlock(&organizer_lock);
	return 0;
}

/* Shared scan for the soonest event at or after `now`. Resolves recurrence
 * before comparing, so a weekly event created years ago still counts as
 * upcoming. Either output may be NULL.
 */
static int next_event_locked(const struct papermono_calendar_time *now,
	struct papermono_calendar_event *event, size_t *index_out)
{
	bool found = false;
	struct papermono_calendar_event earliest;
	size_t earliest_index = 0U;

	k_mutex_lock(&organizer_lock, K_FOREVER);
	for (size_t index = 0U; index < organizer.calendar.count; ++index) {
		struct papermono_calendar_event candidate;

		if (papermono_calendar_next_occurrence(
			&organizer.calendar.events[index], now, &candidate) == 0 &&
		    (!found || compare_time(&candidate.start, &earliest.start) < 0)) {
			earliest = candidate;
			earliest_index = index;
			found = true;
		}
	}
	k_mutex_unlock(&organizer_lock);
	if (!found) {
		return -ENOENT;
	}
	if (event != NULL) {
		*event = earliest;
	}
	if (index_out != NULL) {
		*index_out = earliest_index;
	}
	return 0;
}

int papermono_organizer_get_next_event(
	const struct papermono_calendar_time *now,
	struct papermono_calendar_event *event)
{
	if (now == NULL || event == NULL) {
		return -EINVAL;
	}
	return next_event_locked(now, event, NULL);
}

int papermono_organizer_find_upcoming_index(
	const struct papermono_calendar_time *now, size_t *index)
{
	if (now == NULL || index == NULL) {
		return -EINVAL;
	}
	return next_event_locked(now, NULL, index);
}

static struct task_overlay *task_state(uint64_t key, bool create)
{
	for (size_t index = 0U; index < overlay.task_count; ++index) {
		if (overlay.tasks[index].key == key) {
			return &overlay.tasks[index];
		}
	}
	if (!create || overlay.task_count >= TASK_OVERLAY_MAX) {
		return NULL;
	}
	struct task_overlay *state = &overlay.tasks[overlay.task_count++];
	memset(state, 0, sizeof(*state));
	state->key = key;
	return state;
}

size_t papermono_organizer_task_count(void)
{
	size_t count;
	k_mutex_lock(&organizer_lock, K_FOREVER);
	count = organizer.task_list.count;
	k_mutex_unlock(&organizer_lock);
	return count;
}

int papermono_organizer_get_task(size_t index, struct papermono_task *task)
{
	if (task == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&organizer_lock, K_FOREVER);
	if (index >= organizer.task_list.count) {
		k_mutex_unlock(&organizer_lock);
		return -ENOENT;
	}
	*task = organizer.task_list.tasks[index];
	struct task_overlay *state = task_state(stable_key(task->id), false);
	if (state != NULL && state->valid != 0U) {
		task->done = state->done != 0U;
	}
	k_mutex_unlock(&organizer_lock);
	return 0;
}

int papermono_organizer_toggle_task(size_t index)
{
	int ret;

	k_mutex_lock(&organizer_lock, K_FOREVER);
	if (index >= organizer.task_list.count) {
		k_mutex_unlock(&organizer_lock);
		return -ENOENT;
	}
	const struct papermono_task *task = &organizer.task_list.tasks[index];
	struct task_overlay *state = task_state(stable_key(task->id), true);
	if (state == NULL) {
		k_mutex_unlock(&organizer_lock);
		return -ENOSPC;
	}
	const bool current = state->valid != 0U ? state->done != 0U : task->done;
	state->done = !current;
	state->valid = 1U;
	ret = save_overlay();
	k_mutex_unlock(&organizer_lock);
	return ret;
}

static struct note_overlay *note_state(uint64_t key, bool create)
{
	for (size_t index = 0U; index < overlay.note_count; ++index) {
		if (overlay.notes[index].key == key) {
			return &overlay.notes[index];
		}
	}
	if (!create || overlay.note_count >= NOTE_OVERLAY_MAX) {
		return NULL;
	}
	struct note_overlay *state = &overlay.notes[overlay.note_count++];
	memset(state, 0, sizeof(*state));
	state->key = key;
	return state;
}

size_t papermono_organizer_note_count(void)
{
	size_t count;
	k_mutex_lock(&organizer_lock, K_FOREVER);
	count = organizer.note_count;
	k_mutex_unlock(&organizer_lock);
	return count;
}

int papermono_organizer_get_note(size_t index, struct papermono_note *note,
	bool *favorite, bool *read)
{
	if (note == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&organizer_lock, K_FOREVER);
	if (index >= organizer.note_count) {
		k_mutex_unlock(&organizer_lock);
		return -ENOENT;
	}
	*note = organizer.notes[index];
	struct note_overlay *state = note_state(stable_key(note->id), false);
	if (favorite != NULL) {
		*favorite = state != NULL && state->favorite != 0U;
	}
	if (read != NULL) {
		*read = state != NULL && state->read != 0U;
	}
	k_mutex_unlock(&organizer_lock);
	return 0;
}

static int update_note_state(size_t index, bool favorite, bool value)
{
	int ret;

	k_mutex_lock(&organizer_lock, K_FOREVER);
	if (index >= organizer.note_count) {
		k_mutex_unlock(&organizer_lock);
		return -ENOENT;
	}
	struct note_overlay *state = note_state(
		stable_key(organizer.notes[index].id), true);
	if (state == NULL) {
		k_mutex_unlock(&organizer_lock);
		return -ENOSPC;
	}
	if (favorite) {
		state->favorite = value;
	} else {
		state->read = value;
	}
	ret = save_overlay();
	k_mutex_unlock(&organizer_lock);
	return ret;
}

int papermono_organizer_set_note_favorite(size_t index, bool favorite)
{
	return update_note_state(index, true, favorite);
}

int papermono_organizer_set_note_read(size_t index, bool read)
{
	return update_note_state(index, false, read);
}

int papermono_organizer_export_state_json(char *buffer, size_t capacity)
{
	size_t length = 0U;
	int ret;

	if (buffer == NULL || capacity == 0U) {
		return -EINVAL;
	}
	k_mutex_lock(&organizer_lock, K_FOREVER);
	ret = snprintf(buffer, capacity, "{\"version\":1,\"tasks\":[");
	if (ret < 0 || ret >= (int)capacity) {
		ret = -ENOSPC;
		goto out;
	}
	length = (size_t)ret;
	for (size_t index = 0U; index < organizer.task_list.count; ++index) {
		struct task_overlay *state = task_state(
			stable_key(organizer.task_list.tasks[index].id), false);

		if (state == NULL || state->valid == 0U) {
			continue;
		}
		ret = snprintf(buffer + length, capacity - length,
			"%s{\"id\":\"%s\",\"done\":%s}",
			length > strlen("{\"version\":1,\"tasks\":[") ? "," : "",
			organizer.task_list.tasks[index].id,
			state->done != 0U ? "true" : "false");
		if (ret < 0 || ret >= (int)(capacity - length)) {
			ret = -ENOSPC;
			goto out;
		}
		length += (size_t)ret;
	}
	ret = snprintf(buffer + length, capacity - length, "],\"notes\":[");
	if (ret < 0 || ret >= (int)(capacity - length)) {
		ret = -ENOSPC;
		goto out;
	}
	length += (size_t)ret;
	bool first = true;
	for (size_t index = 0U; index < organizer.note_count; ++index) {
		struct note_overlay *state = note_state(
			stable_key(organizer.notes[index].id), false);
		if (state == NULL) {
			continue;
		}
		ret = snprintf(buffer + length, capacity - length,
			"%s{\"id\":\"%s\",\"favorite\":%s,\"read\":%s}",
			first ? "" : ",", organizer.notes[index].id,
			state->favorite != 0U ? "true" : "false",
			state->read != 0U ? "true" : "false");
		if (ret < 0 || ret >= (int)(capacity - length)) {
			ret = -ENOSPC;
			goto out;
		}
		length += (size_t)ret;
		first = false;
	}
	ret = snprintf(buffer + length, capacity - length, "]}");
	if (ret < 0 || ret >= (int)(capacity - length)) {
		ret = -ENOSPC;
		goto out;
	}
	length += (size_t)ret;
	ret = (int)length;
out:
	k_mutex_unlock(&organizer_lock);
	return ret;
}

int papermono_organizer_import_state_json(char *buffer, size_t length)
{
	if (buffer == NULL || length == 0U) {
		return -EINVAL;
	}
	int ret = papermono_parse_state_json(buffer, length, &overlay_import);
	if (ret < 0) {
		return ret;
	}
	k_mutex_lock(&organizer_lock, K_FOREVER);
	for (size_t index = 0U; index < overlay_import.task_count; ++index) {
		struct task_overlay *state = task_state(
			stable_key(overlay_import.tasks[index].id), true);
		if (state == NULL) {
			k_mutex_unlock(&organizer_lock);
			return -ENOSPC;
		}
		state->valid = 1U;
		state->done = overlay_import.tasks[index].done;
	}
	for (size_t index = 0U; index < overlay_import.note_count; ++index) {
		struct note_overlay *state = note_state(
			stable_key(overlay_import.notes[index].id), true);
		if (state == NULL) {
			k_mutex_unlock(&organizer_lock);
			return -ENOSPC;
		}
		state->favorite = overlay_import.notes[index].favorite;
		state->read = overlay_import.notes[index].read;
	}
	ret = save_overlay();
	k_mutex_unlock(&organizer_lock);
	return ret;
}
