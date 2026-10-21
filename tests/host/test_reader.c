/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/settings/settings.h>

#include <papermono/reader.h>
#include <papermono/settings.h>

static const uint8_t *active_data;
static size_t active_length;
static struct papermono_reader_cache_entry cache_entries[128];
static size_t cache_entry_count;
static const struct papermono_preferences preferences = {
	.reader_density = PAPERMONO_READER_DENSITY_STANDARD,
};

ssize_t fs_read(struct fs_file_t *file, void *buffer, size_t length)
{
	const size_t remaining = file->offset < active_length ?
		active_length - file->offset : 0U;
	const size_t copied = remaining < length ? remaining : length;

	memcpy(buffer, active_data + file->offset, copied);
	file->offset += copied;
	return (ssize_t)copied;
}

int fs_seek(struct fs_file_t *file, off_t offset, int whence)
{
	if (whence != FS_SEEK_SET || offset < 0 || (size_t)offset > active_length) {
		return -EINVAL;
	}
	file->offset = (size_t)offset;
	return 0;
}

int fs_close(struct fs_file_t *file)
{
	(void)file;
	return 0;
}

int papermono_content_store_open(const struct papermono_content_entry *entry,
	struct fs_file_t *file)
{
	(void)entry;
	file->offset = 0U;
	return 0;
}

int papermono_content_store_scan_books(papermono_content_scan_cb callback,
	void *context, size_t *count)
{
	(void)callback;
	(void)context;
	if (count != NULL) {
		*count = 0U;
	}
	return 0;
}

/* Controllable EPUB stand-in. Progress weighting only depends on chapter
 * count, chapter sizes and the reported text_end, so driving those is enough
 * to exercise papermono_reader_read_page's EPUB branch.
 */
static const uint32_t *fake_chapter_sizes;
static uint16_t fake_chapter_count;
static uint32_t fake_text_end;
static bool fake_chapter_eof;

int papermono_epub_open(struct papermono_epub_book *epub,
	const struct papermono_content_entry *source)
{
	(void)source;
	epub->chapter_count = fake_chapter_count;
	for (uint16_t index = 0U; index < fake_chapter_count; ++index) {
		epub->chapters[index].size = fake_chapter_sizes[index];
	}
	return 0;
}

int papermono_epub_read_page(struct papermono_epub_book *epub,
	uint16_t chapter, uint32_t text_start, char *output, size_t output_size,
	uint8_t columns, uint8_t lines, struct papermono_epub_page *page)
{
	(void)output_size;
	(void)columns;
	(void)lines;
	if (chapter >= epub->chapter_count) {
		return -EINVAL;
	}
	/* The real implementation no longer writes chapters[].size here: that field
	 * now holds a codepoint total learned at end of chapter by the reader.
	 */
	output[0] = '\0';
	*page = (struct papermono_epub_page) {
		.text_start = text_start,
		.text_end = fake_text_end,
		.chapter_eof = fake_chapter_eof,
	};
	return 0;
}

int settings_save_one(const char *key, const void *value, size_t length)
{
	(void)key;
	(void)value;
	(void)length;
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

const struct papermono_preferences *papermono_settings_get(void)
{
	return &preferences;
}

int papermono_reader_cache_open(struct papermono_reader_cache *cache,
	const struct papermono_reader_cache_key *key,
	const struct papermono_reader_cache_entry *initial)
{
	return papermono_reader_cache_reset(cache, key, initial);
}

int papermono_reader_cache_reset(struct papermono_reader_cache *cache,
	const struct papermono_reader_cache_key *key,
	const struct papermono_reader_cache_entry *initial)
{
	cache->key = *key;
	cache->ready = true;
	cache->count = 1U;
	cache_entries[0] = *initial;
	cache_entry_count = 1U;
	return 0;
}

int papermono_reader_cache_record(struct papermono_reader_cache *cache,
	uint32_t index, const struct papermono_reader_cache_entry *entry)
{
	if (!cache->ready || index > cache_entry_count ||
	    index >= sizeof(cache_entries) / sizeof(cache_entries[0])) {
		return -EINVAL;
	}
	cache_entries[index] = *entry;
	cache_entry_count = index + 1U;
	cache->count = (uint32_t)cache_entry_count;
	return 0;
}

int papermono_reader_cache_read(const struct papermono_reader_cache *cache,
	uint32_t index, struct papermono_reader_cache_entry *entry)
{
	if (!cache->ready || index >= cache_entry_count) {
		return -EINVAL;
	}
	*entry = cache_entries[index];
	return 0;
}

int papermono_reader_cache_find(const struct papermono_reader_cache *cache,
	const struct papermono_reader_cache_entry *entry, uint32_t *index)
{
	if (!cache->ready) {
		return -EINVAL;
	}
	for (size_t current = 0U; current < cache_entry_count; ++current) {
		if (cache_entries[current].offset == entry->offset &&
		    cache_entries[current].chapter == entry->chapter) {
			*index = (uint32_t)current;
			return 0;
		}
	}
	return -ENOENT;
}

bool papermono_reader_cache_matches(const struct papermono_reader_cache *cache,
	const struct papermono_reader_cache_key *key)
{
	return cache->ready && cache->key.encoding == key->encoding &&
		cache->key.density == key->density &&
		cache->key.columns == key->columns && cache->key.lines == key->lines;
}

void papermono_reader_cache_disable(struct papermono_reader_cache *cache)
{
	cache->ready = false;
	cache->count = 0U;
}

static struct papermono_reader_session make_session(
	enum papermono_text_encoding encoding, size_t length)
{
	struct papermono_reader_session session = {
		.book = {
			.kind = PAPERMONO_CONTENT_TXT,
			.size = length,
		},
		.encoding = encoding,
	};
	(void)snprintf(session.book.id, sizeof(session.book.id),
		"0123456789abcdef");
	return session;
}

static void test_utf8_paging_and_history(void)
{
	static uint8_t text[1400];
	static const char prefix[] = "第一行";

	memcpy(text, prefix, sizeof(prefix) - 1U);
	for (size_t index = sizeof(prefix) - 1U; index < sizeof(text); ++index) {
		text[index] = index % 40U == 39U ? '\n' :
			(uint8_t)('a' + index % 26U);
	}
	struct papermono_reader_session session = make_session(
		PAPERMONO_TEXT_ENCODING_UTF8, sizeof(text));
	struct papermono_reader_page page;
	char output[512];

	active_data = text;
	active_length = sizeof(text);
	assert(papermono_reader_read_page(&session, output, sizeof(output),
		&page) == 0);
	assert(strstr(output, "第一行") != NULL);
	assert(strchr(output, '\n') != NULL);
	assert(page.byte_end > page.byte_start);
	assert(session.next_page_start == page.byte_end);
	assert(papermono_reader_next(&session) == 0);
	assert(session.history_count == 1U);
	assert(papermono_reader_previous(&session) == 0);
	assert(session.page_start == 0U);
}

static void test_gbk_boundary_and_empty_file(void)
{
	static const uint8_t gbk[] = {
		0xD6U, 0xD0U, 0xCEU, 0xC4U, '\n',
		0xD6U, 0xD0U, 0xCEU, 0xC4U,
	};
	struct papermono_reader_session session = make_session(
		PAPERMONO_TEXT_ENCODING_GBK, sizeof(gbk));
	struct papermono_reader_page page;
	char output[128];

	active_data = gbk;
	active_length = sizeof(gbk);
	assert(papermono_reader_read_page(&session, output, sizeof(output),
		&page) == 0);
	assert(strcmp(output, "中文\n中文") == 0);
	assert(page.eof);

	active_data = (const uint8_t *)"";
	active_length = 0U;
	session = make_session(PAPERMONO_TEXT_ENCODING_UTF8, 0U);
	assert(papermono_reader_read_page(&session, output, sizeof(output),
		&page) == 0);
	assert(output[0] == '\0');
	assert(page.eof && page.approximate_percent == 100U);
}

static void test_cached_previous_beyond_ram_history(void)
{
	static uint8_t text[50000];
	struct papermono_reader_session session;
	struct papermono_reader_page page;
	char output[1024];

	for (size_t index = 0U; index < sizeof(text); ++index) {
		text[index] = (uint8_t)('a' + index % 26U);
	}
	active_data = text;
	active_length = sizeof(text);
	session = make_session(PAPERMONO_TEXT_ENCODING_UTF8, sizeof(text));
	struct papermono_reader_cache_key key = {
		.book_size = sizeof(text),
		.kind = PAPERMONO_CONTENT_TXT,
		.encoding = PAPERMONO_TEXT_ENCODING_UTF8,
		.density = PAPERMONO_READER_DENSITY_STANDARD,
		.columns = PAPERMONO_READER_COLUMNS,
		.lines = PAPERMONO_READER_LINES,
	};
	(void)snprintf(key.id, sizeof(key.id), "%s", session.book.id);
	const struct papermono_reader_cache_entry initial = { 0 };
	assert(papermono_reader_cache_reset(&session.cache, &key, &initial) == 0);

	for (size_t page_index = 0U; page_index < 40U; ++page_index) {
		assert(papermono_reader_read_page(&session, output, sizeof(output),
			&page) == 0);
		assert(papermono_reader_next(&session) == 0);
	}
	assert(session.page_number == 40U);
	assert(session.history_count == PAPERMONO_READER_HISTORY_DEPTH);
	for (size_t page_index = 0U; page_index < 35U; ++page_index) {
		assert(papermono_reader_previous(&session) == 0);
	}
	assert(session.page_number == 5U);
	assert(session.page_start == cache_entries[5].offset);
}

static size_t append_codepoint(uint8_t *text, size_t offset, uint32_t codepoint)
{
	char encoded[4];
	const size_t length = papermono_text_encode_utf8(codepoint, encoded);

	assert(length > 0U);
	memcpy(text + offset, encoded, length);
	return offset + length;
}

static void test_chinese_punctuation_wrapping(void)
{
	uint8_t text[256];
	struct papermono_reader_session session;
	struct papermono_reader_page page;
	char output[512];
	size_t length = 0U;

	/* An opening quote in the last regular cell moves to the next line. */
	for (size_t index = 0U; index < PAPERMONO_READER_COLUMNS - 1U; ++index) {
		length = append_codepoint(text, length, 0x4E2DU);
	}
	length = append_codepoint(text, length, 0x300CU);
	length = append_codepoint(text, length, 0x6587U);
	active_data = text;
	active_length = length;
	session = make_session(PAPERMONO_TEXT_ENCODING_UTF8, length);
	assert(papermono_reader_read_page(&session, output, sizeof(output),
		&page) == 0);
	assert(strstr(output, "\n「文") != NULL);

	/* A closing full stop uses the spare margin, then the next glyph wraps. */
	length = 0U;
	for (size_t index = 0U; index < PAPERMONO_READER_COLUMNS; ++index) {
		length = append_codepoint(text, length, 0x4E2DU);
	}
	length = append_codepoint(text, length, 0x3002U);
	length = append_codepoint(text, length, 0x6587U);
	active_data = text;
	active_length = length;
	session = make_session(PAPERMONO_TEXT_ENCODING_UTF8, length);
	assert(papermono_reader_read_page(&session, output, sizeof(output),
		&page) == 0);
	assert(strstr(output, "。\n文") != NULL);
}

/* Progress must follow content volume, not chapter ordinal: the old formula
 * reported 50% for any position inside a single chapter book, and treated a
 * one page preface the same as a 500 page chapter.
 */
static void test_epub_progress_weighted_by_size(void)
{
	static const uint32_t single[] = { 1000U };
	static const uint32_t lopsided[] = { 40U, 4000U };
	struct papermono_reader_page page;
	char output[64];

	/* Single chapter: 100 of 1000 bytes read is 10%, not 50%. */
	struct papermono_content_entry book = {
		.kind = PAPERMONO_CONTENT_EPUB,
		.size = 4096U,
	};
	struct papermono_reader_session session;

	(void)snprintf(book.id, sizeof(book.id), "0123456789abcdef");
	fake_chapter_sizes = single;
	fake_chapter_count = 1U;
	memset(&session, 0, sizeof(session));
	assert(papermono_reader_open(&session, &book) == 0);
	fake_text_end = 100U;
	fake_chapter_eof = false;
	assert(papermono_reader_read_page(&session, output, sizeof(output),
		&page) == 0);
	assert(page.approximate_percent == 10U);

	/* Half way through the same chapter. */
	fake_text_end = 500U;
	assert(papermono_reader_read_page(&session, output, sizeof(output),
		&page) == 0);
	assert(page.approximate_percent == 50U);

	/* A short first chapter must not dominate: after finishing chapter 0
	 * (40 bytes of a 4040 byte book) progress stays near 1%, whereas
	 * counting chapters would have claimed 50%.
	 */
	fake_chapter_sizes = lopsided;
	fake_chapter_count = 2U;
	struct papermono_content_entry lopsided_book = {
		.kind = PAPERMONO_CONTENT_EPUB,
		.size = 8192U,
	};
	struct papermono_reader_session lopsided_session;

	(void)snprintf(lopsided_book.id, sizeof(lopsided_book.id),
		"0123456789abcdef");
	memset(&lopsided_session, 0, sizeof(lopsided_session));
	assert(papermono_reader_open(&lopsided_session, &lopsided_book) == 0);
	fake_text_end = 40U;
	fake_chapter_eof = true;
	assert(papermono_reader_read_page(&lopsided_session, output,
		sizeof(output), &page) == 0);
	assert(page.approximate_percent <= 2U);
}

/* Chapter lengths are CODEPOINT totals learned when a chapter ends, matching
 * the unit of text_end. Storing inflated bytes there instead made the progress
 * figure add codepoints to bytes and under-report for CJK text. Before any
 * length is known the figure must fall back to chapter ordinal rather than
 * claiming completion.
 */
static void test_epub_progress_learns_codepoints(void)
{
	static const uint32_t unknown[] = { 0U, 0U, 0U, 0U };
	struct papermono_content_entry book = {
		.kind = PAPERMONO_CONTENT_EPUB,
		.size = 4096U,
	};
	struct papermono_reader_session session;
	struct papermono_reader_page page;
	char output[64];

	(void)snprintf(book.id, sizeof(book.id), "0123456789abcdef");
	fake_chapter_sizes = unknown;
	fake_chapter_count = 4U;
	memset(&session, 0, sizeof(session));
	assert(papermono_reader_open(&session, &book) == 0);

	/* No length learned yet: chapter 0 of 4 must not read as 100%. */
	fake_text_end = 250U;
	fake_chapter_eof = false;
	assert(papermono_reader_read_page(&session, output, sizeof(output),
		&page) == 0);
	assert(page.approximate_percent == 0U);

	/* Reaching the end of chapter 0 records the codepoint total it was read
	 * with - the same unit as text_end. */
	fake_text_end = 700U;
	fake_chapter_eof = true;
	assert(papermono_reader_read_page(&session, output, sizeof(output),
		&page) == 0);
	assert(session.epub.chapters[0].size == 700U);
	/* One of four chapters of roughly equal estimated length. */
	assert(page.approximate_percent >= 20U);
	assert(page.approximate_percent <= 30U);
}

/* Bookmarks must be tellable apart, so each stores the opening of its page.
 * The snippet is cut at a byte limit, which must never leave a partial UTF-8
 * sequence behind.
 */
static void test_bookmark_preview(void)
{
	static uint8_t text[600];
	struct papermono_content_entry book = {
		.kind = PAPERMONO_CONTENT_TXT,
		.size = sizeof(text),
	};
	struct papermono_reader_session session;
	struct papermono_reader_page page;
	char output[512];

	/* A leading ASCII byte deliberately misaligns the 3 byte sequences so
	 * the size limit lands mid-character and the trim is actually needed.
	 */
	text[0] = 'A';
	for (size_t index = 1U; index + 3U <= sizeof(text); index += 3U) {
		text[index] = 0xE4U;      /* U+4E2D */
		text[index + 1U] = 0xB8U;
		text[index + 2U] = 0xADU;
	}
	active_data = text;
	active_length = sizeof(text);
	(void)snprintf(book.id, sizeof(book.id), "0123456789abcdef");
	memset(&session, 0, sizeof(session));
	assert(papermono_reader_open(&session, &book) == 0);
	assert(papermono_reader_read_page(&session, output, sizeof(output),
		&page) == 0);

	assert(papermono_reader_toggle_bookmark(&session, output) == 0);
	assert(session.bookmark_count == 1U);
	assert(session.bookmarks[0].preview[0] != '\0');
	assert(papermono_text_utf8_valid(
		(const uint8_t *)session.bookmarks[0].preview,
		strlen(session.bookmarks[0].preview), false));
	assert(strlen(session.bookmarks[0].preview) <
		PAPERMONO_READER_PREVIEW_MAX);

	/* Toggling the same page removes it again. */
	assert(papermono_reader_toggle_bookmark(&session, output) == 0);
	assert(session.bookmark_count == 0U);
}

int main(void)
{
	test_utf8_paging_and_history();
	test_gbk_boundary_and_empty_file();
	test_cached_previous_beyond_ram_history();
	test_chinese_punctuation_wrapping();
	test_epub_progress_weighted_by_size();
	test_epub_progress_learns_codepoints();
	test_bookmark_preview();
	puts("reader pagination tests: PASS");
	return 0;
}
