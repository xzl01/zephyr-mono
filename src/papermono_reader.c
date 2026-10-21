/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <papermono/fs_read.h>
#include <papermono/reader.h>
#include <papermono/settings.h>
#include <papermono/text_layout.h>

LOG_MODULE_REGISTER(papermono_reader, LOG_LEVEL_INF);

/* v4 stores a text preview with each bookmark, which changes the record
 * size. Records of a different length are ignored on load, so old state is
 * simply dropped rather than misread.
 */
#define READER_STATE_VERSION 4U
#define READER_INPUT_BUFFER_SIZE 1024U
#define READER_RECENT_VERSION 1U

static void reader_layout(uint8_t *columns, uint8_t *lines);

struct reader_recent_record {
	uint8_t version;
	uint8_t reserved[3];
	char id[PAPERMONO_CONTENT_ID_LENGTH + 1U];
};

struct reader_state_record {
	uint8_t version;
	uint8_t encoding;
	uint8_t bookmark_count;
	uint8_t reserved;
	uint32_t byte_offset;
	uint16_t chapter;
	uint16_t reserved2;
	struct papermono_reader_bookmark
		bookmarks[PAPERMONO_READER_BOOKMARK_LIMIT];
};

struct reader_state_load_context {
	struct reader_state_record record;
	bool found;
};

static int load_state_callback(const char *key, size_t length,
	settings_read_cb read_cb, void *cb_arg, void *param)
{
	struct reader_state_load_context *context = param;

	if ((key != NULL && key[0] != '\0') ||
	    length != sizeof(context->record)) {
		return 0;
	}
	if (read_cb(cb_arg, &context->record, sizeof(context->record)) ==
	    sizeof(context->record)) {
		context->found = true;
	}
	return 0;
}

static int state_key(const struct papermono_reader_session *session,
	char *key, size_t key_size)
{
	int ret = snprintf(key, key_size, "reader/%s", session->book.id);

	return ret < 0 || ret >= (int)key_size ? -ENAMETOOLONG : 0;
}

static int mark_recent(const struct papermono_content_entry *book)
{
	const struct reader_recent_record recent = {
		.version = READER_RECENT_VERSION,
	};
	struct reader_recent_record stored = recent;

	(void)snprintf(stored.id, sizeof(stored.id), "%s", book->id);
	return settings_save_one("reader/recent", &stored, sizeof(stored));
}

struct recent_load_context {
	struct reader_recent_record record;
	bool found;
};

static int load_recent_callback(const char *key, size_t length,
	settings_read_cb read_cb, void *cb_arg, void *param)
{
	struct recent_load_context *context = param;

	if ((key == NULL || key[0] == '\0') &&
	    length == sizeof(context->record) &&
	    read_cb(cb_arg, &context->record, sizeof(context->record)) ==
		    sizeof(context->record) &&
	    context->record.version == READER_RECENT_VERSION &&
	    strnlen(context->record.id, sizeof(context->record.id)) ==
		    PAPERMONO_CONTENT_ID_LENGTH) {
		context->found = true;
	}
	return 0;
}

struct recent_find_context {
	const char *id;
	struct papermono_content_entry *book;
	bool found;
};

static int find_recent_book(const struct papermono_content_entry *entry,
	void *param)
{
	struct recent_find_context *context = param;

	if (strcmp(entry->id, context->id) != 0) {
		return 0;
	}
	*context->book = *entry;
	context->found = true;
	return 1;
}

int papermono_reader_recent(struct papermono_content_entry *book)
{
	struct recent_load_context loaded = { 0 };
	int ret;

	if (book == NULL) {
		return -EINVAL;
	}
	ret = settings_load_subtree_direct("reader/recent",
		load_recent_callback, &loaded);
	if (ret < 0 || !loaded.found) {
		return ret < 0 ? ret : -ENOENT;
	}
	struct recent_find_context find = {
		.id = loaded.record.id,
		.book = book,
	};

	ret = papermono_content_store_scan_books(find_recent_book, &find, NULL);
	if (find.found) {
		return 0;
	}
	return ret < 0 ? ret : -ENOENT;
}

int papermono_reader_save(const struct papermono_reader_session *session)
{
	struct reader_state_record record;
	char key[32];
	int ret;

	if (session == NULL) {
		return -EINVAL;
	}
	ret = state_key(session, key, sizeof(key));
	if (ret < 0) {
		return ret;
	}
	record = (struct reader_state_record) {
		.version = READER_STATE_VERSION,
		.encoding = (uint8_t)session->encoding,
		.bookmark_count = session->bookmark_count,
		.byte_offset = session->page_start,
		.chapter = session->chapter,
	};
	memcpy(record.bookmarks, session->bookmarks,
		session->bookmark_count * sizeof(record.bookmarks[0]));
	return settings_save_one(key, &record, sizeof(record));
}

static int detect_encoding(const struct papermono_content_entry *book,
	enum papermono_text_encoding *encoding, size_t *bom_length)
{
	uint8_t sample[4096];
	struct fs_file_t file;
	ssize_t length;
	int ret;

	ret = papermono_content_store_open(book, &file);
	if (ret < 0) {
		return ret;
	}
	length = papermono_fs_read_chunked(&file, sample, sizeof(sample));
	(void)fs_close(&file);
	if (length < 0) {
		return (int)length;
	}
	*encoding = papermono_text_detect(sample, (size_t)length, bom_length);
	return *encoding == PAPERMONO_TEXT_ENCODING_UNKNOWN ? -EILSEQ : 0;
}

static void reader_cache_key(const struct papermono_reader_session *session,
	struct papermono_reader_cache_key *key)
{
	uint8_t columns;
	uint8_t lines;

	reader_layout(&columns, &lines);
	memset(key, 0, sizeof(*key));
	(void)snprintf(key->id, sizeof(key->id), "%s", session->book.id);
	key->book_size = session->book.size;
	key->book_modified = session->book.modified;
	key->kind = (uint8_t)session->book.kind;
	key->encoding = (uint8_t)session->encoding;
	key->density = (uint8_t)papermono_settings_get()->reader_density;
	key->columns = columns;
	key->lines = lines;
}

static struct papermono_reader_cache_entry reader_cache_entry(
	const struct papermono_reader_session *session)
{
	return (struct papermono_reader_cache_entry) {
		.offset = session->page_start,
		.chapter = session->chapter,
	};
}

static int reader_cache_rebase(struct papermono_reader_session *session)
{
	struct papermono_reader_cache_key key;
	const struct papermono_reader_cache_entry current =
		reader_cache_entry(session);
	int ret;

	reader_cache_key(session, &key);
	ret = papermono_reader_cache_reset(&session->cache, &key, &current);
	if (ret < 0) {
		papermono_reader_cache_disable(&session->cache);
	}
	session->page_number = 0U;
	return ret;
}

static int reader_cache_open_at_current(
	struct papermono_reader_session *session)
{
	struct papermono_reader_cache_key key;
	const struct papermono_reader_cache_entry current =
		reader_cache_entry(session);
	uint32_t index;
	int ret;

	reader_cache_key(session, &key);
	ret = papermono_reader_cache_open(&session->cache, &key, &current);
	if (ret == 0) {
		ret = papermono_reader_cache_find(&session->cache, &current, &index);
	}
	if (ret == 0) {
		session->page_number = index;
		return 0;
	}
	if (session->cache.ready) {
		ret = reader_cache_rebase(session);
	}
	if (ret < 0) {
		papermono_reader_cache_disable(&session->cache);
	}
	return ret;
}

static int reader_cache_ensure_layout(struct papermono_reader_session *session)
{
	struct papermono_reader_cache_key key;

	reader_cache_key(session, &key);
	if (papermono_reader_cache_matches(&session->cache, &key)) {
		return 0;
	}
	return reader_cache_rebase(session);
}

int papermono_reader_open(struct papermono_reader_session *session,
	const struct papermono_content_entry *book)
{
	struct reader_state_load_context loaded = { 0 };
	char key[32];
	size_t bom_length = 0U;
	int ret;

	if (session == NULL || book == NULL) {
		return -EINVAL;
	}
	if (book->kind != PAPERMONO_CONTENT_TXT &&
	    book->kind != PAPERMONO_CONTENT_EPUB) {
		return -ENOTSUP;
	}
	memset(session, 0, sizeof(*session));
	session->book = *book;
	if (book->kind == PAPERMONO_CONTENT_EPUB) {
		session->encoding = PAPERMONO_TEXT_ENCODING_UTF8;
		ret = papermono_epub_open(&session->epub, book);
	} else {
		ret = detect_encoding(book, &session->encoding, &bom_length);
	}
	if (ret < 0) {
		return ret;
	}
	session->page_start = (uint32_t)bom_length;
	ret = state_key(session, key, sizeof(key));
	if (ret == 0) {
		ret = settings_load_subtree_direct(key, load_state_callback, &loaded);
	}
	if (ret < 0) {
		LOG_WRN("Reader state load failed for %s (%d)", book->id, ret);
		ret = 0;
	}
	if (loaded.found && loaded.record.version == READER_STATE_VERSION &&
	    (book->kind == PAPERMONO_CONTENT_EPUB ||
	     loaded.record.byte_offset < book->size)) {
		session->page_start = MAX(loaded.record.byte_offset,
			(uint32_t)bom_length);
		session->bookmark_count = MIN(loaded.record.bookmark_count,
			PAPERMONO_READER_BOOKMARK_LIMIT);
		memcpy(session->bookmarks, loaded.record.bookmarks,
			session->bookmark_count * sizeof(session->bookmarks[0]));
		if (book->kind == PAPERMONO_CONTENT_EPUB &&
		    loaded.record.chapter < session->epub.chapter_count) {
			session->chapter = loaded.record.chapter;
		} else if (loaded.record.encoding == PAPERMONO_TEXT_ENCODING_UTF8 ||
		    loaded.record.encoding == PAPERMONO_TEXT_ENCODING_GBK) {
			session->encoding =
				(enum papermono_text_encoding)loaded.record.encoding;
		}
	}
	ret = reader_cache_open_at_current(session);
	if (ret < 0) {
		LOG_WRN("Reader page cache unavailable for %s (%d)", book->id, ret);
	}
	int recent_ret = mark_recent(book);
	if (recent_ret < 0) {
		LOG_WRN("Unable to persist recent book %s (%d)", book->id,
			recent_ret);
	}
	return 0;
}

static void reader_layout(uint8_t *columns, uint8_t *lines)
{
	const struct papermono_reader_layout layout = papermono_reader_layout_for(
		papermono_settings_get()->reader_density);

	*columns = layout.columns;
	*lines = layout.lines;
}

static int refill(struct fs_file_t *file, uint8_t *buffer, size_t *position,
	size_t *available, bool *eof)
{
	size_t remaining = *available - *position;
	ssize_t received;

	if (remaining > 0U) {
		memmove(buffer, buffer + *position, remaining);
	}
	*position = 0U;
	received = papermono_fs_read_chunked(file, buffer + remaining,
		READER_INPUT_BUFFER_SIZE - remaining);
	if (received < 0) {
		return (int)received;
	}
	*available = remaining + (size_t)received;
	*eof = received == 0;
	return 0;
}

/* EPUB progress weighted by chapter length instead of chapter count.
 *
 * Counting chapters treated a one page preface and a 500 page chapter as
 * equal, and left a single chapter book stuck at 50%.
 *
 * Units: text_end counts CODEPOINTS emitted from the inflated chapter, so a
 * chapter's learned length must count codepoints too. It is recorded as the
 * final text_end when the end of that chapter is reached (see
 * papermono_reader_read_page); chapters not yet opened are estimated from the
 * mean of the known ones and their estimate caps the current chapter's partial
 * contribution, so progress never over-reports. Before any chapter length is
 * known the figure falls back to chapter ordinal.
 */
static uint8_t epub_progress_percent(
	const struct papermono_reader_session *session, uint32_t text_end)
{
	uint64_t known_sum = 0U;
	uint16_t known_count = 0U;
	uint64_t total = 0U;
	uint64_t done = 0U;
	uint32_t mean;

	for (uint16_t i = 0U; i < session->epub.chapter_count; ++i) {
		const uint32_t size = session->epub.chapters[i].size;

		if (size != 0U) {
			known_sum += size;
			++known_count;
		}
	}
	mean = known_count == 0U ? 0U : (uint32_t)(known_sum / known_count);

	for (uint16_t i = 0U; i < session->epub.chapter_count; ++i) {
		uint32_t size = session->epub.chapters[i].size;

		if (size == 0U) {
			size = mean;
		}
		if (i < session->chapter) {
			done += size;
		} else if (i == session->chapter) {
			/* Cap by the chapter's own estimate: text_end can exceed it
			 * while the chapter length is still unknown.
			 */
			done += MIN(text_end, size);
		}
		total += size;
	}

	if (total == 0U) {
		/* No chapter length learned yet: weight chapters evenly. */
		if (session->epub.chapter_count == 0U) {
			return 100U;
		}
		return (uint8_t)MIN((session->chapter * 100U) /
			session->epub.chapter_count, 100U);
	}
	if (done > total) {
		done = total;
	}
	return (uint8_t)MIN((done * 100U) / total, 100U);
}

int papermono_reader_read_page(struct papermono_reader_session *session,
	char *output, size_t output_size, struct papermono_reader_page *page)
{
	if (session == NULL || output == NULL || output_size < 2U ||
	    page == NULL) {
		return -EINVAL;
	}
	if (reader_cache_ensure_layout(session) < 0) {
		papermono_reader_cache_disable(&session->cache);
	}
	if (session->book.kind == PAPERMONO_CONTENT_EPUB) {
		struct papermono_epub_page epub_page;
		uint8_t columns;
		uint8_t lines;
		int epub_ret;

		reader_layout(&columns, &lines);
		epub_ret = papermono_epub_read_page(&session->epub,
			session->chapter, session->page_start, output, output_size,
			columns, lines, &epub_page);
		if (epub_ret < 0) {
			return epub_ret;
		}
		session->next_chapter = session->chapter;
		session->next_page_start = epub_page.text_end;
		if (epub_page.chapter_eof) {
			/* The end of a chapter reveals its length in the same unit as
			 * text_end (emitted codepoints). Recording it here is what lets
			 * the progress figure weight chapters by length.
			 */
			session->epub.chapters[session->chapter].size =
				epub_page.text_end;
		}
		if (epub_page.chapter_eof &&
		    session->chapter + 1U < session->epub.chapter_count) {
			session->next_chapter = session->chapter + 1U;
			session->next_page_start = 0U;
		}
		session->eof = epub_page.chapter_eof &&
			session->chapter + 1U >= session->epub.chapter_count;
		session->page_loaded = true;
		*page = (struct papermono_reader_page) {
			.byte_start = session->page_start,
			.byte_end = epub_page.text_end,
			.approximate_percent = epub_progress_percent(session,
				epub_page.text_end),
			.eof = session->eof,
		};
		return 0;
	}

	uint8_t input[READER_INPUT_BUFFER_SIZE];
	struct fs_file_t file;
	size_t position = 0U;
	size_t available = 0U;
	size_t output_length = 0U;
	uint32_t consumed_total = 0U;
	uint8_t column = 0U;
	uint8_t line = 0U;
	uint8_t max_columns;
	uint8_t max_lines;
	bool eof = false;
	bool previous_was_cr = false;
	int ret;

	reader_layout(&max_columns, &max_lines);
	ret = papermono_content_store_open(&session->book, &file);
	if (ret < 0) {
		return ret;
	}
	ret = fs_seek(&file, session->page_start, FS_SEEK_SET);
	if (ret < 0) {
		goto close;
	}
	ret = refill(&file, input, &position, &available, &eof);
	while (ret == 0 && (position < available || !eof) &&
	       line < max_lines) {
		struct papermono_decoded_codepoint decoded;
		char encoded[4];
		size_t encoded_length;

		ret = papermono_text_decode_one(session->encoding,
			input + position, available - position, &decoded);
		if (ret == -EAGAIN && !eof) {
			ret = refill(&file, input, &position, &available, &eof);
			continue;
		}
		if (ret == -EAGAIN && eof) {
			ret = 0;
			break;
		}
		if (ret == -EILSEQ) {
			decoded.codepoint = 0xFFFDU;
			decoded.consumed = 1U;
			ret = 0;
		}
		if (ret < 0) {
			break;
		}
		if (decoded.codepoint == '\r') {
			decoded.codepoint = '\n';
			previous_was_cr = true;
		} else if (decoded.codepoint == '\n' && previous_was_cr) {
			position += decoded.consumed;
			consumed_total += decoded.consumed;
			previous_was_cr = false;
			continue;
		} else {
			previous_was_cr = false;
		}
		if (papermono_text_should_wrap_before(decoded.codepoint, column,
			max_columns)) {
			if (line + 1U >= max_lines ||
			    output_length + 1U >= output_size) {
				break;
			}
			output[output_length++] = '\n';
			++line;
			column = 0U;
		}
		encoded_length = papermono_text_encode_utf8(decoded.codepoint,
			encoded);
		if (encoded_length == 0U ||
		    output_length + encoded_length >= output_size) {
			break;
		}
		memcpy(output + output_length, encoded, encoded_length);
		output_length += encoded_length;
		position += decoded.consumed;
		consumed_total += decoded.consumed;
		if (decoded.codepoint == '\n') {
			++line;
			column = 0U;
		} else {
			++column;
		}
		if (available - position < 4U && !eof) {
			ret = refill(&file, input, &position, &available, &eof);
		}
	}
	output[output_length] = '\0';
	session->next_page_start = session->page_start + consumed_total;
	session->next_chapter = 0U;
	session->eof = session->next_page_start >= session->book.size ||
		(consumed_total == 0U && eof);
	session->page_loaded = true;
	*page = (struct papermono_reader_page) {
		.byte_start = session->page_start,
		.byte_end = session->next_page_start,
		.approximate_percent = session->book.size == 0U ? 100U :
			(uint8_t)MIN((session->next_page_start * 100ULL) /
				session->book.size, 100ULL),
		.eof = session->eof,
	};
close:
	(void)fs_close(&file);
	return ret;
}

int papermono_reader_next(struct papermono_reader_session *session)
{
	struct papermono_reader_cache_entry next;

	if (session == NULL || !session->page_loaded || session->eof ||
	    (session->next_chapter == session->chapter &&
	     session->next_page_start <= session->page_start)) {
		return -ENOENT;
	}
	if (session->history_count == PAPERMONO_READER_HISTORY_DEPTH) {
		memmove(session->history, session->history + 1,
			(PAPERMONO_READER_HISTORY_DEPTH - 1U) *
				sizeof(session->history[0]));
		--session->history_count;
	}
	session->history[session->history_count++] =
		(struct papermono_reader_history_entry) {
			.offset = session->page_start,
			.chapter = session->chapter,
		};
	next = (struct papermono_reader_cache_entry) {
		.offset = session->next_page_start,
		.chapter = session->next_chapter,
	};
	if (session->cache.ready) {
		const int cache_ret = papermono_reader_cache_record(&session->cache,
			session->page_number + 1U, &next);

		if (cache_ret == 0) {
			++session->page_number;
		} else {
			papermono_reader_cache_disable(&session->cache);
		}
	}
	session->page_start = session->next_page_start;
	session->chapter = session->next_chapter;
	session->page_loaded = false;
	return papermono_reader_save(session);
}

int papermono_reader_previous(struct papermono_reader_session *session)
{
	struct papermono_reader_cache_entry cached;

	if (session == NULL) {
		return -EINVAL;
	}
	if (session->cache.ready && session->page_number > 0U) {
		const int cache_ret = papermono_reader_cache_read(&session->cache,
			session->page_number - 1U, &cached);

		if (cache_ret == 0) {
			--session->page_number;
			session->page_start = cached.offset;
			session->chapter = cached.chapter;
			if (session->history_count > 0U) {
				--session->history_count;
			}
			session->page_loaded = false;
			session->eof = false;
			return papermono_reader_save(session);
		}
		papermono_reader_cache_disable(&session->cache);
	}
	if (session->history_count == 0U) {
		return -ENOENT;
	}
	const struct papermono_reader_history_entry previous =
		session->history[--session->history_count];

	session->page_start = previous.offset;
	session->chapter = previous.chapter;
	session->page_loaded = false;
	session->eof = false;
	return papermono_reader_save(session);
}

int papermono_reader_set_encoding(struct papermono_reader_session *session,
	enum papermono_text_encoding encoding)
{
	if (session == NULL || session->book.kind != PAPERMONO_CONTENT_TXT ||
	    (encoding != PAPERMONO_TEXT_ENCODING_UTF8 &&
	    encoding != PAPERMONO_TEXT_ENCODING_GBK)) {
		return -EINVAL;
	}
	size_t bom_length = 0U;
	enum papermono_text_encoding detected;

	session->encoding = encoding;
	if (detect_encoding(&session->book, &detected, &bom_length) < 0 ||
	    encoding != PAPERMONO_TEXT_ENCODING_UTF8) {
		bom_length = 0U;
	}
	session->page_start = (uint32_t)bom_length;
	session->history_count = 0U;
	session->page_loaded = false;
	session->eof = false;
	(void)reader_cache_rebase(session);
	return papermono_reader_save(session);
}

int papermono_reader_jump_to_chapter(struct papermono_reader_session *session,
	uint16_t chapter)
{
	if (session == NULL || session->book.kind != PAPERMONO_CONTENT_EPUB ||
	    chapter >= session->epub.chapter_count) {
		return -EINVAL;
	}
	session->chapter = chapter;
	session->page_start = 0U;
	session->history_count = 0U;
	session->page_loaded = false;
	session->eof = false;
	(void)reader_cache_rebase(session);
	return papermono_reader_save(session);
}

int papermono_reader_jump_to_bookmark(struct papermono_reader_session *session,
	uint8_t bookmark)
{
	if (session == NULL || bookmark >= session->bookmark_count) {
		return -EINVAL;
	}
	session->chapter = session->bookmarks[bookmark].chapter;
	session->page_start = session->bookmarks[bookmark].offset;
	session->history_count = 0U;
	session->page_loaded = false;
	session->eof = false;
	if (session->cache.ready) {
		const struct papermono_reader_cache_entry current =
			reader_cache_entry(session);
		uint32_t index;

		if (papermono_reader_cache_find(&session->cache, &current, &index) == 0) {
			session->page_number = index;
		} else {
			(void)reader_cache_rebase(session);
		}
	}
	return papermono_reader_save(session);
}

bool papermono_reader_is_bookmarked(
	const struct papermono_reader_session *session)
{
	if (session == NULL) {
		return false;
	}
	for (uint8_t index = 0U; index < session->bookmark_count; ++index) {
		if (session->bookmarks[index].chapter == session->chapter &&
		    session->bookmarks[index].offset == session->page_start) {
			return true;
		}
	}
	return false;
}

/* Snapshot the opening of the page so the bookmark list can show something
 * the reader recognises. Trims to whole UTF-8 sequences.
 */
static void copy_preview(char *dest, size_t size, const char *text)
{
	size_t start = 0U;
	size_t length = 0U;

	if (text == NULL || size == 0U) {
		return;
	}
	while (text[start] == ' ' || text[start] == '\t' ||
	       text[start] == '\n' || text[start] == '\r') {
		++start;
	}
	while (length + 1U < size && text[start + length] != '\0' &&
	       text[start + length] != '\n' && text[start + length] != '\r') {
		++length;
	}
	while (length > 0U &&
	       ((unsigned char)text[start + length] & 0xC0U) == 0x80U) {
		--length;
	}
	memcpy(dest, text + start, length);
	dest[length] = '\0';
}

int papermono_reader_toggle_bookmark(struct papermono_reader_session *session,
	const char *page_text)
{
	if (session == NULL) {
		return -EINVAL;
	}
	for (uint8_t index = 0U; index < session->bookmark_count; ++index) {
		if (session->bookmarks[index].chapter != session->chapter ||
		    session->bookmarks[index].offset != session->page_start) {
			continue;
		}
		memmove(&session->bookmarks[index], &session->bookmarks[index + 1U],
			(session->bookmark_count - index - 1U) *
				sizeof(session->bookmarks[0]));
		--session->bookmark_count;
		return papermono_reader_save(session);
	}
	if (session->bookmark_count == PAPERMONO_READER_BOOKMARK_LIMIT) {
		memmove(session->bookmarks, session->bookmarks + 1,
			(PAPERMONO_READER_BOOKMARK_LIMIT - 1U) *
				sizeof(session->bookmarks[0]));
		--session->bookmark_count;
	}
	session->bookmarks[session->bookmark_count] =
		(struct papermono_reader_bookmark) {
			.offset = session->page_start,
			.chapter = session->chapter,
		};
	copy_preview(session->bookmarks[session->bookmark_count].preview,
		sizeof(session->bookmarks[0].preview), page_text);
	++session->bookmark_count;
	return papermono_reader_save(session);
}
