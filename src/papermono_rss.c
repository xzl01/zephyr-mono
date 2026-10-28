/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * RSS 2.0 reader data layer: an SD-card cache plus an on-demand fetcher.
 *
 * The RAM-resident snapshot only carries the index (feed title, item titles,
 * publication dates and body lengths). Full article bodies are written to
 * /SD:/papermono/rss/bodies.bin by the fetcher and read back one article at a
 * time by the UI through papermono_rss_body_load(), so the news page never
 * keeps a whole feed in RAM.
 *
 * The XML scanner is dependency free and shared with tests/host/test_rss.c; the
 * firmware adds the HTTP client, the SD cache and the worker thread on top.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <papermono/rss.h>
#include <papermono/rss_table.h>

#if defined(__ZEPHYR__)
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/fs/fs.h>

#include <papermono/content_store.h>
#include <papermono/fs_read.h>
#include <papermono/settings.h>
#include <papermono/ui.h>

LOG_MODULE_REGISTER(papermono_rss, LOG_LEVEL_INF);
#define RSS_LOG_WRN(...) LOG_WRN(__VA_ARGS__)
#define RSS_LOG_INF(...) LOG_INF(__VA_ARGS__)
#else
#include <stdio.h>

#define RSS_LOG_WRN(...) ((void)0)
#define RSS_LOG_INF(...) ((void)0)
#endif

/* Largest feed body / all bodies kept on the SD card, per the public header. */
#define RSS_BODY_LIMIT ((size_t)PAPERMONO_RSS_BODY_MAX)
#define RSS_BODIES_TOTAL_MAX (256U * 1024U)
/* Every news-page open checks the source; the SD cache remains readable while
 * that request runs. There is no background polling or cache-age gate.
 */
#define RSS_FETCH_ATTEMPTS 3U
#define RSS_FETCH_RETRY_MS 20000U
#define RSS_IPV4_WAIT_MS 30000U
/* The feed is still held in PSRAM while it is parsed. 128 KB keeps a feed with
 * a few 10 KB digests intact; the article bodies themselves go to the SD card.
 */
#define RSS_RESPONSE_MAX (128U * 1024U)
#define RSS_WRITE_BOUNCE 512U
#define RSS_THREAD_STACK_SIZE 8192U
#define RSS_HOST_MAX 63U
#define RSS_PATH_MAX 191U
#define RSS_CACHE_PATH_MAX 192U

#define RSS_INDEX_FILE "index.bin"
#define RSS_INDEX_TMP_FILE "index.tmp"
#define RSS_INDEX_BAK_FILE "index.bak"
#define RSS_BODIES_FILE "bodies.bin"
#define RSS_BODIES_TMP_FILE "bodies.tmp"
#define RSS_BODIES_BAK_FILE "bodies.bak"

#define RSS_INDEX_MAGIC 0x52535343UL /* "RSSC" */
#define RSS_INDEX_VERSION 2U

#if defined(__ZEPHYR__) && defined(CONFIG_ESP_SPIRAM)
#define RSS_PSRAM __attribute__((section(".ext_ram.bss")))
#else
#define RSS_PSRAM
#endif

#define RSS_MIN(a, b) ((a) < (b) ? (a) : (b))

/*
 * SD cache index format. The record is written as one block so a torn write is
 * detected by the magic/version check instead of being half applied.
 */
struct rss_index_record_disk {
	char title[PAPERMONO_RSS_TITLE_MAX + 1U];
	char published[PAPERMONO_RSS_DATE_MAX + 1U];
	uint32_t body_length;
};

struct rss_index_disk {
	uint32_t magic;
	uint16_t version;
	uint16_t count;
	int64_t updated_at;
	char title[PAPERMONO_RSS_TITLE_MAX + 1U];
	struct rss_index_record_disk items[PAPERMONO_RSS_ITEM_MAX];
};

_Static_assert(sizeof(struct rss_index_disk) == 2112U,
	       "RSS index block layout changed");

/* ------------------------------------------------------------------------- */
/* Portable XML scanner                                                       */
/* ------------------------------------------------------------------------- */

struct rss_raw_field {
	const char *text;
	size_t length;
	bool literal;
};

static bool rss_is_space(char ch)
{
	return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
	       ch == '\f' || ch == '\v';
}

static const char *rss_mem_find(const char *start, const char *end,
				const char *needle, size_t needle_len)
{
	if (needle_len == 0U || end < start + needle_len) {
		return NULL;
	}
	for (const char *p = start; p + needle_len <= end; ++p) {
		if (memcmp(p, needle, needle_len) == 0) {
			return p;
		}
	}
	return NULL;
}

/* True when the element name starting at @p is followed by a tag delimiter. */
static bool rss_name_ends(const char *p, const char *end, size_t name_len)
{
	const char *after = p + name_len;

	if (after >= end) {
		return true;
	}
	const char ch = *after;

	return ch == '>' || ch == '/' || ch == ' ' || ch == '\t' ||
	       ch == '\n' || ch == '\r';
}

/*
 * When @p points at a comment, CDATA section, declaration, processing
 * instruction or closing tag, return the position just after the construct.
 * Returns NULL when @p does not start a construct, and @p end when a construct
 * opener is not terminated (the caller then stops scanning).
 */
static const char *rss_skip_construct(const char *p, const char *end)
{
	const char *gt;
	const char *close;

	if (end - p < 2) {
		return NULL;
	}
	if (p[1] != '/' && p[1] != '!' && p[1] != '?') {
		return NULL;
	}
	if (end - p >= 4 && memcmp(p, "<!--", 4U) == 0) {
		close = rss_mem_find(p + 4, end, "-->", 3U);
		return close != NULL ? close + 3 : end;
	}
	if (end - p >= 9 && memcmp(p, "<![CDATA[", 9U) == 0) {
		close = rss_mem_find(p + 9, end, "]]>", 3U);
		return close != NULL ? close + 3 : end;
	}
	gt = memchr(p, '>', (size_t)(end - p));
	return gt != NULL ? gt + 1 : end;
}

/*
 * Return the first byte of content of the next <name ...> opening tag in
 * [start, end), or NULL when there is none. Self-closing tags are reported
 * through @p self_closed and yield no content.
 */
static const char *rss_xml_find_open(const char *start, const char *end,
				     const char *name, size_t name_len,
				     bool *self_closed)
{
	const char *p = start;

	if (self_closed != NULL) {
		*self_closed = false;
	}
	while (p < end) {
		const char *lt = memchr(p, '<', (size_t)(end - p));
		const char *skip;

		if (lt == NULL) {
			return NULL;
		}
		p = lt;
		skip = rss_skip_construct(p, end);
		if (skip != NULL) {
			if (skip >= end) {
				return NULL;
			}
			p = skip;
			continue;
		}
		if (end - p < 2) {
			return NULL;
		}
		if ((size_t)(end - p - 1) >= name_len &&
		    memcmp(p + 1, name, name_len) == 0 &&
		    rss_name_ends(p + 1, end, name_len)) {
			const char *gt = memchr(p, '>', (size_t)(end - p));

			if (gt == NULL) {
				return NULL;
			}
			if (self_closed != NULL && gt > p && gt[-1] == '/') {
				*self_closed = true;
			}
			return gt + 1;
		}
		p = lt + 1;
	}
	return NULL;
}

/* Return a pointer to the '<' of the next </name> tag in [start, end). */
static const char *rss_xml_find_close(const char *start, const char *end,
				      const char *name, size_t name_len)
{
	const char *p = start;

	while (p < end) {
		const char *lt = memchr(p, '<', (size_t)(end - p));
		const char *skip;

		if (lt == NULL) {
			return NULL;
		}
		p = lt;
		if (end - p > 2 && (size_t)(end - p - 2) >= name_len &&
		    p[1] == '/' && memcmp(p + 2, name, name_len) == 0 &&
		    rss_name_ends(p + 2, end, name_len)) {
			return p;
		}
		skip = rss_skip_construct(p, end);
		if (skip != NULL) {
			if (skip >= end) {
				return NULL;
			}
			p = skip;
			continue;
		}
		p = lt + 1;
	}
	return NULL;
}

static size_t rss_utf8_encode(uint32_t code_point, char out[4])
{
	if (code_point == 0U || code_point > 0x10FFFFU ||
	    (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
		return 0U;
	}
	if (code_point < 0x80U) {
		out[0] = (char)code_point;
		return 1U;
	}
	if (code_point < 0x800U) {
		out[0] = (char)(0xC0U | (code_point >> 6));
		out[1] = (char)(0x80U | (code_point & 0x3FU));
		return 2U;
	}
	if (code_point < 0x10000U) {
		out[0] = (char)(0xE0U | (code_point >> 12));
		out[1] = (char)(0x80U | ((code_point >> 6) & 0x3FU));
		out[2] = (char)(0x80U | (code_point & 0x3FU));
		return 3U;
	}
	out[0] = (char)(0xF0U | (code_point >> 18));
	out[1] = (char)(0x80U | ((code_point >> 12) & 0x3FU));
	out[2] = (char)(0x80U | ((code_point >> 6) & 0x3FU));
	out[3] = (char)(0x80U | (code_point & 0x3FU));
	return 4U;
}

/*
 * Decode one XML/HTML entity at @p src. Returns the number of UTF-8 bytes
 * written to @p out (0 when the text is not a supported entity) and stores the
 * number of source bytes consumed in @p consumed.
 */
static size_t rss_decode_entity(const char *src, size_t avail, char out[4],
				size_t *consumed)
{
	static const struct {
		const char *name;
		char value;
	} named[] = {
		{ "amp", '&' },
		{ "lt", '<' },
		{ "gt", '>' },
		{ "quot", '"' },
		{ "apos", '\'' },
		{ "nbsp", ' ' },
	};
	const char *semicolon;
	size_t name_len;
	uint32_t code_point = 0U;
	uint32_t base = 10U;
	size_t index;

	*consumed = 0U;
	if (avail < 3U || src[0] != '&') {
		return 0U;
	}
	semicolon = memchr(src + 1, ';', avail - 1U);
	if (semicolon == NULL || semicolon == src + 1) {
		return 0U;
	}
	name_len = (size_t)(semicolon - (src + 1));
	if (name_len > 12U) {
		return 0U;
	}
	if (src[1] != '#') {
		for (size_t i = 0U; i < sizeof(named) / sizeof(named[0]); ++i) {
			if (strlen(named[i].name) == name_len &&
			    memcmp(named[i].name, src + 1, name_len) == 0) {
				out[0] = named[i].value;
				*consumed = name_len + 2U;
				return 1U;
			}
		}
		return 0U;
	}
	index = 2U;
	if (index < name_len + 1U &&
	    (src[index] == 'x' || src[index] == 'X')) {
		base = 16U;
		++index;
	}
	if (index >= name_len + 1U) {
		return 0U;
	}
	for (; index < name_len + 1U; ++index) {
		const char ch = src[index];
		uint32_t digit;

		if (ch >= '0' && ch <= '9') {
			digit = (uint32_t)(ch - '0');
		} else if (base == 16U && ch >= 'a' && ch <= 'f') {
			digit = (uint32_t)(ch - 'a') + 10U;
		} else if (base == 16U && ch >= 'A' && ch <= 'F') {
			digit = (uint32_t)(ch - 'A') + 10U;
		} else {
			return 0U;
		}
		if (code_point > 0x10FFFFU / base) {
			return 0U;
		}
		code_point = code_point * base + digit;
	}
	size_t written = rss_utf8_encode(code_point, out);

	if (written == 0U) {
		out[0] = '?';
		written = 1U;
	}
	*consumed = name_len + 2U;
	return written;
}

/*
 * Decoded-text output. `stored` counts bytes accepted by the consumer,
 * `total` counts every byte produced (so callers can tell that a body was
 * truncated), and `limit` bounds `stored` while keeping UTF-8 characters
 * whole: a character that does not fit is dropped together with the rest.
 */
struct rss_sink {
	int (*emit)(void *context, const char *data, size_t length);
	void *context;
	size_t stored;
	size_t total;
	size_t limit;
	bool overflow;
	bool failed;
};

static bool rss_sink_put(struct rss_sink *sink, const char *data, size_t length)
{
	if (sink == NULL || length == 0U) {
		return true;
	}
	sink->total += length;
	if (sink->overflow) {
		return true;
	}
	if (sink->stored + length > sink->limit) {
		sink->overflow = true;
		return true;
	}
	if (sink->emit != NULL &&
	    sink->emit(sink->context, data, length) != 0) {
		sink->failed = true;
		return false;
	}
	sink->stored += length;
	return true;
}

/*
 * Plain-text rendering for titles and publication dates: XML entities are
 * decoded, whitespace is folded to single spaces and the output is truncated
 * on a UTF-8 boundary. Article bodies go through the HTML-aware converter
 * below, which keeps block structure.
 */
struct rss_plain_writer {
	struct rss_sink *sink;
	bool pending_space;
	bool started;
};

static void rss_plain_put(struct rss_plain_writer *writer, const char *data,
			  size_t length)
{
	if (writer->pending_space) {
		if (writer->started) {
			(void)rss_sink_put(writer->sink, " ", 1U);
		}
		writer->pending_space = false;
	}
	(void)rss_sink_put(writer->sink, data, length);
	if (length > 0U) {
		writer->started = true;
	}
}

/* A source unit is accepted only when it is a complete Unicode scalar.
 * Invalid bytes are skipped individually by callers, so an ASCII delimiter
 * after a bad lead still reaches the HTML/control-character scanner.
 */
static size_t rss_utf8_scalar_length(const char *text, size_t available)
{
	if (available == 0U) {
		return 0U;
	}
	const unsigned char lead = (unsigned char)text[0];
	size_t length;

	if (lead < 0x80U) {
		return 1U;
	}
	if (lead >= 0xC2U && lead <= 0xDFU) {
		length = 2U;
	} else if (lead >= 0xE0U && lead <= 0xEFU) {
		length = 3U;
	} else if (lead >= 0xF0U && lead <= 0xF4U) {
		length = 4U;
	} else {
		return 0U;
	}
	if (length > available) {
		return 0U;
	}
	for (size_t i = 1U; i < length; ++i) {
		if (((unsigned char)text[i] & 0xC0U) != 0x80U) {
			return 0U;
		}
	}
	const unsigned char second = (unsigned char)text[1];

	if ((lead == 0xE0U && second < 0xA0U) ||
	    (lead == 0xEDU && second >= 0xA0U) ||
	    (lead == 0xF0U && second < 0x90U) ||
	    (lead == 0xF4U && second >= 0x90U)) {
		return 0U;
	}
	return length;
}

static void rss_render_title_text(const char *text, size_t length,
				  bool literal, struct rss_sink *sink)
{
	struct rss_plain_writer writer = {
		.sink = sink,
		.pending_space = false,
		.started = false,
	};
	size_t index = 0U;

	while (index < length) {
		if (!literal && text[index] == '&') {
			char decoded[4];
			size_t consumed = 0U;
			const size_t written =
				rss_decode_entity(text + index, length - index,
						  decoded, &consumed);

			if (written > 0U) {
				if (written == 1U) {
					if (rss_is_space(decoded[0])) {
						writer.pending_space = true;
					} else if ((unsigned char)decoded[0] >= 0x20U) {
						rss_plain_put(&writer, decoded, 1U);
					}
				} else {
					rss_plain_put(&writer, decoded, written);
				}
				index += consumed;
				continue;
			}
		}
		const unsigned char lead = (unsigned char)text[index];

		if (lead < 0x80U) {
			if (rss_is_space((char)lead)) {
				writer.pending_space = true;
			} else if (lead >= 0x20U) {
				rss_plain_put(&writer, text + index, 1U);
			}
			++index;
			continue;
		}
		const size_t sequence =
			rss_utf8_scalar_length(text + index, length - index);

		if (sequence == 0U) {
			++index;
			continue;
		}
		rss_plain_put(&writer, text + index, sequence);
		index += sequence;
	}
}

struct rss_buffer_sink {
	char *out;
	size_t written;
};

static int rss_buffer_emit(void *context, const char *data, size_t length)
{
	struct rss_buffer_sink *buffer = context;

	memcpy(buffer->out + buffer->written, data, length);
	buffer->written += length;
	return 0;
}

static size_t rss_render_title_to_buffer(const char *text, size_t length,
					 bool literal, char *out,
					 size_t capacity)
{
	struct rss_buffer_sink buffer = { .out = out, .written = 0U };
	struct rss_sink sink;

	if (capacity == 0U) {
		return 0U;
	}
	sink.emit = rss_buffer_emit;
	sink.context = &buffer;
	sink.stored = 0U;
	sink.total = 0U;
	sink.limit = capacity - 1U;
	sink.overflow = false;
	sink.failed = false;
	rss_render_title_text(text, length, literal, &sink);
	out[buffer.written] = '\0';
	return buffer.written;
}

/* ------------------------------------------------------------------------- */
/* HTML-aware body conversion                                                */
/* ------------------------------------------------------------------------- */

/* The device has no rich-text font, so the structure is encoded in plain text:
 * blank lines between paragraphs, headings on their own line, two spaces per
 * blockquote level, "· " bullets and shortened URLs.
 */

#define RSS_HTML_TAG_MAX 16U
#define RSS_HTML_ENTITY_MAX 16U
#define RSS_HTML_URL_MAX 320U
#define RSS_HTML_HEADING_MAX 192U
#define RSS_HTML_DISPLAY_MAX 48U
#define RSS_HTML_INDENT_MAX 3U
#define RSS_HTML_RAW_TAG_MAX 8U
#define RSS_HTML_ATTR_MAX 192U
#define RSS_HTML_TABLE_COLUMNS 32U
#define RSS_HTML_TABLE_ROWSPAN 64U

struct rss_line_writer {
	struct rss_sink *sink;
	unsigned int indent;
	unsigned int pending_breaks; /* 0, 1 or 2 newlines before next text */
	bool line_open;
	bool has_content;
	bool pending_space;
	bool last_cjk;
	size_t line_text;
};

static bool rss_unit_is_cjk(const char *data, size_t length)
{
	const unsigned char lead = (unsigned char)data[0];

	/* CJK Unified Ideographs (U+4E00..U+9FFF) start with E4..E9. The device
	 * only renders the README's CJK subset, so a lead-byte check is enough.
	 */
	return length >= 3U && lead >= 0xE4U && lead <= 0xE9U;
}

static char rss_lower_ascii(char ch)
{
	if (ch >= 'A' && ch <= 'Z') {
		return (char)(ch - 'A' + 'a');
	}
	return ch;
}

static bool rss_is_word_char(char ch)
{
	return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
	       (ch >= '0' && ch <= '9');
}

static bool rss_is_alpha(char ch)
{
	return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

static bool rss_memcmp_ci(const char *left, const char *right, size_t length)
{
	for (size_t i = 0U; i < length; ++i) {
		if (rss_lower_ascii(left[i]) != rss_lower_ascii(right[i])) {
			return false;
		}
	}
	return true;
}

static void rss_lw_flush(struct rss_line_writer *writer)
{
	const unsigned int breaks = writer->pending_breaks;

	writer->pending_breaks = 0U;
	if (breaks == 0U || !writer->has_content) {
		return;
	}
	for (unsigned int i = 0U; i < breaks; ++i) {
		(void)rss_sink_put(writer->sink, "\n", 1U);
	}
	writer->line_open = false;
	writer->line_text = 0U;
	writer->pending_space = false;
	writer->last_cjk = false;
}

static void rss_lw_begin(struct rss_line_writer *writer)
{
	if (writer->line_open) {
		return;
	}
	for (unsigned int i = 0U; i < writer->indent; ++i) {
		(void)rss_sink_put(writer->sink, "  ", 2U);
	}
	writer->line_open = true;
}

/* 1 = line break, 2 = paragraph break (one empty line). */
static void rss_lw_break(struct rss_line_writer *writer, unsigned int count)
{
	unsigned int wanted = count > 2U ? 2U : count;

	if (!writer->has_content || wanted == 0U) {
		return;
	}
	if (wanted > writer->pending_breaks) {
		writer->pending_breaks = wanted;
	}
}

static void rss_lw_space(struct rss_line_writer *writer)
{
	if (writer->line_open && writer->line_text > 0U) {
		writer->pending_space = true;
	}
}

static void rss_lw_text(struct rss_line_writer *writer, const char *data,
			size_t length)
{
	if (length == 0U) {
		return;
	}
	rss_lw_flush(writer);
	rss_lw_begin(writer);
	if (writer->pending_space && writer->line_text > 0U &&
	    !(writer->last_cjk && rss_unit_is_cjk(data, length))) {
		(void)rss_sink_put(writer->sink, " ", 1U);
	}
	writer->pending_space = false;
	(void)rss_sink_put(writer->sink, data, length);
	writer->line_text += length;
	writer->last_cjk = rss_unit_is_cjk(data, length);
	writer->has_content = true;
}

static void rss_lw_bullet(struct rss_line_writer *writer)
{
	rss_lw_flush(writer);
	rss_lw_begin(writer);
	(void)rss_sink_put(writer->sink, "\xC2\xB7", 2U); /* middle dot */
	/* The word separator is added when the item text arrives, so an empty
	 * list item does not leave a trailing space.
	 */
	writer->line_text = 2U;
	writer->pending_space = true;
	writer->last_cjk = false;
	writer->has_content = true;
}

enum rss_html_state {
	RSS_HTML_TEXT,
	RSS_HTML_LT,
	RSS_HTML_TAG_NAME,
	RSS_HTML_TAG_SKIP,
	RSS_HTML_DECL_START,
	RSS_HTML_DECL,
	RSS_HTML_COMMENT,
	RSS_HTML_RAW_TEXT,
	RSS_HTML_RAW_LT,
	RSS_HTML_RAW_NAME,
};

struct rss_html_render {
	struct rss_sink *sink;
	struct rss_line_writer line;
	const char *item_title;
	enum rss_html_state state;
	char name[RSS_HTML_TAG_MAX];
	size_t name_len;
	bool name_overflow;
	bool closing;
	bool self_closing;
	bool last_slash;
	char quote;
	unsigned int comment_dashes;
	char raw_tag[RSS_HTML_RAW_TAG_MAX];
	size_t raw_tag_len;
	size_t raw_pos;
	char entity[RSS_HTML_ENTITY_MAX];
	size_t entity_len;
	bool entity_active;
	char url[RSS_HTML_URL_MAX];
	size_t url_len;
	bool url_active;
	bool url_prefix_ok;
	bool url_discard;
	bool heading_candidate;
	bool heading_done;
	bool heading_live;
	char heading[RSS_HTML_HEADING_MAX];
	size_t heading_len;
	bool heading_pending_space;
	bool heading_overflow;
	bool prev_word_char;
	char attributes[RSS_HTML_ATTR_MAX];
	size_t attributes_length;
	bool attributes_overflow;
	unsigned int table_depth;
	unsigned int table_saved_indent;
	unsigned int table_column;
	unsigned int table_cell_start;
	unsigned int table_colspan;
	unsigned int table_rowspan;
	unsigned int table_plain_cells;
	uint8_t table_rowspans[RSS_HTML_TABLE_COLUMNS];
	bool table_grid_open;
	bool table_row_open;
	bool table_row_started;
	bool table_cell_open;
	bool table_plain;
	bool table_caption;
};

static void rss_html_feed_ascii(struct rss_html_render *render, char ch);
static void rss_html_text_ascii(struct rss_html_render *render, char ch);
static void rss_html_table_text_prepare(struct rss_html_render *render);
static void rss_html_entity_flush(struct rss_html_render *render);
static void rss_url_append(struct rss_html_render *render, const char *data,
			   size_t length);

static bool rss_tag_is(const char *name, size_t length, const char *literal)
{
	const size_t literal_len = strlen(literal);

	return length == literal_len && memcmp(name, literal, length) == 0;
}

static void rss_html_heading_collect(struct rss_html_render *render,
				     const char *data, size_t length)
{
	size_t needed;

	if (render->heading_overflow) {
		return;
	}
	if (length == 1U && rss_is_space(data[0])) {
		render->heading_pending_space = render->heading_len > 0U;
		return;
	}
	needed = length + (render->heading_pending_space ? 1U : 0U);
	if (render->heading_len + needed >= sizeof(render->heading)) {
		/* Not a usable candidate any more: emit it as a normal heading
		 * and continue streaming its text.
		 */
		render->heading_overflow = true;
		render->heading_candidate = false;
		render->heading_live = true;
		rss_lw_break(&render->line, 2U);
		rss_lw_text(&render->line, render->heading, render->heading_len);
		rss_lw_text(&render->line, data, length);
		return;
	}
	if (render->heading_pending_space) {
		render->heading[render->heading_len++] = ' ';
		render->heading_pending_space = false;
	}
	memcpy(render->heading + render->heading_len, data, length);
	render->heading_len += length;
}

static void rss_html_heading_finish(struct rss_html_render *render)
{
	if (!render->heading_candidate) {
		if (render->heading_live) {
			render->heading_live = false;
			rss_lw_break(&render->line, 2U);
		}
		return;
	}
	render->heading_candidate = false;
	render->heading_pending_space = false;
	if (render->item_title != NULL && render->item_title[0] != '\0' &&
	    strlen(render->item_title) == render->heading_len &&
	    memcmp(render->heading, render->item_title,
		   render->heading_len) == 0) {
		/* The feed repeats the item title as the first <h1>; drop it. */
		return;
	}
	if (render->heading_len > 0U) {
		rss_lw_break(&render->line, 2U);
		rss_lw_text(&render->line, render->heading, render->heading_len);
		rss_lw_break(&render->line, 2U);
	}
}

static void rss_html_text_units(struct rss_html_render *render,
				const char *data, size_t length)
{
	if (length == 0U) {
		return;
	}
	if (render->state == RSS_HTML_LT ||
	    (render->state == RSS_HTML_TAG_NAME && render->closing &&
	     render->name_len == 0U)) {
		/* A non-ASCII character cannot start an HTML tag name. Keep a
		 * malformed literal <中文 whole rather than emitting byte fragments.
		 */
		const bool closing = render->state == RSS_HTML_TAG_NAME;
		render->state = RSS_HTML_TEXT;
		render->closing = false;
		rss_html_text_ascii(render, '<');
		if (closing) rss_html_text_ascii(render, '/');
	} else if (render->state != RSS_HTML_TEXT) {
		/* UTF-8 inside attributes, comments and script/style is markup, not
		 * visible text. Still feed the scanner so quotes and limits stay in
		 * sync, without routing these bytes to the line writer.
		 */
		for (size_t i = 0U; i < length; ++i) rss_html_feed_ascii(render, data[i]);
		return;
	}
	if (render->entity_active) rss_html_entity_flush(render);
	rss_html_table_text_prepare(render);
	if (render->heading_candidate) {
		rss_html_heading_collect(render, data, length);
		return;
	}
	if (render->url_active) {
		rss_url_append(render, data, length);
		return;
	}
	rss_lw_text(&render->line, data, length);
	render->prev_word_char = false;
}

static bool rss_url_prefix_possible(struct rss_html_render *render)
{
	static const char *schemes[] = { "http://", "https://" };
	bool possible = false;

	for (size_t i = 0U; i < sizeof(schemes) / sizeof(schemes[0]); ++i) {
		const size_t scheme_len = strlen(schemes[i]);

		if (render->url_len <= scheme_len &&
		    rss_memcmp_ci(render->url, schemes[i],
				  render->url_len)) {
			if (render->url_len == scheme_len) {
				render->url_prefix_ok = true;
				return true;
			}
			possible = true;
		}
	}
	return possible;
}

static void rss_url_reset(struct rss_html_render *render)
{
	render->url_active = false;
	render->url_prefix_ok = false;
	render->url_discard = false;
	render->url_len = 0U;
}

static void rss_url_emit_plain(struct rss_html_render *render)
{
	for (size_t i = 0U; i < render->url_len;) {
		const size_t sequence = rss_utf8_scalar_length(
			render->url + i, render->url_len - i);

		if (sequence == 0U) {
			++i;
			continue;
		}
		rss_lw_text(&render->line, render->url + i, sequence);
		i += sequence;
	}
	render->prev_word_char =
		render->url_len > 0U &&
		rss_is_word_char(render->url[render->url_len - 1U]);
	rss_url_reset(render);
}

static bool rss_url_trailing_punct(const char *data, size_t length)
{
	static const char *cjk[] = {
		"\xE3\x80\x82", /* 。 */
		"\xE3\x80\x81", /* 、 */
		"\xEF\xBC\x8C", /* ， */
		"\xEF\xBC\x9B", /* ； */
		"\xEF\xBC\x9A", /* ： */
		"\xEF\xBC\x81", /* ！ */
		"\xEF\xBC\x9F", /* ？ */
		"\xEF\xBC\x89", /* ） */
		"\xE3\x80\x8D", /* 」 */
		"\xE3\x80\x8F", /* 』 */
		"\xE3\x80\x91", /* 】 */
	};

	if (length == 1U) {
		switch (data[0]) {
		case '.':
		case ',':
		case ';':
		case ':':
		case '!':
		case '?':
		case ')':
		case ']':
		case '}':
		case '>':
		case '"':
		case '\'':
			return true;
		default:
			return false;
		}
	}
	for (size_t i = 0U; i < sizeof(cjk) / sizeof(cjk[0]); ++i) {
		if (length == 3U && memcmp(data, cjk[i], 3U) == 0) {
			return true;
		}
	}
	return false;
}

static size_t rss_utf8_clamp(const char *data, size_t length, size_t limit)
{
	size_t cut = limit;

	while (cut > 0U && ((unsigned char)data[cut] & 0xC0U) == 0x80U) {
		--cut;
	}
	(void)length;
	return cut;
}

static void rss_url_terminate(struct rss_html_render *render)
{
	char display[RSS_HTML_DISPLAY_MAX + 4U];
	size_t url_end;
	size_t dlen = 0U;
	size_t scheme = 0U;
	size_t host_end;

	if (!render->url_active) {
		return;
	}
	if (!render->url_prefix_ok) {
		rss_url_emit_plain(render);
		return;
	}
	url_end = render->url_len;
	while (url_end > 0U) {
		size_t start = url_end - 1U;

		while (start > 0U &&
		       ((unsigned char)render->url[start] & 0xC0U) == 0x80U) {
			--start;
		}
		if (!rss_url_trailing_punct(render->url + start,
					    url_end - start)) {
			break;
		}
		url_end = start;
	}
	if (url_end >= 8U && rss_memcmp_ci(render->url, "https://", 8U)) {
		scheme = 8U;
	} else if (url_end >= 7U &&
		   rss_memcmp_ci(render->url, "http://", 7U)) {
		scheme = 7U;
	}
	host_end = scheme;
	while (host_end < url_end && render->url[host_end] != '/' &&
	       render->url[host_end] != '?' && render->url[host_end] != '#') {
		++host_end;
	}
	for (size_t i = scheme; i < host_end; ++i) {
		if (dlen < sizeof(display)) {
			display[dlen++] = render->url[i];
		}
	}
	if (host_end < url_end && render->url[host_end] == '/') {
		size_t segment_start = host_end + 1U;
		size_t segment_end = segment_start;

		while (segment_end < url_end && render->url[segment_end] != '/' &&
		       render->url[segment_end] != '?' &&
		       render->url[segment_end] != '#') {
			++segment_end;
		}
		if (segment_end > segment_start) {
			if (dlen < sizeof(display)) {
				display[dlen++] = '/';
			}
			for (size_t i = segment_start;
			     i < segment_end && dlen < sizeof(display); ++i) {
				display[dlen++] = render->url[i];
			}
		}
	}
	if (dlen == 0U) {
		rss_url_emit_plain(render);
		return;
	}
	if (dlen > RSS_HTML_DISPLAY_MAX) {
		const size_t room = RSS_HTML_DISPLAY_MAX - 3U;
		const size_t cut = rss_utf8_clamp(display, dlen, room);

		dlen = cut;
		memcpy(display + dlen, "\xE2\x80\xA6", 3U); /* … */
		dlen += 3U;
	}
	rss_lw_text(&render->line, display, dlen);
	/* Punctuation that had been collected as part of the URL stays as text
	 * right after the shortened link.
	 */
	for (size_t i = url_end; i < render->url_len;) {
		const size_t sequence = rss_utf8_scalar_length(
			render->url + i, render->url_len - i);

		if (sequence == 0U) {
			++i;
			continue;
		}
		rss_lw_text(&render->line, render->url + i, sequence);
		i += sequence;
	}
	rss_url_reset(render);
	render->prev_word_char = false;
}

static void rss_url_append(struct rss_html_render *render, const char *data,
			   size_t length)
{
	if (render->url_discard) {
		return;
	}
	/* Keep the validated scalar whole, including at the fixed URL limit. */
	if (length > sizeof(render->url) - 1U - render->url_len) {
		if (!render->url_prefix_ok) {
			rss_url_emit_plain(render);
			rss_lw_text(&render->line, data, length);
			return;
		}
		render->url_discard = true;
		return;
	}
	memcpy(render->url + render->url_len, data, length);
	render->url_len += length;
	if (!render->url_prefix_ok &&
	    ((unsigned char)data[0] >= 0x80U ||
	     !rss_url_prefix_possible(render))) {
		rss_url_emit_plain(render);
	}
}

static void rss_html_text_ascii(struct rss_html_render *render, char ch)
{
	/* Source text cannot inject the private table records, NUL terminators or
	 * other control characters into a cached body. Whitespace is folded below.
	 */
	if ((unsigned char)ch < 0x20U && !rss_is_space(ch)) {
		return;
	}
	if (!rss_is_space(ch)) {
		rss_html_table_text_prepare(render);
	}
	if (render->heading_candidate) {
		rss_html_heading_collect(render, &ch, 1U);
		return;
	}
	if (rss_is_space(ch)) {
		if (render->url_active) {
			rss_url_terminate(render);
		}
		rss_lw_space(&render->line);
		render->prev_word_char = false;
		return;
	}
	if (render->url_active) {
		rss_url_append(render, &ch, 1U);
		return;
	}
	if ((ch == 'h' || ch == 'H') && !render->prev_word_char) {
		rss_url_reset(render);
		render->url_active = true;
		rss_url_append(render, &ch, 1U);
		return;
	}
	rss_lw_text(&render->line, &ch, 1U);
	render->prev_word_char = rss_is_word_char(ch);
}

static void rss_html_emit_entity(struct rss_html_render *render,
				 const char *data, size_t length)
{
	if (length == 1U && (unsigned char)data[0] < 0x80U) {
		rss_html_text_ascii(render, data[0]);
		return;
	}
	rss_html_text_units(render, data, length);
}

static void rss_html_entity_flush(struct rss_html_render *render)
{
	for (size_t i = 0U; i < render->entity_len; ++i) {
		rss_html_text_ascii(render, render->entity[i]);
	}
	render->entity_len = 0U;
	render->entity_active = false;
}

static void rss_html_entity_char(struct rss_html_render *render, char ch)
{
	char decoded[4];
	size_t consumed = 0U;
	size_t written;

	if (render->entity_len >= sizeof(render->entity) - 1U) {
		rss_html_entity_flush(render);
		rss_html_feed_ascii(render, ch);
		return;
	}
	render->entity[render->entity_len++] = ch;
	if (ch == ';') {
		written = rss_decode_entity(render->entity, render->entity_len,
					    decoded, &consumed);
		if (written > 0U && consumed == render->entity_len) {
			render->entity_len = 0U;
			render->entity_active = false;
			rss_html_emit_entity(render, decoded, written);
		} else {
			rss_html_entity_flush(render);
		}
		return;
	}
	if (ch != '#' && !rss_is_alpha(ch) && !(ch >= '0' && ch <= '9')) {
		rss_html_entity_flush(render);
		rss_html_feed_ascii(render, ch);
	}
}

static void rss_html_start_raw_text(struct rss_html_render *render,
				    const char *name, size_t length)
{
	if (length >= sizeof(render->raw_tag)) {
		return;
	}
	memcpy(render->raw_tag, name, length);
	render->raw_tag_len = length;
	render->raw_pos = 0U;
	render->state = RSS_HTML_RAW_TEXT;
}

/* A small, bounded attribute buffer is enough for normal merged cells. If a
 * cell's attributes cannot be inspected completely, the whole remaining table
 * is explicitly rendered as sequential text instead of risking shifted data.
 */
static unsigned int rss_html_span(struct rss_html_render *render,
				 const char *wanted, unsigned int maximum,
				 bool *unsupported)
{
	const char *data = render->attributes;
	const size_t length = render->attributes_length;
	size_t p = 0U;

	if (render->attributes_overflow) {
		*unsupported = true;
		return 1U;
	}
	while (p < length) {
		while (p < length && (rss_is_space(data[p]) || data[p] == '/')) ++p;
		const size_t name_start = p;
		while (p < length && (rss_is_alpha(data[p]) ||
		       (data[p] >= '0' && data[p] <= '9') || data[p] == '-' ||
		       data[p] == '_' || data[p] == ':')) ++p;
		const size_t name_length = p - name_start;
		if (name_length == 0U) {
			++p;
			continue;
		}
		while (p < length && rss_is_space(data[p])) ++p;
		if (p >= length || data[p] != '=') continue;
		++p;
		while (p < length && rss_is_space(data[p])) ++p;
		char quote = '\0';
		if (p < length && (data[p] == '\'' || data[p] == '"')) quote = data[p++];
		const size_t value_start = p;
		while (p < length && (quote ? data[p] != quote : !rss_is_space(data[p]))) ++p;
		const size_t value_length = p - value_start;
		if (quote && p < length) ++p;
		if (name_length != strlen(wanted) ||
		    !rss_memcmp_ci(data + name_start, wanted, name_length)) continue;
		unsigned int value = 0U;
		if (value_length == 0U) *unsupported = true;
		for (size_t i = 0U; i < value_length; ++i) {
			const char ch = data[value_start + i];
			if (ch < '0' || ch > '9' || value > maximum / 10U) {
				*unsupported = true;
				return 1U;
			}
			value = value * 10U + (unsigned int)(ch - '0');
			if (value > maximum) {
				*unsupported = true;
				return 1U;
			}
		}
		if (value == 0U) *unsupported = true;
		return value > 0U ? value : 1U;
	}
	return 1U;
}

static void rss_table_line_reset(struct rss_html_render *render)
{
	render->line.pending_breaks = 0U;
	render->line.pending_space = false;
	render->line.line_open = false;
	render->line.line_text = 0U;
	render->line.last_cjk = false;
}

static void rss_table_cell_prefix(struct rss_html_render *render, bool header)
{
	if (!render->table_grid_open) {
		rss_lw_break(&render->line, 2U);
		rss_lw_flush(&render->line);
		(void)rss_sink_put(render->sink, PAPERMONO_RSS_TABLE_BEGIN, 4U);
		render->table_grid_open = true;
	}
	if (!render->table_row_started) {
		(void)rss_sink_put(render->sink, header ? PAPERMONO_RSS_TABLE_HEADER :
			PAPERMONO_RSS_TABLE_ROW, 3U);
		render->table_row_started = true;
	} else {
		(void)rss_sink_put(render->sink, "\t", 1U);
	}
	rss_table_line_reset(render);
	render->line.line_open = true;
	render->line.has_content = true;
}

static void rss_table_cell_close(struct rss_html_render *render)
{
	if (!render->table_cell_open) return;
	for (unsigned int i = 0U; i < render->table_colspan; ++i) {
		if (render->table_cell_start + i < RSS_HTML_TABLE_COLUMNS) {
			render->table_rowspans[render->table_cell_start + i] =
				(uint8_t)render->table_rowspan;
		}
		if (i > 0U) {
			rss_table_cell_prefix(render, false);
			++render->table_column;
		}
	}
	render->line.pending_space = false;
	render->table_cell_open = false;
}

static void rss_table_row_close(struct rss_html_render *render)
{
	rss_table_cell_close(render);
	if (render->table_row_open && !render->table_row_started) {
		rss_table_cell_prefix(render, false);
	}
	if (render->table_row_started) (void)rss_sink_put(render->sink, "\n", 1U);
	render->table_row_open = false;
	render->table_row_started = false;
	rss_table_line_reset(render);
}

static void rss_table_grid_close(struct rss_html_render *render)
{
	rss_table_row_close(render);
	if (render->table_grid_open) {
		(void)rss_sink_put(render->sink, PAPERMONO_RSS_TABLE_END, 3U);
		render->table_grid_open = false;
	}
	rss_lw_break(&render->line, 2U);
}

static void rss_table_finish(struct rss_html_render *render)
{
	if (!render->table_plain) rss_table_grid_close(render);
	render->table_depth = 0U;
	render->table_plain = false;
	render->table_caption = false;
	render->line.indent = render->table_saved_indent;
	rss_lw_break(&render->line, 2U);
}

static void rss_table_row_open(struct rss_html_render *render)
{
	rss_table_row_close(render);
	for (size_t i = 0U; i < RSS_HTML_TABLE_COLUMNS; ++i) {
		if (render->table_rowspans[i] > 0U) --render->table_rowspans[i];
	}
	render->table_row_open = true;
	render->table_column = 0U;
	render->table_caption = false;
}

static void rss_table_degrade(struct rss_html_render *render)
{
	static const char notice[] = "表格含超范围合并单元格，以下按单元格顺序显示。";
	if (!render->table_row_started) render->table_row_open = false;
	rss_table_grid_close(render);
	render->table_plain = true;
	render->table_plain_cells = 0U;
	rss_lw_text(&render->line, notice, sizeof(notice) - 1U);
	rss_lw_break(&render->line, 2U);
}

static void rss_html_table_text_prepare(struct rss_html_render *render)
{
	if (render->table_depth > 0U && !render->table_cell_open &&
	    !render->table_plain && !render->table_caption) {
		/* Captions and malformed visible text between rows remain ordinary
		 * text at their source position instead of corrupting a row record.
		 */
		rss_table_grid_close(render);
		render->table_caption = true;
	}
}

static bool rss_table_break_tag(const char *name, size_t length)
{
	return rss_tag_is(name, length, "br") || rss_tag_is(name, length, "p") ||
	       rss_tag_is(name, length, "div") || rss_tag_is(name, length, "li") ||
	       rss_tag_is(name, length, "tr") || rss_tag_is(name, length, "td") ||
	       rss_tag_is(name, length, "th") || rss_tag_is(name, length, "ul") ||
	       rss_tag_is(name, length, "ol") || rss_tag_is(name, length, "blockquote") ||
	       rss_tag_is(name, length, "section") || rss_tag_is(name, length, "pre") ||
	       (length == 2U && name[0] == 'h' && name[1] >= '1' && name[1] <= '6');
}

static bool rss_html_table_tag(struct rss_html_render *render,
			       const char *name, size_t length, bool closing)
{
	if (rss_tag_is(name, length, "table")) {
		if (closing) {
			if (render->table_depth == 1U) rss_table_finish(render);
			else if (render->table_depth > 1U) --render->table_depth;
		} else if (render->table_depth > 0U) {
			++render->table_depth;
			rss_lw_space(&render->line);
		} else {
			render->table_depth = 1U;
			render->table_saved_indent = render->line.indent;
			render->line.indent = 0U;
			render->table_plain = false;
			render->table_caption = false;
			memset(render->table_rowspans, 0, sizeof(render->table_rowspans));
			rss_lw_break(&render->line, 2U);
		}
		return true;
	}
	if (render->table_depth == 0U) return false;
	if (render->table_depth > 1U) {
		/* Nested rows/cells stay inside the containing outer cell. */
		if (rss_table_break_tag(name, length)) rss_lw_space(&render->line);
		return true;
	}
	if (rss_tag_is(name, length, "caption")) {
		if (!closing) {
			if (!render->table_plain) rss_table_grid_close(render);
			render->table_caption = true;
		} else render->table_caption = false;
		rss_lw_break(&render->line, 2U);
		return true;
	}
	if (rss_tag_is(name, length, "tr")) {
		if (render->table_plain) {
			rss_lw_break(&render->line, 1U);
			render->table_plain_cells = 0U;
		} else if (closing) rss_table_row_close(render);
		else rss_table_row_open(render);
		return true;
	}
	if (rss_tag_is(name, length, "td") || rss_tag_is(name, length, "th")) {
		if (closing) {
			if (!render->table_plain) rss_table_cell_close(render);
			else rss_lw_space(&render->line);
			return true;
		}
		bool unsupported = false;
		const unsigned int colspan = rss_html_span(render, "colspan",
			RSS_HTML_TABLE_COLUMNS, &unsupported);
		const unsigned int rowspan = rss_html_span(render, "rowspan",
			RSS_HTML_TABLE_ROWSPAN, &unsupported);
		if (!render->table_plain) {
			if (!render->table_row_open) rss_table_row_open(render);
			rss_table_cell_close(render);
			while (render->table_column < RSS_HTML_TABLE_COLUMNS &&
			       render->table_rowspans[render->table_column] > 0U) {
				rss_table_cell_prefix(render, rss_tag_is(name, length, "th"));
				++render->table_column;
			}
			if ((colspan > 1U || rowspan > 1U) &&
			    (render->table_column >= RSS_HTML_TABLE_COLUMNS ||
			     colspan > RSS_HTML_TABLE_COLUMNS - render->table_column)) unsupported = true;
			if (!unsupported) {
				for (unsigned int i = 0U; i < colspan; ++i) {
					if (render->table_column + i < RSS_HTML_TABLE_COLUMNS &&
					    render->table_rowspans[render->table_column + i] > 0U) unsupported = true;
				}
			}
			if (unsupported) rss_table_degrade(render);
		}
		if (render->table_plain) {
			if (render->table_plain_cells++ > 0U) {
				render->line.pending_space = false;
				rss_lw_text(&render->line, " | ", 3U);
			}
			return true;
		}
		rss_table_cell_prefix(render, rss_tag_is(name, length, "th"));
		render->table_cell_start = render->table_column++;
		render->table_colspan = colspan;
		render->table_rowspan = rowspan;
		render->table_cell_open = true;
		return true;
	}
	if (rss_table_break_tag(name, length)) {
		if (render->table_cell_open || render->table_plain) rss_lw_space(&render->line);
		else if (render->table_caption) rss_lw_break(&render->line, 1U);
	}
	return true;
}

static void rss_html_dispatch(struct rss_html_render *render)
{
	const char *name = render->name;
	const size_t length = render->name_len;
	const bool closing = render->closing;
	const bool self_closing = render->self_closing;

	render->closing = false;
	render->self_closing = false;
	if (render->name_overflow) {
		return;
	}
	if (render->heading_candidate) {
		if (closing && rss_tag_is(name, length, "h1")) {
			rss_html_heading_finish(render);
		}
		return;
	}
	if ((rss_tag_is(name, length, "script") ||
	     rss_tag_is(name, length, "style")) && !closing) {
		rss_html_start_raw_text(render, name, length);
		return;
	}
	if (rss_html_table_tag(render, name, length, closing)) {
		if (self_closing && !closing) (void)rss_html_table_tag(render, name, length, true);
		return;
	}
	if (rss_tag_is(name, length, "br")) {
		rss_lw_break(&render->line, 1U);
		return;
	}
	if (!closing) {
		if (rss_tag_is(name, length, "p")) {
			rss_lw_break(&render->line, 2U);
		} else if (rss_tag_is(name, length, "h1")) {
			/* The first <h1> usually repeats the item title, sometimes
			 * after a short preamble paragraph, so deduplicate it even
			 * when a paragraph precedes it.
			 */
			if (!render->heading_done && render->item_title != NULL &&
			    render->item_title[0] != '\0') {
				render->heading_candidate = true;
				render->heading_done = true;
				render->heading_len = 0U;
				render->heading_pending_space = false;
				render->heading_overflow = false;
			} else {
				render->heading_done = true;
				render->heading_live = true;
				rss_lw_break(&render->line, 2U);
			}
		} else if (rss_tag_is(name, length, "h2") ||
			   rss_tag_is(name, length, "h3") ||
			   rss_tag_is(name, length, "h4") ||
			   rss_tag_is(name, length, "h5") ||
			   rss_tag_is(name, length, "h6")) {
			render->heading_live = true;
			rss_lw_break(&render->line, 2U);
		} else if (rss_tag_is(name, length, "blockquote")) {
			rss_lw_break(&render->line, 2U);
			if (render->line.indent < RSS_HTML_INDENT_MAX) {
				++render->line.indent;
			}
		} else if (rss_tag_is(name, length, "ul") ||
			   rss_tag_is(name, length, "ol")) {
			rss_lw_break(&render->line, 2U);
		} else if (rss_tag_is(name, length, "li")) {
			rss_lw_break(&render->line, 1U);
			rss_lw_bullet(&render->line);
		}
		return;
	}
	if (rss_tag_is(name, length, "p")) {
		rss_lw_break(&render->line, 2U);
	} else if (rss_tag_is(name, length, "h1") ||
		   rss_tag_is(name, length, "h2") ||
		   rss_tag_is(name, length, "h3") ||
		   rss_tag_is(name, length, "h4") ||
		   rss_tag_is(name, length, "h5") ||
		   rss_tag_is(name, length, "h6")) {
		render->heading_live = false;
		rss_lw_break(&render->line, 2U);
	} else if (rss_tag_is(name, length, "blockquote")) {
		if (render->line.indent > 0U) {
			--render->line.indent;
		}
		rss_lw_break(&render->line, 2U);
	} else if (rss_tag_is(name, length, "ul") ||
		   rss_tag_is(name, length, "ol")) {
		rss_lw_break(&render->line, 2U);
	} else if (rss_tag_is(name, length, "li")) {
		rss_lw_break(&render->line, 1U);
	} else if (rss_tag_is(name, length, "div") ||
		   rss_tag_is(name, length, "tr") ||
		   rss_tag_is(name, length, "table") ||
		   rss_tag_is(name, length, "thead") ||
		   rss_tag_is(name, length, "tbody") ||
		   rss_tag_is(name, length, "section") ||
		   rss_tag_is(name, length, "article") ||
		   rss_tag_is(name, length, "header") ||
		   rss_tag_is(name, length, "footer") ||
		   rss_tag_is(name, length, "figure") ||
		   rss_tag_is(name, length, "figcaption") ||
		   rss_tag_is(name, length, "pre")) {
		rss_lw_break(&render->line, 1U);
	}
}

static void rss_html_feed_ascii(struct rss_html_render *render, char ch)
{
	for (unsigned int guard = 0U; guard < 8U; ++guard) {
		switch (render->state) {
		case RSS_HTML_TEXT:
			if (render->entity_active) {
				rss_html_entity_char(render, ch);
				return;
			}
			if (ch == '<') {
				if (render->url_active) {
					rss_url_terminate(render);
				}
				render->state = RSS_HTML_LT;
				render->attributes_length = 0U;
				render->attributes_overflow = false;
				return;
			}
			if (ch == '&') {
				render->entity_active = true;
				render->entity_len = 1U;
				render->entity[0] = '&';
				return;
			}
			rss_html_text_ascii(render, ch);
			return;
		case RSS_HTML_LT:
			if (ch == '/') {
				render->closing = true;
				render->name_len = 0U;
				render->name_overflow = false;
				render->state = RSS_HTML_TAG_NAME;
				return;
			}
			if (ch == '!') {
				render->state = RSS_HTML_DECL_START;
				return;
			}
			if (ch == '?') {
				render->state = RSS_HTML_DECL;
				return;
			}
			if (rss_is_alpha(ch)) {
				render->closing = false;
				render->self_closing = false;
				render->last_slash = false;
				render->name_len = 0U;
				render->name_overflow = false;
				render->name[render->name_len++] =
					rss_lower_ascii(ch);
				render->state = RSS_HTML_TAG_NAME;
				return;
			}
			rss_html_text_ascii(render, '<');
			render->state = RSS_HTML_TEXT;
			continue;
		case RSS_HTML_TAG_NAME:
			if (render->name_len == 0U && render->closing &&
			    !rss_is_alpha(ch)) {
				rss_html_text_ascii(render, '<');
				rss_html_text_ascii(render, '/');
				render->closing = false;
				render->state = RSS_HTML_TEXT;
				continue;
			}
			if (rss_is_space(ch)) {
				render->state = RSS_HTML_TAG_SKIP;
				return;
			}
			if (ch == '/') {
				render->self_closing = true;
				render->state = RSS_HTML_TAG_SKIP;
				return;
			}
			if (ch == '>') {
				rss_html_dispatch(render);
				if (render->state != RSS_HTML_RAW_TEXT) {
					render->state = RSS_HTML_TEXT;
				}
				return;
			}
			if (!render->name_overflow) {
				if (render->name_len < sizeof(render->name) - 1U) {
					render->name[render->name_len++] =
						rss_lower_ascii(ch);
				} else {
					render->name_overflow = true;
				}
			}
			return;
		case RSS_HTML_TAG_SKIP:
			if (ch != '>' || render->quote != '\0') {
				if (render->attributes_length < sizeof(render->attributes)) {
					render->attributes[render->attributes_length++] = ch;
				} else render->attributes_overflow = true;
			}
			if (render->quote != '\0') {
				if (ch == render->quote) {
					render->quote = '\0';
				}
				return;
			}
			if (ch == '"' || ch == '\'') {
				render->quote = ch;
				return;
			}
			if (ch == '>') {
				if (render->last_slash) {
					render->self_closing = true;
				}
				rss_html_dispatch(render);
				if (render->state != RSS_HTML_RAW_TEXT) {
					render->state = RSS_HTML_TEXT;
				}
				return;
			}
			render->last_slash = ch == '/';
			return;
		case RSS_HTML_DECL_START:
			if (ch == '-') {
				render->comment_dashes = 1U;
				render->state = RSS_HTML_COMMENT;
				return;
			}
			if (ch == '>') {
				render->state = RSS_HTML_TEXT;
				return;
			}
			render->state = RSS_HTML_DECL;
			return;
		case RSS_HTML_DECL:
			if (ch == '>') {
				render->state = RSS_HTML_TEXT;
			}
			return;
		case RSS_HTML_COMMENT:
			if (ch == '>' && render->comment_dashes >= 2U) {
				render->comment_dashes = 0U;
				render->state = RSS_HTML_TEXT;
				return;
			}
			if (ch == '-') {
				++render->comment_dashes;
			} else {
				render->comment_dashes = 0U;
			}
			return;
		case RSS_HTML_RAW_TEXT:
			if (ch == '<') {
				render->state = RSS_HTML_RAW_LT;
			}
			return;
		case RSS_HTML_RAW_LT:
			if (ch == '/') {
				render->raw_pos = 0U;
				render->state = RSS_HTML_RAW_NAME;
				return;
			}
			render->state = RSS_HTML_RAW_TEXT;
			return;
		case RSS_HTML_RAW_NAME:
			if (render->raw_pos < render->raw_tag_len &&
			    rss_lower_ascii(ch) ==
				    render->raw_tag[render->raw_pos]) {
				++render->raw_pos;
				return;
			}
			if (render->raw_pos == render->raw_tag_len &&
			    (ch == '>' || rss_is_space(ch))) {
				if (ch == '>') {
					render->state = RSS_HTML_TEXT;
				}
				return;
			}
			render->state = RSS_HTML_RAW_TEXT;
			return;
		}
		return;
	}
}

static void rss_html_finish(struct rss_html_render *render)
{
	if (render->entity_active) {
		rss_html_entity_flush(render);
	}
	if (render->url_active) {
		rss_url_terminate(render);
	}
	if (render->heading_candidate) {
		rss_html_heading_finish(render);
	}
	if (render->table_depth > 0U) rss_table_finish(render);
}

static void rss_render_body(const char *text, size_t length, bool literal,
			    const char *item_title, struct rss_sink *sink)
{
	struct rss_html_render render;

	memset(&render, 0, sizeof(render));
	render.sink = sink;
	render.line.sink = sink;
	render.item_title = item_title;
	render.state = RSS_HTML_TEXT;
	for (size_t index = 0U; index < length;) {
		if (!literal && text[index] == '&') {
			char decoded[4];
			size_t consumed = 0U;
			const size_t written =
				rss_decode_entity(text + index, length - index,
						  decoded, &consumed);

			if (written > 0U) {
				for (size_t i = 0U; i < written; ++i) {
					if ((unsigned char)decoded[i] < 0x80U) {
						rss_html_feed_ascii(&render,
								    decoded[i]);
					} else {
						rss_html_text_units(&render,
								    decoded + i,
								    written - i);
						break;
					}
				}
				index += consumed;
				continue;
			}
		}
		const unsigned char lead = (unsigned char)text[index];

		if (lead < 0x80U) {
			rss_html_feed_ascii(&render, (char)lead);
			++index;
			continue;
		}
		const size_t sequence =
			rss_utf8_scalar_length(text + index, length - index);

		if (sequence == 0U) {
			++index;
			continue;
		}
		rss_html_text_units(&render, text + index, sequence);
		index += sequence;
	}
	rss_html_finish(&render);
}

/* Extract the raw payload of the first <name> element in [start, end). */
static bool rss_find_raw_field(const char *start, const char *end,
			       const char *name, struct rss_raw_field *field)
{
	static const char cdata_open[] = "<![CDATA[";
	const size_t marker = sizeof(cdata_open) - 1U;
	const size_t name_len = strlen(name);
	const char *content;
	const char *limit;
	bool self_closed = false;

	content = rss_xml_find_open(start, end, name, name_len, &self_closed);
	if (content == NULL || self_closed) {
		return false;
	}
	limit = rss_xml_find_close(content, end, name, name_len);
	if (limit == NULL) {
		limit = end;
	}
	field->text = content;
	field->length = (size_t)(limit - content);
	field->literal = false;
	{
		const char *p = content;

		while (p < limit && rss_is_space(*p)) {
			++p;
		}
		if ((size_t)(limit - p) >= marker &&
		    memcmp(p, cdata_open, marker) == 0) {
			const char *cdata_end =
				rss_mem_find(p + marker, limit, "]]>", 3U);

			field->text = p + marker;
			field->length = cdata_end != NULL ?
				(size_t)(cdata_end - field->text) :
				(size_t)(limit - field->text);
			field->literal = true;
		}
	}
	return true;
}

/* Count the stored plain-text length of an item body, with truncation logged. */
static size_t rss_count_body(const struct rss_raw_field *field,
			     unsigned int item_index,
			     const char *item_title)
{
	struct rss_sink count_sink = {
		.emit = NULL,
		.context = NULL,
		.stored = 0U,
		.total = 0U,
		.limit = RSS_BODY_LIMIT,
		.overflow = false,
		.failed = false,
	};

	(void)item_index;
	rss_render_body(field->text, field->length, field->literal,
			item_title, &count_sink);
	if (count_sink.total > count_sink.stored) {
		RSS_LOG_WRN("RSS item %u body truncated to %u of %u bytes",
			    item_index, (unsigned int)count_sink.stored,
			    (unsigned int)count_sink.total);
	}
	return count_sink.stored;
}

/* Receives one plain-text body; supplied by the caller. */
struct rss_body_consumer {
	int (*append)(void *context, const char *raw, size_t raw_length,
		      bool literal, const char *item_title,
		      size_t plain_length);
	void *context;
};

/*
 * Parse a complete RSS 2.0 document into the RAM index. Bodies are handed to
 * @p consumer as soon as an item is accepted, so no body text is kept in RAM.
 * A truncated tail (the HTTP buffer filled up mid-feed) drops the last item,
 * which may be partial.
 */
static int rss_parse_feed(const char *xml, size_t length,
			  struct papermono_rss_feed *out,
			  struct rss_body_consumer *consumer, bool truncated)
{
	const char *end;
	const char *channel;
	const char *channel_end;
	const char *first_item;
	bool self_closed = false;
	size_t bodies_total = 0U;
	const char *cursor;

	if (out == NULL) {
		return -EINVAL;
	}
	memset(out, 0, sizeof(*out));
	if (xml == NULL || length == 0U) {
		return -EINVAL;
	}
	end = xml + length;
	channel = rss_xml_find_open(xml, end, "channel", 7U, &self_closed);
	if (channel == NULL || self_closed) {
		return -EBADMSG;
	}
	channel_end = rss_xml_find_close(channel, end, "channel", 7U);
	if (channel_end == NULL) {
		channel_end = end;
	}
	/* The channel title is the first title before any item; item titles
	 * must not win when the channel omits its own title.
	 */
	first_item = rss_xml_find_open(channel, channel_end, "item", 4U,
				       &self_closed);
	{
		struct rss_raw_field field;

		if (rss_find_raw_field(channel,
				       first_item != NULL ? first_item :
							    channel_end,
				       "title", &field)) {
			(void)rss_render_title_to_buffer(
				field.text, field.length, field.literal,
				out->title, sizeof(out->title));
		}
	}

	cursor = channel;
	while (out->count < PAPERMONO_RSS_ITEM_MAX) {
		struct papermono_rss_item_meta entry;
		const char *item;
		const char *item_end;
		struct rss_raw_field field;
		bool item_self = false;
		size_t body_length = 0U;

		item = rss_xml_find_open(cursor, channel_end, "item", 4U,
					 &item_self);
		if (item == NULL) {
			break;
		}
		if (item_self) {
			if (item <= cursor) {
				break;
			}
			cursor = item;
			continue;
		}
		item_end = rss_xml_find_close(item, channel_end, "item", 4U);
		if (item_end == NULL) {
			item_end = channel_end;
		}
		memset(&entry, 0, sizeof(entry));
		if (rss_find_raw_field(item, item_end, "title", &field)) {
			(void)rss_render_title_to_buffer(
				field.text, field.length, field.literal,
				entry.title, sizeof(entry.title));
		}
		if (rss_find_raw_field(item, item_end, "pubDate", &field)) {
			(void)rss_render_title_to_buffer(
				field.text, field.length, field.literal,
				entry.published, sizeof(entry.published));
		}
		field.text = NULL;
		field.length = 0U;
		field.literal = false;
		if (rss_find_raw_field(item, item_end, "description", &field)) {
			body_length = rss_count_body(
				&field, (unsigned int)out->count, entry.title);
		}
		/* Some feeds ship an empty <description> and put the article in
		 * <content:encoded>; fall back only when the description had no
		 * displayable text.
		 */
		if (body_length == 0U &&
		    rss_find_raw_field(item, item_end, "content:encoded",
				       &field)) {
			body_length = rss_count_body(
				&field, (unsigned int)out->count, entry.title);
		}
		if (body_length > 0U) {
			if (bodies_total + body_length >
			    (size_t)RSS_BODIES_TOTAL_MAX) {
				RSS_LOG_WRN("RSS item %u skipped: body cache limit of %u KB reached",
					    (unsigned int)out->count,
					    (unsigned int)(RSS_BODIES_TOTAL_MAX /
							   1024U));
				goto next_item;
			}
			if (consumer != NULL) {
				const int ret = consumer->append(
					consumer->context, field.text,
					field.length, field.literal,
					entry.title, body_length);

				if (ret < 0) {
					return ret;
				}
				bodies_total += body_length;
			}
			entry.body_length = (uint32_t)body_length;
		}
		if (entry.title[0] != '\0' || entry.body_length > 0U) {
			out->items[out->count] = entry;
			++out->count;
		}
next_item:
		if (item_end <= cursor) {
			break;
		}
		cursor = item_end;
	}
	if (truncated && out->count > 0U) {
		RSS_LOG_WRN("RSS feed truncated in the response buffer; dropping item %u",
			    (unsigned int)(out->count - 1U));
		--out->count;
	}
	out->valid = true;
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Snapshot state and SD cache                                                */
/* ------------------------------------------------------------------------- */

struct rss_source_state {
	struct papermono_rss_source source;
	struct papermono_rss_feed feed;
	char key[17];
	uint32_t generation;
	uint32_t revision;
	int result;
	bool index_loaded;
	bool pending;
	bool inflight;
};

struct rss_registry_disk {
	uint32_t magic;
	uint16_t version;
	uint16_t count;
	struct papermono_rss_source sources[PAPERMONO_RSS_SOURCE_MAX];
	uint32_t checksum;
};

struct rss_cache_identity {
	char id[PAPERMONO_RSS_SOURCE_ID_MAX + 1U];
	char url[PAPERMONO_RSS_SOURCE_URL_MAX + 1U];
};

static struct rss_source_state source_states[PAPERMONO_RSS_SOURCE_MAX] RSS_PSRAM;
static struct rss_source_state next_states[PAPERMONO_RSS_SOURCE_MAX] RSS_PSRAM;
static struct rss_registry_disk registry_block RSS_PSRAM;
static struct rss_cache_identity identity_block RSS_PSRAM;
static size_t source_count RSS_PSRAM;
static uint32_t registry_generation RSS_PSRAM;
static uint32_t revision_serial RSS_PSRAM;
static bool registry_initialized;
static bool registry_checked;
static bool registry_persisted;
struct rss_fetch_job {
    size_t slot;
    uint32_t generation;
    struct papermono_rss_source source;
    char key[17];
};

static struct papermono_rss_feed staging RSS_PSRAM;
/* Serialised index block; only touched while rss_lock is held. Keeping it out
 * of the caller stack matters for the 2 KB read/dump in body_load and on the
 * worker thread.
 */
static struct rss_index_disk index_block RSS_PSRAM;

#if !defined(__ZEPHYR__)
static char rss_host_cache_directory[160] = ".";
#endif

#if defined(__ZEPHYR__)
static K_MUTEX_DEFINE(rss_lock);

static inline void rss_lock_take(void)
{
	k_mutex_lock(&rss_lock, K_FOREVER);
}

static inline void rss_lock_release(void)
{
	k_mutex_unlock(&rss_lock);
}
#else
static inline void rss_lock_take(void)
{
}

static inline void rss_lock_release(void)
{
}
#endif

struct rss_file {
#if defined(__ZEPHYR__)
	struct fs_file_t handle;
#else
	FILE *handle;
#endif
	bool open;
};

static int rss_file_exists(const char *path);

#if defined(__ZEPHYR__)
static int rss_platform_mount(void)
{
	return papermono_content_store_poll();
}

static int rss_file_open_read(struct rss_file *file, const char *path)
{
	int ret;

	fs_file_t_init(&file->handle);
	ret = fs_open(&file->handle, path, FS_O_READ);
	if (ret < 0) {
		return ret;
	}
	file->open = true;
	return 0;
}

static int rss_file_open_write(struct rss_file *file, const char *path)
{
	int ret;

	fs_file_t_init(&file->handle);
	ret = fs_open(&file->handle, path, FS_O_CREATE | FS_O_WRITE |
						FS_O_TRUNC);
	if (ret < 0) {
		return ret;
	}
	file->open = true;
	return 0;
}

static int rss_file_close(struct rss_file *file)
{
	int ret = 0;

	if (!file->open) {
		return 0;
	}
	ret = fs_close(&file->handle);
	file->open = false;
	return ret;
}

static int rss_file_sync(struct rss_file *file)
{
	return file->open ? fs_sync(&file->handle) : -EBADF;
}

static int rss_file_write(struct rss_file *file, const void *data,
			  size_t length)
{
	const ssize_t written = fs_write(&file->handle, data, length);

	return written < 0 ? (int)written :
		(size_t)written == length ? 0 : -EIO;
}

static int rss_file_read(struct rss_file *file, void *buffer, size_t length)
{
	const ssize_t received = papermono_fs_read_chunked(&file->handle,
							   buffer, length);

	return (int)received;
}

static int rss_file_seek(struct rss_file *file, size_t offset)
{
	return fs_seek(&file->handle, (off_t)offset, FS_SEEK_SET);
}

static int rss_file_size(const char *path, size_t *size)
{
	struct fs_dirent entry;
	int ret = fs_stat(path, &entry);

	if (ret < 0) {
		return ret;
	}
	*size = entry.size;
	return 0;
}

static int rss_file_rename(const char *from, const char *to)
{
	return fs_rename(from, to);
}

static int rss_file_unlink(const char *path)
{
	return fs_unlink(path);
}

static int rss_mkdir_if_missing(const char *path)
{
	struct fs_dirent entry;
	int ret = fs_stat(path, &entry);

	if (ret == 0) {
		return entry.type == FS_DIR_ENTRY_DIR ? 0 : -EEXIST;
	}
	return fs_mkdir(path);
}
#else /* Host build: real files, so the SD cache can be round-tripped. */
static int rss_platform_mount(void)
{
	return 0;
}

static int rss_file_open_read(struct rss_file *file, const char *path)
{
	file->handle = fopen(path, "rb");
	if (file->handle == NULL) {
		return -errno;
	}
	file->open = true;
	return 0;
}

static int rss_file_open_write(struct rss_file *file, const char *path)
{
	file->handle = fopen(path, "wb");
	if (file->handle == NULL) {
		return -errno;
	}
	file->open = true;
	return 0;
}

static int rss_file_close(struct rss_file *file)
{
	int ret = 0;

	if (!file->open) {
		return 0;
	}
	if (fclose(file->handle) != 0) {
		ret = -EIO;
	}
	file->open = false;
	return ret;
}

static int rss_file_sync(struct rss_file *file)
{
	return file->open && fflush(file->handle) == 0 ? 0 : -EIO;
}

static int rss_file_write(struct rss_file *file, const void *data,
			  size_t length)
{
	return fwrite(data, 1U, length, file->handle) == length ? 0 : -EIO;
}

static int rss_file_read(struct rss_file *file, void *buffer, size_t length)
{
	const size_t received = fread(buffer, 1U, length, file->handle);

	if (received == 0U && ferror(file->handle)) {
		return -EIO;
	}
	return (int)received;
}

static int rss_file_seek(struct rss_file *file, size_t offset)
{
	return fseek(file->handle, (long)offset, SEEK_SET) == 0 ? 0 : -EIO;
}

static int rss_file_size(const char *path, size_t *size)
{
	FILE *handle = fopen(path, "rb");
	long end;

	if (handle == NULL) {
		return -errno;
	}
	if (fseek(handle, 0L, SEEK_END) != 0) {
		(void)fclose(handle);
		return -EIO;
	}
	end = ftell(handle);
	(void)fclose(handle);
	if (end < 0) {
		return -EIO;
	}
	*size = (size_t)end;
	return 0;
}

static int rss_file_rename(const char *from, const char *to)
{
	return rename(from, to) == 0 ? 0 : -errno;
}

static int rss_file_unlink(const char *path)
{
	return remove(path) == 0 ? 0 : -errno;
}
#endif

static int rss_build_path(const char *key, char *out, size_t capacity,
                          const char *leaf)
{
#if defined(__ZEPHYR__)
    const int written = key != NULL && key[0] != '\0' ?
        snprintf(out, capacity, "%s/rss/%s/%s", PAPERMONO_CONTENT_ROOT, key, leaf) :
        snprintf(out, capacity, "%s/rss/%s", PAPERMONO_CONTENT_ROOT, leaf);
#else
    const int written = key != NULL && key[0] != '\0' ?
        snprintf(out, capacity, "%s/papermono/rss/%s/%s", rss_host_cache_directory, key, leaf) :
        snprintf(out, capacity, "%s/papermono/rss/%s", rss_host_cache_directory, leaf);
#endif
    return written < 0 || (size_t)written >= capacity ? -ENAMETOOLONG : 0;
}

#if !defined(__ZEPHYR__)
#include <sys/stat.h>
static int rss_mkdir_if_missing(const char *path)
{
    return mkdir(path, 0700) == 0 || errno == EEXIST ? 0 : -errno;
}
#endif

static int rss_ensure_directory(const char *key)
{
    char path[RSS_CACHE_PATH_MAX];
    int ret;
#if defined(__ZEPHYR__)
    ret = rss_mkdir_if_missing(PAPERMONO_CONTENT_ROOT);
#else
    const int length = snprintf(path, sizeof(path), "%s/papermono", rss_host_cache_directory);
    if (length < 0 || (size_t)length >= sizeof(path)) return -ENAMETOOLONG;
    ret = rss_mkdir_if_missing(path);
#endif
    if (ret < 0) return ret;
    ret = rss_build_path(NULL, path, sizeof(path), "");
    if (ret < 0) return ret;
    ret = rss_mkdir_if_missing(path);
    if (ret < 0 || key == NULL || key[0] == '\0') return ret;
    ret = rss_build_path(NULL, path, sizeof(path), key);
    return ret < 0 ? ret : rss_mkdir_if_missing(path);
}

static int rss_unlink_leaf(const char *key, const char *leaf)
{
	char path[RSS_CACHE_PATH_MAX];
	int ret = rss_build_path(key, path, sizeof(path), leaf);

	if (ret < 0) {
		return ret;
	}
	return rss_file_unlink(path);
}

static int rss_leaf_exists(const char *key, const char *leaf)
{
	char path[RSS_CACHE_PATH_MAX];
	int ret = rss_build_path(key, path, sizeof(path), leaf);

	if (ret < 0) {
		return ret;
	}
	return rss_file_exists(path);
}

static int rss_leaf_size(const char *key, const char *leaf, size_t *size)
{
	char path[RSS_CACHE_PATH_MAX];
	int ret = rss_build_path(key, path, sizeof(path), leaf);

	if (ret < 0) {
		return ret;
	}
	return rss_file_size(path, size);
}

static int rss_leaf_rename(const char *key, const char *from_leaf, const char *to_leaf)
{
	char from[RSS_CACHE_PATH_MAX];
	char to[RSS_CACHE_PATH_MAX];
	int ret = rss_build_path(key, from, sizeof(from), from_leaf);

	if (ret < 0) {
		return ret;
	}
	ret = rss_build_path(key, to, sizeof(to), to_leaf);
	if (ret < 0) {
		return ret;
	}
	return rss_file_rename(from, to);
}

static int rss_file_exists(const char *path)
{
	size_t size;

	return rss_file_size(path, &size) == 0;
}

static int rss_write_file(const char *key, const char *leaf, const void *data, size_t length,
			  uint8_t *bounce)
{
	char path[RSS_CACHE_PATH_MAX];
	struct rss_file file = { 0 };
	int ret;

	ret = rss_build_path(key, path, sizeof(path), leaf);
	if (ret < 0) {
		return ret;
	}
	ret = rss_file_open_write(&file, path);
	if (ret < 0) {
		return ret;
	}
	while (length > 0U) {
		const size_t chunk = RSS_MIN(length, RSS_WRITE_BOUNCE);

		memcpy(bounce, data, chunk);
		ret = rss_file_write(&file, bounce, chunk);
		if (ret < 0) {
			break;
		}
		data = (const uint8_t *)data + chunk;
		length -= chunk;
	}
	if (ret == 0) {
		ret = rss_file_sync(&file);
	}
	{
		const int close_ret = rss_file_close(&file);

		if (ret == 0) {
			ret = close_ret;
		}
	}
	if (ret < 0) {
		(void)rss_file_unlink(path);
	}
	return ret;
}

static int rss_read_file(const char *key, const char *leaf, void *data, size_t capacity,
			 size_t *length)
{
	char path[RSS_CACHE_PATH_MAX];
	struct rss_file file = { 0 };
	size_t done = 0U;
	int ret;

	ret = rss_build_path(key, path, sizeof(path), leaf);
	if (ret < 0) {
		return ret;
	}
	ret = rss_file_open_read(&file, path);
	if (ret < 0) {
		return ret;
	}
	while (done < capacity) {
		const int received = rss_file_read(&file, (uint8_t *)data + done,
						   capacity - done);

		if (received < 0) {
			ret = received;
			break;
		}
		if (received == 0) {
			break;
		}
		done += (size_t)received;
	}
	{
		const int close_ret = rss_file_close(&file);

		if (ret == 0) {
			ret = close_ret;
		}
	}
	if (ret < 0) {
		return ret;
	}
	*length = done;
	return 0;
}

/*
 * Replace `final` with `tmp`, keeping the previous file in `bak` so the caller
 * can restore it if the companion file cannot be committed too.
 */
static int rss_swap_file(const char *key, const char *tmp_leaf, const char *final_leaf,
			 const char *bak_leaf, bool *had_backup)
{
	bool backup = false;
	int ret;

	*had_backup = false;
	if (rss_leaf_exists(key, final_leaf) == 1) {
		(void)rss_unlink_leaf(key, bak_leaf);
		ret = rss_leaf_rename(key, final_leaf, bak_leaf);
		if (ret < 0) {
			return ret;
		}
		backup = true;
	}
	ret = rss_leaf_rename(key, tmp_leaf, final_leaf);
	if (ret < 0) {
		if (backup) {
			(void)rss_leaf_rename(key, bak_leaf, final_leaf);
		}
		return ret;
	}
	*had_backup = backup;
	return 0;
}

/* Recover a cache left behind by a reset in the middle of a commit. */
static void rss_cache_recover(const char *key)
{
	const int bodies_bak = rss_leaf_exists(key, RSS_BODIES_BAK_FILE);
	const int index_bak = rss_leaf_exists(key, RSS_INDEX_BAK_FILE);

	if (bodies_bak == 1) {
		if (index_bak == 1) {
			/* Both files were already replaced; the leftovers are
			 * stale backups.
			 */
			(void)rss_unlink_leaf(key, RSS_BODIES_BAK_FILE);
			(void)rss_unlink_leaf(key, RSS_INDEX_BAK_FILE);
		} else {
			/* Bodies were replaced but the index was not: roll the
			 * bodies back so the old index still matches.
			 */
			(void)rss_unlink_leaf(key, RSS_BODIES_FILE);
			(void)rss_leaf_rename(key, RSS_BODIES_BAK_FILE,
					      RSS_BODIES_FILE);
		}
	} else if (index_bak == 1) {
		(void)rss_unlink_leaf(key, RSS_INDEX_BAK_FILE);
	}
	(void)rss_unlink_leaf(key, RSS_INDEX_TMP_FILE);
	(void)rss_unlink_leaf(key, RSS_BODIES_TMP_FILE);
}

#define RSS_REGISTRY_MAGIC 0x52535353UL
#define RSS_REGISTRY_VERSION 1U
#define RSS_SOURCES_FILE "sources.bin"
#define RSS_SOURCES_TMP_FILE "sources.tmp"
#define RSS_SOURCES_BAK_FILE "sources.bak"
#define RSS_IDENTITY_FILE "source.bin"
#define RSS_IDENTITY_TMP_FILE "source.tmp"

static uint32_t rss_checksum(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t hash = 2166136261U;
    for (size_t i = 0U; i < length; ++i) hash = (hash ^ bytes[i]) * 16777619U;
    return hash;
}

static uint32_t rss_next_revision(void)
{
    if (++revision_serial == 0U) ++revision_serial;
    return revision_serial;
}

static bool rss_source_equal(const struct papermono_rss_source *a,
                             const struct papermono_rss_source *b)
{
    return strcmp(a->id, b->id) == 0 && strcmp(a->url, b->url) == 0;
}

static void rss_source_copy(struct papermono_rss_source *out,
                            const struct papermono_rss_source *source)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->id, source->id, strlen(source->id));
    memcpy(out->name, source->name, strlen(source->name));
    memcpy(out->url, source->url, strlen(source->url));
}

static size_t rss_bounded_length(const char *text, size_t capacity)
{
    size_t length = 0U;
    while (length < capacity && text[length] != '\0') ++length;
    return length;
}

static bool rss_name_valid(const char *text, size_t length)
{
    bool visible = false;
    for (size_t i = 0U; i < length;) {
        const uint8_t lead = (uint8_t)text[i];
        uint32_t point;
        size_t bytes;
        uint32_t minimum;
        if (lead < 0x80U) { point = lead; bytes = 1U; minimum = 0U; }
        else if (lead >= 0xc2U && lead <= 0xdfU) {
            point = lead & 0x1fU; bytes = 2U; minimum = 0x80U;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            point = lead & 0x0fU; bytes = 3U; minimum = 0x800U;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            point = lead & 0x07U; bytes = 4U; minimum = 0x10000U;
        } else return false;
        if (i + bytes > length) return false;
        for (size_t j = 1U; j < bytes; ++j) {
            const uint8_t next = (uint8_t)text[i + j];
            if ((next & 0xc0U) != 0x80U) return false;
            point = (point << 6) | (next & 0x3fU);
        }
        if (point < minimum || point > 0x10ffffU ||
            (point >= 0xd800U && point <= 0xdfffU) || point < 32U ||
            (point >= 0x7fU && point <= 0x9fU)) return false;
        if (point != 32U && point != 0xa0U && point != 0x3000U) visible = true;
        i += bytes;
    }
    return visible;
}

static bool rss_source_valid(const struct papermono_rss_source *source)
{
    const size_t id = rss_bounded_length(source->id, sizeof(source->id));
    const size_t name = rss_bounded_length(source->name, sizeof(source->name));
    const size_t url = rss_bounded_length(source->url, sizeof(source->url));
    if (id == 0U || id >= sizeof(source->id) || name == 0U ||
        name >= sizeof(source->name) || url < 8U || url >= sizeof(source->url) ||
        strncmp(source->url, "http://", 7U) != 0) return false;
    for (size_t i = 0U; i < id; ++i) {
        const char ch = source->id[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) return false;
    }
    if (!rss_name_valid(source->name, name)) return false;
    for (size_t i = 0U; i < url; ++i) {
        if ((uint8_t)source->url[i] <= 32U || (uint8_t)source->url[i] >= 127U ||
            source->url[i] == '\\' || source->url[i] == '#') return false;
    }
    const char *authority = source->url + 7U;
    const char *end = strchr(authority, '/');
    if (end == NULL) end = source->url + url;
    const char *colon = memchr(authority, ':', (size_t)(end - authority));
    const char *host_end = colon != NULL ? colon : end;
    if (host_end == authority || (size_t)(host_end - authority) > RSS_HOST_MAX) return false;
    for (const char *p = authority; p < host_end; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '.')) return false;
    }
    if (colon != NULL) {
        unsigned port = 0U;
        if (colon + 1U == end || end - colon > 6) return false;
        for (const char *p = colon + 1U; p < end; ++p) {
            if (*p < '0' || *p > '9') return false;
            port = port * 10U + (unsigned)(*p - '0');
            if (port > 65535U) return false;
        }
        if (port == 0U) return false;
    }
    return (size_t)(source->url + url - end) <= RSS_PATH_MAX;
}

static bool rss_sources_valid(const struct papermono_rss_source *sources, size_t count)
{
    if (count > PAPERMONO_RSS_SOURCE_MAX || (count != 0U && sources == NULL)) return false;
    for (size_t i = 0U; i < count; ++i) {
        if (!rss_source_valid(&sources[i])) return false;
        for (size_t j = 0U; j < i; ++j) {
            if (strcmp(sources[i].id, sources[j].id) == 0 ||
                strcmp(sources[i].url, sources[j].url) == 0) return false;
        }
    }
    return true;
}

static void rss_source_key(const struct papermono_rss_source *source, char key[17])
{
    uint64_t hash = UINT64_C(14695981039346656037);
    const char *values[] = { source->id, source->url };
    for (size_t i = 0U; i < 2U; ++i) {
        for (const unsigned char *p = (const unsigned char *)values[i]; *p; ++p)
            hash = (hash ^ *p) * UINT64_C(1099511628211);
        hash = (hash ^ 0U) * UINT64_C(1099511628211);
    }
    (void)snprintf(key, 17U, "%08x%08x", (unsigned)(hash >> 32), (unsigned)hash);
#if !defined(__ZEPHYR__)
    /* Existing parser/cache tests exercise the old root-level cache through
     * their legacy source. Explicit multi-source tests use keyed directories. */
    if (strcmp(source->id, "legacy") == 0 &&
        strcmp(source->url, "http://legacy.test/rss") == 0) key[0] = '\0';
#endif
}

static void rss_apply_sources_locked(const struct papermono_rss_source *sources,
                                    size_t count)
{
    uint32_t generation = registry_generation + 1U;
    if (generation == 0U) ++generation;
    memset(next_states, 0, sizeof(next_states));
    for (size_t i = 0U; i < count; ++i) {
        bool same_slot = false;
        bool requested = false;
        for (size_t j = 0U; j < source_count; ++j) {
            if (rss_source_equal(&sources[i], &source_states[j].source)) {
                next_states[i] = source_states[j];
                same_slot = i == j;
                requested = source_states[j].pending || source_states[j].inflight;
                break;
            }
        }
        rss_source_copy(&next_states[i].source, &sources[i]);
        rss_source_key(&sources[i], next_states[i].key);
        if (!same_slot) {
            next_states[i].generation = generation;
            /* Preserve an explicit request when its stable source moves to
             * another tab slot; its old job cannot publish to the new slot. */
            next_states[i].pending = requested;
            next_states[i].inflight = false;
            if (requested) next_states[i].result = 1;
            else if (next_states[i].result > 0) next_states[i].result = 0;
        }
    }
    memcpy(source_states, next_states, sizeof(source_states));
    source_count = count;
    registry_generation = generation;
}

static bool rss_registry_block_valid(size_t length)
{
    return length == sizeof(registry_block) &&
        registry_block.magic == RSS_REGISTRY_MAGIC &&
        registry_block.version == RSS_REGISTRY_VERSION &&
        registry_block.count <= PAPERMONO_RSS_SOURCE_MAX &&
        registry_block.checksum == rss_checksum(&registry_block,
            offsetof(struct rss_registry_disk, checksum)) &&
        rss_sources_valid(registry_block.sources, registry_block.count);
}

static int rss_registry_read(const char *leaf)
{
    size_t size = 0U;
    size_t length = 0U;
    int ret = rss_leaf_size(NULL, leaf, &size);
    if (ret < 0) return ret;
    if (size != sizeof(registry_block)) return -EBADMSG;
    ret = rss_read_file(NULL, leaf, &registry_block, sizeof(registry_block), &length);
    return ret < 0 ? ret : rss_registry_block_valid(length) ? 0 : -EBADMSG;
}

static void rss_registry_defaults_locked(void)
{
    size_t count = 0U;
    memset(&registry_block, 0, sizeof(registry_block));
#if defined(__ZEPHYR__)
    const char *legacy_url = papermono_settings_get()->rss_url;
    if (legacy_url[0] != '\0') {
        struct papermono_rss_source *source = &registry_block.sources[count];
        (void)snprintf(source->id, sizeof(source->id), "gold");
        (void)snprintf(source->name, sizeof(source->name), "金价");
        (void)snprintf(source->url, sizeof(source->url), "%s", legacy_url);
        if (rss_source_valid(source)) ++count;
    }
#ifdef CONFIG_PAPERMONO_RSS_AI_URL
    if (CONFIG_PAPERMONO_RSS_AI_URL[0] != '\0') {
        struct papermono_rss_source *source = &registry_block.sources[count];
        (void)snprintf(source->id, sizeof(source->id), "ai");
        (void)snprintf(source->name, sizeof(source->name), "AI资讯");
        (void)snprintf(source->url, sizeof(source->url), "%s", CONFIG_PAPERMONO_RSS_AI_URL);
        if (rss_source_valid(source)) ++count;
    }
#endif
#else
    struct papermono_rss_source *source = &registry_block.sources[count++];
    (void)snprintf(source->id, sizeof(source->id), "legacy");
    (void)snprintf(source->name, sizeof(source->name), "RSS");
    (void)snprintf(source->url, sizeof(source->url), "http://legacy.test/rss");
#endif
    rss_apply_sources_locked(registry_block.sources, count);
}

static void rss_registry_ensure_locked(void)
{
    int ret;
    bool final_valid;
    if (!registry_initialized) {
        registry_initialized = true;
        rss_registry_defaults_locked();
    }
    if (registry_checked || rss_platform_mount() < 0) return;
    registry_checked = true;
    if (rss_leaf_exists(NULL, RSS_SOURCES_FILE) != 1 &&
        rss_leaf_exists(NULL, RSS_SOURCES_BAK_FILE) == 1)
        (void)rss_leaf_rename(NULL, RSS_SOURCES_BAK_FILE, RSS_SOURCES_FILE);
    (void)rss_unlink_leaf(NULL, RSS_SOURCES_TMP_FILE);
    ret = rss_registry_read(RSS_SOURCES_FILE);
    final_valid = ret == 0;
    if (ret < 0) {
        if (rss_registry_read(RSS_SOURCES_BAK_FILE) < 0) return;
        (void)rss_unlink_leaf(NULL, RSS_SOURCES_FILE);
        final_valid = rss_leaf_rename(NULL, RSS_SOURCES_BAK_FILE, RSS_SOURCES_FILE) == 0;
    }
    rss_apply_sources_locked(registry_block.sources, registry_block.count);
    registry_persisted = true;
    if (final_valid) (void)rss_unlink_leaf(NULL, RSS_SOURCES_BAK_FILE);
}

static bool rss_job_current_locked(const struct rss_fetch_job *job)
{
    return job->slot < source_count &&
        source_states[job->slot].generation == job->generation &&
        rss_source_equal(&source_states[job->slot].source, &job->source);
}

static void rss_job_copy_locked(size_t source, struct rss_fetch_job *job)
{
    job->slot = source;
    job->generation = source_states[source].generation;
    job->source = source_states[source].source;
    memcpy(job->key, source_states[source].key, sizeof(job->key));
}

static int rss_cache_identity(const struct papermono_rss_source *source,
                              const char *key, bool create, uint8_t *bounce)
{
    size_t length = 0U;
    int ret;
#if !defined(__ZEPHYR__)
    if (key[0] == '\0') return 0;
#endif
    ret = rss_read_file(key, RSS_IDENTITY_FILE, &identity_block, sizeof(identity_block), &length);
    if (ret == 0) {
        size_t size = 0U;
        return rss_leaf_size(key, RSS_IDENTITY_FILE, &size) == 0 &&
            size == sizeof(identity_block) && length == sizeof(identity_block) &&
            memchr(identity_block.id, '\0', sizeof(identity_block.id)) != NULL &&
            memchr(identity_block.url, '\0', sizeof(identity_block.url)) != NULL &&
            strcmp(identity_block.id, source->id) == 0 &&
            strcmp(identity_block.url, source->url) == 0 ? 0 : -EBADMSG;
    }
    if (!create || ret != -ENOENT) return ret;
    if (rss_leaf_exists(key, RSS_INDEX_FILE) == 1 ||
        rss_leaf_exists(key, RSS_BODIES_FILE) == 1) return -EBADMSG;
    memset(&identity_block, 0, sizeof(identity_block));
    memcpy(identity_block.id, source->id, strlen(source->id));
    memcpy(identity_block.url, source->url, strlen(source->url));
    ret = rss_write_file(key, RSS_IDENTITY_TMP_FILE, &identity_block,
                         sizeof(identity_block), bounce);
    return ret < 0 ? ret : rss_leaf_rename(key, RSS_IDENTITY_TMP_FILE, RSS_IDENTITY_FILE);
}

static void rss_feed_to_disk(const struct papermono_rss_feed *feed,
			     struct rss_index_disk *disk)
{
	memset(disk, 0, sizeof(*disk));
	disk->magic = RSS_INDEX_MAGIC;
	disk->version = RSS_INDEX_VERSION;
	disk->count = (uint16_t)RSS_MIN(feed->count, PAPERMONO_RSS_ITEM_MAX);
	disk->updated_at = feed->updated_at;
	memcpy(disk->title, feed->title, sizeof(disk->title));
	for (size_t i = 0U; i < disk->count; ++i) {
		memcpy(disk->items[i].title, feed->items[i].title,
		       sizeof(disk->items[i].title));
		memcpy(disk->items[i].published, feed->items[i].published,
		       sizeof(disk->items[i].published));
		disk->items[i].body_length = feed->items[i].body_length;
	}
}

static int rss_cache_load(struct rss_source_state *state)
{
	const char *key = state->key;
	struct papermono_rss_feed *snapshot = &state->feed;
	struct rss_index_disk *disk = &index_block;
	size_t read = 0U;
	size_t bodies_size = 0U;
	size_t total = 0U;
	uint64_t total_check = 0U;
	int ret;

	rss_cache_recover(key);
	memset(snapshot, 0, sizeof(*snapshot));
	ret = rss_cache_identity(&state->source, key, false, NULL);
	if (ret < 0) return ret;
	ret = rss_read_file(key, RSS_INDEX_FILE, disk, sizeof(*disk), &read);
	if (ret < 0) {
		return ret;
	}
	if (read != sizeof(*disk) || disk->magic != RSS_INDEX_MAGIC ||
	    disk->version != RSS_INDEX_VERSION ||
	    disk->count > PAPERMONO_RSS_ITEM_MAX) {
		return -EBADMSG;
	}
	for (size_t i = 0U; i < disk->count; ++i) {
		if (disk->items[i].body_length > PAPERMONO_RSS_BODY_MAX) {
			return -EBADMSG;
		}
		total_check += disk->items[i].body_length;
	}
	if (total_check > (uint64_t)RSS_BODIES_TOTAL_MAX) {
		return -EBADMSG;
	}
	total = (size_t)total_check;
	if (total > 0U) {
		ret = rss_leaf_size(key, RSS_BODIES_FILE, &bodies_size);
		if (ret < 0 || bodies_size < total) {
			return -EBADMSG;
		}
	}
	snapshot->valid = true;
	snapshot->updated_at = disk->updated_at;
	memcpy(snapshot->title, disk->title, sizeof(snapshot->title));
	snapshot->title[PAPERMONO_RSS_TITLE_MAX] = '\0';
	snapshot->count = disk->count;
	for (size_t i = 0U; i < disk->count; ++i) {
		memcpy(snapshot->items[i].title, disk->items[i].title,
		       sizeof(snapshot->items[i].title));
		snapshot->items[i].title[PAPERMONO_RSS_TITLE_MAX] = '\0';
		memcpy(snapshot->items[i].published, disk->items[i].published,
		       sizeof(snapshot->items[i].published));
		snapshot->items[i]
			.published[PAPERMONO_RSS_DATE_MAX] = '\0';
		snapshot->items[i].body_length = disk->items[i].body_length;
	}
	state->revision = rss_next_revision();
	return 0;
}

/*
 * Load the SD index into RAM once. Returns 1 when the cache was loaded now,
 * 0 when it was already loaded, -ENODEV when the SD card is not available
 * (retried on the next call) and another negative errno when the cache is
 * missing or corrupt (also latched; a fetch will replace it).
 */
static int rss_ensure_index(struct rss_source_state *state)
{
	int ret;

	if (state->index_loaded) {
		return 0;
	}
	ret = rss_platform_mount();
	if (ret < 0) {
		return -ENODEV;
	}
	ret = rss_cache_load(state);
	state->index_loaded = true;
	if (ret < 0) {
		RSS_LOG_WRN("RSS SD cache unusable (%d); RAM index stays empty",
			    ret);
		return ret;
	}
	RSS_LOG_INF("RSS cache loaded: %u items, updated_at %lld",
		    (unsigned int)state->feed.count,
		    (long long)state->feed.updated_at);
	return 1;
}

struct rss_body_writer {
	const char *key;
	struct rss_file file;
	uint8_t *bounce;
	size_t bounce_used;
	bool open;
};

static int rss_body_writer_flush(struct rss_body_writer *writer)
{
	int ret;

	if (writer->bounce_used == 0U) {
		return 0;
	}
	ret = rss_file_write(&writer->file, writer->bounce,
			     writer->bounce_used);
	if (ret < 0) {
		return ret;
	}
	writer->bounce_used = 0U;
	return 0;
}

static int rss_body_writer_emit(void *context, const char *data,
				size_t length)
{
	struct rss_body_writer *writer = context;

	while (length > 0U) {
		const size_t space = RSS_WRITE_BOUNCE - writer->bounce_used;
		const size_t chunk = RSS_MIN(space, length);

		memcpy(writer->bounce + writer->bounce_used, data, chunk);
		writer->bounce_used += chunk;
		data += chunk;
		length -= chunk;
		if (writer->bounce_used == RSS_WRITE_BOUNCE &&
		    rss_body_writer_flush(writer) < 0) {
			return -EIO;
		}
	}
	return 0;
}

static int rss_body_writer_begin(struct rss_body_writer *writer,
				 uint8_t *bounce, const char *key)
{
	char path[RSS_CACHE_PATH_MAX];
	int ret;

	memset(writer, 0, sizeof(*writer));
	writer->bounce = bounce;
	writer->key = key;
	ret = rss_build_path(key, path, sizeof(path), RSS_BODIES_TMP_FILE);
	if (ret < 0) {
		return ret;
	}
	ret = rss_file_open_write(&writer->file, path);
	if (ret < 0) {
		return ret;
	}
	writer->open = true;
	return 0;
}

static int rss_body_writer_finish(struct rss_body_writer *writer)
{
	int ret = rss_body_writer_flush(writer);

	if (ret == 0) {
		ret = rss_file_sync(&writer->file);
	}
	{
		const int close_ret = rss_file_close(&writer->file);

		writer->open = false;
		if (ret == 0) {
			ret = close_ret;
		}
	}
	return ret;
}

static void rss_body_writer_abort(struct rss_body_writer *writer)
{
	const char *key = writer->key;
	char path[RSS_CACHE_PATH_MAX];

	if (writer->open) {
		(void)rss_file_close(&writer->file);
		writer->open = false;
	}
	if (rss_build_path(key, path, sizeof(path), RSS_BODIES_TMP_FILE) == 0) {
		(void)rss_file_unlink(path);
	}
}

static int rss_body_append(void *context, const char *raw, size_t raw_length,
			   bool literal, const char *item_title,
			   size_t plain_length)
{
	struct rss_body_writer *writer = context;
	struct rss_sink sink = {
		.emit = rss_body_writer_emit,
		.context = writer,
		.stored = 0U,
		.total = 0U,
		.limit = RSS_BODY_LIMIT,
		.overflow = false,
		.failed = false,
	};

	if (!writer->open) {
		return -EBADF;
	}
	rss_render_body(raw, raw_length, literal, item_title, &sink);
	if (sink.failed) {
		return -EIO;
	}
	if (sink.stored != plain_length) {
		RSS_LOG_WRN("RSS body render mismatch (%u != %u)",
			    (unsigned int)sink.stored,
			    (unsigned int)plain_length);
		return -EIO;
	}
	return 0;
}

static uint8_t *rss_write_bounce_get(void)
{
#if defined(__ZEPHYR__)
	static uint8_t *bounce;

	if (bounce == NULL) {
		bounce = k_malloc(RSS_WRITE_BOUNCE);
	}
	return bounce;
#else
	static uint8_t bounce[RSS_WRITE_BOUNCE];

	return bounce;
#endif
}

static int rss_cache_commit(const struct rss_fetch_job *job,
			    const struct papermono_rss_feed *feed,
			    const uint8_t *bounce)
{
	const char *key = job->key;
	bool bodies_backup = false;
	bool index_backup = false;
	int ret;

	rss_lock_take();
	if (!rss_job_current_locked(job)) {
		(void)rss_unlink_leaf(key, RSS_INDEX_TMP_FILE);
		(void)rss_unlink_leaf(key, RSS_BODIES_TMP_FILE);
		rss_lock_release();
		return -ESTALE;
	}
	rss_feed_to_disk(feed, &index_block);
	ret = rss_write_file(key, RSS_INDEX_TMP_FILE, &index_block,
			     sizeof(index_block), (uint8_t *)bounce);
	if (ret < 0) {
		(void)rss_unlink_leaf(key, RSS_INDEX_TMP_FILE);
		(void)rss_unlink_leaf(key, RSS_BODIES_TMP_FILE);
		rss_lock_release();
		return ret;
	}
	ret = rss_swap_file(key, RSS_BODIES_TMP_FILE, RSS_BODIES_FILE,
			    RSS_BODIES_BAK_FILE, &bodies_backup);
	if (ret < 0) {
		(void)rss_unlink_leaf(key, RSS_INDEX_TMP_FILE);
		(void)rss_unlink_leaf(key, RSS_BODIES_TMP_FILE);
		rss_lock_release();
		return ret;
	}
	ret = rss_swap_file(key, RSS_INDEX_TMP_FILE, RSS_INDEX_FILE,
			    RSS_INDEX_BAK_FILE, &index_backup);
	if (ret < 0) {
		if (bodies_backup) {
			(void)rss_unlink_leaf(key, RSS_BODIES_FILE);
			(void)rss_leaf_rename(key, RSS_BODIES_BAK_FILE,
					      RSS_BODIES_FILE);
		}
		(void)rss_unlink_leaf(key, RSS_INDEX_TMP_FILE);
		rss_lock_release();
		return ret;
	}
	if (bodies_backup) {
		(void)rss_unlink_leaf(key, RSS_BODIES_BAK_FILE);
	}
	if (index_backup) {
		(void)rss_unlink_leaf(key, RSS_INDEX_BAK_FILE);
	}
	source_states[job->slot].feed = *feed;
	source_states[job->slot].index_loaded = true;
	source_states[job->slot].revision = rss_next_revision();
	rss_lock_release();
	return 0;
}

/*
 * Parse @p xml and replace the SD cache with the result. The old cache is left
 * untouched unless every step succeeds.
 */
static int rss_cache_store(const struct rss_fetch_job *job, const char *xml, size_t length, int64_t updated_at,
			   bool truncated)
{
	const char *key = job->key;
	struct rss_body_writer writer;
	struct rss_body_consumer consumer = {
		.append = rss_body_append,
		.context = &writer,
	};
	uint8_t *bounce = rss_write_bounce_get();
	int ret;

	if (bounce == NULL) {
		return -ENOMEM;
	}
	ret = rss_ensure_directory(key);
	if (ret < 0) {
		return ret;
	}
	rss_lock_take();
	ret = rss_job_current_locked(job) ?
		rss_cache_identity(&job->source, key, true, bounce) : -ESTALE;
	rss_lock_release();
	if (ret < 0) return ret;
	ret = rss_body_writer_begin(&writer, bounce, key);
	if (ret < 0) {
		return ret;
	}
	ret = rss_parse_feed(xml, length, &staging, &consumer, truncated);
	if (ret < 0) {
		rss_body_writer_abort(&writer);
		return ret;
	}
	staging.updated_at = updated_at;
	ret = rss_body_writer_finish(&writer);
	if (ret < 0) {
		rss_body_writer_abort(&writer);
		return ret;
	}
	ret = rss_cache_commit(job, &staging, bounce);
	if (ret < 0) {
		return ret;
	}
	RSS_LOG_INF("RSS cache updated: %u items, %u feed bytes",
		    (unsigned int)staging.count, (unsigned int)length);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Public data API                                                            */
/* ------------------------------------------------------------------------- */

size_t papermono_rss_source_count(void)
{
    size_t count;
    rss_lock_take();
    rss_registry_ensure_locked();
    count = source_count;
    rss_lock_release();
    return count;
}

int papermono_rss_source_get(size_t source, struct papermono_rss_source *out)
{
    if (out == NULL) return -EINVAL;
    rss_lock_take();
    rss_registry_ensure_locked();
    const int ret = source < source_count ? 0 : -ENOENT;
    if (ret == 0) *out = source_states[source].source;
    else memset(out, 0, sizeof(*out));
    rss_lock_release();
    return ret;
}

uint32_t papermono_rss_sources_generation(void)
{
    uint32_t generation;
    rss_lock_take();
    rss_registry_ensure_locked();
    generation = registry_generation;
    rss_lock_release();
    return generation;
}

int papermono_rss_sources_replace(const struct papermono_rss_source *sources,
                                 size_t count)
{
    uint8_t bounce[RSS_WRITE_BOUNCE];
    bool backup = false;
    bool same;
    int ret;
    if (!rss_sources_valid(sources, count)) return -EINVAL;
    rss_lock_take();
    rss_registry_ensure_locked();
    same = count == source_count;
    for (size_t i = 0U; same && i < count; ++i) {
        same = rss_source_equal(&sources[i], &source_states[i].source) &&
            strcmp(sources[i].name, source_states[i].source.name) == 0;
    }
    if (same && registry_persisted) {
        rss_lock_release();
        return 0;
    }
    ret = rss_platform_mount();
    if (ret < 0) {
        rss_lock_release();
        return -ENODEV;
    }
    ret = rss_ensure_directory(NULL);
    if (ret < 0) {
        rss_lock_release();
        return ret;
    }
    memset(&registry_block, 0, sizeof(registry_block));
    registry_block.magic = RSS_REGISTRY_MAGIC;
    registry_block.version = RSS_REGISTRY_VERSION;
    registry_block.count = (uint16_t)count;
    for (size_t i = 0U; i < count; ++i) rss_source_copy(&registry_block.sources[i], &sources[i]);
    registry_block.checksum = rss_checksum(&registry_block,
        offsetof(struct rss_registry_disk, checksum));
    ret = rss_write_file(NULL, RSS_SOURCES_TMP_FILE, &registry_block,
                         sizeof(registry_block), bounce);
    if (ret == 0) ret = rss_swap_file(NULL, RSS_SOURCES_TMP_FILE, RSS_SOURCES_FILE,
                                      RSS_SOURCES_BAK_FILE, &backup);
    if (ret == 0) {
        rss_apply_sources_locked(registry_block.sources, count);
        registry_checked = true;
        registry_persisted = true;
        if (backup) (void)rss_unlink_leaf(NULL, RSS_SOURCES_BAK_FILE);
    } else {
        (void)rss_unlink_leaf(NULL, RSS_SOURCES_TMP_FILE);
    }
    rss_lock_release();
#if defined(__ZEPHYR__)
    if (ret == 0) papermono_ui_request_content_refresh();
#endif
    return ret;
}

uint32_t papermono_rss_revision(size_t source)
{
    uint32_t revision = 0U;
    rss_lock_take();
    rss_registry_ensure_locked();
    if (source < source_count) revision = source_states[source].revision;
    rss_lock_release();
    return revision;
}

void papermono_rss_get_source(size_t source, struct papermono_rss_feed *out)
{
    if (out == NULL) return;
    rss_lock_take();
    rss_registry_ensure_locked();
    if (source < source_count) {
        (void)rss_ensure_index(&source_states[source]);
        *out = source_states[source].feed;
    } else memset(out, 0, sizeof(*out));
    rss_lock_release();
}

void papermono_rss_get(struct papermono_rss_feed *out)
{
    papermono_rss_get_source(0U, out);
}

int papermono_rss_body_load_source(size_t source, size_t index, char *buffer,
                                  size_t capacity)
{
    struct rss_file file = { 0 };
    size_t offset = 0U, done = 0U;
    int ret;
    if (buffer == NULL || capacity == 0U) return -EINVAL;
    buffer[0] = '\0';
    rss_lock_take();
    rss_registry_ensure_locked();
    if (source >= source_count) {
        rss_lock_release();
        return -ENOENT;
    }
    struct rss_source_state *state = &source_states[source];
    const char *key = state->key;
    ret = rss_ensure_index(state);
    if (ret == -ENODEV || !state->feed.valid || index >= state->feed.count) {
        rss_lock_release();
        return -ENOENT;
    }
    for (size_t i = 0U; i < index; ++i) offset += state->feed.items[i].body_length;
    const size_t length = state->feed.items[index].body_length;
    const size_t to_read = RSS_MIN(length, capacity - 1U);
    if (to_read > 0U) {
        char path[RSS_CACHE_PATH_MAX];
        ret = rss_build_path(key, path, sizeof(path), RSS_BODIES_FILE);
        if (ret == 0) ret = rss_file_open_read(&file, path);
        if (ret == 0) ret = rss_file_seek(&file, offset);
        while (ret == 0 && done < to_read) {
            const int received = rss_file_read(&file, buffer + done, to_read - done);
            if (received < 0) { ret = received; break; }
            if (received == 0) { ret = -EIO; break; }
            done += (size_t)received;
        }
        (void)rss_file_close(&file);
    } else ret = 0;
    rss_lock_release();
    if (ret < 0) return ret;
    buffer[done] = '\0';
    return (int)done;
}

int papermono_rss_body_load(size_t index, char *buffer, size_t capacity)
{
    return papermono_rss_body_load_source(0U, index, buffer, capacity);
}


#if defined(__ZEPHYR__)

/* ------------------------------------------------------------------------- */
/* HTTP fetch                                                                 */
/* ------------------------------------------------------------------------- */

#define RSS_HTTP_IDLE_TIMEOUT_MS 10000U
#define RSS_HTTP_TOTAL_TIMEOUT_MS 30000U

struct rss_url {
	char host[RSS_HOST_MAX + 1U];
	char path[RSS_PATH_MAX + 1U];
	uint16_t port;
};

static struct net_if *rss_iface(void)
{
	struct net_if *iface = net_if_get_wifi_sta();

	return iface != NULL ? iface : net_if_get_default();
}

static bool rss_network_ready(void)
{
	struct net_if *iface = rss_iface();

	if (iface == NULL) {
		return false;
	}
	return net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL;
}

/* This tree's DHCP client stores the gateway but never installs it as a
 * default router, so off-link traffic fails with ENOENT while the LAN works.
 */
static void ensure_default_router(void)
{
	struct net_if *iface = rss_iface();
	struct net_in_addr gw;

	if (iface == NULL) {
		return;
	}
	gw = net_if_ipv4_get_gw(iface);
	if (gw.s4_addr[0] == 0U) {
		return;
	}
	if (net_if_ipv4_router_lookup(iface, &gw) != NULL) {
		return;
	}
	struct net_if_router *stale = net_if_ipv4_router_find_default(iface,
									NULL);

	if (stale != NULL) {
		(void)net_if_ipv4_router_rm(stale);
	}
	if (net_if_ipv4_router_add(iface, &gw, true, 0) != NULL) {
		RSS_LOG_INF("RSS using DHCP default router %u.%u.%u.%u",
			    gw.s4_addr[0], gw.s4_addr[1], gw.s4_addr[2],
			    gw.s4_addr[3]);
	} else {
		RSS_LOG_WRN("RSS could not install DHCP default router");
	}
}

static int rss_url_parse(const char *url, struct rss_url *parts)
{
	const char *authority;
	const char *slash;
	const char *colon;
	size_t authority_len;
	size_t host_len;
	size_t path_len;
	unsigned long port = 80UL;

	if (url == NULL) {
		return -EINVAL;
	}
	if (strncmp(url, "https://", 8U) == 0) {
		return -EPROTONOSUPPORT;
	}
	if (strncmp(url, "http://", 7U) != 0) {
		return -EINVAL;
	}
	authority = url + 7U;
	slash = strchr(authority, '/');
	authority_len = slash != NULL ? (size_t)(slash - authority) :
					strlen(authority);
	if (authority_len == 0U || authority_len > RSS_HOST_MAX + 6U) {
		return -EINVAL;
	}
	colon = memchr(authority, ':', authority_len);
	host_len = colon != NULL ? (size_t)(colon - authority) : authority_len;
	if (host_len == 0U || host_len > RSS_HOST_MAX) {
		return -EINVAL;
	}
	memcpy(parts->host, authority, host_len);
	parts->host[host_len] = '\0';
	if (colon != NULL) {
		const size_t digit_count = authority_len - host_len - 1U;

		if (digit_count == 0U || digit_count > 5U) {
			return -EINVAL;
		}
		port = 0UL;
		for (size_t i = 0U; i < digit_count; ++i) {
			const char ch = colon[1U + i];

			if (ch < '0' || ch > '9') {
				return -EINVAL;
			}
			port = port * 10UL + (unsigned long)(ch - '0');
		}
		if (port == 0UL || port > 65535UL) {
			return -EINVAL;
		}
	}
	parts->port = (uint16_t)port;
	path_len = slash != NULL ? strlen(slash) : 1U;
	if (path_len > RSS_PATH_MAX) {
		return -EINVAL;
	}
	if (slash != NULL) {
		memcpy(parts->path, slash, path_len + 1U);
	} else {
		(void)strcpy(parts->path, "/");
	}
	return 0;
}

static bool rss_contains_ci(const char *haystack, size_t haystack_len,
			    const char *needle)
{
	const size_t needle_len = strlen(needle);

	if (needle_len == 0U || haystack_len < needle_len) {
		return false;
	}
	for (size_t i = 0U; i + needle_len <= haystack_len; ++i) {
		size_t j = 0U;

		for (; j < needle_len; ++j) {
			char ch = haystack[i + j];

			if (ch >= 'A' && ch <= 'Z') {
				ch = (char)(ch - 'A' + 'a');
			}
			if (ch != needle[j]) {
				break;
			}
		}
		if (j == needle_len) {
			return true;
		}
	}
	return false;
}

static int rss_http_status(const char *response_text, size_t header_len)
{
	const char *space;
	unsigned int status = 0U;

	if (header_len < 12U || memcmp(response_text, "HTTP/", 5U) != 0) {
		return -EBADMSG;
	}
	space = memchr(response_text, ' ', header_len);
	if (space == NULL) {
		return -EBADMSG;
	}
	++space;
	for (size_t i = 0U; i < 3U; ++i) {
		if (space + i >= response_text + header_len ||
		    space[i] < '0' || space[i] > '9') {
			return -EBADMSG;
		}
		status = status * 10U + (unsigned int)(space[i] - '0');
	}
	return (int)status;
}

/* Decode a chunked transfer-encoding body in place. */
static size_t rss_dechunk(char *body, size_t length)
{
	size_t input = 0U;
	size_t output = 0U;

	while (input < length) {
		size_t chunk = 0U;
		bool have_digits = false;

		while (input < length) {
			const char ch = body[input];
			unsigned int digit;

			if (ch >= '0' && ch <= '9') {
				digit = (unsigned int)(ch - '0');
			} else if (ch >= 'a' && ch <= 'f') {
				digit = (unsigned int)(ch - 'a') + 10U;
			} else if (ch >= 'A' && ch <= 'F') {
				digit = (unsigned int)(ch - 'A') + 10U;
			} else {
				break;
			}
			if (chunk > length) {
				return output;
			}
			chunk = chunk * 16U + digit;
			have_digits = true;
			++input;
		}
		if (!have_digits) {
			break;
		}
		while (input < length && body[input] != '\r' &&
		       body[input] != '\n') {
			++input;
		}
		if (input < length && body[input] == '\r') {
			++input;
		}
		if (input < length && body[input] == '\n') {
			++input;
		}
		if (chunk == 0U) {
			break;
		}
		if (chunk > length - input) {
			return output;
		}
		if (output != input) {
			memmove(body + output, body + input, chunk);
		}
		output += chunk;
		input += chunk;
		if (input < length && body[input] == '\r') {
			++input;
		}
		if (input < length && body[input] == '\n') {
			++input;
		}
	}
	return output;
}

static int rss_socket_timeout(int sock, int option, int64_t deadline)
{
	const int64_t remaining = deadline - k_uptime_get();
	const int64_t timeout_ms = RSS_MIN(remaining,
					 (int64_t)RSS_HTTP_IDLE_TIMEOUT_MS);
	struct zsock_timeval timeout;

	if (remaining <= 0) {
		return -ETIMEDOUT;
	}
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_usec = (timeout_ms % 1000) * 1000;
	if (zsock_setsockopt(sock, SOL_SOCKET, option, &timeout,
			     sizeof(timeout)) < 0) {
		return -errno;
	}
	return 0;
}

static int rss_bind_source(int sock, const struct sockaddr_in *remote)
{
	struct net_if *iface = rss_iface();
	const struct net_in_addr *address;
	struct sockaddr_in local = {
		.sin_family = AF_INET,
	};
	char source_text[16];
	char remote_text[16];

	if (iface == NULL) {
		return -ENETDOWN;
	}
	address = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
	if (address == NULL) {
		return -ENETDOWN;
	}
	local.sin_addr = *address;
	if (local.sin_addr.s_addr == 0U) {
		return -ENETDOWN;
	}
	/* In STA+AP mode, Zephyr's unbound IPv4 source selection can prefer
	 * the AP for a destination outside both subnets. Binding the concrete
	 * station address fixes this connection's interface without changing
	 * the AP's routing or the global default interface.
	 */
	if (zsock_bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
		const int ret = -errno;

		RSS_LOG_WRN("RSS source bind failed (%d)", ret);
		return ret;
	}
	RSS_LOG_INF("RSS connect source %s -> %s",
		    net_addr_ntop(AF_INET, &local.sin_addr, source_text,
				  sizeof(source_text)),
		    net_addr_ntop(AF_INET, &remote->sin_addr, remote_text,
				  sizeof(remote_text)));
	return 0;
}

static int rss_http_get(const char *url, char *response, size_t capacity,
			size_t *body_offset, size_t *body_length,
			bool *truncated)
{
	const int64_t deadline = k_uptime_get() + RSS_HTTP_TOTAL_TIMEOUT_MS;
	struct rss_url parts;
	struct sockaddr_in address = {
		.sin_family = AF_INET,
	};
	char host_header[RSS_HOST_MAX + 8U];
	char request[400];
	size_t sent = 0U;
	size_t used = 0U;
	size_t header_len;
	const char *header_end;
	int sock;
	int ret;

	ret = rss_url_parse(url, &parts);
	if (ret < 0) {
		return ret;
	}
	address.sin_port = htons(parts.port);
	if (net_addr_pton(AF_INET, parts.host, &address.sin_addr) < 0) {
		struct zsock_addrinfo hints = {
			.ai_family = AF_INET,
			.ai_socktype = SOCK_STREAM,
		};
		struct zsock_addrinfo *result = NULL;
		char port_text[6];

		(void)snprintf(port_text, sizeof(port_text), "%u",
			       (unsigned int)parts.port);
		ret = zsock_getaddrinfo(parts.host, port_text, &hints, &result);
		if (k_uptime_get() >= deadline) {
			if (result != NULL) {
				zsock_freeaddrinfo(result);
			}
			return -ETIMEDOUT;
		}
		if (ret != 0 || result == NULL) {
			RSS_LOG_WRN("RSS DNS lookup failed for %s (%d)",
				    parts.host, ret);
			return -EHOSTUNREACH;
		}
		memcpy(&address, result->ai_addr, sizeof(address));
		zsock_freeaddrinfo(result);
	}
	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		return -errno;
	}
	ret = rss_socket_timeout(sock, SO_RCVTIMEO, deadline);
	if (ret == 0) {
		ret = rss_socket_timeout(sock, SO_SNDTIMEO, deadline);
	}
	if (ret < 0) {
		zsock_close(sock);
		return ret;
	}
	ret = rss_bind_source(sock, &address);
	if (ret < 0) {
		zsock_close(sock);
		return ret;
	}
	if (k_uptime_get() >= deadline) {
		zsock_close(sock);
		return -ETIMEDOUT;
	}
	ret = zsock_connect(sock, (struct sockaddr *)&address, sizeof(address));
	if (ret < 0) {
		ret = -errno;
	}
	if (k_uptime_get() >= deadline) {
		ret = -ETIMEDOUT;
	}
	if (ret < 0) {
		RSS_LOG_WRN("RSS connect failed (%d) to %s", ret,
			    net_addr_ntop(AF_INET, &address.sin_addr,
					  (char[16]){ 0 }, 16));
		zsock_close(sock);
		return ret;
	}
	if (parts.port == 80U) {
		(void)snprintf(host_header, sizeof(host_header), "%s",
			       parts.host);
	} else {
		(void)snprintf(host_header, sizeof(host_header), "%s:%u",
			       parts.host, (unsigned int)parts.port);
	}
	const int request_len = snprintf(
		request, sizeof(request),
		"GET %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Accept: application/rss+xml, application/xml;q=0.9, */*;q=0.1\r\n"
		"User-Agent: PaperMono/1.0\r\n"
		"Connection: close\r\n\r\n",
		parts.path, host_header);

	if (request_len <= 0 || (size_t)request_len >= sizeof(request)) {
		zsock_close(sock);
		return -EINVAL;
	}
	while (sent < (size_t)request_len) {
		ret = rss_socket_timeout(sock, SO_SNDTIMEO, deadline);
		if (ret < 0) {
			zsock_close(sock);
			return ret;
		}
		const ssize_t wrote = zsock_send(sock, request + sent,
						 (size_t)request_len - sent, 0);

		if (k_uptime_get() >= deadline) {
			zsock_close(sock);
			return -ETIMEDOUT;
		}
		if (wrote < 0) {
			if (errno == EINTR) {
				continue;
			}
			ret = errno == EAGAIN || errno == EWOULDBLOCK ?
				-ETIMEDOUT : -errno;
			zsock_close(sock);
			return ret;
		}
		if (wrote == 0) {
			zsock_close(sock);
			return -EIO;
		}
		sent += (size_t)wrote;
	}
	while (used + 1U < capacity) {
		ret = rss_socket_timeout(sock, SO_RCVTIMEO, deadline);
		if (ret < 0) {
			zsock_close(sock);
			return ret;
		}
		const ssize_t received = zsock_recv(sock, response + used,
						    capacity - 1U - used, 0);

		if (k_uptime_get() >= deadline) {
			zsock_close(sock);
			return -ETIMEDOUT;
		}
		if (received == 0) {
			break;
		}
		if (received < 0) {
			if (errno == EINTR) {
				continue;
			}
			ret = errno == EAGAIN || errno == EWOULDBLOCK ?
				-ETIMEDOUT : -errno;
			zsock_close(sock);
			return ret;
		}
		used += (size_t)received;
	}
	response[used] = '\0';
	zsock_close(sock);
	*truncated = used + 1U == capacity &&
		     rss_mem_find(response, response + used, "</channel", 9U) ==
			     NULL;
	if (*truncated) {
		RSS_LOG_WRN("RSS feed larger than the %u KB response buffer",
			    (unsigned int)(capacity / 1024U));
	}
	header_end = rss_mem_find(response, response + used, "\r\n\r\n", 4U);
	if (header_end == NULL) {
		RSS_LOG_WRN("RSS response has no HTTP header terminator");
		return -EBADMSG;
	}
	header_len = (size_t)(header_end - response);
	ret = rss_http_status(response, header_len);
	if (ret < 0) {
		return ret;
	}
	if (ret != 200) {
		RSS_LOG_WRN("RSS HTTP status %d", ret);
		return -EBADMSG;
	}
	*body_offset = header_len + 4U;
	*body_length = used - *body_offset;
	if (rss_contains_ci(response, header_len,
			    "transfer-encoding: chunked")) {
		*body_length = rss_dechunk(response + *body_offset,
					   *body_length);
	}
	if (*body_length == 0U) {
		return -EBADMSG;
	}
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Worker thread                                                              */
/* ------------------------------------------------------------------------- */

static char response[RSS_RESPONSE_MAX] RSS_PSRAM;
static struct rss_fetch_job worker_job RSS_PSRAM;
static K_SEM_DEFINE(request_sem, 0, 1);
static atomic_t worker_started;
static struct k_thread worker_thread;
static size_t worker_cursor RSS_PSRAM;

#if defined(CONFIG_ESP_SPIRAM)
static Z_KERNEL_STACK_DEFINE_IN(worker_stack, RSS_THREAD_STACK_SIZE, RSS_PSRAM);
#else
static K_THREAD_STACK_DEFINE(worker_stack, RSS_THREAD_STACK_SIZE);
#endif

static int64_t rss_wall_clock(void)
{
    const time_t now = time(NULL);
    return now > 1600000000 ? (int64_t)now : 0;
}

static bool rss_job_current(const struct rss_fetch_job *job)
{
    bool current;
    rss_lock_take();
    current = rss_job_current_locked(job);
    rss_lock_release();
    return current;
}

static void rss_fetch_complete(const struct rss_fetch_job *job, int result)
{
    bool publish = false;
    rss_lock_take();
    if (rss_job_current_locked(job)) {
        struct rss_source_state *state = &source_states[job->slot];
        state->result = result;
        state->inflight = false;
        publish = true;
    }
    rss_lock_release();
    if (publish && result == 0) papermono_ui_request_content_refresh();
    else if (publish) papermono_ui_request_background_refresh();
}

static bool rss_wait_ipv4(const struct rss_fetch_job *job)
{
    for (uint32_t waited = 0U; waited < RSS_IPV4_WAIT_MS; waited += 1000U) {
        if (!rss_job_current(job)) return false;
        if (rss_network_ready()) return true;
        k_msleep(1000);
    }
    return rss_network_ready();
}

static int rss_fetch_once(const struct rss_fetch_job *job)
{
    size_t body_offset = 0U, body_length = 0U;
    bool truncated = false;
    int ret = rss_http_get(job->source.url, response, sizeof(response),
                           &body_offset, &body_length, &truncated);
    if (ret < 0) return ret;
    return rss_cache_store(job, response + body_offset, body_length,
                            rss_wall_clock(), truncated);
}

static bool rss_next_job(struct rss_fetch_job *job)
{
    bool found = false;
    rss_lock_take();
    rss_registry_ensure_locked();
    for (size_t offset = 0U; offset < source_count; ++offset) {
        const size_t index = (worker_cursor + offset) % source_count;
        struct rss_source_state *state = &source_states[index];
        if (state->pending && !state->inflight) {
            rss_job_copy_locked(index, job);
            state->pending = false;
            state->inflight = true;
            worker_cursor = (index + 1U) % source_count;
            found = true;
            break;
        }
    }
    rss_lock_release();
    return found;
}

static bool rss_retry_delay(const struct rss_fetch_job *job)
{
    for (uint32_t waited = 0U; ; waited += 1000U) {
        bool retry = true;
        rss_lock_take();
        if (!rss_job_current_locked(job)) retry = false;
        for (size_t i = 0U; retry && i < source_count; ++i) {
            if (i != job->slot && source_states[i].pending) retry = false;
        }
        rss_lock_release();
        if (!retry) return false;
        if (waited >= RSS_FETCH_RETRY_MS) return true;
        k_msleep(1000);
    }
}

static void rss_worker(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    RSS_LOG_INF("RSS worker started (per-source SD cache, fetch on demand)");
    for (;;) {
        (void)k_sem_take(&request_sem, K_FOREVER);
        while (rss_next_job(&worker_job)) {
            int loaded = -ENODEV;
            int ret = -EIO;
            for (uint8_t retry = 0U; retry <= 15U; ++retry) {
                rss_lock_take();
                if (!rss_job_current_locked(&worker_job)) {
                    rss_lock_release();
                    loaded = -ESTALE;
                    break;
                }
                loaded = rss_ensure_index(&source_states[worker_job.slot]);
                rss_lock_release();
                if (loaded != -ENODEV || retry == 15U) break;
                k_msleep(2000);
            }
            if (loaded == -ESTALE) continue;
            if (loaded == 1) papermono_ui_request_background_refresh();
            if (loaded == -ENODEV) {
                rss_fetch_complete(&worker_job, -ENODEV);
                continue;
            }
            if (!rss_wait_ipv4(&worker_job)) {
                rss_fetch_complete(&worker_job, -ENETDOWN);
                continue;
            }
            for (uint8_t attempt = 0U; attempt < RSS_FETCH_ATTEMPTS; ++attempt) {
                if (!rss_job_current(&worker_job)) { ret = -ESTALE; break; }
                ensure_default_router();
                ret = rss_fetch_once(&worker_job);
                if (ret == 0 || ret == -ESTALE) break;
                RSS_LOG_WRN("RSS source %s attempt %u/%u failed (%d)",
                            worker_job.source.id, attempt + 1U, RSS_FETCH_ATTEMPTS, ret);
                if (ret == -EINVAL || ret == -EPROTONOSUPPORT) break;
                /* A failed source must not hold a newly selected source
                 * behind its 20-second retry backoff. Keep only this explicit
                 * request; the user can retry the failed source later. */
                if (attempt + 1U < RSS_FETCH_ATTEMPTS &&
                    !rss_retry_delay(&worker_job)) break;
            }
            if (ret != 0 && ret != -ESTALE) RSS_LOG_WRN("RSS source %s refresh failed (%d)",
                                                       worker_job.source.id, ret);
            rss_fetch_complete(&worker_job, ret);
        }
    }
}

int papermono_rss_init(void)
{
    if (!atomic_cas(&worker_started, 0, 1)) return 0;
    rss_lock_take();
    rss_registry_ensure_locked();
    if (source_count != 0U) (void)rss_ensure_index(&source_states[0]);
    rss_lock_release();
    (void)k_thread_create(&worker_thread, worker_stack,
                          K_THREAD_STACK_SIZEOF(worker_stack), rss_worker,
                          NULL, NULL, NULL, 9, 0, K_NO_WAIT);
    return 0;
}

#else /* Host build: no network or worker; tests drive individual caches. */

int papermono_rss_init(void)
{
    rss_registry_ensure_locked();
    return 0;
}

#endif /* __ZEPHYR__ */

int papermono_rss_request_source(size_t source)
{
    bool queued = false;
    rss_lock_take();
    rss_registry_ensure_locked();
    if (source >= source_count) {
        rss_lock_release();
        return -ENOENT;
    }
    struct rss_source_state *state = &source_states[source];
    if (!state->pending && !state->inflight) {
        state->pending = true;
        state->result = 1;
        queued = true;
    }
    rss_lock_release();
#if defined(__ZEPHYR__)
    if (queued) k_sem_give(&request_sem);
#else
    (void)queued;
#endif
    return 0;
}

int papermono_rss_status_source(size_t source)
{
    int result;
    rss_lock_take();
    rss_registry_ensure_locked();
    result = source < source_count ? source_states[source].result : -ENOENT;
    rss_lock_release();
    return result;
}

int papermono_rss_request(void) { return papermono_rss_request_source(0U); }
int papermono_rss_status(void) { return papermono_rss_status_source(0U); }

#if !defined(__ZEPHYR__)
void papermono_rss_test_set_cache_dir(const char *directory)
{
    (void)snprintf(rss_host_cache_directory, sizeof(rss_host_cache_directory), "%s",
                   directory != NULL ? directory : ".");
    memset(source_states, 0, sizeof(source_states));
    source_count = 0U;
    registry_initialized = registry_checked = registry_persisted = false;
    registry_generation = revision_serial = 0U;
    memset(&staging, 0, sizeof(staging));
}

void papermono_rss_test_reset(void)
{
    for (size_t i = 0U; i < source_count; ++i) {
        memset(&source_states[i].feed, 0, sizeof(source_states[i].feed));
        source_states[i].index_loaded = false;
    }
    memset(&staging, 0, sizeof(staging));
}

int papermono_rss_test_ingest_source(size_t source, const char *xml, size_t length)
{
    struct rss_fetch_job job;
    rss_registry_ensure_locked();
    if (source >= source_count) return -ENOENT;
    rss_job_copy_locked(source, &job);
    const int ret = rss_cache_store(&job, xml, length, 1700000000, false);
    if (rss_job_current_locked(&job)) {
        source_states[source].result = ret;
        source_states[source].pending = false;
        source_states[source].inflight = false;
    }
    return ret;
}

int papermono_rss_test_ingest(const char *xml, size_t length)
{
    return papermono_rss_test_ingest_source(0U, xml, length);
}

int papermono_rss_test_load(void)
{
    rss_registry_ensure_locked();
    if (source_count == 0U) return -ENOENT;
    memset(&source_states[0].feed, 0, sizeof(source_states[0].feed));
    source_states[0].index_loaded = false;
    memset(&staging, 0, sizeof(staging));
    return rss_ensure_index(&source_states[0]);
}
#endif
