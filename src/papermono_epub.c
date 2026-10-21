/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <papermono/epub.h>
#include <papermono/text_layout.h>
#include <papermono/reader.h>
#include <papermono/text_codec.h>

LOG_MODULE_REGISTER(papermono_epub, LOG_LEVEL_INF);

#define EPUB_CONTAINER_LIMIT 8192U
#define EPUB_METADATA_LIMIT (128U * 1024U)
#define EPUB_MANIFEST_LIMIT 128U

#if defined(CONFIG_ESP_SPIRAM)
#define PAPERMONO_EPUB_BSS __attribute__((section(".ext_ram.bss")))
#else
#define PAPERMONO_EPUB_BSS
#endif

struct epub_manifest_item {
	char id[64];
	char href[PAPERMONO_ZIP_NAME_MAX + 1U];
	char media_type[64];
	char properties[64];
};

struct memory_sink {
	char *buffer;
	size_t capacity;
	size_t length;
};

/* Deflate streams cannot be entered halfway through, so every page turn used
 * to re-inflate the chapter from its first byte: reading a chapter of P pages
 * cost O(P * chapter size), which reaches tens of milliseconds per turn on the
 * single huge XHTML files that novel exports often use.
 *
 * Caching the inflated XHTML turns every later turn into a plain memory scan.
 * The cached bytes are fed to the exact same state machine, so text offsets
 * keep their original meaning and saved bookmarks and progress stay valid.
 * Chapters larger than the buffer simply fall back to re-inflating.
 */
#define EPUB_CHAPTER_CACHE_LIMIT (256U * 1024U)

struct epub_chapter_cache {
	char id[PAPERMONO_CONTENT_ID_LENGTH + 1U];
	uint32_t length;
	uint16_t chapter;
	bool complete;
};

/* Import validation and the reader are different threads. Everything below
 * this line parses through shared static buffers and the chapter cache, so the
 * two public entry points are serialised.
 */
K_MUTEX_DEFINE(epub_parse_lock);
static char chapter_buffer[EPUB_CHAPTER_CACHE_LIMIT] PAPERMONO_EPUB_BSS;
static struct epub_chapter_cache chapter_cache;

static bool chapter_cache_matches(const struct papermono_epub_book *epub,
	uint16_t chapter)
{
	return chapter_cache.complete &&
		chapter_cache.chapter == chapter &&
		strncmp(chapter_cache.id, epub->archive.source.id,
			sizeof(chapter_cache.id)) == 0;
}

static void chapter_cache_invalidate(void)
{
	chapter_cache.complete = false;
	chapter_cache.length = 0U;
}

static char metadata_buffer[EPUB_METADATA_LIMIT + 1U] PAPERMONO_EPUB_BSS;
static char container_buffer[EPUB_CONTAINER_LIMIT] PAPERMONO_EPUB_BSS;
static struct epub_manifest_item manifest[EPUB_MANIFEST_LIMIT]
	PAPERMONO_EPUB_BSS;
static char spine_ids[PAPERMONO_EPUB_SPINE_LIMIT][64] PAPERMONO_EPUB_BSS;

static void copy_cstring(char *destination, size_t capacity,
	const char *source)
{
	if (capacity == 0U) {
		return;
	}
	const size_t length = strnlen(source, capacity - 1U);
	memcpy(destination, source, length);
	destination[length] = '\0';
}

static int memory_write(const uint8_t *data, size_t length, void *context)
{
	struct memory_sink *sink = context;

	if (sink->length + length > sink->capacity) {
		return -EFBIG;
	}
	memcpy(sink->buffer + sink->length, data, length);
	sink->length += length;
	return 0;
}

static int extract_string(const struct papermono_zip_archive *archive,
	const char *path, char *buffer, size_t capacity, size_t *length)
{
	struct papermono_zip_entry entry;
	struct memory_sink sink = {
		.buffer = buffer,
		.capacity = capacity - 1U,
	};
	int ret = papermono_zip_find(archive, path, &entry);

	if (ret < 0) {
		return ret;
	}
	if (entry.uncompressed_size >= capacity) {
		return -EFBIG;
	}
	ret = papermono_zip_extract(archive, &entry, memory_write, &sink);
	if (ret != 0) {
		return ret == PAPERMONO_ZIP_EXTRACT_STOPPED ? -EFBIG : ret;
	}
	buffer[sink.length] = '\0';
	if (length != NULL) {
		*length = sink.length;
	}
	return 0;
}

static bool ascii_name_char(char value)
{
	return (value >= 'a' && value <= 'z') ||
		(value >= 'A' && value <= 'Z') ||
		(value >= '0' && value <= '9') || value == '-' || value == '_' ||
		value == ':';
}

static bool name_equal(const char *left, size_t left_length,
	const char *right)
{
	const size_t right_length = strlen(right);

	if (left_length != right_length) {
		return false;
	}
	for (size_t index = 0U; index < left_length; ++index) {
		char a = left[index];
		char b = right[index];

		if (a >= 'A' && a <= 'Z') {
			a += 'a' - 'A';
		}
		if (b >= 'A' && b <= 'Z') {
			b += 'a' - 'A';
		}
		if (a != b) {
			return false;
		}
	}
	return true;
}

static int copy_attribute(const char *tag_start, const char *tag_end,
	const char *wanted, char *output, size_t output_size)
{
	const char *cursor = tag_start;

	while (cursor < tag_end) {
		const char *name_start;
		const char *name_end;
		const char *value_start;
		char quote;
		size_t length;

		while (cursor < tag_end && !ascii_name_char(*cursor)) {
			++cursor;
		}
		name_start = cursor;
		while (cursor < tag_end && ascii_name_char(*cursor)) {
			++cursor;
		}
		name_end = cursor;
		while (cursor < tag_end && (*cursor == ' ' || *cursor == '\t' ||
		       *cursor == '\r' || *cursor == '\n')) {
			++cursor;
		}
		if (cursor >= tag_end || *cursor != '=') {
			continue;
		}
		++cursor;
		while (cursor < tag_end && (*cursor == ' ' || *cursor == '\t' ||
		       *cursor == '\r' || *cursor == '\n')) {
			++cursor;
		}
		if (cursor >= tag_end || (*cursor != '\'' && *cursor != '"')) {
			continue;
		}
		quote = *cursor++;
		value_start = cursor;
		while (cursor < tag_end && *cursor != quote) {
			++cursor;
		}
		if (cursor >= tag_end) {
			return -EBADMSG;
		}
		if (!name_equal(name_start, (size_t)(name_end - name_start),
			wanted)) {
			++cursor;
			continue;
		}
		length = (size_t)(cursor - value_start);
		if (length == 0U || length >= output_size) {
			return -ENAMETOOLONG;
		}
		memcpy(output, value_start, length);
		output[length] = '\0';
		return 0;
	}
	return -ENOENT;
}

/* Return the '<' of the next opening <name> tag, or NULL.
 *
 * Closing tags are skipped rather than matched. A caller that resumed from a
 * matched closing tag would re-scan the parent's remaining siblings as if they
 * were still inside it: parse_ncx_navigation() relied on this distinction and
 * emitted every leaf navPoint a second time, with the wrong nesting level,
 * because "</navPoint>" matched here and its "<text>" was then read from the
 * following sibling.
 */
static const char *find_tag(const char *xml, const char *name)
{
	const size_t name_length = strlen(name);
	const char *cursor = xml;

	while ((cursor = strchr(cursor, '<')) != NULL) {
		const char *tag = cursor;

		++cursor;
		if (*cursor == '/') {
			/* Closing tag: not a match, but step past this '<' so the
			 * next search cannot land on it again.
			 */
			continue;
		}
		if (name_equal(cursor, name_length, name) &&
		    (cursor[name_length] == ' ' || cursor[name_length] == '\t' ||
		     cursor[name_length] == '\r' || cursor[name_length] == '\n' ||
		     cursor[name_length] == '>' || cursor[name_length] == '/')) {
			return tag;
		}
	}
	return NULL;
}

static int digit_value(char value, unsigned int base)
{
	if (value >= '0' && value <= '9') {
		return value - '0' < (int)base ? value - '0' : -1;
	}
	if (base == 16U && value >= 'a' && value <= 'f') {
		return value - 'a' + 10;
	}
	if (base == 16U && value >= 'A' && value <= 'F') {
		return value - 'A' + 10;
	}
	return -1;
}

static int parse_unsigned(const char *text, unsigned int base,
	uint32_t *result)
{
	uint32_t value = 0U;
	size_t count = 0U;

	while (text[count] != '\0') {
		const int digit = digit_value(text[count], base);

		if (digit < 0 || value > (UINT32_MAX - (uint32_t)digit) / base) {
			return -EINVAL;
		}
		value = value * base + (uint32_t)digit;
		++count;
	}
	if (count == 0U) {
		return -EINVAL;
	}
	*result = value;
	return 0;
}

static int decode_percent_path(const char *input, char *output,
	size_t output_size)
{
	size_t written = 0U;

	for (size_t index = 0U; input[index] != '\0' && input[index] != '#';
	     ++index) {
		unsigned int value;

		if (input[index] == '%') {
			if (input[index + 1U] == '\0' || input[index + 1U] == '#' ||
			    input[index + 2U] == '\0' || input[index + 2U] == '#') {
				return -EINVAL;
			}
			const int high = digit_value(input[index + 1U], 16U);
			const int low = digit_value(input[index + 2U], 16U);

			if (high < 0 || low < 0) {
				return -EINVAL;
			}
			value = (unsigned int)((high << 4) | low);
			index += 2U;
		} else {
			value = (unsigned char)input[index];
		}
		if (written + 1U >= output_size || value == 0U || value == '\\' ||
		    value == ':') {
			return -ENAMETOOLONG;
		}
		output[written++] = (char)value;
	}
	output[written] = '\0';
	return 0;
}

static int normalize_path(const char *base, const char *href, char *output,
	size_t output_size)
{
	char decoded[PAPERMONO_ZIP_NAME_MAX + 1U];
	char combined[PAPERMONO_ZIP_NAME_MAX * 2U + 2U];
	size_t output_length = 0U;
	int ret = decode_percent_path(href, decoded, sizeof(decoded));

	if (ret < 0 || decoded[0] == '/') {
		return -EINVAL;
	}
	ret = snprintf(combined, sizeof(combined), "%s%s", base, decoded);
	if (ret < 0 || ret >= (int)sizeof(combined)) {
		return -ENAMETOOLONG;
	}
	const char *cursor = combined;
	while (*cursor != '\0') {
		const char *slash = strchr(cursor, '/');
		const size_t segment_length = slash == NULL ? strlen(cursor) :
			(size_t)(slash - cursor);

		if (segment_length == 0U ||
		    (segment_length == 1U && cursor[0] == '.')) {
			/* Skip. */
		} else if (segment_length == 2U && cursor[0] == '.' &&
			   cursor[1] == '.') {
			if (output_length == 0U) {
				return -EPERM;
			}
			if (output[output_length - 1U] == '/') {
				--output_length;
			}
			while (output_length > 0U &&
			       output[output_length - 1U] != '/') {
				--output_length;
			}
		} else {
			if (output_length != 0U && output[output_length - 1U] != '/') {
				if (output_length + 1U >= output_size) {
					return -ENAMETOOLONG;
				}
				output[output_length++] = '/';
			}
			if (output_length + segment_length >= output_size) {
				return -ENAMETOOLONG;
			}
			memcpy(output + output_length, cursor, segment_length);
			output_length += segment_length;
		}
		if (slash == NULL) {
			break;
		}
		cursor = slash + 1U;
	}
	output[output_length] = '\0';
	return papermono_zip_safe_path(output) ? 0 : -EPERM;
}

static void package_base(const char *package_path, char *base,
	size_t base_size)
{
	const char *slash = strrchr(package_path, '/');
	const size_t length = slash == NULL ? 0U :
		MIN((size_t)(slash - package_path + 1U), base_size - 1U);

	memcpy(base, package_path, length);
	base[length] = '\0';
}

static void default_chapter_title(struct papermono_epub_chapter *chapter,
	uint16_t index)
{
	(void)snprintf(chapter->title, sizeof(chapter->title), "第 %u 章",
		(unsigned int)index + 1U);
}

static size_t copy_plain_text(const char *start, const char *end, char *output,
	size_t output_size)
{
	size_t written = 0U;
	bool in_tag = false;

	for (const char *cursor = start; cursor < end && written + 1U < output_size;
	     ++cursor) {
		if (*cursor == '<') {
			in_tag = true;
			continue;
		}
		if (*cursor == '>') {
			in_tag = false;
			continue;
		}
		if (in_tag) {
			continue;
		}
		if (*cursor == '&') {
			const char *semicolon = memchr(cursor, ';', (size_t)(end - cursor));
			const size_t entity_length = semicolon == NULL ? 0U :
				(size_t)(semicolon - cursor - 1U);

			if (entity_length == 3U &&
			    memcmp(cursor + 1U, "amp", 3U) == 0) {
				output[written++] = '&';
				cursor = semicolon;
				continue;
			}
			if (entity_length == 4U &&
			    memcmp(cursor + 1U, "nbsp", 4U) == 0) {
				output[written++] = ' ';
				cursor = semicolon;
				continue;
			}
		}
		if (*cursor == '\r' || *cursor == '\n' || *cursor == '\t') {
			if (written > 0U && output[written - 1U] != ' ') {
				output[written++] = ' ';
			}
		} else {
			output[written++] = *cursor;
		}
	}
	while (written > 0U && output[written - 1U] == ' ') {
		--written;
	}
	output[written] = '\0';
	return written;
}

static bool xml_tag_matches(const char *tag, const char *name, bool *closing)
{
	const char *cursor = tag + 1U;
	const size_t length = strlen(name);

	*closing = false;
	if (*cursor == '/') {
		*closing = true;
		++cursor;
	}
	if (strncmp(cursor, name, length) != 0) {
		return false;
	}
	cursor += length;
	return *cursor == '>' || *cursor == '/' || *cursor == ' ' ||
		*cursor == '\t' || *cursor == '\r' || *cursor == '\n';
}

/* Find the '>' that closes the tag starting at `tag`, stopping at `limit`.
 *
 * A plain memchr() is wrong here: a '>' inside a quoted attribute value (for
 * example href='a>b.xhtml') does not end the tag, and taking it as the end
 * truncates the tag so that xml_tag_matches() never recognises it.
 */
static const char *find_tag_end(const char *tag, const char *limit)
{
	char quote = '\0';

	for (const char *cursor = tag + 1U; cursor < limit; ++cursor) {
		if (quote != '\0') {
			if (*cursor == quote) {
				quote = '\0';
			}
			continue;
		}
		if (*cursor == '"' || *cursor == '\'') {
			quote = *cursor;
			continue;
		}
		if (*cursor == '>') {
			return cursor;
		}
	}
	return NULL;
}

/* Advance the nesting depth of <name> from *scan_pos up to position, and leave
 * *scan_pos at the last byte actually scanned.
 *
 * Callers walk the navigation document in a single forward pass, so they call
 * this once per entry with a monotonically increasing position. Rescanning
 * from the start of the buffer each time made TOC parsing quadratic: a 500
 * chapter book re-scanned roughly 6 MB of PSRAM per open. Carrying the depth
 * across iterations makes the whole parse linear.
 *
 * Every recognised tag is stepped over exactly once, so callers drive this
 * with boundaries that sit on tag edges: scanning up to a tag's '<' reports
 * the depth outside it, and scanning over the tag itself counts it.
 */
static void xml_depth_advance(const char **scan_pos, uint8_t *depth,
	const char *position, const char *name)
{
	const char *cursor = *scan_pos;

	if (position < cursor) {
		return;
	}
	while (cursor < position) {
		const char *tag = memchr(cursor, '<', (size_t)(position - cursor));
		const char *end;
		bool closing;

		if (tag == NULL) {
			break;
		}
		end = find_tag_end(tag, position);
		if (end == NULL) {
			break;
		}
		if (xml_tag_matches(tag, name, &closing)) {
			if (closing) {
				if (*depth > 0U) {
					--*depth;
				}
			} else if ((end == tag || end[-1] != '/') &&
				   *depth < UINT8_MAX) {
				++*depth;
			}
		}
		cursor = end + 1U;
	}
	*scan_pos = cursor;
}

static void assign_navigation_title(struct papermono_epub_book *epub,
	const char *base, const char *href, const char *title_start,
	const char *title_end, uint8_t level)
{
	char path[PAPERMONO_ZIP_NAME_MAX + 1U];
	char title[PAPERMONO_EPUB_TITLE_MAX + 1U];

	if (normalize_path(base, href, path, sizeof(path)) < 0 ||
	    copy_plain_text(title_start, title_end, title, sizeof(title)) == 0U) {
		return;
	}
	for (uint16_t index = 0U; index < epub->chapter_count; ++index) {
		if (strcmp(epub->chapters[index].path, path) == 0) {
			bool first_for_chapter = true;

			for (uint16_t toc = 0U; toc < epub->toc_count; ++toc) {
				if (epub->toc[toc].chapter == index) {
					first_for_chapter = false;
					break;
				}
			}
			if (first_for_chapter) {
				copy_cstring(epub->chapters[index].title,
					sizeof(epub->chapters[index].title), title);
			}
			if (epub->toc_count < ARRAY_SIZE(epub->toc)) {
				struct papermono_epub_toc_entry *entry =
					&epub->toc[epub->toc_count++];

				copy_cstring(entry->title, sizeof(entry->title), title);
				entry->chapter = index;
				entry->level = MIN(level, 7U);
			}
			return;
		}
	}
}

static void parse_epub3_navigation(struct papermono_epub_book *epub,
	const char *base)
{
	const char *cursor = metadata_buffer;
	const char *scan_pos = metadata_buffer;
	uint8_t list_depth = 0U;

	while ((cursor = find_tag(cursor, "a")) != NULL) {
		const char *tag_end = strchr(cursor, '>');
		const char *close;
		char href[PAPERMONO_ZIP_NAME_MAX + 1U];

		xml_depth_advance(&scan_pos, &list_depth, cursor, "ol");
		if (tag_end == NULL) {
			break;
		}
		close = strstr(tag_end + 1U, "</a>");
		if (close == NULL) {
			break;
		}
		if (copy_attribute(cursor, tag_end, "href", href,
			sizeof(href)) == 0) {
			assign_navigation_title(epub, base, href, tag_end + 1U,
				close, list_depth > 0U ? list_depth - 1U : 0U);
		}
		cursor = close + 4U;
	}
}

/* Record the nesting level of every navPoint opening tag, in document order.
 *
 * A single left-to-right pass over the navigation document is the only way to
 * get these levels right. Deriving them from the entry walk instead is fragile:
 * find_tag() returns the next *opening* tag, so the region between two sibling
 * entries contains closing tags that the walk never visits, and a sibling that
 * outdents keeps the depth of the subtree it just left. Counting every tag here
 * costs one linear scan and removes that whole class of error.
 *
 * Returns the number of levels recorded, capped at `capacity`.
 */
static size_t collect_navpoint_levels(uint8_t *depths, size_t capacity)
{
	const char *limit = metadata_buffer + strlen(metadata_buffer);
	const char *cursor = metadata_buffer;
	size_t count = 0U;
	uint8_t depth = 0U;

	for (;;) {
		const char *tag = strchr(cursor, '<');
		const char *end;
		bool closing;

		if (tag == NULL) {
			break;
		}
		end = find_tag_end(tag, limit);
		if (end == NULL) {
			break;
		}
		if (xml_tag_matches(tag, "navPoint", &closing)) {
			if (closing) {
				if (depth > 0U) {
					--depth;
				}
			} else if (end == tag || end[-1] != '/') {
				if (count == capacity) {
					return count;
				}
				depths[count++] = depth;
				if (depth < UINT8_MAX) {
					++depth;
				}
			}
		}
		cursor = end + 1U;
	}
	return count;
}

static void parse_ncx_navigation(struct papermono_epub_book *epub,
	const char *base)
{
	static uint8_t levels[PAPERMONO_EPUB_TOC_LIMIT];
	const size_t level_count =
		collect_navpoint_levels(levels, ARRAY_SIZE(levels));
	const char *cursor = metadata_buffer;
	size_t level_index = 0U;

	while ((cursor = find_tag(cursor, "navPoint")) != NULL) {
		const char *tag_end = strchr(cursor, '>');
		const char *point_end = strstr(cursor, "</navPoint>");
		const char *text_tag = find_tag(cursor, "text");
		const char *text_start = text_tag == NULL ? NULL : strchr(text_tag, '>');
		const char *text_end = text_start == NULL ? NULL :
			strstr(text_start + 1U, "</text>");
		const char *content = find_tag(cursor, "content");
		const char *content_end = content == NULL ? NULL : strchr(content, '>');
		char href[PAPERMONO_ZIP_NAME_MAX + 1U];
		uint8_t level = 0U;

		if (tag_end == NULL) {
			break;
		}
		if (level_index < level_count) {
			level = levels[level_index++];
		}

		if (point_end == NULL || text_start == NULL ||
		    text_end == NULL ||
		    content == NULL || content_end == NULL || text_end > point_end ||
		    content > point_end) {
			break;
		}
		if (copy_attribute(content, content_end, "src", href,
			sizeof(href)) == 0) {
			assign_navigation_title(epub, base, href, text_start + 1U,
				text_end, level);
		}
		/* Advance past this opening tag, not its first closing tag. Nested
		 * navPoint elements must remain visible to the next iteration.
		 */
		cursor = tag_end + 1U;
	}
}

static int parse_package(struct papermono_epub_book *epub,
	const char *package_path)
{
	char base[PAPERMONO_ZIP_NAME_MAX + 1U];
	char navigation_path[PAPERMONO_ZIP_NAME_MAX + 1U] = { 0 };
	bool navigation_is_ncx = false;
	const char *cursor = metadata_buffer;
	size_t manifest_count = 0U;
	size_t spine_count = 0U;
	int ret;

	package_base(package_path, base, sizeof(base));
	const char *title_tag = find_tag(metadata_buffer, "dc:title");
	const char *title_start = title_tag == NULL ? NULL : strchr(title_tag, '>');
	const char *title_end = title_start == NULL ? NULL :
		strstr(title_start + 1U, "</dc:title>");

	if (title_start != NULL && title_end != NULL) {
		(void)copy_plain_text(title_start + 1U, title_end, epub->title,
			sizeof(epub->title));
	}
	memset(manifest, 0, sizeof(manifest));
	while ((cursor = find_tag(cursor, "item")) != NULL &&
	       manifest_count < ARRAY_SIZE(manifest)) {
		const char *tag_end = strchr(cursor, '>');
		struct epub_manifest_item *item = &manifest[manifest_count];

		if (tag_end == NULL) {
			return -EBADMSG;
		}
		ret = copy_attribute(cursor, tag_end, "id", item->id,
			sizeof(item->id));
		ret = ret ?: copy_attribute(cursor, tag_end, "href", item->href,
			sizeof(item->href));
		ret = ret ?: copy_attribute(cursor, tag_end, "media-type",
			item->media_type, sizeof(item->media_type));
		(void)copy_attribute(cursor, tag_end, "properties",
			item->properties, sizeof(item->properties));
		if (ret == 0) {
			if (strstr(item->properties, "nav") != NULL ||
			    strcmp(item->media_type,
				"application/x-dtbncx+xml") == 0) {
				if (normalize_path(base, item->href, navigation_path,
					sizeof(navigation_path)) == 0) {
					navigation_is_ncx = strcmp(item->media_type,
						"application/x-dtbncx+xml") == 0;
				}
			}
			++manifest_count;
		}
		cursor = tag_end + 1U;
	}
	cursor = metadata_buffer;
	while ((cursor = find_tag(cursor, "itemref")) != NULL &&
	       spine_count < ARRAY_SIZE(spine_ids)) {
		const char *tag_end = strchr(cursor, '>');

		if (tag_end == NULL) {
			return -EBADMSG;
		}
		if (copy_attribute(cursor, tag_end, "idref",
			spine_ids[spine_count], sizeof(spine_ids[spine_count])) == 0) {
			++spine_count;
		}
		cursor = tag_end + 1U;
	}
	for (size_t spine_index = 0U; spine_index < spine_count;
	     ++spine_index) {
		for (size_t item_index = 0U; item_index < manifest_count;
		     ++item_index) {
			if (strcmp(spine_ids[spine_index], manifest[item_index].id) != 0 ||
			    strcmp(manifest[item_index].media_type,
				"application/xhtml+xml") != 0) {
				continue;
			}
			struct papermono_epub_chapter *chapter =
				&epub->chapters[epub->chapter_count];

			ret = normalize_path(base, manifest[item_index].href,
				chapter->path, sizeof(chapter->path));
			if (ret < 0) {
				return ret;
			}
			default_chapter_title(chapter, epub->chapter_count);
			++epub->chapter_count;
			break;
		}
	}
	if (epub->chapter_count == 0U) {
		return -ENODATA;
	}
	if (navigation_path[0] != '\0' &&
	    extract_string(&epub->archive, navigation_path, metadata_buffer,
		sizeof(metadata_buffer), NULL) == 0) {
		char navigation_base[PAPERMONO_ZIP_NAME_MAX + 1U];

		package_base(navigation_path, navigation_base,
			sizeof(navigation_base));
		if (navigation_is_ncx) {
			parse_ncx_navigation(epub, navigation_base);
		} else {
			parse_epub3_navigation(epub, navigation_base);
		}
	}
	if (epub->toc_count == 0U) {
		for (uint16_t chapter = 0U;
		     chapter < epub->chapter_count &&
		     epub->toc_count < ARRAY_SIZE(epub->toc); ++chapter) {
			struct papermono_epub_toc_entry *entry =
				&epub->toc[epub->toc_count++];

			copy_cstring(entry->title, sizeof(entry->title),
				epub->chapters[chapter].title);
			entry->chapter = chapter;
		}
	}
	return 0;
}

static int epub_open_locked(struct papermono_epub_book *epub,
	const struct papermono_content_entry *source)
{
	char package_path[PAPERMONO_ZIP_NAME_MAX + 1U];
	const char *rootfile;
	const char *tag_end;
	int ret;

	if (epub == NULL || source == NULL) {
		return -EINVAL;
	}
	memset(epub, 0, sizeof(*epub));
	/* Belongs to whichever book was open before, regardless of id. */
	chapter_cache_invalidate();
	ret = papermono_zip_open(&epub->archive, source);
	ret = ret ?: extract_string(&epub->archive, "META-INF/container.xml",
		container_buffer, sizeof(container_buffer), NULL);
	if (ret < 0) {
		return ret;
	}
	rootfile = find_tag(container_buffer, "rootfile");
	tag_end = rootfile == NULL ? NULL : strchr(rootfile, '>');
	if (rootfile == NULL || tag_end == NULL) {
		return -EBADMSG;
	}
	ret = copy_attribute(rootfile, tag_end, "full-path", package_path,
		sizeof(package_path));
	if (ret < 0 || !papermono_zip_safe_path(package_path)) {
		return ret < 0 ? ret : -EPERM;
	}
	ret = extract_string(&epub->archive, package_path, metadata_buffer,
		sizeof(metadata_buffer), NULL);
	if (ret < 0) {
		return ret;
	}
	copy_cstring(epub->title, sizeof(epub->title), source->title);
	ret = parse_package(epub, package_path);
	if (ret == 0) {
		LOG_INF("EPUB opened: %s (%u spine items)", epub->title,
			epub->chapter_count);
	}
	return ret;
}

struct xhtml_page_context {
	char *output;
	size_t output_size;
	size_t output_length;
	uint32_t text_start;
	uint32_t text_position;
	uint32_t text_end;
	uint8_t line;
	uint8_t column;
	uint8_t max_lines;
	uint8_t max_columns;
	/* Keep consuming the stream after the page fills, so the rest of the
	 * chapter still reaches the cache instead of being thrown away.
	 */
	bool collect;
	char tag[96];
	uint8_t tag_length;
	char entity[16];
	uint8_t entity_length;
	uint8_t utf8_pending[4];
	uint8_t utf8_length;
	bool in_tag;
	bool in_entity;
	bool skip_content;
	bool stopped;
};

static int emit_codepoint(struct xhtml_page_context *page, uint32_t codepoint)
{
	char encoded[4];
	size_t encoded_length;

	if (page->text_position < page->text_start) {
		++page->text_position;
		return 0;
	}
	if (papermono_text_should_wrap_before(codepoint, page->column,
		page->max_columns)) {
		if (page->line + 1U >= page->max_lines) {
			page->text_end = page->text_position;
			page->stopped = true;
			return page->collect ? 0 :
				PAPERMONO_ZIP_EXTRACT_STOPPED;
		}
		if (page->output_length + 1U >= page->output_size) {
			return -ENOSPC;
		}
		page->output[page->output_length++] = '\n';
		++page->line;
		page->column = 0U;
	}
	encoded_length = papermono_text_encode_utf8(codepoint, encoded);
	if (encoded_length == 0U ||
	    page->output_length + encoded_length >= page->output_size) {
		return -ENOSPC;
	}
	memcpy(page->output + page->output_length, encoded, encoded_length);
	page->output_length += encoded_length;
	++page->text_position;
	page->text_end = page->text_position;
	if (codepoint == '\n') {
		if (page->line + 1U >= page->max_lines) {
			page->stopped = true;
			return page->collect ? 0 :
				PAPERMONO_ZIP_EXTRACT_STOPPED;
		}
		++page->line;
		page->column = 0U;
	} else {
		++page->column;
	}
	return 0;
}

static bool tag_is(const char *tag, const char *name)
{
	const char *cursor = tag;

	while (*cursor == ' ' || *cursor == '\t' || *cursor == '/') {
		++cursor;
	}
	return name_equal(cursor, strcspn(cursor, " \t\r\n/>"), name);
}

static int finish_tag(struct xhtml_page_context *page)
{
	page->tag[page->tag_length] = '\0';
	const bool closing = page->tag[0] == '/';
	const bool script = tag_is(page->tag, "script");
	const bool style = tag_is(page->tag, "style");
	int ret = 0;

	if (script || style) {
		page->skip_content = !closing;
	} else if (!page->skip_content &&
		   (tag_is(page->tag, "p") || tag_is(page->tag, "br") ||
		    tag_is(page->tag, "div") || tag_is(page->tag, "li") ||
		    tag_is(page->tag, "h1") || tag_is(page->tag, "h2") ||
		    tag_is(page->tag, "h3") || tag_is(page->tag, "h4") ||
		    tag_is(page->tag, "h5") || tag_is(page->tag, "h6") ||
		    tag_is(page->tag, "blockquote"))) {
		if (page->text_position > 0U && page->column != 0U) {
			ret = emit_codepoint(page, '\n');
		}
	}
	page->tag_length = 0U;
	return ret;
}

static uint32_t named_entity(const char *entity)
{
	if (strcmp(entity, "amp") == 0) {
		return '&';
	}
	if (strcmp(entity, "lt") == 0) {
		return '<';
	}
	if (strcmp(entity, "gt") == 0) {
		return '>';
	}
	if (strcmp(entity, "quot") == 0) {
		return '"';
	}
	if (strcmp(entity, "apos") == 0) {
		return '\'';
	}
	if (strcmp(entity, "nbsp") == 0) {
		return ' ';
	}
	return 0xFFFDU;
}

static int finish_entity(struct xhtml_page_context *page)
{
	uint32_t codepoint;

	page->entity[page->entity_length] = '\0';
	if (page->entity[0] == '#') {
		const bool hex = page->entity[1] == 'x' ||
			page->entity[1] == 'X';
		uint32_t value;

		codepoint = parse_unsigned(page->entity + (hex ? 2U : 1U),
			hex ? 16U : 10U, &value) == 0 ? value : 0xFFFDU;
		/* A numeric reference can name a value that is not a Unicode scalar
		 * (above U+10FFFF, or a surrogate). The UTF-8 encoder rejects those
		 * by returning 0, which emit_codepoint() cannot tell apart from a
		 * full output buffer, so one out-of-range reference used to abort the
		 * rest of the chapter. Map it to the replacement character instead,
		 * which is what XML specifies for a character reference outside the
		 * Char production.
		 */
		if (codepoint > 0x10FFFFU ||
		    (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
			codepoint = 0xFFFDU;
		}
	} else {
		codepoint = named_entity(page->entity);
	}
	page->entity_length = 0U;
	return emit_codepoint(page, codepoint);
}

static int feed_text_byte(struct xhtml_page_context *page, uint8_t value)
{
	struct papermono_decoded_codepoint decoded;
	int ret;

	if (page->utf8_length >= sizeof(page->utf8_pending)) {
		page->utf8_length = 0U;
	}
	page->utf8_pending[page->utf8_length++] = value;
	ret = papermono_text_decode_one(PAPERMONO_TEXT_ENCODING_UTF8,
		page->utf8_pending, page->utf8_length, &decoded);
	if (ret == -EAGAIN) {
		return 0;
	}
	if (ret < 0) {
		decoded.codepoint = 0xFFFDU;
	}
	page->utf8_length = 0U;
	return emit_codepoint(page, decoded.codepoint);
}

static int xhtml_write(const uint8_t *data, size_t length, void *context)
{
	struct xhtml_page_context *page = context;

	for (size_t index = 0U; index < length; ++index) {
		const uint8_t value = data[index];
		int ret = 0;

		if (page->in_tag) {
			if (value == '>') {
				page->in_tag = false;
				ret = finish_tag(page);
			} else if (page->tag_length + 1U < sizeof(page->tag)) {
				page->tag[page->tag_length++] = (char)value;
			}
		} else if (page->skip_content) {
			if (value == '<') {
				page->in_tag = true;
				page->tag_length = 0U;
			}
		} else if (page->in_entity) {
			if (value == ';') {
				page->in_entity = false;
				ret = finish_entity(page);
			} else if (page->entity_length + 1U < sizeof(page->entity)) {
				page->entity[page->entity_length++] = (char)value;
			} else {
				page->in_entity = false;
				page->entity_length = 0U;
				ret = emit_codepoint(page, 0xFFFDU);
			}
		} else if (value == '<') {
			page->in_tag = true;
			page->tag_length = 0U;
		} else if (value == '&') {
			page->in_entity = true;
			page->entity_length = 0U;
		} else if (value != '\r' && value != '\n' && value != '\t') {
			ret = feed_text_byte(page, value);
		} else if (value == '\n' && page->column != 0U) {
			ret = emit_codepoint(page, ' ');
		}
		if (ret != 0) {
			return ret;
		}
	}
	return 0;
}

struct chapter_tee {
	struct xhtml_page_context *page;
	bool enabled;
};

/* Feed the page state machine while copying the inflated bytes aside. */
static int xhtml_write_tee(const uint8_t *data, size_t length, void *context)
{
	struct chapter_tee *tee = context;

	if (tee->enabled) {
		if (chapter_cache.length + length > sizeof(chapter_buffer)) {
			/* Too large to keep: give up on caching this chapter. */
			tee->enabled = false;
			chapter_cache_invalidate();
		} else {
			memcpy(chapter_buffer + chapter_cache.length, data,
				length);
			chapter_cache.length += (uint32_t)length;
		}
	}
	/* Once the page is full there is nothing left to emit; the remaining
	 * bytes are only being consumed to complete the cached chapter.
	 */
	if (tee->page->stopped) {
		return 0;
	}
	/* Return the real status. papermono_zip_extract() folds any non-zero
	 * callback result into PAPERMONO_ZIP_EXTRACT_STOPPED, so masking a
	 * failure here would make the caller believe the stream was fully
	 * consumed while the chapter was in fact truncated, and the shortened
	 * prefix would be cached as a complete chapter.
	 */
	return xhtml_write(data, length, tee->page);
}

static int replay_cached_chapter(struct xhtml_page_context *page)
{
	const char *data = chapter_buffer;
	size_t remaining = chapter_cache.length;

	while (remaining > 0U) {
		const size_t chunk = MIN(remaining, 1024U);
		int ret = xhtml_write((const uint8_t *)data, chunk, page);

		if (ret == PAPERMONO_ZIP_EXTRACT_STOPPED) {
			page->stopped = true;
			return 0;
		}
		if (ret < 0) {
			return ret;
		}
		data += chunk;
		remaining -= chunk;
	}
	return 0;
}

static int inflate_and_cache_chapter(struct papermono_epub_book *epub,
	uint16_t chapter, const struct papermono_zip_entry *entry,
	struct xhtml_page_context *page)
{
	struct chapter_tee tee = { .page = page, .enabled = true };
	int ret;

	page->collect = true;
	chapter_cache_invalidate();
	chapter_cache.length = 0U;
	ret = papermono_zip_extract(&epub->archive, entry, xhtml_write_tee,
		&tee);
	/* A negative status means the page state machine rejected the stream:
	 * either the output buffer is too small or malformed content stopped the
	 * collection. Either way the buffer holds a prefix, not the chapter.
	 */
	if (ret < 0) {
		chapter_cache_invalidate();
		return ret;
	}
	/* Collection keeps going past the page break, so a clean return means the
	 * stream was consumed to the end and the buffer holds the whole chapter.
	 * A chapter too large for the buffer turns tee off and leaves it invalid.
	 */
	if (tee.enabled) {
		chapter_cache.chapter = chapter;
		chapter_cache.complete = true;
		memcpy(chapter_cache.id, epub->archive.source.id,
			sizeof(chapter_cache.id));
	} else {
		chapter_cache_invalidate();
	}
	return 0;
}

static int epub_read_page_locked(struct papermono_epub_book *epub,
	uint16_t chapter, uint32_t text_start, char *output, size_t output_size,
	uint8_t columns, uint8_t lines, struct papermono_epub_page *result)
{
	struct papermono_zip_entry entry;
	struct xhtml_page_context page = {
		.output = output,
		.output_size = output_size,
		.text_start = text_start,
		.max_lines = lines,
		.max_columns = columns,
	};
	int ret;

	if (epub == NULL || output == NULL || output_size < 2U ||
	    result == NULL || chapter >= epub->chapter_count || columns == 0U ||
	    lines == 0U) {
		return -EINVAL;
	}
	ret = papermono_zip_find(&epub->archive, epub->chapters[chapter].path,
		&entry);
	/* chapters[].size is NOT filled here. It holds the chapter length in
	 * emitted codepoints, the same unit as text_start/text_end, and that total
	 * is only known once the chapter has been read to its end, so the reader
	 * records it there. entry.uncompressed_size is inflated BYTES and used to
	 * be stored here, which made the progress figure add codepoints to bytes.
	 */
	if (ret == 0 && chapter_cache_matches(epub, chapter)) {
		ret = replay_cached_chapter(&page);
	} else if (ret == 0) {
		ret = inflate_and_cache_chapter(epub, chapter, &entry, &page);
	}
	if (ret < 0) {
		return ret;
	}
	output[page.output_length] = '\0';
	*result = (struct papermono_epub_page) {
		.text_start = text_start,
		.text_end = page.text_end,
		.chapter_eof = !page.stopped,
	};
	return 0;
}

int papermono_epub_open(struct papermono_epub_book *epub,
	const struct papermono_content_entry *source)
{
	k_mutex_lock(&epub_parse_lock, K_FOREVER);
	const int ret = epub_open_locked(epub, source);

	k_mutex_unlock(&epub_parse_lock);
	return ret;
}

int papermono_epub_read_page(struct papermono_epub_book *epub,
	uint16_t chapter, uint32_t text_start, char *output,
	size_t output_size, uint8_t columns, uint8_t lines,
	struct papermono_epub_page *result)
{
	k_mutex_lock(&epub_parse_lock, K_FOREVER);
	const int ret = epub_read_page_locked(epub, chapter, text_start, output,
		output_size, columns, lines, result);

	k_mutex_unlock(&epub_parse_lock);
	return ret;
}
