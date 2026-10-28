/* SPDX-License-Identifier: GPL-3.0-only */
/* Standalone host test: cc -std=c17 -Wall -Wextra -Werror
 * -Itests/host/stubs -Iinclude tests/host/test_organizer.c
 * src/papermono_personal_data.c -o /tmp/test_organizer
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The minimal host util shim does not provide Zephyr's ARG_UNUSED. */
#define ARG_UNUSED(value) (void)(value)
#include "../../src/papermono_organizer.c"

static const char *fake_suffix;
static unsigned int read_count;
static const char *fake_note_text;
static bool fake_scan_enabled;
static int fake_scan_ret;
static struct organizer_overlay saved_overlay = {
	.version = ORGANIZER_OVERLAY_VERSION,
	.task_count = 1U,
	.note_count = 1U,
	.tasks = { { .key = 123U, .done = 1U, .valid = 1U } },
	.notes = { { .key = 456U, .favorite = 1U, .read = 1U } },
};

static int read_overlay(void *arg, void *data, size_t length)
{
	++read_count;
	memcpy(data, arg, length);
	return (int)length;
}

int settings_load_subtree_direct(const char *subtree,
	settings_load_direct_cb callback, void *param)
{
	assert(strcmp(subtree, "papermono_state/overlay") == 0);
	return callback(fake_suffix, sizeof(saved_overlay), read_overlay,
		&saved_overlay, param);
}

int settings_save_one(const char *key, const void *value, size_t length)
{
	(void)key;
	(void)value;
	(void)length;
	return 0;
}

int papermono_content_store_scan_all(papermono_content_scan_cb callback,
	void *context, size_t *count)
{
	size_t found = 0U;

	if (fake_scan_enabled) {
		struct papermono_content_entry entry = {
			.kind = PAPERMONO_CONTENT_NOTE,
			.size = strlen(fake_note_text),
		};

		(void)snprintf(entry.path, sizeof(entry.path),
			"/SD:/papermono/notes/test.md");
		(void)snprintf(entry.title, sizeof(entry.title), "test");
		(void)callback(&entry, context);
		++found;
	}
	if (count != NULL) {
		*count = found;
	}
	return fake_scan_ret;
}

int papermono_content_store_open(const struct papermono_content_entry *entry,
	struct fs_file_t *file)
{
	(void)file;
	return entry != NULL && entry->kind == PAPERMONO_CONTENT_NOTE ? 0 :
		-ENOENT;
}

ssize_t papermono_fs_read_chunked(struct fs_file_t *file, void *buffer,
	size_t length)
{
	(void)file;
	if (!fake_scan_enabled) {
		return -EIO;
	}
	const size_t copy = MIN(length, strlen(fake_note_text));

	memcpy(buffer, fake_note_text, copy);
	return (ssize_t)copy;
}

int fs_close(struct fs_file_t *file)
{
	(void)file;
	return 0;
}

int main(void)
{
	static const char *const suffixes[] = { NULL, "", "child" };

	for (size_t index = 0U; index < 3U; ++index) {
		initialized = false;
		read_count = 0U;
		fake_suffix = suffixes[index];
		assert(papermono_organizer_init() == 0);
		assert(read_count == (index < 2U ? 1U : 0U));
		assert(overlay.task_count == (index < 2U ? 1U : 0U));
		assert(overlay.note_count == (index < 2U ? 1U : 0U));
		if (index < 2U) {
			assert(overlay.tasks[0].key == 123U);
			assert(overlay.tasks[0].done && overlay.tasks[0].valid);
			assert(overlay.notes[0].key == 456U);
			assert(overlay.notes[0].favorite && overlay.notes[0].read);
		}
	}

	/* Reload now builds the published organizer in place instead of a second
	 * 89 KB staging copy. The lock still makes the rebuild atomic, and every
	 * scan starts from zero rather than appending to the previous result.
	 */
	initialized = false;
	fake_scan_enabled = true;
	fake_scan_ret = 0;
	fake_note_text = "# Hello\n\nWorld\n";
	assert(papermono_organizer_init() == 0);
	assert(papermono_organizer_note_count() == 1U);
	assert(strcmp(organizer.notes[0].title, "Hello") == 0);

	/* A failing rescan must not leave the old note visible. */
	fake_scan_enabled = false;
	fake_scan_ret = -EIO;
	assert(papermono_organizer_reload() == -EIO);
	assert(papermono_organizer_note_count() == 0U);
	assert(papermono_organizer_calendar_count() == 0U);
	assert(organizer.last_error == -EIO);

	/* Recovery: a later successful scan repopulates from empty. */
	fake_scan_enabled = true;
	fake_scan_ret = 0;
	assert(papermono_organizer_reload() == 0);
	assert(papermono_organizer_note_count() == 1U);

	fake_scan_enabled = false;
	fake_scan_ret = 0;
	assert(papermono_organizer_reload() == 0);
	assert(papermono_organizer_note_count() == 0U);
	puts("organizer exact-root settings tests: PASS");
	return 0;
}
