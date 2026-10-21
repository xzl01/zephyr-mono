/* SPDX-License-Identifier: GPL-3.0-only */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <papermono/personal_data.h>

static void test_ics(void)
{
	static const char calendar_text[] =
		"BEGIN:VCALENDAR\r\n"
		"BEGIN:VEVENT\r\n"
		"UID:meeting-1\r\n"
		"DTSTART:20260827T093000\r\n"
		"DTEND:20260827T103000\r\n"
		"SUMMARY:产品周会\r\n"
		"RRULE:FREQ=WEEKLY;INTERVAL=2;COUNT=8\r\n"
		"END:VEVENT\r\n"
		"BEGIN:VEVENT\r\n"
		"DTSTART;VALUE=DATE:20260828\r\n"
		"SUMMARY:全天\\,外出\r\n"
		" 计划\r\n"
		"END:VEVENT\r\n"
		"END:VCALENDAR\r\n";
	struct papermono_calendar calendar;

	assert(papermono_parse_ics(calendar_text, sizeof(calendar_text) - 1U,
		&calendar, false) == 0);
	assert(calendar.count == 2U);
	assert(strcmp(calendar.events[0].id, "meeting-1") == 0);
	assert(strcmp(calendar.events[0].title, "产品周会") == 0);
	assert(calendar.events[0].recurrence == PAPERMONO_RECURRENCE_WEEKLY);
	assert(calendar.events[0].recurrence_interval == 2U);
	assert(calendar.events[0].recurrence_count == 8U);
	assert(calendar.events[1].start.all_day);
	assert(strcmp(calendar.events[1].title, "全天,外出计划") == 0);
	assert(papermono_parse_ics("BEGIN:VEVENT\nSUMMARY:x\n", 23U,
		&calendar, false) == -EBADMSG);
}

static void test_recurrence_occurrences(void)
{
	struct papermono_calendar_event weekly = {
		.start = { .year = 2026, .month = 8, .day = 27, .hour = 9,
			.minute = 30 },
		.end = { .year = 2026, .month = 8, .day = 27, .hour = 10,
			.minute = 30 },
		.recurrence = PAPERMONO_RECURRENCE_WEEKLY,
		.recurrence_interval = 2U,
		.recurrence_count = 3U,
		.has_end = true,
	};
	const struct papermono_calendar_time now = {
		.year = 2026, .month = 9, .day = 1,
	};
	struct papermono_calendar_event next;

	assert(papermono_calendar_next_occurrence(&weekly, &now, &next) == 0);
	assert(next.start.year == 2026 && next.start.month == 9U &&
		next.start.day == 10U && next.start.hour == 9U);
	assert(next.end.day == 10U && next.end.hour == 10U &&
		next.end.minute == 30U);
	const struct papermono_calendar_time after_count = {
		.year = 2026, .month = 10, .day = 1,
	};
	assert(papermono_calendar_next_occurrence(&weekly, &after_count,
		&next) == -ENOENT);

	struct papermono_calendar_event leap_day = {
		.start = { .year = 2024, .month = 2, .day = 29, .hour = 8 },
		.recurrence = PAPERMONO_RECURRENCE_YEARLY,
		.recurrence_interval = 1U,
	};
	const struct papermono_calendar_time after_leap = {
		.year = 2025, .month = 1, .day = 1,
	};
	assert(papermono_calendar_next_occurrence(&leap_day, &after_leap,
		&next) == 0);
	assert(next.start.year == 2028 && next.start.month == 2U &&
		next.start.day == 29U);
}

static void test_tasks(void)
{
	static const char tasks_json[] =
		"[{\"id\":\"t-1\",\"title\":\"完成文档\",\"due\":\"2026-08-27\","
		"\"priority\":2,\"done\":false,\"extra\":{\"safe\":[1,true]}},"
		"{\"id\":\"t-2\",\"title\":\"Review \\u4ee3\\u7801\","
		"\"priority\":-1,\"done\":true}]";
	struct papermono_task_list tasks;

	assert(papermono_parse_tasks_json(tasks_json, sizeof(tasks_json) - 1U,
		&tasks) == 0);
	assert(tasks.count == 2U);
	assert(strcmp(tasks.tasks[0].title, "完成文档") == 0);
	assert(tasks.tasks[0].priority == 2);
	assert(!tasks.tasks[0].done);
	assert(strcmp(tasks.tasks[1].title, "Review 代码") == 0);
	assert(tasks.tasks[1].done);
	assert(papermono_parse_tasks_json("[{\"title\":\"missing id\"}]", 24U,
		&tasks) == -EBADMSG);
}

static void test_json_rejection(void)
{
	static const char valid_unknown[] =
		"[{\"id\":\"t-1\",\"title\":\"Task\",\"extra\":[null,-0.5e+2,"
		"true,false,{\"value\":0}]}]";
	static const char *const invalid_tasks[] = {
		"[{\"id\":\"t-1\",\"title\":\"Task\",\"extra\":tru}]",
		"[{\"id\":\"t-1\",\"title\":\"Task\",\"extra\":garbage}]",
		"[{\"id\":\"t-1\",\"title\":\"Task\",\"extra\":}]",
		"[{\"id\":\"t-1\",\"title\":\"Task\",}]",
		"[{\"id\":\"t-1\",\"title\":\"Task\"},]",
		"[{\"id\":\"t-1\",\"title\":\"Task\",\"extra\":[1,]}]",
		"[{\"id\":\"t-1\",\"title\":\"Task\",\"extra\":{\"n\":1,}}]",
		"[{\"id\":\"t-1\",\"title\":\"bad\\u0000title\"}]",
		"[{\"id\":\"t-1\",\"title\":\"Task\",\"extra\":\"bad\\u0000value\"}]",
	};
	static const char trailing_state_object[] =
		"{\"version\":1,\"tasks\":[],\"notes\":[],}";
	struct papermono_task_list tasks;
	struct papermono_state_overlay state;

	assert(papermono_parse_tasks_json(valid_unknown,
		sizeof(valid_unknown) - 1U, &tasks) == 0);
	for (size_t index = 0U; index < sizeof(invalid_tasks) /
		 sizeof(invalid_tasks[0]); ++index) {
		assert(papermono_parse_tasks_json(invalid_tasks[index],
			strlen(invalid_tasks[index]), &tasks) == -EBADMSG);
	}
	assert(papermono_parse_state_json(trailing_state_object,
		sizeof(trailing_state_object) - 1U, &state) == -EBADMSG);
}

static void test_markdown(void)
{
	static const char markdown[] =
		"# 今日记录\n\n第一段。\n- 项目一\n> 一条引用\n---\n## 小节\n";
	struct papermono_note note;

	assert(papermono_parse_markdown(markdown, sizeof(markdown) - 1U,
		&note) == 0);
	assert(strcmp(note.title, "今日记录") == 0);
	assert(note.block_count == 6U);
	assert(note.blocks[0].type == PAPERMONO_NOTE_HEADING);
	assert(note.blocks[2].type == PAPERMONO_NOTE_LIST_ITEM);
	assert(note.blocks[3].type == PAPERMONO_NOTE_QUOTE);
	assert(note.blocks[4].type == PAPERMONO_NOTE_SEPARATOR);
}

static void test_state_overlay_merge(void)
{
	static const char state_json[] =
		"{\"version\":1,\"tasks\":[{\"id\":\"t-1\",\"done\":true}],"
		"\"notes\":[{\"id\":\"0123456789abcdef\",\"favorite\":true,"
		"\"read\":false}],\"ignored\":{\"future\":1}}";
	static const char reimported_tasks[] =
		"[{\"id\":\"t-1\",\"title\":\"重新上传后的标题\",\"done\":false},"
		"{\"id\":\"t-2\",\"title\":\"新任务\",\"done\":false}]";
	struct papermono_state_overlay state;
	struct papermono_task_list tasks;

	assert(papermono_parse_state_json(state_json,
		sizeof(state_json) - 1U, &state) == 0);
	assert(state.task_count == 1U && state.note_count == 1U);
	assert(strcmp(state.tasks[0].id, "t-1") == 0 && state.tasks[0].done);
	assert(state.notes[0].favorite && !state.notes[0].read);
	assert(papermono_parse_tasks_json(reimported_tasks,
		sizeof(reimported_tasks) - 1U, &tasks) == 0);
	for (size_t task = 0U; task < tasks.count; ++task) {
		for (size_t saved = 0U; saved < state.task_count; ++saved) {
			if (strcmp(tasks.tasks[task].id, state.tasks[saved].id) == 0) {
				tasks.tasks[task].done = state.tasks[saved].done;
			}
		}
	}
	assert(tasks.tasks[0].done);
	assert(!tasks.tasks[1].done);
	assert(strcmp(tasks.tasks[0].title, "重新上传后的标题") == 0);
	static const char wrong_version[] =
		"{\"version\":2,\"tasks\":[],\"notes\":[]}";
	static const char missing_notes[] = "{\"version\":1,\"tasks\":[]}";
	assert(papermono_parse_state_json(wrong_version,
		sizeof(wrong_version) - 1U, &state) < 0);
	assert(papermono_parse_state_json(missing_notes,
		sizeof(missing_notes) - 1U, &state) < 0);
}

static void test_weread_catalog(void)
{
	static const char catalog_json[] =
		"{\"version\":1,\"source\":\"desktop-companion\","
		"\"synced_at\":\"2026-08-31T18:30:00Z\",\"total\":3,"
		"\"future\":{\"safe\":[1,true]},\"books\":["
		"{\"id\":\"book-1\",\"title\":\"三体\",\"author\":\"刘慈欣\","
		"\"kind\":\"book\",\"progress\":42,\"reading_seconds\":5400,"
		"\"updated_at\":\"2026-08-30\"},"
		"{\"id\":\"audio-1\",\"title\":\"有声书\",\"kind\":\"audio\"},"
		"{\"id\":\"articles\",\"title\":\"公众号文章\","
		"\"kind\":\"articles\",\"progress\":100,\"finished\":false}]}";
	static const char duplicate_id[] =
		"{\"version\":1,\"source\":\"test\","
		"\"synced_at\":\"2026-08-31T00:00:00Z\",\"total\":2,\"books\":["
		"{\"id\":\"same\",\"title\":\"A\"},"
		"{\"id\":\"same\",\"title\":\"B\"}]}";
	static const char wrong_version[] =
		"{\"version\":2,\"source\":\"test\","
		"\"synced_at\":\"2026-08-31T00:00:00Z\",\"total\":0,\"books\":[]}";
	static const char bad_progress[] =
		"{\"version\":1,\"source\":\"test\","
		"\"synced_at\":\"2026-08-31T00:00:00Z\",\"total\":1,"
		"\"books\":[{\"id\":\"bad\",\"title\":\"Bad\",\"progress\":101}]}";
	static const char trailing_root[] =
		"{\"version\":1,\"source\":\"test\","
		"\"synced_at\":\"2026-08-31T00:00:00Z\",\"total\":0,\"books\":[],}";
	static const char trailing_books[] =
		"{\"version\":1,\"source\":\"test\","
		"\"synced_at\":\"2026-08-31T00:00:00Z\",\"total\":1,"
		"\"books\":[{\"id\":\"book\",\"title\":\"Book\"},]}";
	static const char trailing_book[] =
		"{\"version\":1,\"source\":\"test\","
		"\"synced_at\":\"2026-08-31T00:00:00Z\",\"total\":1,"
		"\"books\":[{\"id\":\"book\",\"title\":\"Book\",}]}";
	struct papermono_weread_catalog catalog;

	assert(papermono_parse_weread_json(catalog_json,
		sizeof(catalog_json) - 1U, &catalog) == 0);
	assert(catalog.count == 3U && catalog.total == 3U && !catalog.truncated);
	assert(strcmp(catalog.source, "desktop-companion") == 0);
	assert(strcmp(catalog.books[0].title, "三体") == 0);
	assert(strcmp(catalog.books[0].author, "刘慈欣") == 0);
	assert(catalog.books[0].progress == 42U &&
		catalog.books[0].reading_seconds == 5400U);
	assert(catalog.books[1].kind == PAPERMONO_WEREAD_AUDIO);
	assert(catalog.books[2].kind == PAPERMONO_WEREAD_ARTICLES);
	assert(catalog.books[2].finished);
	assert(papermono_parse_weread_json(duplicate_id,
		sizeof(duplicate_id) - 1U, &catalog) == -EEXIST);
	assert(papermono_parse_weread_json(wrong_version,
		sizeof(wrong_version) - 1U, &catalog) == -EPROTONOSUPPORT);
	assert(papermono_parse_weread_json(bad_progress,
		sizeof(bad_progress) - 1U, &catalog) == -ERANGE);
	assert(papermono_parse_weread_json(trailing_root,
		sizeof(trailing_root) - 1U, &catalog) == -EBADMSG);
	assert(papermono_parse_weread_json(trailing_books,
		sizeof(trailing_books) - 1U, &catalog) == -EBADMSG);
	assert(papermono_parse_weread_json(trailing_book,
		sizeof(trailing_book) - 1U, &catalog) == -EBADMSG);

	char many_books[8192];
	size_t used = (size_t)snprintf(many_books, sizeof(many_books),
		"{\"version\":1,\"source\":\"test\","
		"\"synced_at\":\"2026-08-31T00:00:00Z\",\"total\":34,\"books\":[");
	for (unsigned int index = 0U; index < 34U; ++index) {
		const int written = snprintf(many_books + used, sizeof(many_books) - used,
			"%s{\"id\":\"book-%u\",\"title\":\"Book %u\"}",
			index == 0U ? "" : ",", index, index);

		assert(written > 0 && (size_t)written < sizeof(many_books) - used);
		used += (size_t)written;
	}
	assert(used + 3U < sizeof(many_books));
	many_books[used++] = ']';
	many_books[used++] = '}';
	many_books[used] = '\0';
	assert(papermono_parse_weread_json(many_books, used, &catalog) == 0);
	assert(catalog.count == PAPERMONO_WEREAD_BOOK_MAX);
	assert(catalog.total == 34U && catalog.truncated);

	char *declared_total = strstr(many_books, "\"total\":34");
	assert(declared_total != NULL);
	declared_total[strlen("\"total\":3")] = '3';
	assert(papermono_parse_weread_json(many_books, used, &catalog) == -EBADMSG);
	declared_total[strlen("\"total\":3")] = '4';

	char *overflow_id = strstr(many_books, "\"id\":\"book-33\"");
	assert(overflow_id != NULL);
	overflow_id[strlen("\"id\":\"book-3")] = '2';
	assert(papermono_parse_weread_json(many_books, used, &catalog) == -EEXIST);
	overflow_id[strlen("\"id\":\"book-3")] = '3';
}

int main(void)
{
	test_ics();
	test_recurrence_occurrences();
	test_tasks();
	test_json_rejection();
	test_markdown();
	test_state_overlay_merge();
	test_weread_catalog();
	puts("personal data tests: PASS");
	return 0;
}
