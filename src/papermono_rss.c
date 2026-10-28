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

static size_t rss_utf8_sequence_length(unsigned char lead)
{
	if (lead >= 0xF0U) {
		return 4U;
	}
	if (lead >= 0xE0U) {
		return 3U;
	}
	if (lead >= 0xC0U) {
		return 2U;
	}
	return 1U;
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
				for (size_t i = 0U; i < written; ++i) {
					if (rss_is_space(decoded[i])) {
						writer.pending_space = true;
					} else {
						rss_plain_put(&writer,
							      &decoded[i], 1U);
					}
				}
				index += consumed;
				continue;
			}
		}
		const unsigned char lead = (unsigned char)text[index];

		if (lead < 0x80U) {
			if (rss_is_space((char)lead)) {
				writer.pending_space = true;
			} else {
				rss_plain_put(&writer, text + index, 1U);
			}
			++index;
			continue;
		}
		size_t sequence = rss_utf8_sequence_length(lead);

		if (sequence > length - index) {
			sequence = length - index;
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
};

static void rss_html_feed_ascii(struct rss_html_render *render, char ch);

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
	if (render->heading_candidate) {
		rss_html_heading_collect(render, data, length);
		return;
	}
	if (render->url_active) {
		goto append_url;
	}
	rss_lw_text(&render->line, data, length);
	render->prev_word_char = false;
	return;
append_url:
	{
		/* URL collection is handled by the caller through the byte
		 * interface; multibyte URL content simply gets appended.
		 */
		for (size_t i = 0U; i < length; ++i) {
			rss_html_feed_ascii(render, data[i]);
			if (!render->url_active) {
				break;
			}
		}
	}
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
	for (size_t i = 0U; i < render->url_len; ++i) {
		rss_lw_text(&render->line, &render->url[i], 1U);
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
	for (size_t i = url_end; i < render->url_len; i +=
	     rss_utf8_sequence_length((unsigned char)render->url[i])) {
		size_t sequence =
			rss_utf8_sequence_length((unsigned char)render->url[i]);

		if (sequence > render->url_len - i) {
			sequence = render->url_len - i;
		}
		rss_lw_text(&render->line, render->url + i, sequence);
	}
	rss_url_reset(render);
	render->prev_word_char = false;
}

static void rss_url_append(struct rss_html_render *render, const char *data,
			   size_t length)
{
	for (size_t i = 0U; i < length; ++i) {
		const char ch = data[i];

		if (render->url_discard) {
			continue;
		}
		if (render->url_len >= sizeof(render->url) - 1U) {
			if (!render->url_prefix_ok) {
				rss_url_emit_plain(render);
				return;
			}
			render->url_discard = true;
			continue;
		}
		render->url[render->url_len++] = ch;
		if (!render->url_prefix_ok) {
			if ((unsigned char)ch >= 0x80U) {
				rss_url_emit_plain(render);
				return;
			}
			if (!rss_url_prefix_possible(render)) {
				rss_url_emit_plain(render);
				return;
			}
		}
	}
}

static void rss_html_text_ascii(struct rss_html_render *render, char ch)
{
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

static void rss_html_dispatch(struct rss_html_render *render)
{
	const char *name = render->name;
	const size_t length = render->name_len;
	const bool closing = render->closing;

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
		size_t sequence = rss_utf8_sequence_length(lead);

		if (sequence > length - index) {
			sequence = length - index;
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

static struct papermono_rss_feed snapshot RSS_PSRAM;
static struct papermono_rss_feed staging RSS_PSRAM;
/* Serialised index block; only touched while rss_lock is held. Keeping it out
 * of the caller stack matters for the 2 KB read/dump in body_load and on the
 * worker thread.
 */
static struct rss_index_disk index_block RSS_PSRAM;
static bool index_loaded;

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

static int rss_build_path(char *out, size_t capacity, const char *leaf)
{
#if defined(__ZEPHYR__)
	const int written = snprintf(out, capacity, "%s/rss/%s",
				     PAPERMONO_CONTENT_ROOT, leaf);
#else
	const int written = snprintf(out, capacity, "%s/papermono/rss/%s",
				     rss_host_cache_directory, leaf);
#endif

	return written < 0 || (size_t)written >= capacity ? -ENAMETOOLONG : 0;
}

static int rss_ensure_directory(void)
{
#if defined(__ZEPHYR__)
	int ret = rss_mkdir_if_missing(PAPERMONO_CONTENT_ROOT);

	if (ret < 0) {
		return ret;
	}
	return rss_mkdir_if_missing(PAPERMONO_CONTENT_ROOT "/rss");
#else
	return 0;
#endif
}

static int rss_unlink_leaf(const char *leaf)
{
	char path[RSS_CACHE_PATH_MAX];
	int ret = rss_build_path(path, sizeof(path), leaf);

	if (ret < 0) {
		return ret;
	}
	return rss_file_unlink(path);
}

static int rss_leaf_exists(const char *leaf)
{
	char path[RSS_CACHE_PATH_MAX];
	int ret = rss_build_path(path, sizeof(path), leaf);

	if (ret < 0) {
		return ret;
	}
	return rss_file_exists(path);
}

static int rss_leaf_size(const char *leaf, size_t *size)
{
	char path[RSS_CACHE_PATH_MAX];
	int ret = rss_build_path(path, sizeof(path), leaf);

	if (ret < 0) {
		return ret;
	}
	return rss_file_size(path, size);
}

static int rss_leaf_rename(const char *from_leaf, const char *to_leaf)
{
	char from[RSS_CACHE_PATH_MAX];
	char to[RSS_CACHE_PATH_MAX];
	int ret = rss_build_path(from, sizeof(from), from_leaf);

	if (ret < 0) {
		return ret;
	}
	ret = rss_build_path(to, sizeof(to), to_leaf);
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

static int rss_write_file(const char *leaf, const void *data, size_t length,
			  uint8_t *bounce)
{
	char path[RSS_CACHE_PATH_MAX];
	struct rss_file file = { 0 };
	int ret;

	ret = rss_build_path(path, sizeof(path), leaf);
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

static int rss_read_file(const char *leaf, void *data, size_t capacity,
			 size_t *length)
{
	char path[RSS_CACHE_PATH_MAX];
	struct rss_file file = { 0 };
	size_t done = 0U;
	int ret;

	ret = rss_build_path(path, sizeof(path), leaf);
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
static int rss_swap_file(const char *tmp_leaf, const char *final_leaf,
			 const char *bak_leaf, bool *had_backup)
{
	bool backup = false;
	int ret;

	*had_backup = false;
	if (rss_leaf_exists(final_leaf) == 1) {
		(void)rss_unlink_leaf(bak_leaf);
		ret = rss_leaf_rename(final_leaf, bak_leaf);
		if (ret < 0) {
			return ret;
		}
		backup = true;
	}
	ret = rss_leaf_rename(tmp_leaf, final_leaf);
	if (ret < 0) {
		if (backup) {
			(void)rss_leaf_rename(bak_leaf, final_leaf);
		}
		return ret;
	}
	*had_backup = backup;
	return 0;
}

/* Recover a cache left behind by a reset in the middle of a commit. */
static void rss_cache_recover(void)
{
	const int bodies_bak = rss_leaf_exists(RSS_BODIES_BAK_FILE);
	const int index_bak = rss_leaf_exists(RSS_INDEX_BAK_FILE);

	if (bodies_bak == 1) {
		if (index_bak == 1) {
			/* Both files were already replaced; the leftovers are
			 * stale backups.
			 */
			(void)rss_unlink_leaf(RSS_BODIES_BAK_FILE);
			(void)rss_unlink_leaf(RSS_INDEX_BAK_FILE);
		} else {
			/* Bodies were replaced but the index was not: roll the
			 * bodies back so the old index still matches.
			 */
			(void)rss_unlink_leaf(RSS_BODIES_FILE);
			(void)rss_leaf_rename(RSS_BODIES_BAK_FILE,
					      RSS_BODIES_FILE);
		}
	} else if (index_bak == 1) {
		(void)rss_unlink_leaf(RSS_INDEX_BAK_FILE);
	}
	(void)rss_unlink_leaf(RSS_INDEX_TMP_FILE);
	(void)rss_unlink_leaf(RSS_BODIES_TMP_FILE);
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

static int rss_cache_load(void)
{
	struct rss_index_disk *disk = &index_block;
	size_t read = 0U;
	size_t bodies_size = 0U;
	size_t total = 0U;
	uint64_t total_check = 0U;
	int ret;

	rss_cache_recover();
	memset(&snapshot, 0, sizeof(snapshot));
	memset(&staging, 0, sizeof(staging));
	ret = rss_read_file(RSS_INDEX_FILE, disk, sizeof(*disk), &read);
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
		ret = rss_leaf_size(RSS_BODIES_FILE, &bodies_size);
		if (ret < 0 || bodies_size < total) {
			return -EBADMSG;
		}
	}
	snapshot.valid = true;
	snapshot.updated_at = disk->updated_at;
	memcpy(snapshot.title, disk->title, sizeof(snapshot.title));
	snapshot.title[PAPERMONO_RSS_TITLE_MAX] = '\0';
	snapshot.count = disk->count;
	for (size_t i = 0U; i < disk->count; ++i) {
		memcpy(snapshot.items[i].title, disk->items[i].title,
		       sizeof(snapshot.items[i].title));
		snapshot.items[i].title[PAPERMONO_RSS_TITLE_MAX] = '\0';
		memcpy(snapshot.items[i].published, disk->items[i].published,
		       sizeof(snapshot.items[i].published));
		snapshot.items[i]
			.published[PAPERMONO_RSS_DATE_MAX] = '\0';
		snapshot.items[i].body_length = disk->items[i].body_length;
	}
	return 0;
}

/*
 * Load the SD index into RAM once. Returns 1 when the cache was loaded now,
 * 0 when it was already loaded, -ENODEV when the SD card is not available
 * (retried on the next call) and another negative errno when the cache is
 * missing or corrupt (also latched; a fetch will replace it).
 */
static int rss_ensure_index(void)
{
	int ret;

	if (index_loaded) {
		return 0;
	}
	ret = rss_platform_mount();
	if (ret < 0) {
		return -ENODEV;
	}
	ret = rss_cache_load();
	index_loaded = true;
	if (ret < 0) {
		RSS_LOG_WRN("RSS SD cache unusable (%d); RAM index stays empty",
			    ret);
		return ret;
	}
	RSS_LOG_INF("RSS cache loaded: %u items, updated_at %lld",
		    (unsigned int)snapshot.count,
		    (long long)snapshot.updated_at);
	return 1;
}

struct rss_body_writer {
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
				 uint8_t *bounce)
{
	char path[RSS_CACHE_PATH_MAX];
	int ret;

	memset(writer, 0, sizeof(*writer));
	writer->bounce = bounce;
	ret = rss_build_path(path, sizeof(path), RSS_BODIES_TMP_FILE);
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
	char path[RSS_CACHE_PATH_MAX];

	if (writer->open) {
		(void)rss_file_close(&writer->file);
		writer->open = false;
	}
	if (rss_build_path(path, sizeof(path), RSS_BODIES_TMP_FILE) == 0) {
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

static int rss_cache_commit(const struct papermono_rss_feed *feed,
			    const uint8_t *bounce)
{
	bool bodies_backup = false;
	bool index_backup = false;
	int ret;

	rss_lock_take();
	rss_feed_to_disk(feed, &index_block);
	ret = rss_write_file(RSS_INDEX_TMP_FILE, &index_block,
			     sizeof(index_block), (uint8_t *)bounce);
	if (ret < 0) {
		(void)rss_unlink_leaf(RSS_INDEX_TMP_FILE);
		(void)rss_unlink_leaf(RSS_BODIES_TMP_FILE);
		rss_lock_release();
		return ret;
	}
	ret = rss_swap_file(RSS_BODIES_TMP_FILE, RSS_BODIES_FILE,
			    RSS_BODIES_BAK_FILE, &bodies_backup);
	if (ret < 0) {
		(void)rss_unlink_leaf(RSS_INDEX_TMP_FILE);
		(void)rss_unlink_leaf(RSS_BODIES_TMP_FILE);
		rss_lock_release();
		return ret;
	}
	ret = rss_swap_file(RSS_INDEX_TMP_FILE, RSS_INDEX_FILE,
			    RSS_INDEX_BAK_FILE, &index_backup);
	if (ret < 0) {
		if (bodies_backup) {
			(void)rss_unlink_leaf(RSS_BODIES_FILE);
			(void)rss_leaf_rename(RSS_BODIES_BAK_FILE,
					      RSS_BODIES_FILE);
		}
		(void)rss_unlink_leaf(RSS_INDEX_TMP_FILE);
		rss_lock_release();
		return ret;
	}
	if (bodies_backup) {
		(void)rss_unlink_leaf(RSS_BODIES_BAK_FILE);
	}
	if (index_backup) {
		(void)rss_unlink_leaf(RSS_INDEX_BAK_FILE);
	}
	snapshot = *feed;
	index_loaded = true;
	rss_lock_release();
	return 0;
}

/*
 * Parse @p xml and replace the SD cache with the result. The old cache is left
 * untouched unless every step succeeds.
 */
static int rss_cache_store(const char *xml, size_t length, int64_t updated_at,
			   bool truncated)
{
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
	ret = rss_ensure_directory();
	if (ret < 0) {
		return ret;
	}
	ret = rss_body_writer_begin(&writer, bounce);
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
	ret = rss_cache_commit(&staging, bounce);
	if (ret < 0) {
		return ret;
	}
	RSS_LOG_INF("RSS cache updated: %u items, %u feed bytes",
		    (unsigned int)snapshot.count, (unsigned int)length);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Public data API                                                            */
/* ------------------------------------------------------------------------- */

void papermono_rss_get(struct papermono_rss_feed *out)
{
	if (out == NULL) {
		return;
	}
	rss_lock_take();
	*out = snapshot;
	rss_lock_release();
}

int papermono_rss_body_load(size_t index, char *buffer, size_t capacity)
{
	struct rss_file file = { 0 };
	size_t offset = 0U;
	size_t to_read;
	size_t done = 0U;
	uint32_t length;
	int ret = 0;

	if (buffer == NULL || capacity == 0U) {
		return -EINVAL;
	}
	rss_lock_take();
	ret = rss_ensure_index();
	if (ret == -ENODEV) {
		rss_lock_release();
		return -ENOENT;
	}
	if (!snapshot.valid || index >= snapshot.count) {
		rss_lock_release();
		return -ENOENT;
	}
	length = snapshot.items[index].body_length;
	for (size_t i = 0U; i < index; ++i) {
		offset += snapshot.items[i].body_length;
	}
	if (length == 0U) {
		buffer[0] = '\0';
		rss_lock_release();
		return 0;
	}
	to_read = RSS_MIN((size_t)length, capacity - 1U);
	{
		char path[RSS_CACHE_PATH_MAX];

		ret = rss_build_path(path, sizeof(path), RSS_BODIES_FILE);
		if (ret < 0) {
			rss_lock_release();
			return -ENOENT;
		}
		ret = rss_file_open_read(&file, path);
		if (ret < 0) {
			rss_lock_release();
			return -ENOENT;
		}
		ret = rss_file_seek(&file, offset);
		while (ret == 0 && done < to_read) {
			const int received = rss_file_read(
				&file, buffer + done, to_read - done);

			if (received < 0) {
				ret = received;
				break;
			}
			if (received == 0) {
				break;
			}
			done += (size_t)received;
		}
		(void)rss_file_close(&file);
	}
	rss_lock_release();
	if (ret < 0) {
		return ret;
	}
	buffer[done] = '\0';
	return (int)done;
}

#if defined(__ZEPHYR__)

/* ------------------------------------------------------------------------- */
/* HTTP fetch                                                                 */
/* ------------------------------------------------------------------------- */

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
	ret = rss_bind_source(sock, &address);
	if (ret < 0) {
		zsock_close(sock);
		return ret;
	}
	if (zsock_connect(sock, (struct sockaddr *)&address,
			  sizeof(address)) < 0) {
		ret = -errno;
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
		const ssize_t wrote = zsock_send(sock, request + sent,
						 (size_t)request_len - sent, 0);

		if (wrote < 0) {
			if (errno == EINTR) {
				continue;
			}
			ret = -errno;
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
		const ssize_t received = zsock_recv(sock, response + used,
						    capacity - 1U - used, 0);

		if (received == 0) {
			break;
		}
		if (received < 0) {
			if (errno == EINTR) {
				continue;
			}
			ret = -errno;
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
static K_SEM_DEFINE(request_sem, 0, 1);
static atomic_t fetch_busy;
/* 0 idle, 1 fetching, negative errno of the last failed refresh. */
static atomic_t last_result;
static atomic_t worker_started;
static struct k_thread worker_thread;

#if defined(CONFIG_ESP_SPIRAM)
/* The worker stack lives in PSRAM: the parser is iterative and uses little
 * stack, and internal DRAM has no room for another 6 KB. */
static Z_KERNEL_STACK_DEFINE_IN(worker_stack, RSS_THREAD_STACK_SIZE, RSS_PSRAM);
#else
static K_THREAD_STACK_DEFINE(worker_stack, RSS_THREAD_STACK_SIZE);
#endif

static int64_t rss_wall_clock(void)
{
	const time_t now = time(NULL);

	return now > 1600000000 ? (int64_t)now : 0;
}

static void rss_fetch_complete(int result)
{
	atomic_set(&last_result, result);
	atomic_set(&fetch_busy, 0);
	if (result == 0) {
		/* CONTENT_CHANGED reloads a body opened from the previous cache;
		 * a plain redraw would only update its title/index metadata.
		 */
		papermono_ui_request_content_refresh();
	} else {
		/* Include pre-fetch failures so an empty page cannot remain on
		 * "syncing" after the worker has stopped.
		 */
		papermono_ui_request_background_refresh();
	}
}

static bool rss_wait_ipv4(void)
{
	for (uint32_t waited = 0U; waited < RSS_IPV4_WAIT_MS;
	     waited += 1000U) {
		if (rss_network_ready()) {
			return true;
		}
		k_msleep(1000);
	}
	return rss_network_ready();
}

static int rss_fetch_once(const char *url)
{
	size_t body_offset = 0U;
	size_t body_length = 0U;
	bool truncated = false;
	int ret;

	ret = rss_http_get(url, response, sizeof(response), &body_offset,
			   &body_length, &truncated);
	if (ret < 0) {
		return ret;
	}
	return rss_cache_store(response + body_offset, body_length,
			       rss_wall_clock(), truncated);
}

static void rss_worker(void *a, void *b, void *c)
{
	static uint8_t sd_retries;
	char url[PAPERMONO_RSS_URL_MAX + 1U];

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	RSS_LOG_INF("RSS worker started (SD cache, fetch on demand)");
	for (;;) {
		int loaded;
		int ret = -EIO;

		(void)k_sem_take(&request_sem, K_FOREVER);
		rss_lock_take();
		loaded = rss_ensure_index();
		rss_lock_release();
		if (loaded == 1) {
			papermono_ui_request_background_refresh();
		}
		if (loaded == -ENODEV) {
			/* The UI mounts the card a few seconds after boot; keep the
			 * page's request alive instead of leaving it on "syncing".
			 */
			if (sd_retries < 15U) {
				++sd_retries;
				/* Keep the queued/in-flight reservation through SD retry
				 * delays so repeated opens cannot enqueue a second fetch.
				 */
				k_msleep(2000);
				k_sem_give(&request_sem);
				continue;
			}
			RSS_LOG_WRN("RSS skipped: SD cache is not available");
			sd_retries = 0U;
			rss_fetch_complete(-ENODEV);
			continue;
		}
		sd_retries = 0U;
		(void)snprintf(url, sizeof(url), "%s",
			       papermono_settings_get()->rss_url);
		if (url[0] == '\0' || strncmp(url, "http://", 7U) != 0) {
			RSS_LOG_WRN("RSS URL is not a plain http:// URL");
			rss_fetch_complete(-EINVAL);
			continue;
		}
		if (!rss_wait_ipv4()) {
			RSS_LOG_WRN("RSS fetch skipped: IPv4 is not ready");
			rss_fetch_complete(-ENETDOWN);
			continue;
		}
		for (uint8_t attempt = 0U; attempt < RSS_FETCH_ATTEMPTS;
		     ++attempt) {
			ensure_default_router();
			ret = rss_fetch_once(url);
			if (ret == 0) {
				break;
			}
			RSS_LOG_WRN("RSS attempt %u/%u failed (%d)",
				    attempt + 1U, RSS_FETCH_ATTEMPTS, ret);
			if (ret == -EINVAL || ret == -EPROTONOSUPPORT) {
				break;
			}
			if (attempt + 1U < RSS_FETCH_ATTEMPTS) {
				k_msleep(RSS_FETCH_RETRY_MS);
			}
		}
		if (ret != 0) {
			RSS_LOG_WRN("RSS refresh failed (%d); the next news-page open will retry",
				    ret);
		}
		rss_fetch_complete(ret);
	}
}

int papermono_rss_init(void)
{
	if (!atomic_cas(&worker_started, 0, 1)) {
		return 0;
	}
	(void)k_thread_create(&worker_thread, worker_stack,
			      K_THREAD_STACK_SIZEOF(worker_stack), rss_worker,
			      NULL, NULL, NULL, 9, 0, K_NO_WAIT);
	/* The SD card is usually still being brought up by the UI at this
	 * point; this attempt only helps when it is already mounted. The worker
	 * retries through papermono_rss_request().
	 */
	rss_lock_take();
	if (rss_ensure_index() == 1) {
		RSS_LOG_INF("RSS index restored from SD at boot");
	}
	rss_lock_release();
	return 0;
}

int papermono_rss_status(void)
{
	return (int)atomic_get(&last_result);
}

int papermono_rss_request(void)
{
	/* Reserve before waking the worker, so both pending and active requests
	 * coalesce. Cached content and wall-clock validity never suppress a fetch.
	 */
	if (!atomic_cas(&fetch_busy, 0, 1)) {
		return 0;
	}
	atomic_set(&last_result, 1);
	k_sem_give(&request_sem);
	return 0;
}

#else /* Host build: no network, no worker; the tests drive the cache. */

int papermono_rss_init(void)
{
	return 0;
}

int papermono_rss_request(void)
{
	return 0;
}

int papermono_rss_status(void)
{
	return 0;
}

void papermono_rss_test_set_cache_dir(const char *directory)
{
	if (directory == NULL) {
		rss_host_cache_directory[0] = '\0';
	} else {
		(void)snprintf(rss_host_cache_directory,
			       sizeof(rss_host_cache_directory), "%s",
			       directory);
	}
	index_loaded = false;
	memset(&snapshot, 0, sizeof(snapshot));
	memset(&staging, 0, sizeof(staging));
}

void papermono_rss_test_reset(void)
{
	index_loaded = false;
	memset(&snapshot, 0, sizeof(snapshot));
	memset(&staging, 0, sizeof(staging));
}

/* Simulates one fetch+parse+cache-write cycle without the network. */
int papermono_rss_test_ingest(const char *xml, size_t length)
{
	return rss_cache_store(xml, length, 1700000000, false);
}

/* Simulates a reboot: the RAM index is dropped and re-read from the SD cache. */
int papermono_rss_test_load(void)
{
	int ret;

	index_loaded = false;
	memset(&snapshot, 0, sizeof(snapshot));
	memset(&staging, 0, sizeof(staging));
	rss_lock_take();
	ret = rss_ensure_index();
	rss_lock_release();
	return ret;
}

#endif /* __ZEPHYR__ */
