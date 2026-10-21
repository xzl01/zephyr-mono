/* SPDX-License-Identifier: GPL-3.0-only */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <papermono/personal_data.h>

#define LOGICAL_LINE_MAX 512U
#define JSON_NESTING_MAX 8U

static size_t copy_text(char *destination, size_t capacity,
	const char *source, size_t length)
{
	const size_t copied = length < capacity - 1U ? length : capacity - 1U;

	memcpy(destination, source, copied);
	destination[copied] = '\0';
	return copied;
}

static bool decimal_range(const char *text, size_t length, int minimum,
	int maximum, int *value)
{
	int result = 0;

	if (length == 0U) {
		return false;
	}
	for (size_t index = 0U; index < length; ++index) {
		if (text[index] < '0' || text[index] > '9') {
			return false;
		}
		result = result * 10 + text[index] - '0';
	}
	if (result < minimum || result > maximum) {
		return false;
	}
	*value = result;
	return true;
}

static bool parse_calendar_time(const char *value,
	struct papermono_calendar_time *time)
{
	const size_t length = strlen(value);
	int parsed;

	memset(time, 0, sizeof(*time));
	if (length != 8U && length != 15U && length != 16U) {
		return false;
	}
	if (!decimal_range(value, 4U, 1970, 9999, &parsed)) {
		return false;
	}
	time->year = (int16_t)parsed;
	if (!decimal_range(value + 4U, 2U, 1, 12, &parsed)) {
		return false;
	}
	time->month = (uint8_t)parsed;
	if (!decimal_range(value + 6U, 2U, 1, 31, &parsed)) {
		return false;
	}
	time->day = (uint8_t)parsed;
	if (length == 8U) {
		time->all_day = true;
		return true;
	}
	if (value[8] != 'T' ||
	    !decimal_range(value + 9U, 2U, 0, 23, &parsed)) {
		return false;
	}
	time->hour = (uint8_t)parsed;
	if (!decimal_range(value + 11U, 2U, 0, 59, &parsed)) {
		return false;
	}
	time->minute = (uint8_t)parsed;
	if (!decimal_range(value + 13U, 2U, 0, 60, &parsed)) {
		return false;
	}
	time->second = (uint8_t)parsed;
	time->utc = length == 16U && value[15] == 'Z';
	return length == 15U || time->utc;
}

static void unescape_ics(char *destination, size_t capacity,
	const char *source)
{
	size_t output = 0U;

	for (size_t index = 0U; source[index] != '\0' && output + 1U < capacity;
	     ++index) {
		if (source[index] == '\\' && source[index + 1U] != '\0') {
			const char escaped = source[++index];

			if (escaped == 'n' || escaped == 'N') {
				destination[output++] = '\n';
			} else {
				destination[output++] = escaped;
			}
		} else {
			destination[output++] = source[index];
		}
	}
	destination[output] = '\0';
}

static bool next_logical_line(const char *data, size_t length, size_t *offset,
	char *line, size_t capacity)
{
	size_t output = 0U;
	bool found = false;

	while (*offset < length) {
		if (found && data[*offset] != ' ' && data[*offset] != '\t') {
			break;
		}
		if (found) {
			++*offset;
		}
		while (*offset < length && data[*offset] != '\r' &&
		       data[*offset] != '\n') {
			if (output + 1U < capacity) {
				line[output++] = data[*offset];
			}
			++*offset;
		}
		while (*offset < length &&
		       (data[*offset] == '\r' || data[*offset] == '\n')) {
			++*offset;
		}
		found = true;
	}
	line[output] = '\0';
	return found;
}

static void parse_rrule(const char *value,
	struct papermono_calendar_event *event)
{
	char copy[256];
	char *save = NULL;

	copy_text(copy, sizeof(copy), value, strlen(value));
	event->recurrence_interval = 1U;
	for (char *part = strtok_r(copy, ";", &save); part != NULL;
	     part = strtok_r(NULL, ";", &save)) {
		char *separator = strchr(part, '=');

		if (separator == NULL) {
			continue;
		}
		*separator++ = '\0';
		if (strcmp(part, "FREQ") == 0) {
			event->recurrence = strcmp(separator, "DAILY") == 0 ?
				PAPERMONO_RECURRENCE_DAILY :
				strcmp(separator, "WEEKLY") == 0 ?
				PAPERMONO_RECURRENCE_WEEKLY :
				strcmp(separator, "MONTHLY") == 0 ?
				PAPERMONO_RECURRENCE_MONTHLY :
				strcmp(separator, "YEARLY") == 0 ?
				PAPERMONO_RECURRENCE_YEARLY :
				PAPERMONO_RECURRENCE_NONE;
		} else if (strcmp(part, "INTERVAL") == 0) {
			const unsigned long interval = strtoul(separator, NULL, 10);

			if (interval >= 1UL && interval <= UINT16_MAX) {
				event->recurrence_interval = (uint16_t)interval;
			}
		} else if (strcmp(part, "COUNT") == 0) {
			const unsigned long count = strtoul(separator, NULL, 10);

			if (count <= UINT16_MAX) {
				event->recurrence_count = (uint16_t)count;
			}
		} else if (strcmp(part, "UNTIL") == 0) {
			event->has_recurrence_until = parse_calendar_time(separator,
				&event->recurrence_until);
		}
	}
}

static bool calendar_leap_year(int year)
{
	return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static uint8_t calendar_days_in_month(int year, uint8_t month)
{
	static const uint8_t days[] = {
		31U, 28U, 31U, 30U, 31U, 30U,
		31U, 31U, 30U, 31U, 30U, 31U,
	};

	if (month < 1U || month > 12U) {
		return 0U;
	}
	return month == 2U && calendar_leap_year(year) ? 29U :
		days[month - 1U];
}

/* Days relative to 1970-01-01. Based on the proleptic Gregorian calendar. */
static int64_t calendar_days_from_civil(int year, unsigned int month,
	unsigned int day)
{
	year -= month <= 2U;
	const int era = (year >= 0 ? year : year - 399) / 400;
	const unsigned int year_of_era = (unsigned int)(year - era * 400);
	const unsigned int adjusted_month = month > 2U ? month - 3U : month + 9U;
	const unsigned int day_of_year =
		(153U * adjusted_month + 2U) / 5U + day - 1U;
	const unsigned int day_of_era = year_of_era * 365U + year_of_era / 4U -
		year_of_era / 100U + day_of_year;

	return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

static void calendar_civil_from_days(int64_t days, int *year,
	unsigned int *month, unsigned int *day)
{
	days += 719468;
	const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
	const unsigned int day_of_era = (unsigned int)(days - era * 146097);
	const unsigned int year_of_era = (day_of_era - day_of_era / 1460U +
		day_of_era / 36524U - day_of_era / 146096U) / 365U;
	int calculated_year = (int)year_of_era + (int)era * 400;
	const unsigned int day_of_year = day_of_era -
		(365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
	const unsigned int month_piece = (5U * day_of_year + 2U) / 153U;
	const unsigned int calculated_day = day_of_year -
		(153U * month_piece + 2U) / 5U + 1U;
	const unsigned int calculated_month = month_piece < 10U ?
		month_piece + 3U : month_piece - 9U;

	calculated_year += calculated_month <= 2U;
	*year = calculated_year;
	*month = calculated_month;
	*day = calculated_day;
}

static bool calendar_time_valid(const struct papermono_calendar_time *time)
{
	return time != NULL && time->year >= 1 && time->month >= 1U &&
		time->month <= 12U && time->day >= 1U &&
		time->day <= calendar_days_in_month(time->year, time->month) &&
		time->hour <= 23U && time->minute <= 59U && time->second <= 60U;
}

static int calendar_time_compare(const struct papermono_calendar_time *left,
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

static int64_t calendar_time_seconds(const struct papermono_calendar_time *time)
{
	return calendar_days_from_civil(time->year, time->month, time->day) *
		86400 + (int64_t)time->hour * 3600 +
		(int64_t)time->minute * 60 + time->second;
}

static struct papermono_calendar_time calendar_time_from_seconds(
	int64_t seconds, const struct papermono_calendar_time *model)
{
	int64_t days = seconds / 86400;
	int64_t within_day = seconds % 86400;
	int year;
	unsigned int month;
	unsigned int day;

	if (within_day < 0) {
		within_day += 86400;
		--days;
	}
	calendar_civil_from_days(days, &year, &month, &day);
	return (struct papermono_calendar_time) {
		.year = (int16_t)year,
		.month = (uint8_t)month,
		.day = (uint8_t)day,
		.hour = (uint8_t)(within_day / 3600),
		.minute = (uint8_t)((within_day % 3600) / 60),
		.second = (uint8_t)(within_day % 60),
		.all_day = model->all_day,
		.utc = model->utc,
	};
}

static bool recurrence_candidate(const struct papermono_calendar_event *event,
	uint32_t recurrence_index, struct papermono_calendar_time *candidate)
{
	const uint32_t interval = event->recurrence_interval == 0U ? 1U :
		event->recurrence_interval;

	*candidate = event->start;
	if (event->recurrence == PAPERMONO_RECURRENCE_DAILY ||
	    event->recurrence == PAPERMONO_RECURRENCE_WEEKLY) {
		const int64_t days = (int64_t)recurrence_index * interval *
			(event->recurrence == PAPERMONO_RECURRENCE_WEEKLY ? 7 : 1);
		*candidate = calendar_time_from_seconds(
			calendar_time_seconds(&event->start) + days * 86400,
			&event->start);
		return true;
	}
	if (event->recurrence == PAPERMONO_RECURRENCE_MONTHLY) {
		const int64_t month_index = (int64_t)event->start.year * 12 +
			event->start.month - 1 + (int64_t)recurrence_index * interval;
		candidate->year = (int16_t)(month_index / 12);
		candidate->month = (uint8_t)(month_index % 12 + 1);
		return candidate->day <= calendar_days_in_month(candidate->year,
			candidate->month);
	}
	if (event->recurrence == PAPERMONO_RECURRENCE_YEARLY) {
		candidate->year = (int16_t)(event->start.year +
			(int64_t)recurrence_index * interval);
		return candidate->day <= calendar_days_in_month(candidate->year,
			candidate->month);
	}
	return recurrence_index == 0U;
}

int papermono_calendar_next_occurrence(
	const struct papermono_calendar_event *event,
	const struct papermono_calendar_time *now,
	struct papermono_calendar_event *occurrence)
{
	if (event == NULL || now == NULL || occurrence == NULL ||
	    !calendar_time_valid(&event->start) || !calendar_time_valid(now)) {
		return -EINVAL;
	}
	const int64_t duration = event->has_end &&
		calendar_time_valid(&event->end) ?
		calendar_time_seconds(&event->end) -
			calendar_time_seconds(&event->start) : 0;
	uint32_t generated = 0U;

	/* A uint16 COUNT is the natural bound. Without COUNT, this guard still
	 * covers more than 2700 years of daily events while rejecting malformed
	 * input that could otherwise spin forever.
	 */
	for (uint32_t index = 0U; index < 1000000U; ++index) {
		struct papermono_calendar_time candidate;

		if (!recurrence_candidate(event, index, &candidate)) {
			continue;
		}
		++generated;
		if (event->recurrence_count != 0U &&
		    generated > event->recurrence_count) {
			return -ENOENT;
		}
		if (event->has_recurrence_until &&
		    calendar_time_compare(&candidate,
			&event->recurrence_until) > 0) {
			return -ENOENT;
		}
		if (calendar_time_compare(&candidate, now) < 0) {
			if (event->recurrence == PAPERMONO_RECURRENCE_NONE) {
				return -ENOENT;
			}
			continue;
		}
		*occurrence = *event;
		occurrence->start = candidate;
		if (event->has_end && duration >= 0) {
			occurrence->end = calendar_time_from_seconds(
				calendar_time_seconds(&candidate) + duration,
				&event->end);
		}
		return 0;
	}
	return -ERANGE;
}

int papermono_parse_ics(const char *data, size_t length,
	struct papermono_calendar *calendar, bool append)
{
	struct papermono_calendar_event current = { 0 };
	char line[LOGICAL_LINE_MAX];
	size_t offset = 0U;
	bool inside_event = false;
	bool have_start = false;

	if (data == NULL || calendar == NULL) {
		return -EINVAL;
	}
	if (!append) {
		memset(calendar, 0, sizeof(*calendar));
	}
	while (next_logical_line(data, length, &offset, line, sizeof(line))) {
		if (strcmp(line, "BEGIN:VEVENT") == 0) {
			memset(&current, 0, sizeof(current));
			inside_event = true;
			have_start = false;
			continue;
		}
		if (strcmp(line, "END:VEVENT") == 0) {
			if (inside_event && have_start && current.title[0] != '\0') {
				if (current.id[0] == '\0') {
					(void)snprintf(current.id, sizeof(current.id),
						"%04d%02u%02u-%zu", current.start.year,
						current.start.month, current.start.day,
						calendar->count);
				}
				if (calendar->count < PAPERMONO_CALENDAR_EVENT_MAX) {
					calendar->events[calendar->count++] = current;
				} else {
					calendar->truncated = true;
				}
			}
			inside_event = false;
			continue;
		}
		if (!inside_event) {
			continue;
		}
		char *colon = strchr(line, ':');
		if (colon == NULL) {
			continue;
		}
		*colon++ = '\0';
		char *parameters = strchr(line, ';');
		if (parameters != NULL) {
			*parameters = '\0';
		}
		if (strcmp(line, "UID") == 0) {
			copy_text(current.id, sizeof(current.id), colon, strlen(colon));
		} else if (strcmp(line, "SUMMARY") == 0) {
			unescape_ics(current.title, sizeof(current.title), colon);
		} else if (strcmp(line, "DTSTART") == 0) {
			have_start = parse_calendar_time(colon, &current.start);
		} else if (strcmp(line, "DTEND") == 0) {
			current.has_end = parse_calendar_time(colon, &current.end);
		} else if (strcmp(line, "RRULE") == 0) {
			parse_rrule(colon, &current);
		}
	}
	return inside_event ? -EBADMSG : 0;
}

struct json_cursor {
	const char *data;
	size_t length;
	size_t offset;
};

static void json_whitespace(struct json_cursor *cursor)
{
	while (cursor->offset < cursor->length &&
	       isspace((unsigned char)cursor->data[cursor->offset])) {
		++cursor->offset;
	}
}

static int append_utf8(char *destination, size_t capacity, size_t *output,
	uint32_t codepoint)
{
	uint8_t encoded[4];
	size_t count;

	if (codepoint <= 0x7FU) {
		encoded[0] = (uint8_t)codepoint;
		count = 1U;
	} else if (codepoint <= 0x7FFU) {
		encoded[0] = 0xC0U | (uint8_t)(codepoint >> 6);
		encoded[1] = 0x80U | (uint8_t)(codepoint & 0x3FU);
		count = 2U;
	} else {
		encoded[0] = 0xE0U | (uint8_t)(codepoint >> 12);
		encoded[1] = 0x80U | (uint8_t)((codepoint >> 6) & 0x3FU);
		encoded[2] = 0x80U | (uint8_t)(codepoint & 0x3FU);
		count = 3U;
	}
	if (destination != NULL && *output + count + 1U > capacity) {
		return -ENOSPC;
	}
	if (destination != NULL) {
		memcpy(destination + *output, encoded, count);
	}
	*output += count;
	return 0;
}

static int json_hex4(struct json_cursor *cursor, uint32_t *value)
{
	uint32_t result = 0U;

	if (cursor->offset + 4U > cursor->length) {
		return -EBADMSG;
	}
	for (int index = 0; index < 4; ++index) {
		const char digit = cursor->data[cursor->offset++];

		result <<= 4;
		if (digit >= '0' && digit <= '9') {
			result |= (uint32_t)(digit - '0');
		} else if (digit >= 'a' && digit <= 'f') {
			result |= (uint32_t)(digit - 'a' + 10);
		} else if (digit >= 'A' && digit <= 'F') {
			result |= (uint32_t)(digit - 'A' + 10);
		} else {
			return -EBADMSG;
		}
	}
	*value = result;
	return 0;
}

static int json_string(struct json_cursor *cursor, char *destination,
	size_t capacity)
{
	size_t output = 0U;

	json_whitespace(cursor);
	if (cursor->offset >= cursor->length ||
	    cursor->data[cursor->offset++] != '"') {
		return -EBADMSG;
	}
	while (cursor->offset < cursor->length) {
		unsigned char value = (unsigned char)cursor->data[cursor->offset++];

		if (value == '"') {
			if (destination != NULL) {
				destination[output] = '\0';
			}
			return 0;
		}
		if (value < 0x20U) {
			return -EBADMSG;
		}
		if (value == '\\') {
			if (cursor->offset >= cursor->length) {
				return -EBADMSG;
			}
			value = (unsigned char)cursor->data[cursor->offset++];
			if (value == 'u') {
				uint32_t codepoint;
				int ret = json_hex4(cursor, &codepoint);

				if (ret < 0 || codepoint == 0U ||
				    (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
					return -EBADMSG;
				}
				ret = append_utf8(destination, capacity, &output,
					codepoint);
				if (ret < 0) {
					return ret;
				}
				continue;
			}
			value = value == 'n' ? '\n' : value == 'r' ? '\r' :
				value == 't' ? '\t' : value == 'b' ? '\b' :
				value == 'f' ? '\f' : value;
			if (strchr("\"\\/bfnrt", cursor->data[cursor->offset - 1U]) ==
			    NULL) {
				return -EBADMSG;
			}
		}
		if (destination != NULL) {
			if (output + 2U > capacity) {
				return -ENOSPC;
			}
			destination[output] = (char)value;
		}
		++output;
	}
	return -EBADMSG;
}

static int json_skip_value(struct json_cursor *cursor, unsigned int depth);

static int json_require_next_value(struct json_cursor *cursor, char close)
{
	json_whitespace(cursor);
	return cursor->offset >= cursor->length ||
		cursor->data[cursor->offset] == close ? -EBADMSG : 0;
}

static int json_skip_compound(struct json_cursor *cursor, char open,
	char close, unsigned int depth)
{
	bool object = open == '{';

	if (depth >= JSON_NESTING_MAX || cursor->data[cursor->offset++] != open) {
		return -EBADMSG;
	}
	json_whitespace(cursor);
	if (cursor->offset < cursor->length && cursor->data[cursor->offset] == close) {
		++cursor->offset;
		return 0;
	}
	for (;;) {
		int ret;

		if (object) {
			ret = json_string(cursor, NULL, 0U);
			json_whitespace(cursor);
			if (ret < 0 || cursor->offset >= cursor->length ||
			    cursor->data[cursor->offset++] != ':') {
				return -EBADMSG;
			}
		}
		ret = json_skip_value(cursor, depth + 1U);
		if (ret < 0) {
			return ret;
		}
		json_whitespace(cursor);
		if (cursor->offset >= cursor->length) {
			return -EBADMSG;
		}
		const char separator = cursor->data[cursor->offset++];
		if (separator == close) {
			return 0;
		}
		if (separator != ',') {
			return -EBADMSG;
		}
		if (json_require_next_value(cursor, close) < 0) {
			return -EBADMSG;
		}
	}
}

static bool json_value_terminator(const struct json_cursor *cursor)
{
	return cursor->offset >= cursor->length ||
		strchr(",]} \t\r\n", cursor->data[cursor->offset]) != NULL;
}

static int json_skip_literal(struct json_cursor *cursor, const char *literal)
{
	const size_t length = strlen(literal);

	if (cursor->offset + length > cursor->length ||
	    memcmp(cursor->data + cursor->offset, literal, length) != 0) {
		return -EBADMSG;
	}
	cursor->offset += length;
	return json_value_terminator(cursor) ? 0 : -EBADMSG;
}

static int json_skip_number(struct json_cursor *cursor)
{
	if (cursor->data[cursor->offset] == '-') {
		++cursor->offset;
		if (cursor->offset >= cursor->length) {
			return -EBADMSG;
		}
	}
	if (cursor->data[cursor->offset] == '0') {
		++cursor->offset;
		if (cursor->offset < cursor->length &&
		    isdigit((unsigned char)cursor->data[cursor->offset])) {
			return -EBADMSG;
		}
	} else if (cursor->data[cursor->offset] >= '1' &&
		   cursor->data[cursor->offset] <= '9') {
		do {
			++cursor->offset;
		} while (cursor->offset < cursor->length &&
			 isdigit((unsigned char)cursor->data[cursor->offset]));
	} else {
		return -EBADMSG;
	}
	if (cursor->offset < cursor->length &&
	    cursor->data[cursor->offset] == '.') {
		++cursor->offset;
		if (cursor->offset >= cursor->length ||
		    !isdigit((unsigned char)cursor->data[cursor->offset])) {
			return -EBADMSG;
		}
		while (cursor->offset < cursor->length &&
		       isdigit((unsigned char)cursor->data[cursor->offset])) {
			++cursor->offset;
		}
	}
	if (cursor->offset < cursor->length &&
	    (cursor->data[cursor->offset] == 'e' ||
	     cursor->data[cursor->offset] == 'E')) {
		++cursor->offset;
		if (cursor->offset < cursor->length &&
		    (cursor->data[cursor->offset] == '+' ||
		     cursor->data[cursor->offset] == '-')) {
			++cursor->offset;
		}
		if (cursor->offset >= cursor->length ||
		    !isdigit((unsigned char)cursor->data[cursor->offset])) {
			return -EBADMSG;
		}
		while (cursor->offset < cursor->length &&
		       isdigit((unsigned char)cursor->data[cursor->offset])) {
			++cursor->offset;
		}
	}
	return json_value_terminator(cursor) ? 0 : -EBADMSG;
}

static int json_skip_value(struct json_cursor *cursor, unsigned int depth)
{
	json_whitespace(cursor);
	if (cursor->offset >= cursor->length) {
		return -EBADMSG;
	}
	const char value = cursor->data[cursor->offset];
	if (value == '"') {
		return json_string(cursor, NULL, 0U);
	}
	if (value == '{') {
		return json_skip_compound(cursor, '{', '}', depth);
	}
	if (value == '[') {
		return json_skip_compound(cursor, '[', ']', depth);
	}
	if (value == 't') {
		return json_skip_literal(cursor, "true");
	}
	if (value == 'f') {
		return json_skip_literal(cursor, "false");
	}
	if (value == 'n') {
		return json_skip_literal(cursor, "null");
	}
	return value == '-' || (value >= '0' && value <= '9') ?
		json_skip_number(cursor) : -EBADMSG;
}

static int json_boolean(struct json_cursor *cursor, bool *value)
{
	json_whitespace(cursor);
	if (cursor->offset + 4U <= cursor->length &&
	    memcmp(cursor->data + cursor->offset, "true", 4U) == 0) {
		cursor->offset += 4U;
		*value = true;
		return 0;
	}
	if (cursor->offset + 5U <= cursor->length &&
	    memcmp(cursor->data + cursor->offset, "false", 5U) == 0) {
		cursor->offset += 5U;
		*value = false;
		return 0;
	}
	return -EBADMSG;
}

static int json_integer(struct json_cursor *cursor, int *value)
{
	int64_t result = 0;
	bool negative = false;
	bool digit = false;

	json_whitespace(cursor);
	if (cursor->offset >= cursor->length) {
		return -EBADMSG;
	}
	if (cursor->data[cursor->offset] == '-') {
		negative = true;
		++cursor->offset;
	}
	while (cursor->offset < cursor->length &&
	       cursor->data[cursor->offset] >= '0' &&
	       cursor->data[cursor->offset] <= '9') {
		digit = true;
		result = result * 10 + cursor->data[cursor->offset++] - '0';
		if (result > (int64_t)INT_MAX + (negative ? 1LL : 0LL)) {
			return -ERANGE;
		}
	}
	if (!digit) {
		return -EBADMSG;
	}
	*value = negative ? (int)-result : (int)result;
	return 0;
}

static int parse_task_object(struct json_cursor *cursor,
	struct papermono_task *task)
{
	bool have_id = false;
	bool have_title = false;

	memset(task, 0, sizeof(*task));
	json_whitespace(cursor);
	if (cursor->offset >= cursor->length || cursor->data[cursor->offset++] != '{') {
		return -EBADMSG;
	}
	for (;;) {
		char key[24];
		int ret;

		json_whitespace(cursor);
		if (cursor->offset < cursor->length && cursor->data[cursor->offset] == '}') {
			++cursor->offset;
			return have_id && have_title ? 0 : -EBADMSG;
		}
		ret = json_string(cursor, key, sizeof(key));
		json_whitespace(cursor);
		if (ret < 0 || cursor->offset >= cursor->length ||
		    cursor->data[cursor->offset++] != ':') {
			return -EBADMSG;
		}
		if (strcmp(key, "id") == 0) {
			ret = json_string(cursor, task->id, sizeof(task->id));
			have_id = ret == 0 && task->id[0] != '\0';
		} else if (strcmp(key, "title") == 0) {
			ret = json_string(cursor, task->title, sizeof(task->title));
			have_title = ret == 0 && task->title[0] != '\0';
		} else if (strcmp(key, "due") == 0) {
			ret = json_string(cursor, task->due, sizeof(task->due));
		} else if (strcmp(key, "priority") == 0) {
			int priority;
			ret = json_integer(cursor, &priority);
			if (ret == 0 && priority >= INT8_MIN && priority <= INT8_MAX) {
				task->priority = (int8_t)priority;
			} else if (ret == 0) {
				ret = -ERANGE;
			}
		} else if (strcmp(key, "done") == 0) {
			ret = json_boolean(cursor, &task->done);
		} else {
			ret = json_skip_value(cursor, 0U);
		}
		if (ret < 0) {
			return ret;
		}
		json_whitespace(cursor);
		if (cursor->offset >= cursor->length) {
			return -EBADMSG;
		}
		const char separator = cursor->data[cursor->offset++];
		if (separator == '}') {
			return have_id && have_title ? 0 : -EBADMSG;
		}
		if (separator != ',') {
			return -EBADMSG;
		}
		if (json_require_next_value(cursor, '}') < 0) {
			return -EBADMSG;
		}
	}
}

int papermono_parse_tasks_json(const char *data, size_t length,
	struct papermono_task_list *tasks)
{
	struct json_cursor cursor = { .data = data, .length = length };

	if (data == NULL || tasks == NULL) {
		return -EINVAL;
	}
	memset(tasks, 0, sizeof(*tasks));
	json_whitespace(&cursor);
	if (cursor.offset >= length || data[cursor.offset++] != '[') {
		return -EBADMSG;
	}
	for (;;) {
		struct papermono_task task;
		int ret;

		json_whitespace(&cursor);
		if (cursor.offset < length && data[cursor.offset] == ']') {
			++cursor.offset;
			break;
		}
		ret = parse_task_object(&cursor, &task);
		if (ret < 0) {
			return ret;
		}
		if (tasks->count < PAPERMONO_TASK_MAX) {
			tasks->tasks[tasks->count++] = task;
		} else {
			tasks->truncated = true;
		}
		json_whitespace(&cursor);
		if (cursor.offset >= length) {
			return -EBADMSG;
		}
		const char separator = data[cursor.offset++];
		if (separator == ']') {
			break;
		}
		if (separator != ',') {
			return -EBADMSG;
		}
		if (json_require_next_value(&cursor, ']') < 0) {
			return -EBADMSG;
		}
	}
	json_whitespace(&cursor);
	return cursor.offset == length ? 0 : -EBADMSG;
}

static int parse_weread_kind(struct json_cursor *cursor,
	enum papermono_weread_kind *kind)
{
	char value[16];
	int ret = json_string(cursor, value, sizeof(value));

	if (ret < 0) {
		return ret;
	}
	if (strcmp(value, "book") == 0) {
		*kind = PAPERMONO_WEREAD_EBOOK;
	} else if (strcmp(value, "audio") == 0) {
		*kind = PAPERMONO_WEREAD_AUDIO;
	} else if (strcmp(value, "articles") == 0) {
		*kind = PAPERMONO_WEREAD_ARTICLES;
	} else {
		return -EINVAL;
	}
	return 0;
}

static int parse_weread_book_object(struct json_cursor *cursor,
	struct papermono_weread_book *book)
{
	bool have_id = false;
	bool have_title = false;

	memset(book, 0, sizeof(*book));
	book->kind = PAPERMONO_WEREAD_EBOOK;
	json_whitespace(cursor);
	if (cursor->offset >= cursor->length ||
	    cursor->data[cursor->offset++] != '{') {
		return -EBADMSG;
	}
	for (;;) {
		char key[24];
		int ret;

		json_whitespace(cursor);
		if (cursor->offset < cursor->length &&
		    cursor->data[cursor->offset] == '}') {
			++cursor->offset;
			break;
		}
		ret = json_string(cursor, key, sizeof(key));
		json_whitespace(cursor);
		if (ret < 0 || cursor->offset >= cursor->length ||
		    cursor->data[cursor->offset++] != ':') {
			return -EBADMSG;
		}
		if (strcmp(key, "id") == 0) {
			ret = json_string(cursor, book->id, sizeof(book->id));
			have_id = ret == 0 && book->id[0] != '\0';
		} else if (strcmp(key, "title") == 0) {
			ret = json_string(cursor, book->title, sizeof(book->title));
			have_title = ret == 0 && book->title[0] != '\0';
		} else if (strcmp(key, "author") == 0) {
			ret = json_string(cursor, book->author, sizeof(book->author));
		} else if (strcmp(key, "kind") == 0) {
			ret = parse_weread_kind(cursor, &book->kind);
		} else if (strcmp(key, "progress") == 0) {
			int value;

			ret = json_integer(cursor, &value);
			if (ret == 0 && value >= 0 && value <= 100) {
				book->progress = (uint8_t)value;
				book->has_progress = true;
			} else if (ret == 0) {
				ret = -ERANGE;
			}
		} else if (strcmp(key, "reading_seconds") == 0) {
			int value;

			ret = json_integer(cursor, &value);
			if (ret == 0 && value >= 0) {
				book->reading_seconds = (uint32_t)value;
			} else if (ret == 0) {
				ret = -ERANGE;
			}
		} else if (strcmp(key, "updated_at") == 0) {
			ret = json_string(cursor, book->updated_at,
				sizeof(book->updated_at));
		} else if (strcmp(key, "finished") == 0) {
			ret = json_boolean(cursor, &book->finished);
		} else {
			ret = json_skip_value(cursor, 0U);
		}
		if (ret < 0) {
			return ret;
		}
		json_whitespace(cursor);
		if (cursor->offset >= cursor->length) {
			return -EBADMSG;
		}
		const char separator = cursor->data[cursor->offset++];
		if (separator == '}') {
			break;
		}
		if (separator != ',') {
			return -EBADMSG;
		}
		if (json_require_next_value(cursor, '}') < 0) {
			return -EBADMSG;
		}
	}
	if (book->has_progress && book->progress == 100U) {
		book->finished = true;
	}
	return have_id && have_title ? 0 : -EBADMSG;
}

static int parse_weread_books_array(struct json_cursor *cursor,
	struct papermono_weread_catalog *catalog, size_t *book_count)
{
	size_t parsed = 0U;
	/* Previous entry of the discarded tail, for the O(1) duplicate check. */
	struct papermono_weread_book overflow_previous;
	bool have_overflow = false;

	json_whitespace(cursor);
	if (cursor->offset >= cursor->length ||
	    cursor->data[cursor->offset++] != '[') {
		return -EBADMSG;
	}
	for (;;) {
		struct papermono_weread_book book;
		int ret;

		json_whitespace(cursor);
		if (cursor->offset < cursor->length &&
		    cursor->data[cursor->offset] == ']') {
			++cursor->offset;
			*book_count = parsed;
			return 0;
		}
		ret = parse_weread_book_object(cursor, &book);
		if (ret < 0) {
			return ret;
		}
		/* Duplicate IDs make the catalog malformed and are rejected.
		 *
		 * Against the retained prefix a linear scan is enough, because at most
		 * PAPERMONO_WEREAD_BOOK_MAX entries are kept. For the discarded tail
		 * only the immediately preceding entry is compared: the old code
		 * re-parsed the whole overflow range once per book, which made a
		 * crafted 64 KiB catalog quadratic on the UI thread. Discarded entries
		 * cannot affect the stored catalog, so the weaker check still rejects
		 * the realistic malformed-catalog shapes without the quadratic cost.
		 */
		for (size_t index = 0U; index < catalog->count; ++index) {
			if (strcmp(catalog->books[index].id, book.id) == 0) {
				return -EEXIST;
			}
		}
		if (catalog->count < PAPERMONO_WEREAD_BOOK_MAX) {
			catalog->books[catalog->count++] = book;
		} else {
			if (have_overflow && strcmp(overflow_previous.id,
				book.id) == 0) {
				return -EEXIST;
			}
			overflow_previous = book;
			have_overflow = true;
			catalog->truncated = true;
		}
		++parsed;
		json_whitespace(cursor);
		if (cursor->offset >= cursor->length) {
			return -EBADMSG;
		}
		const char separator = cursor->data[cursor->offset++];
		if (separator == ']') {
			*book_count = parsed;
			return 0;
		}
		if (separator != ',') {
			return -EBADMSG;
		}
		if (json_require_next_value(cursor, ']') < 0) {
			return -EBADMSG;
		}
	}
}

int papermono_parse_weread_json(const char *data, size_t length,
	struct papermono_weread_catalog *catalog)
{
	struct json_cursor cursor = { .data = data, .length = length };
	bool have_version = false;
	bool have_source = false;
	bool have_synced_at = false;
	bool have_total = false;
	bool have_books = false;
	size_t book_count = 0U;

	if (data == NULL || catalog == NULL) {
		return -EINVAL;
	}
	memset(catalog, 0, sizeof(*catalog));
	json_whitespace(&cursor);
	if (cursor.offset >= length || data[cursor.offset++] != '{') {
		return -EBADMSG;
	}
	for (;;) {
		char key[24];
		int ret;

		json_whitespace(&cursor);
		if (cursor.offset < length && data[cursor.offset] == '}') {
			++cursor.offset;
			break;
		}
		ret = json_string(&cursor, key, sizeof(key));
		json_whitespace(&cursor);
		if (ret < 0 || cursor.offset >= length ||
		    data[cursor.offset++] != ':') {
			return -EBADMSG;
		}
		if (strcmp(key, "version") == 0) {
			int version;

			ret = json_integer(&cursor, &version);
			have_version = ret == 0 &&
				version == PAPERMONO_WEREAD_CATALOG_VERSION;
			if (ret == 0 && !have_version) {
				ret = -EPROTONOSUPPORT;
			}
		} else if (strcmp(key, "source") == 0) {
			ret = json_string(&cursor, catalog->source,
				sizeof(catalog->source));
			have_source = ret == 0 && catalog->source[0] != '\0';
		} else if (strcmp(key, "synced_at") == 0) {
			ret = json_string(&cursor, catalog->synced_at,
				sizeof(catalog->synced_at));
			have_synced_at = ret == 0 && catalog->synced_at[0] != '\0';
		} else if (strcmp(key, "total") == 0) {
			int total;

			ret = json_integer(&cursor, &total);
			if (ret == 0 && total >= 0) {
				catalog->total = (size_t)total;
				have_total = true;
			} else if (ret == 0) {
				ret = -ERANGE;
			}
		} else if (strcmp(key, "books") == 0) {
			if (have_books) {
				return -EEXIST;
			}
			ret = parse_weread_books_array(&cursor, catalog, &book_count);
			have_books = ret == 0;
		} else {
			ret = json_skip_value(&cursor, 0U);
		}
		if (ret < 0) {
			return ret;
		}
		json_whitespace(&cursor);
		if (cursor.offset >= length) {
			return -EBADMSG;
		}
		const char separator = data[cursor.offset++];
		if (separator == '}') {
			break;
		}
		if (separator != ',') {
			return -EBADMSG;
		}
		if (json_require_next_value(&cursor, '}') < 0) {
			return -EBADMSG;
		}
	}
	json_whitespace(&cursor);
	if (cursor.offset != length || !have_version || !have_source ||
	    !have_synced_at || !have_total || !have_books ||
	    catalog->total < book_count) {
		return -EBADMSG;
	}
	return 0;
}

static int parse_task_state_object(struct json_cursor *cursor,
	struct papermono_task_state *state)
{
	bool have_id = false;
	bool have_done = false;

	memset(state, 0, sizeof(*state));
	json_whitespace(cursor);
	if (cursor->offset >= cursor->length ||
	    cursor->data[cursor->offset++] != '{') {
		return -EBADMSG;
	}
	for (;;) {
		char key[24];
		int ret;

		json_whitespace(cursor);
		if (cursor->offset < cursor->length &&
		    cursor->data[cursor->offset] == '}') {
			++cursor->offset;
			return have_id && have_done ? 0 : -EBADMSG;
		}
		ret = json_string(cursor, key, sizeof(key));
		json_whitespace(cursor);
		if (ret < 0 || cursor->offset >= cursor->length ||
		    cursor->data[cursor->offset++] != ':') {
			return -EBADMSG;
		}
		if (strcmp(key, "id") == 0) {
			ret = json_string(cursor, state->id, sizeof(state->id));
			have_id = ret == 0 && state->id[0] != '\0';
		} else if (strcmp(key, "done") == 0) {
			ret = json_boolean(cursor, &state->done);
			have_done = ret == 0;
		} else {
			ret = json_skip_value(cursor, 0U);
		}
		if (ret < 0) {
			return ret;
		}
		json_whitespace(cursor);
		if (cursor->offset >= cursor->length) {
			return -EBADMSG;
		}
		const char separator = cursor->data[cursor->offset++];
		if (separator == '}') {
			return have_id && have_done ? 0 : -EBADMSG;
		}
		if (separator != ',') {
			return -EBADMSG;
		}
		if (json_require_next_value(cursor, '}') < 0) {
			return -EBADMSG;
		}
	}
}

static int parse_note_state_object(struct json_cursor *cursor,
	struct papermono_note_state *state)
{
	bool have_id = false;
	bool have_favorite = false;
	bool have_read = false;

	memset(state, 0, sizeof(*state));
	json_whitespace(cursor);
	if (cursor->offset >= cursor->length ||
	    cursor->data[cursor->offset++] != '{') {
		return -EBADMSG;
	}
	for (;;) {
		char key[24];
		int ret;

		json_whitespace(cursor);
		if (cursor->offset < cursor->length &&
		    cursor->data[cursor->offset] == '}') {
			++cursor->offset;
			return have_id && have_favorite && have_read ? 0 : -EBADMSG;
		}
		ret = json_string(cursor, key, sizeof(key));
		json_whitespace(cursor);
		if (ret < 0 || cursor->offset >= cursor->length ||
		    cursor->data[cursor->offset++] != ':') {
			return -EBADMSG;
		}
		if (strcmp(key, "id") == 0) {
			ret = json_string(cursor, state->id, sizeof(state->id));
			have_id = ret == 0 && state->id[0] != '\0';
		} else if (strcmp(key, "favorite") == 0) {
			ret = json_boolean(cursor, &state->favorite);
			have_favorite = ret == 0;
		} else if (strcmp(key, "read") == 0) {
			ret = json_boolean(cursor, &state->read);
			have_read = ret == 0;
		} else {
			ret = json_skip_value(cursor, 0U);
		}
		if (ret < 0) {
			return ret;
		}
		json_whitespace(cursor);
		if (cursor->offset >= cursor->length) {
			return -EBADMSG;
		}
		const char separator = cursor->data[cursor->offset++];
		if (separator == '}') {
			return have_id && have_favorite && have_read ? 0 : -EBADMSG;
		}
		if (separator != ',') {
			return -EBADMSG;
		}
		if (json_require_next_value(cursor, '}') < 0) {
			return -EBADMSG;
		}
	}
}

static int parse_task_state_array(struct json_cursor *cursor,
	struct papermono_state_overlay *state)
{
	json_whitespace(cursor);
	if (cursor->offset >= cursor->length ||
	    cursor->data[cursor->offset++] != '[') {
		return -EBADMSG;
	}
	for (;;) {
		json_whitespace(cursor);
		if (cursor->offset < cursor->length &&
		    cursor->data[cursor->offset] == ']') {
			++cursor->offset;
			return 0;
		}
		if (state->task_count >= PAPERMONO_TASK_MAX) {
			return -E2BIG;
		}
		int ret = parse_task_state_object(cursor,
			&state->tasks[state->task_count]);
		if (ret < 0) {
			return ret;
		}
		++state->task_count;
		json_whitespace(cursor);
		if (cursor->offset >= cursor->length) {
			return -EBADMSG;
		}
		const char separator = cursor->data[cursor->offset++];
		if (separator == ']') {
			return 0;
		}
		if (separator != ',') {
			return -EBADMSG;
		}
		if (json_require_next_value(cursor, ']') < 0) {
			return -EBADMSG;
		}
	}
}

static int parse_note_state_array(struct json_cursor *cursor,
	struct papermono_state_overlay *state)
{
	json_whitespace(cursor);
	if (cursor->offset >= cursor->length ||
	    cursor->data[cursor->offset++] != '[') {
		return -EBADMSG;
	}
	for (;;) {
		json_whitespace(cursor);
		if (cursor->offset < cursor->length &&
		    cursor->data[cursor->offset] == ']') {
			++cursor->offset;
			return 0;
		}
		if (state->note_count >= PAPERMONO_NOTE_MAX) {
			return -E2BIG;
		}
		int ret = parse_note_state_object(cursor,
			&state->notes[state->note_count]);
		if (ret < 0) {
			return ret;
		}
		++state->note_count;
		json_whitespace(cursor);
		if (cursor->offset >= cursor->length) {
			return -EBADMSG;
		}
		const char separator = cursor->data[cursor->offset++];
		if (separator == ']') {
			return 0;
		}
		if (separator != ',') {
			return -EBADMSG;
		}
		if (json_require_next_value(cursor, ']') < 0) {
			return -EBADMSG;
		}
	}
}

int papermono_parse_state_json(const char *data, size_t length,
	struct papermono_state_overlay *state)
{
	struct json_cursor cursor = { .data = data, .length = length };
	bool have_version = false;
	bool have_tasks = false;
	bool have_notes = false;

	if (data == NULL || state == NULL) {
		return -EINVAL;
	}
	memset(state, 0, sizeof(*state));
	json_whitespace(&cursor);
	if (cursor.offset >= length || data[cursor.offset++] != '{') {
		return -EBADMSG;
	}
	for (;;) {
		char key[24];
		int ret;

		json_whitespace(&cursor);
		if (cursor.offset < length && data[cursor.offset] == '}') {
			++cursor.offset;
			break;
		}
		ret = json_string(&cursor, key, sizeof(key));
		json_whitespace(&cursor);
		if (ret < 0 || cursor.offset >= length ||
		    data[cursor.offset++] != ':') {
			return -EBADMSG;
		}
		if (strcmp(key, "version") == 0) {
			int version;
			ret = json_integer(&cursor, &version);
			have_version = ret == 0 && version == PAPERMONO_STATE_VERSION;
			if (ret == 0 && !have_version) {
				ret = -ENOTSUP;
			}
		} else if (strcmp(key, "tasks") == 0) {
			ret = parse_task_state_array(&cursor, state);
			have_tasks = ret == 0;
		} else if (strcmp(key, "notes") == 0) {
			ret = parse_note_state_array(&cursor, state);
			have_notes = ret == 0;
		} else {
			ret = json_skip_value(&cursor, 0U);
		}
		if (ret < 0) {
			return ret;
		}
		json_whitespace(&cursor);
		if (cursor.offset >= length) {
			return -EBADMSG;
		}
		const char separator = data[cursor.offset++];
		if (separator == '}') {
			break;
		}
		if (separator != ',') {
			return -EBADMSG;
		}
		if (json_require_next_value(&cursor, '}') < 0) {
			return -EBADMSG;
		}
	}
	json_whitespace(&cursor);
	return cursor.offset == length && have_version && have_tasks && have_notes ?
		0 : -EBADMSG;
}

static bool markdown_separator(const char *line, size_t length)
{
	char marker = '\0';
	size_t count = 0U;

	for (size_t index = 0U; index < length; ++index) {
		if (line[index] == ' ' || line[index] == '\t') {
			continue;
		}
		if (marker == '\0') {
			marker = line[index];
		}
		if (line[index] != marker || strchr("-*_", marker) == NULL) {
			return false;
		}
		++count;
	}
	return count >= 3U;
}

static void add_note_block(struct papermono_note *note,
	enum papermono_note_block_type type, uint8_t level, const char *text,
	size_t length)
{
	if (note->block_count >= PAPERMONO_NOTE_BLOCK_MAX) {
		note->truncated = true;
		return;
	}
	struct papermono_note_block *block = &note->blocks[note->block_count++];
	block->type = type;
	block->level = level;
	copy_text(block->text, sizeof(block->text), text, length);
}

int papermono_parse_markdown(const char *data, size_t length,
	struct papermono_note *note)
{
	size_t offset = 0U;

	if (data == NULL || note == NULL) {
		return -EINVAL;
	}
	memset(note, 0, sizeof(*note));
	while (offset < length) {
		const size_t start = offset;
		while (offset < length && data[offset] != '\r' && data[offset] != '\n') {
			++offset;
		}
		size_t line_length = offset - start;
		while (offset < length && (data[offset] == '\r' || data[offset] == '\n')) {
			++offset;
		}
		const char *line = data + start;
		while (line_length > 0U && (*line == ' ' || *line == '\t')) {
			++line;
			--line_length;
		}
		while (line_length > 0U &&
		       (line[line_length - 1U] == ' ' || line[line_length - 1U] == '\t')) {
			--line_length;
		}
		if (line_length == 0U) {
			continue;
		}
		uint8_t heading = 0U;
		while (heading < line_length && heading < 6U && line[heading] == '#') {
			++heading;
		}
		if (heading > 0U && heading < line_length && line[heading] == ' ') {
			line += heading + 1U;
			line_length -= heading + 1U;
			if (note->title[0] == '\0') {
				copy_text(note->title, sizeof(note->title), line, line_length);
			}
			add_note_block(note, PAPERMONO_NOTE_HEADING, heading, line,
				line_length);
		} else if (markdown_separator(line, line_length)) {
			add_note_block(note, PAPERMONO_NOTE_SEPARATOR, 0U, "", 0U);
		} else if (line[0] == '>' && line_length > 1U) {
			const size_t skip = line[1] == ' ' ? 2U : 1U;
			add_note_block(note, PAPERMONO_NOTE_QUOTE, 0U, line + skip,
				line_length - skip);
		} else if (line_length > 2U && strchr("-*+", line[0]) != NULL &&
			   line[1] == ' ') {
			add_note_block(note, PAPERMONO_NOTE_LIST_ITEM, 0U, line + 2U,
				line_length - 2U);
		} else {
			add_note_block(note, PAPERMONO_NOTE_PARAGRAPH, 0U, line,
				line_length);
		}
	}
	if (note->title[0] == '\0') {
		if (note->block_count > 0U) {
			copy_text(note->title, sizeof(note->title),
				note->blocks[0].text, strlen(note->blocks[0].text));
		} else {
			copy_text(note->title, sizeof(note->title), "空便签", 9U);
		}
	}
	return 0;
}
