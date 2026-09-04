/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_PERSONAL_DATA_H_
#define PAPERMONO_PERSONAL_DATA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAPERMONO_CALENDAR_EVENT_MAX 32U
#define PAPERMONO_TASK_MAX 64U
#define PAPERMONO_NOTE_MAX 16U
#define PAPERMONO_NOTE_BLOCK_MAX 24U
#define PAPERMONO_WEREAD_BOOK_MAX 32U
#define PAPERMONO_STATE_VERSION 1U
#define PAPERMONO_WEREAD_CATALOG_VERSION 1U

enum papermono_recurrence_frequency {
	PAPERMONO_RECURRENCE_NONE,
	PAPERMONO_RECURRENCE_DAILY,
	PAPERMONO_RECURRENCE_WEEKLY,
	PAPERMONO_RECURRENCE_MONTHLY,
	PAPERMONO_RECURRENCE_YEARLY,
};

struct papermono_calendar_time {
	int16_t year;
	uint8_t month;
	uint8_t day;
	uint8_t hour;
	uint8_t minute;
	uint8_t second;
	bool all_day;
	bool utc;
};

struct papermono_calendar_event {
	char id[65];
	char title[129];
	struct papermono_calendar_time start;
	struct papermono_calendar_time end;
	enum papermono_recurrence_frequency recurrence;
	uint16_t recurrence_interval;
	uint16_t recurrence_count;
	struct papermono_calendar_time recurrence_until;
	bool has_end;
	bool has_recurrence_until;
};

struct papermono_calendar {
	struct papermono_calendar_event events[PAPERMONO_CALENDAR_EVENT_MAX];
	size_t count;
	bool truncated;
};

struct papermono_task {
	char id[65];
	char title[129];
	char due[33];
	int8_t priority;
	bool done;
};

struct papermono_task_list {
	struct papermono_task tasks[PAPERMONO_TASK_MAX];
	size_t count;
	bool truncated;
};

enum papermono_note_block_type {
	PAPERMONO_NOTE_HEADING,
	PAPERMONO_NOTE_PARAGRAPH,
	PAPERMONO_NOTE_LIST_ITEM,
	PAPERMONO_NOTE_QUOTE,
	PAPERMONO_NOTE_SEPARATOR,
};

struct papermono_note_block {
	char text[161];
	enum papermono_note_block_type type;
	uint8_t level;
};

struct papermono_note {
	char id[17];
	char title[129];
	struct papermono_note_block blocks[PAPERMONO_NOTE_BLOCK_MAX];
	size_t block_count;
	bool truncated;
};

struct papermono_task_state {
	char id[65];
	bool done;
};

struct papermono_note_state {
	char id[17];
	bool favorite;
	bool read;
};

struct papermono_state_overlay {
	struct papermono_task_state tasks[PAPERMONO_TASK_MAX];
	struct papermono_note_state notes[PAPERMONO_NOTE_MAX];
	size_t task_count;
	size_t note_count;
};

enum papermono_weread_kind {
	PAPERMONO_WEREAD_EBOOK,
	PAPERMONO_WEREAD_AUDIO,
	PAPERMONO_WEREAD_ARTICLES,
};

struct papermono_weread_book {
	char id[65];
	char title[129];
	char author[97];
	char updated_at[11];
	uint32_t reading_seconds;
	uint8_t progress;
	enum papermono_weread_kind kind;
	bool has_progress;
	bool finished;
};

struct papermono_weread_catalog {
	struct papermono_weread_book books[PAPERMONO_WEREAD_BOOK_MAX];
	char source[49];
	char synced_at[21];
	size_t count;
	size_t total;
	bool truncated;
};

int papermono_parse_ics(const char *data, size_t length,
	struct papermono_calendar *calendar, bool append);
int papermono_calendar_next_occurrence(
	const struct papermono_calendar_event *event,
	const struct papermono_calendar_time *now,
	struct papermono_calendar_event *occurrence);
int papermono_parse_tasks_json(const char *data, size_t length,
	struct papermono_task_list *tasks);
int papermono_parse_markdown(const char *data, size_t length,
	struct papermono_note *note);
int papermono_parse_state_json(const char *data, size_t length,
	struct papermono_state_overlay *state);
int papermono_parse_weread_json(const char *data, size_t length,
	struct papermono_weread_catalog *catalog);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_PERSONAL_DATA_H_ */
