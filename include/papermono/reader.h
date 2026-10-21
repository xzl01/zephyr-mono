/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_READER_H_
#define PAPERMONO_READER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <papermono/content_store.h>
#include <papermono/epub.h>
#include <papermono/reader_cache.h>
#include <papermono/reader_layout.h>
#include <papermono/text_codec.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAPERMONO_READER_HISTORY_DEPTH 32U
#define PAPERMONO_READER_BOOKMARK_LIMIT 8U
#define PAPERMONO_READER_COLUMNS 16U
#define PAPERMONO_READER_LINES 16U

struct papermono_reader_history_entry {
	uint32_t offset;
	uint16_t chapter;
};

/* Bookmarks carry a snippet of the page they mark. Without it the list only
 * offers "书签 1 ... 书签 8" and a byte offset, which tells the reader
 * nothing. Kept separate from the page-history entry so the 32 entry back
 * stack does not pay for the text.
 */
#define PAPERMONO_READER_PREVIEW_MAX 28U

struct papermono_reader_bookmark {
	uint32_t offset;
	uint16_t chapter;
	char preview[PAPERMONO_READER_PREVIEW_MAX];
};

struct papermono_reader_session {
	struct papermono_content_entry book;
	struct papermono_epub_book epub;
	enum papermono_text_encoding encoding;
	uint32_t page_start;
	uint32_t next_page_start;
	struct papermono_reader_history_entry
		history[PAPERMONO_READER_HISTORY_DEPTH];
	struct papermono_reader_bookmark
		bookmarks[PAPERMONO_READER_BOOKMARK_LIMIT];
	struct papermono_reader_cache cache;
	uint32_t page_number;
	uint16_t chapter;
	uint16_t next_chapter;
	uint8_t history_count;
	uint8_t bookmark_count;
	bool page_loaded;
	bool eof;
};

struct papermono_reader_page {
	uint32_t byte_start;
	uint32_t byte_end;
	uint8_t approximate_percent;
	bool eof;
};

int papermono_reader_open(struct papermono_reader_session *session,
	const struct papermono_content_entry *book);
int papermono_reader_read_page(struct papermono_reader_session *session,
	char *output, size_t output_size, struct papermono_reader_page *page);
int papermono_reader_next(struct papermono_reader_session *session);
int papermono_reader_previous(struct papermono_reader_session *session);
int papermono_reader_set_encoding(struct papermono_reader_session *session,
	enum papermono_text_encoding encoding);
int papermono_reader_jump_to_chapter(struct papermono_reader_session *session,
	uint16_t chapter);
int papermono_reader_jump_to_bookmark(struct papermono_reader_session *session,
	uint8_t bookmark);
bool papermono_reader_is_bookmarked(
	const struct papermono_reader_session *session);
int papermono_reader_toggle_bookmark(struct papermono_reader_session *session,
	const char *page_text);
int papermono_reader_save(const struct papermono_reader_session *session);
int papermono_reader_recent(struct papermono_content_entry *book);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_READER_H_ */
