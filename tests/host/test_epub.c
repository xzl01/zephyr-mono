/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <papermono/epub.h>
#include <papermono/reader.h>

struct fake_entry {
	const char *name;
	const char *data;
};

static char long_chapter[1400];
static const struct fake_entry valid_entries[] = {
	{ "META-INF/container.xml",
	  "<container><rootfiles><rootfile full-path='OPS/package.opf'/></rootfiles></container>" },
	{ "OPS/package.opf",
	  "<package><metadata><dc:title>测试书</dc:title></metadata><manifest>"
	  "<item id='one' href='chapter1.xhtml' media-type='application/xhtml+xml'/>"
	  "<item id='two' href='chapter2.xhtml' media-type='application/xhtml+xml'/>"
	  "<item id='nav' href='nav.xhtml' media-type='application/xhtml+xml' properties='nav'/>"
	  "</manifest><spine><itemref idref='two'/><itemref idref='one'/></spine></package>" },
	{ "OPS/nav.xhtml",
	  "<nav><ol><li><a href='chapter2.xhtml#p'>第二章</a>"
	  "<ol><li><a href='chapter1.xhtml'>第一章</a></li></ol>"
	  "</li></ol></nav>" },
	{ "OPS/chapter1.xhtml", NULL },
	{ "OPS/chapter2.xhtml",
	  "<html><style>ignore</style><body><h1>第二章</h1><p>中文 &amp; English</p>"
	  "<script>ignore</script><blockquote>引用</blockquote></body></html>" },
};

static const struct fake_entry ncx_entries[] = {
	{ "META-INF/container.xml",
	  "<container><rootfile full-path='OPS/package.opf'/></container>" },
	{ "OPS/package.opf",
	  "<package><manifest>"
	  "<item id='one' href='chapter1.xhtml' media-type='application/xhtml+xml'/>"
	  "<item id='two' href='chapter2.xhtml' media-type='application/xhtml+xml'/>"
	  "<item id='toc' href='toc.ncx' media-type='application/x-dtbncx+xml'/>"
	  "</manifest><spine toc='toc'><itemref idref='one'/>"
	  "<itemref idref='two'/></spine></package>" },
	{ "OPS/toc.ncx",
	  "<ncx><navMap><navPoint><navLabel><text>第一章</text></navLabel>"
	  "<content src='chapter1.xhtml'/><navPoint><navLabel><text>第二章</text>"
	  "</navLabel><content src='chapter2.xhtml'/></navPoint>"
	  "</navPoint></navMap></ncx>" },
	{ "OPS/chapter1.xhtml", "<html><body><p>one</p></body></html>" },
	{ "OPS/chapter2.xhtml", "<html><body><p>two</p></body></html>" },
};

/* Four levels deep, then back out to a sibling: the depth has to be carried
 * correctly across entries, including the decrease when a navPoint closes.
 */
static const struct fake_entry deep_ncx_entries[] = {
	{ "META-INF/container.xml",
	  "<container><rootfile full-path='OPS/package.opf'/></container>" },
	{ "OPS/package.opf",
	  "<package><manifest>"
	  "<item id='c1' href='c1.xhtml' media-type='application/xhtml+xml'/>"
	  "<item id='c2' href='c2.xhtml' media-type='application/xhtml+xml'/>"
	  "<item id='c3' href='c3.xhtml' media-type='application/xhtml+xml'/>"
	  "<item id='c4' href='c4.xhtml' media-type='application/xhtml+xml'/>"
	  "<item id='toc' href='toc.ncx' media-type='application/x-dtbncx+xml'/>"
	  "</manifest><spine toc='toc'><itemref idref='c1'/>"
	  "<itemref idref='c2'/><itemref idref='c3'/>"
	  "<itemref idref='c4'/></spine></package>" },
	{ "OPS/toc.ncx",
	  "<ncx><navMap>"
	  "<navPoint><navLabel><text>L0</text></navLabel>"
	  "<content src='c1.xhtml'/>"
	  "<navPoint><navLabel><text>L1</text></navLabel>"
	  "<content src='c2.xhtml'/>"
	  "<navPoint><navLabel><text>L2</text></navLabel>"
	  "<content src='c3.xhtml'/>"
	  "<navPoint><navLabel><text>L3</text></navLabel>"
	  "<content src='c4.xhtml'/>"
	  "</navPoint></navPoint></navPoint></navPoint>"
	  "<navPoint><navLabel><text>sibling</text></navLabel>"
	  "<content src='c1.xhtml'/>"
	  "</navPoint>"
	  "</navMap></ncx>" },
	{ "OPS/c1.xhtml", "<html><body><p>one</p></body></html>" },
	{ "OPS/c2.xhtml", "<html><body><p>two</p></body></html>" },
	{ "OPS/c3.xhtml", "<html><body><p>three</p></body></html>" },
	{ "OPS/c4.xhtml", "<html><body><p>four</p></body></html>" },
};

/* Flat NCX: three sibling leaf navPoints, the shape most exporters emit. */
static const struct fake_entry flat_ncx_entries[] = {
	{ "META-INF/container.xml",
	  "<container><rootfile full-path='OPS/package.opf'/></container>" },
	{ "OPS/package.opf",
	  "<package><manifest>"
	  "<item id='c1' href='c1.xhtml' media-type='application/xhtml+xml'/>"
	  "<item id='c2' href='c2.xhtml' media-type='application/xhtml+xml'/>"
	  "<item id='c3' href='c3.xhtml' media-type='application/xhtml+xml'/>"
	  "<item id='toc' href='toc.ncx' media-type='application/x-dtbncx+xml'/>"
	  "</manifest><spine toc='toc'><itemref idref='c1'/>"
	  "<itemref idref='c2'/><itemref idref='c3'/>"
	  "</spine></package>" },
	{ "OPS/toc.ncx",
	  "<ncx><navMap>"
	  "<navPoint id='a'><navLabel><text>ONE</text></navLabel>"
	  "<content src='c1.xhtml'/></navPoint>"
	  "<navPoint id='b'><navLabel><text>TWO</text></navLabel>"
	  "<content src='c2.xhtml'/></navPoint>"
	  "<navPoint id='c'><navLabel><text>THREE</text></navLabel>"
	  "<content src='c3.xhtml'/></navPoint>"
	  "</navMap></ncx>" },
	{ "OPS/c1.xhtml", "<html><body><p>one</p></body></html>" },
	{ "OPS/c2.xhtml", "<html><body><p>two</p></body></html>" },
	{ "OPS/c3.xhtml", "<html><body><p>three</p></body></html>" },
};

/* Numeric character references that are not Unicode scalars, mixed with
 * ordinary text on both sides. */
static const struct fake_entry entity_entries[] = {
	{ "META-INF/container.xml",
	  "<container><rootfile full-path='OPS/package.opf'/></container>" },
	{ "OPS/package.opf",
	  "<package><manifest>"
	  "<item id='one' href='chapter1.xhtml' media-type='application/xhtml+xml'/>"
	  "</manifest><spine><itemref idref='one'/></spine></package>" },
	{ "OPS/chapter1.xhtml",
	  "<html><body><p>PRE &#x110000; POST</p></body></html>" },
};

static const struct fake_entry traversal_entries[] = {
	{ "META-INF/container.xml",
	  "<container><rootfile full-path='OPS/package.opf'/></container>" },
	{ "OPS/package.opf",
	  "<package><manifest><item id='bad' href='../../evil.xhtml' "
	  "media-type='application/xhtml+xml'/></manifest>"
	  "<spine><itemref idref='bad'/></spine></package>" },
};

static const struct fake_entry *active_entries;
static size_t active_count;

bool papermono_zip_safe_path(const char *path)
{
	return path != NULL && path[0] != '\0' && path[0] != '/' &&
		strstr(path, "../") == NULL && strstr(path, "/..") == NULL &&
		strchr(path, '\\') == NULL && strchr(path, ':') == NULL;
}

int papermono_zip_open(struct papermono_zip_archive *archive,
	const struct papermono_content_entry *source)
{
	archive->source = *source;
	archive->entry_count = (uint16_t)active_count;
	return 0;
}

int papermono_zip_find(const struct papermono_zip_archive *archive,
	const char *name, struct papermono_zip_entry *entry)
{
	(void)archive;
	for (size_t index = 0U; index < active_count; ++index) {
		if (strcmp(active_entries[index].name, name) == 0) {
			memset(entry, 0, sizeof(*entry));
			(void)snprintf(entry->name, sizeof(entry->name), "%s", name);
			const char *data = active_entries[index].data == NULL ?
				long_chapter : active_entries[index].data;
			entry->uncompressed_size = (uint32_t)strlen(data);
			return 0;
		}
	}
	return -ENOENT;
}

int papermono_zip_extract(const struct papermono_zip_archive *archive,
	const struct papermono_zip_entry *entry, papermono_zip_write_cb callback,
	void *context)
{
	(void)archive;
	for (size_t index = 0U; index < active_count; ++index) {
		if (strcmp(active_entries[index].name, entry->name) != 0) {
			continue;
		}
		const char *data = active_entries[index].data == NULL ?
			long_chapter : active_entries[index].data;
		const size_t length = strlen(data);
		for (size_t offset = 0U; offset < length; offset += 7U) {
			const size_t chunk = length - offset < 7U ? length - offset : 7U;
			int ret = callback((const uint8_t *)data + offset, chunk,
				context);
			if (ret != 0) {
				return PAPERMONO_ZIP_EXTRACT_STOPPED;
			}
		}
		return 0;
	}
	return -ENOENT;
}

static void prepare_long_chapter(void)
{
	size_t offset = 0U;
	offset += (size_t)snprintf(long_chapter + offset,
		sizeof(long_chapter) - offset, "<html><body><p>");
	while (offset + 20U < sizeof(long_chapter)) {
		long_chapter[offset] = 'A' + (char)(offset % 26U);
		++offset;
	}
	(void)snprintf(long_chapter + offset, sizeof(long_chapter) - offset,
		"</p></body></html>");
}

static void test_spine_navigation_and_paging(void)
{
	struct papermono_content_entry source = {
		.kind = PAPERMONO_CONTENT_EPUB,
		.size = 4096U,
	};
	struct papermono_epub_book book;
	struct papermono_epub_page page;
	char output[4096];

	(void)snprintf(source.title, sizeof(source.title), "fallback");
	active_entries = valid_entries;
	active_count = sizeof(valid_entries) / sizeof(valid_entries[0]);
	assert(papermono_epub_open(&book, &source) == 0);
	assert(strcmp(book.title, "测试书") == 0);
	assert(book.chapter_count == 2U);
	assert(strcmp(book.chapters[0].path, "OPS/chapter2.xhtml") == 0);
	assert(strcmp(book.chapters[0].title, "第二章") == 0);
	assert(strcmp(book.chapters[1].title, "第一章") == 0);
	assert(book.toc_count == 2U);
	assert(book.toc[0].chapter == 0U && book.toc[0].level == 0U);
	assert(book.toc[1].chapter == 1U && book.toc[1].level == 1U);
	assert(papermono_epub_read_page(&book, 0U, 0U, output,
		sizeof(output), PAPERMONO_READER_COLUMNS,
		PAPERMONO_READER_LINES, &page) == 0);
	assert(strstr(output, "中文 & English") != NULL);
	assert(strstr(output, "ignore") == NULL);

	assert(papermono_epub_read_page(&book, 1U, 0U, output,
		sizeof(output), PAPERMONO_READER_COLUMNS,
		PAPERMONO_READER_LINES, &page) == 0);
	assert(!page.chapter_eof);
	assert(page.text_end > 0U);
	const uint32_t next = page.text_end;
	assert(papermono_epub_read_page(&book, 1U, next, output,
		sizeof(output), PAPERMONO_READER_COLUMNS,
		PAPERMONO_READER_LINES, &page) == 0);
	assert(page.text_end > next);
}

static void test_nested_ncx_navigation(void)
{
	struct papermono_content_entry source = {
		.kind = PAPERMONO_CONTENT_EPUB,
		.size = 4096U,
	};
	struct papermono_epub_book book;

	active_entries = ncx_entries;
	active_count = sizeof(ncx_entries) / sizeof(ncx_entries[0]);
	assert(papermono_epub_open(&book, &source) == 0);
	assert(book.chapter_count == 2U);
	assert(book.toc_count == 2U);
	assert(strcmp(book.toc[0].title, "第一章") == 0);
	assert(book.toc[0].chapter == 0U && book.toc[0].level == 0U);
	assert(strcmp(book.toc[1].title, "第二章") == 0);
	assert(book.toc[1].chapter == 1U && book.toc[1].level == 1U);
}

/* Forward paging is served from the inflated-chapter cache after the first
 * turn. The cached bytes must reproduce exactly what the zip stream produced,
 * otherwise pages would silently differ depending on read order.
 */
static void test_chapter_cache_matches_stream(void)
{
	struct papermono_content_entry source = {
		.kind = PAPERMONO_CONTENT_EPUB,
		.size = 4096U,
	};
	struct papermono_epub_page page;
	struct papermono_epub_book book;
	char warm[4096];
	char cold[4096];
	uint32_t start;

	/* valid_entries carries the long chapter, so it spans several pages. */
	active_entries = valid_entries;
	active_count = sizeof(valid_entries) / sizeof(valid_entries[0]);
	assert(papermono_epub_open(&book, &source) == 0);
	assert(papermono_epub_read_page(&book, 1U, 0U, warm, sizeof(warm),
		PAPERMONO_READER_COLUMNS, PAPERMONO_READER_LINES, &page) == 0);
	assert(!page.chapter_eof);
	start = page.text_end;

	/* Warm: chapter 1 is already cached from the call above. */
	assert(papermono_epub_read_page(&book, 1U, start, warm, sizeof(warm),
		PAPERMONO_READER_COLUMNS, PAPERMONO_READER_LINES, &page) == 0);

	/* Cold: opening a book drops the cache, so this goes through the zip. */
	struct papermono_epub_book fresh;

	assert(papermono_epub_open(&fresh, &source) == 0);
	assert(papermono_epub_read_page(&fresh, 1U, start, cold, sizeof(cold),
		PAPERMONO_READER_COLUMNS, PAPERMONO_READER_LINES, &page) == 0);
	assert(warm[0] != '\0');
	assert(strcmp(warm, cold) == 0);
}

static void test_deep_ncx_nesting(void)
{
	struct papermono_content_entry source = {
		.kind = PAPERMONO_CONTENT_EPUB,
		.size = 4096U,
	};
	struct papermono_epub_book book;

	active_entries = deep_ncx_entries;
	active_count = sizeof(deep_ncx_entries) / sizeof(deep_ncx_entries[0]);
	assert(papermono_epub_open(&book, &source) == 0);
	assert(book.chapter_count == 4U);
	/* Every navPoint yields exactly one entry, including the trailing sibling
	 * that navigates back to c1.xhtml: it is a real TOC destination even
	 * though chapter 0 keeps the title of its first entry.
	 */
	assert(book.toc_count == 5U);
	assert(book.toc[0].level == 0U);
	assert(book.toc[1].level == 1U);
	assert(book.toc[2].level == 2U);
	assert(book.toc[3].level == 3U);
	assert(book.toc[4].level == 0U);
	assert(book.toc[4].chapter == 0U);
	assert(strcmp(book.toc[4].title, "sibling") == 0);
	assert(strcmp(book.chapters[0].title, "L0") == 0);
}

/* A flat NCX (every navPoint a sibling leaf) is a normal export shape. Each
 * entry must appear exactly once and at level 0; resuming from a matched
 * closing tag used to make every sibling after the first appear twice, once
 * with a spurious nesting level.
 */
static void test_flat_ncx_navigation(void)
{
	struct papermono_content_entry source = {
		.kind = PAPERMONO_CONTENT_EPUB,
		.size = 4096U,
	};
	struct papermono_epub_book book;

	active_entries = flat_ncx_entries;
	active_count = sizeof(flat_ncx_entries) / sizeof(flat_ncx_entries[0]);
	assert(papermono_epub_open(&book, &source) == 0);
	assert(book.chapter_count == 3U);
	assert(book.toc_count == 3U);
	assert(strcmp(book.toc[0].title, "ONE") == 0);
	assert(strcmp(book.toc[1].title, "TWO") == 0);
	assert(strcmp(book.toc[2].title, "THREE") == 0);
	assert(book.toc[0].chapter == 0U && book.toc[0].level == 0U);
	assert(book.toc[1].chapter == 1U && book.toc[1].level == 0U);
	assert(book.toc[2].chapter == 2U && book.toc[2].level == 0U);
}

/* A character reference outside the Unicode scalar range is not an encoding
 * failure. It must render as U+FFFD and leave the rest of the page readable.
 * It used to abort the stream, which made the reader believe the chapter had
 * ended and skip everything after the reference.
 *
 * "PRE <ref> POST" is 12 columns against a 16-column page, so the whole
 * sentence has to survive on one page for the assertion to mean anything.
 */
static void test_out_of_range_character_reference(void)
{
	static const struct {
		const char *reference;
	} cases[] = {
		{ "&#x110000;" },   /* above U+10FFFF */
		{ "&#999999999;" }, /* above U+10FFFF, decimal */
		{ "&#xD800;" },     /* surrogate */
		{ "&#x10FFFF;" },   /* valid upper bound, must stay intact */
	};
	struct papermono_content_entry source = {
		.kind = PAPERMONO_CONTENT_EPUB,
		.size = 4096U,
	};
	char chapter[256];
	struct papermono_epub_book book;
	struct papermono_epub_page page;
	char output[4096];
	/* data is a const pointer, so the live chapter text is injected by
	 * swapping the array element rather than assigning into it.
	 */
	struct fake_entry entries[3];

	memcpy(entries, entity_entries, sizeof(entries));
	entries[2].data = chapter;

	for (size_t index = 0U; index < (sizeof(cases) / sizeof(cases[0])); ++index) {
		(void)snprintf(chapter, sizeof(chapter),
			"<html><body><p>PRE %s POST</p></body></html>",
			cases[index].reference);

		active_entries = entries;
		active_count = sizeof(entries) / sizeof(entries[0]);
		assert(papermono_epub_open(&book, &source) == 0);
		assert(book.chapter_count == 1U);
		assert(papermono_epub_read_page(&book, 0U, 0U, output,
			sizeof(output), PAPERMONO_READER_COLUMNS,
			PAPERMONO_READER_LINES, &page) == 0);
		/* Both the text before the reference and the text after it are
		 * present, so the reference did not end the page.
		 */
		assert(strstr(output, "PRE") != NULL);
		assert(strstr(output, "POST") != NULL);
		assert(page.text_end > 0U);
		if (index + 1U < (sizeof(cases) / sizeof(cases[0]))) {
			/* Everything invalid became U+FFFD, encoded as EF BF BD. */
			assert(strstr(output, "\xEF\xBF\xBD") != NULL);
		}
	}
}

static void test_traversal_rejected(void)
{
	struct papermono_content_entry source = {
		.kind = PAPERMONO_CONTENT_EPUB,
		.size = 1024U,
	};
	struct papermono_epub_book book;

	active_entries = traversal_entries;
	active_count = sizeof(traversal_entries) / sizeof(traversal_entries[0]);
	assert(papermono_epub_open(&book, &source) < 0);
}

int main(void)
{
	prepare_long_chapter();
	test_spine_navigation_and_paging();
	test_nested_ncx_navigation();
	test_chapter_cache_matches_stream();
	test_deep_ncx_nesting();
	test_flat_ncx_navigation();
	test_out_of_range_character_reference();
	test_traversal_rejected();
	puts("EPUB parser tests: PASS");
	return 0;
}
