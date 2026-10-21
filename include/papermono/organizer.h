/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_ORGANIZER_H_
#define PAPERMONO_ORGANIZER_H_

#include <stdbool.h>
#include <stddef.h>

#include <papermono/personal_data.h>

#ifdef __cplusplus
extern "C" {
#endif

int papermono_organizer_init(void);
int papermono_organizer_reload(void);

size_t papermono_organizer_calendar_count(void);
int papermono_organizer_get_event(size_t index,
	struct papermono_calendar_event *event);
int papermono_organizer_get_next_event(
	const struct papermono_calendar_time *now,
	struct papermono_calendar_event *event);
int papermono_organizer_find_upcoming_index(
	const struct papermono_calendar_time *now, size_t *index);
size_t papermono_organizer_task_count(void);
int papermono_organizer_get_task(size_t index, struct papermono_task *task);
int papermono_organizer_toggle_task(size_t index);
size_t papermono_organizer_note_count(void);
int papermono_organizer_get_note(size_t index, struct papermono_note *note,
	bool *favorite, bool *read);
int papermono_organizer_set_note_favorite(size_t index, bool favorite);
int papermono_organizer_set_note_read(size_t index, bool read);

int papermono_organizer_export_state_json(char *buffer, size_t capacity);
int papermono_organizer_import_state_json(char *buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_ORGANIZER_H_ */
