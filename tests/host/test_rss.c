/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * Host tests for the RSS data layer: XML parsing into the RAM index, the SD
 * cache layout (round-tripped through a temporary directory), body loading and
 * the body-cache limits. The firmware worker and HTTP client are not exercised
 * here; the tests enter through the storage half of the fetch pipeline.
 */

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <papermono/rss.h>
#include <papermono/rss_table.h>

/* Host-only seams of src/papermono_rss.c. */
extern void papermono_rss_test_set_cache_dir(const char *directory);
extern void papermono_rss_test_reset(void);
extern int papermono_rss_test_ingest(const char *xml, size_t length);
extern int papermono_rss_test_load(void);

#define TEST_CACHE_DIR "/tmp/papermono-rss-host"
#define TEST_CACHE_ROOT TEST_CACHE_DIR "/papermono/rss"

static struct papermono_rss_feed current;
static char body_buffer[PAPERMONO_RSS_BODY_MAX + 1U];
static char xml_buffer[384000];

static void begin_test(void)
{
	assert(system("rm -rf " TEST_CACHE_DIR " && mkdir -p " TEST_CACHE_ROOT) ==
	       0);
	papermono_rss_test_set_cache_dir(TEST_CACHE_DIR);
	memset(&current, 0, sizeof(current));
}

static void ingest(const char *xml, size_t length)
{
	assert(papermono_rss_test_ingest(xml, length) == 0);
	papermono_rss_get(&current);
}

static void ingest_text(const char *xml)
{
	ingest(xml, strlen(xml));
}

static size_t append_text(size_t offset, const char *text)
{
	const size_t length = strlen(text);

	assert(offset + length < sizeof(xml_buffer));
	memcpy(xml_buffer + offset, text, length);
	return offset + length;
}

static size_t append_repeat(size_t offset, char value, size_t count)
{
	assert(offset + count < sizeof(xml_buffer));
	memset(xml_buffer + offset, value, count);
	return offset + count;
}

static size_t append_feed_open(size_t offset, const char *title)
{
	offset = append_text(offset, "<rss version=\"2.0\"><channel><title>");
	offset = append_text(offset, title);
	return append_text(offset, "</title>");
}

static size_t append_feed_close(size_t offset)
{
	return append_text(offset, "</channel></rss>");
}

static size_t append_item(size_t offset, const char *title, const char *body)
{
	offset = append_text(offset, "<item><title>");
	offset = append_text(offset, title);
	offset = append_text(offset, "</title><pubDate>Tue, 27 Oct 2026 "
				    "01:00:00 GMT</pubDate><description>");
	offset = append_text(offset, body);
	return append_text(offset, "</description></item>");
}

static void corpus_reset(void)
{
	size_t offset = 0U;

	memset(xml_buffer, 0, sizeof(xml_buffer));
	offset = append_feed_open(offset, "AI 新闻 &amp; More");
	offset = append_item(offset, "First &lt;post&gt;",
			     "&lt;p&gt;Hello &lt;b&gt;world&lt;/b&gt; "
			     "&amp;amp; friends&lt;/p&gt;");
	offset = append_text(offset, "<item>");
	offset = append_text(offset,
			     "<title><![CDATA[Second <raw> title]]></title>");
	offset = append_text(offset,
			     "<description><![CDATA[<p>CDATA paragraph</p>]]>");
	offset = append_text(offset, "</description></item>");
	offset = append_text(offset, "<item><title>Third</title>");
	offset = append_text(offset,
			     "<content:encoded><![CDATA[<p>Encoded fallback</p>]]>");
	offset = append_text(offset, "</content:encoded></item>");
	(void)append_feed_close(offset);
}

static int load_body(size_t index, char *buffer, size_t capacity)
{
	memset(buffer, 0x7F, capacity);
	return papermono_rss_body_load(index, buffer, capacity);
}

static void test_corpus_index(void)
{
	begin_test();
	corpus_reset();
	ingest_text(xml_buffer);

	assert(current.valid);
	assert(current.count == 3U);
	assert(strcmp(current.title, "AI 新闻 & More") == 0);
	assert(strcmp(current.items[0].title, "First <post>") == 0);
	assert(strcmp(current.items[0].published,
		      "Tue, 27 Oct 2026 01:00:00 GMT") == 0);
	assert(strcmp(current.items[1].title, "Second <raw> title") == 0);
	assert(strcmp(current.items[2].title, "Third") == 0);
	assert(current.items[0].body_length > 0U);
	assert(current.items[1].body_length > 0U);
	assert(current.items[2].body_length > 0U);

	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "Hello world & friends") == 0);
	assert(load_body(1U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[1].body_length);
	assert(strcmp(body_buffer, "CDATA paragraph") == 0);
	assert(load_body(2U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[2].body_length);
	assert(strcmp(body_buffer, "Encoded fallback") == 0);
}

static void test_entities_and_whitespace(void)
{
	static const char feed_xml[] =
		"<rss><channel><title>x</title><item><title>"
		"A&amp;B &lt;tag&gt; &quot;q&quot; &#39; &#20013; &#x4E2D; "
		"&bogus; &amp;amp;</title>"
		"<description>&lt;p&gt;  alpha \n\t beta   gamma &lt;/p&gt;"
		"</description></item></channel></rss>";

	begin_test();
	ingest_text(feed_xml);
	assert(current.count == 1U);
	assert(strcmp(current.items[0].title,
		      "A&B <tag> \"q\" ' 中 中 &bogus; &amp;") == 0);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "alpha beta gamma") == 0);
}

static void test_cdata_literal(void)
{
	static const char feed_xml[] =
		"<rss><channel><item>"
		"<title><![CDATA[A &amp; B <b>raw</b>]]></title>"
		"<description>  <![CDATA[x &lt; y]]>  </description>"
		"</item></channel></rss>";

	begin_test();
	ingest_text(feed_xml);
	assert(current.count == 1U);
	assert(strcmp(current.items[0].title, "A &amp; B <b>raw</b>") == 0);
	/* A CDATA body is HTML: its tags are stripped and its entities decoded. */
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "x < y") == 0);
}

static void test_raw_html_stripping(void)
{
	static const char feed_xml[] =
		"<rss><channel><item><title>Raw HTML</title>"
		"<description><p>Raw <b>bold</b> text</p>"
		"<a href=\"http://example.com/?a=1&amp;b=2\">link</a>"
		"<script>ignored()</script></description></item>"
		"</channel></rss>";

	begin_test();
	ingest_text(feed_xml);
	assert(current.count == 1U);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	/* </p> ends the paragraph with an empty line; script content is
	 * dropped; inline tags add no spaces.
	 */
	assert(strcmp(body_buffer, "Raw bold text\n\nlink") == 0);
}

/* HTML block structure: blank lines between paragraphs, headings on their own
 * line, blockquote indentation and list bullets.
 */
static void test_html_paragraphs(void)
{
	static const char feed_xml[] =
		"<rss><channel><item><title>P</title><description>"
		"&lt;p&gt;First paragraph.&lt;/p&gt;"
		"&lt;p&gt;Second paragraph.&lt;/p&gt;"
		"</description></item></channel></rss>";

	begin_test();
	ingest_text(feed_xml);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer,
		      "First paragraph.\n\nSecond paragraph.") == 0);
}

static void test_html_headings(void)
{
	static const char feed_xml[] =
		"<rss><channel><item><title>T</title><description>"
		"&lt;p&gt;Intro&lt;/p&gt;&lt;h2&gt;Section&lt;/h2&gt;"
		"&lt;p&gt;Body&lt;/p&gt;"
		"</description></item></channel></rss>";

	begin_test();
	ingest_text(feed_xml);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "Intro\n\nSection\n\nBody") == 0);
}

static void test_html_blockquote(void)
{
	static const char feed_xml[] =
		"<rss><channel><item><title>T</title><description>"
		"&lt;p&gt;Before&lt;/p&gt;&lt;blockquote&gt;&lt;p&gt;Quoted "
		"line&lt;/p&gt;&lt;/blockquote&gt;&lt;p&gt;After&lt;/p&gt;"
		"</description></item></channel></rss>";

	begin_test();
	ingest_text(feed_xml);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "Before\n\n  Quoted line\n\nAfter") == 0);
}

static void test_html_lists(void)
{
	static const char feed_xml[] =
		"<rss><channel><item><title>T</title><description>"
		"&lt;p&gt;Items&lt;/p&gt;&lt;ul&gt;&lt;li&gt;one&lt;/li&gt;"
		"&lt;li&gt;two&lt;/li&gt;&lt;/ul&gt;&lt;p&gt;After&lt;/p&gt;"
		"</description></item></channel></rss>";

	begin_test();
	ingest_text(feed_xml);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "Items\n\n\xC2\xB7 one\n\xC2\xB7 two\n\nAfter") ==
	       0);
}

static void test_html_blank_line_compression(void)
{
	static const char feed_xml[] =
		"<rss><channel><item><title>T</title><description>"
		"&lt;p&gt;a&lt;/p&gt;&lt;p&gt;&lt;/p&gt;&lt;p&gt;&lt;/p&gt;"
		"&lt;p&gt;b&lt;/p&gt;"
		"</description></item></channel></rss>";

	begin_test();
	ingest_text(feed_xml);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "a\n\nb") == 0);
}

static void test_html_inline_spacing(void)
{
	static const char cjk_no_space[] =
		"<rss><channel><item><title>T</title><description>"
		"&lt;p&gt;中&lt;b&gt;文&lt;/b&gt;测试&lt;/p&gt;"
		"</description></item></channel></rss>";
	static const char cjk_source_space[] =
		"<rss><channel><item><title>T</title><description>"
		"&lt;p&gt;中 &lt;b&gt;文&lt;/b&gt;&lt;/p&gt;"
		"</description></item></channel></rss>";
	static const char latin[] =
		"<rss><channel><item><title>T</title><description>"
		"&lt;p&gt;Hello &lt;b&gt;world&lt;/b&gt; &lt;em&gt;again&lt;/em&gt;"
		"&lt;/p&gt;</description></item></channel></rss>";

	begin_test();
	ingest_text(cjk_no_space);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "中文测试") == 0);

	begin_test();
	ingest_text(cjk_source_space);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "中文") == 0);

	begin_test();
	ingest_text(latin);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "Hello world again") == 0);
}

static void test_html_url_shortening(void)
{
	static const char plain[] =
		"<rss><channel><item><title>T</title><description>"
		"&lt;p&gt;See https://example.com/some/long/path?q=1 now"
		"&lt;/p&gt;</description></item></channel></rss>";
	static const char trailing_punct[] =
		"<rss><channel><item><title>T</title><description>"
		"&lt;p&gt;见 https://example.com/a。&lt;/p&gt;"
		"</description></item></channel></rss>";
	static const char anchor[] =
		"<rss><channel><item><title>T</title><description>"
		"&lt;p&gt;&lt;a href=\"https://x.example/ignored\"&gt;"
		"https://example.com/path/rest&lt;/a&gt;&lt;/p&gt;"
		"</description></item></channel></rss>";
	static const char host_only[] =
		"<rss><channel><item><title>T</title><description>"
		"&lt;p&gt;go https://example.com now&lt;/p&gt;"
		"</description></item></channel></rss>";

	begin_test();
	ingest_text(plain);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "See example.com/some now") == 0);

	begin_test();
	ingest_text(trailing_punct);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "见 example.com/a。") == 0);

	begin_test();
	ingest_text(anchor);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "example.com/path") == 0);

	begin_test();
	ingest_text(host_only);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "go example.com now") == 0);
}

static void test_html_url_truncation(void)
{
	size_t offset = 0U;
	const char suffix[] = "…";
	int loaded;

	begin_test();
	offset = append_feed_open(offset, "T");
	offset = append_text(offset, "<item><title>T</title><description>");
	offset = append_text(offset, "&lt;p&gt;https://example.com/");
	offset = append_repeat(offset, 'a', 80U);
	offset = append_text(offset, "&lt;/p&gt;</description></item>");
	offset = append_feed_close(offset);
	ingest(xml_buffer, offset);
	loaded = load_body(0U, body_buffer, sizeof(body_buffer));
	assert(loaded == (int)strlen(body_buffer));
	assert(loaded == 48);
	assert(memcmp(body_buffer + loaded - 3, suffix, 3U) == 0);
}

static void test_html_heading_dedup(void)
{
	static const char matching[] =
		"<rss><channel><item><title>AI 新闻早报</title><description>"
		"&lt;h1&gt;AI 新闻早报&lt;/h1&gt;&lt;p&gt;body&lt;/p&gt;"
		"</description></item></channel></rss>";
	static const char different[] =
		"<rss><channel><item><title>Short title</title><description>"
		"&lt;h1&gt;Other heading&lt;/h1&gt;&lt;p&gt;body&lt;/p&gt;"
		"</description></item></channel></rss>";
	static const char after_preamble[] =
		"<rss><channel><item><title>AI 新闻早报</title><description>"
		"&lt;p&gt;来自 Agent：AMOO&lt;/p&gt;"
		"&lt;h1&gt;AI 新闻早报&lt;/h1&gt;&lt;p&gt;body&lt;/p&gt;"
		"</description></item></channel></rss>";

	begin_test();
	ingest_text(matching);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "body") == 0);

	begin_test();
	ingest_text(different);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "Other heading\n\nbody") == 0);

	/* A short preamble before the repeated heading does not defeat the
	 * deduplication: the feed puts "来自 Agent" before its <h1>.
	 */
	begin_test();
	ingest_text(after_preamble);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "来自 Agent：AMOO\n\nbody") == 0);
}

/* The index version changed with the structured body format; old caches must
 * be rejected so the firmware fetches again instead of showing flattened text.
 */
static void test_cache_version_invalid(void)
{
	FILE *file;
	char path[] = TEST_CACHE_ROOT "/index.bin";
	unsigned char header[8];

	begin_test();
	corpus_reset();
	ingest_text(xml_buffer);
	assert(papermono_rss_test_load() == 1);
	file = fopen(path, "r+b");
	assert(file != NULL);
	assert(fread(header, 1U, sizeof(header), file) == sizeof(header));
	/* Little-endian uint16 at offset 4 is the on-disk version field. */
	header[4] = 1U;
	header[5] = 0U;
	assert(fseek(file, 0L, SEEK_SET) == 0);
	assert(fwrite(header, 1U, sizeof(header), file) == sizeof(header));
	assert(fclose(file) == 0);
	assert(papermono_rss_test_load() < 0);
	papermono_rss_get(&current);
	assert(!current.valid);
}

static void test_content_encoded_fallback(void)
{
	static const char empty_description[] =
		"<rss><channel><item><title>Fallback</title>"
		"<description></description>"
		"<content:encoded><![CDATA[<p>From content</p>]]>"
		"</content:encoded></item></channel></rss>";
	static const char both_present[] =
		"<rss><channel><item><title>Prefer description</title>"
		"<description>From description</description>"
		"<content:encoded>From content</content:encoded>"
		"</item></channel></rss>";

	begin_test();
	ingest_text(empty_description);
	assert(current.count == 1U);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "From content") == 0);

	begin_test();
	ingest_text(both_present);
	assert(current.count == 1U);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "From description") == 0);
}

static void test_missing_channel_title(void)
{
	static const char feed_xml[] =
		"<rss><channel><item><title>Item only</title></item>"
		"</channel></rss>";

	begin_test();
	ingest_text(feed_xml);
	assert(current.title[0] == '\0');
	assert(current.count == 1U);
	assert(strcmp(current.items[0].title, "Item only") == 0);
	assert(current.items[0].body_length == 0U);
	/* A title-only item still reads back as an empty body. */
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) == 0);
	assert(body_buffer[0] == '\0');
}

static void test_empty_feed(void)
{
	static const char feed_xml[] = "<rss><channel></channel></rss>";

	begin_test();
	ingest_text(feed_xml);
	assert(current.valid);
	assert(current.count == 0U);
	assert(strcmp(current.title, "") == 0);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) == -ENOENT);

	/* The empty index survives a reboot too. */
	assert(papermono_rss_test_load() == 1);
	papermono_rss_get(&current);
	assert(current.valid);
	assert(current.count == 0U);
}

static void test_item_limit(void)
{
	size_t offset = 0U;

	begin_test();
	offset = append_feed_open(offset, "Limit");
	for (unsigned int i = 0U; i < 15U; ++i) {
		char title[32];

		(void)snprintf(title, sizeof(title), "Item %u", i);
		offset = append_item(offset, title, "body");
	}
	offset = append_feed_close(offset);
	ingest(xml_buffer, offset);
	assert(current.count == PAPERMONO_RSS_ITEM_MAX);
	assert(strcmp(current.items[0].title, "Item 0") == 0);
	assert(strcmp(current.items[PAPERMONO_RSS_ITEM_MAX - 1U].title,
		      "Item 11") == 0);
}

static void test_title_truncation(void)
{
	size_t offset = 0U;

	begin_test();
	offset = append_feed_open(offset, "T");
	offset = append_text(offset, "<item><title>");
	offset = append_repeat(offset, 'A', 300U);
	offset = append_text(offset, "</title><description>b</description>"
				    "</item>");
	offset = append_feed_close(offset);
	ingest(xml_buffer, offset);
	assert(current.count == 1U);
	assert(strlen(current.items[0].title) == PAPERMONO_RSS_TITLE_MAX);
}

static void test_body_truncation_ascii(void)
{
	size_t offset = 0U;

	begin_test();
	offset = append_feed_open(offset, "T");
	offset = append_text(offset, "<item><title>Long</title><description>");
	offset = append_repeat(offset, 'A', 70000U);
	offset = append_text(offset, "</description></item>");
	offset = append_feed_close(offset);
	ingest(xml_buffer, offset);
	assert(current.count == 1U);
	assert(current.items[0].body_length == PAPERMONO_RSS_BODY_MAX);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)PAPERMONO_RSS_BODY_MAX);
	assert(strlen(body_buffer) == PAPERMONO_RSS_BODY_MAX);
	assert(body_buffer[PAPERMONO_RSS_BODY_MAX] == '\0');
	/* The whole cache still round-trips through the files. */
	assert(papermono_rss_test_load() == 1);
	papermono_rss_get(&current);
	assert(current.items[0].body_length == PAPERMONO_RSS_BODY_MAX);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)PAPERMONO_RSS_BODY_MAX);
}

static void test_body_truncation_utf8(void)
{
	const size_t ascii = 64001U;
	const size_t cjk_count = 1000U;
	size_t offset = 0U;
	int loaded;
	char last_char[4] = { 0 };

	begin_test();
	offset = append_feed_open(offset, "T");
	offset = append_text(offset, "<item><title>UTF-8</title><description>");
	offset = append_repeat(offset, 'A', ascii);
	assert(offset + cjk_count * 3U < sizeof(xml_buffer));
	for (size_t i = 0U; i < cjk_count; ++i) {
		memcpy(xml_buffer + offset, "中", 3U);
		offset += 3U;
	}
	offset = append_text(offset, "</description></item>");
	offset = append_feed_close(offset);
	ingest(xml_buffer, offset);
	assert(current.count == 1U);
	/* 64001 ASCII bytes leave 1535 of the 65536 byte budget: 511 whole
	 * three-byte characters fit, the next one would be split and is dropped.
	 */
	assert(current.items[0].body_length == 65534U);
	loaded = load_body(0U, body_buffer, sizeof(body_buffer));
	assert(loaded == 65534);
	memcpy(last_char, body_buffer + loaded - 3, 3U);
	assert(memcmp(last_char, "中", 3U) == 0);
}

static void test_body_load_capacity(void)
{
	char small[10];

	begin_test();
	corpus_reset();
	ingest_text(xml_buffer);
	assert(current.count == 3U);
	/* A short buffer gets capacity-1 bytes, NUL terminated. */
	assert(load_body(0U, small, sizeof(small)) == (int)sizeof(small) - 1);
	assert(small[sizeof(small) - 1] == '\0');
	assert(strncmp(small, "Hello wor", sizeof(small) - 1U) == 0);
	assert(papermono_rss_body_load(0U, small, 0U) == -EINVAL);
	assert(papermono_rss_body_load(0U, NULL, sizeof(small)) == -EINVAL);
}

static void test_body_load_missing(void)
{
	begin_test();
	corpus_reset();
	ingest_text(xml_buffer);
	assert(load_body(9U, body_buffer, sizeof(body_buffer)) == -ENOENT);
}

static void test_no_cache(void)
{
	begin_test();
	assert(papermono_rss_test_load() < 0);
	papermono_rss_get(&current);
	assert(!current.valid);
	assert(current.count == 0U);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) == -ENOENT);
}

static void test_cache_round_trip(void)
{
	begin_test();
	corpus_reset();
	ingest_text(xml_buffer);
	assert(papermono_rss_test_load() == 1);
	papermono_rss_get(&current);
	assert(current.valid);
	assert(current.count == 3U);
	assert(strcmp(current.title, "AI 新闻 & More") == 0);
	assert(strcmp(current.items[0].title, "First <post>") == 0);
	assert(strcmp(current.items[0].published,
		      "Tue, 27 Oct 2026 01:00:00 GMT") == 0);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "Hello world & friends") == 0);
	assert(load_body(2U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[2].body_length);
	assert(strcmp(body_buffer, "Encoded fallback") == 0);
	assert(current.updated_at == 1700000000);
}

static void test_corrupt_index(void)
{
	FILE *file;
	char path[] = TEST_CACHE_ROOT "/index.bin";

	begin_test();
	corpus_reset();
	ingest_text(xml_buffer);
	file = fopen(path, "wb");
	assert(file != NULL);
	assert(fwrite("garbage", 1U, 7U, file) == 7U);
	assert(fclose(file) == 0);
	assert(papermono_rss_test_load() < 0);
	papermono_rss_get(&current);
	assert(!current.valid);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) == -ENOENT);
}

static void test_missing_bodies(void)
{
	begin_test();
	corpus_reset();
	ingest_text(xml_buffer);
	assert(remove(TEST_CACHE_ROOT "/bodies.bin") == 0);
	assert(papermono_rss_test_load() < 0);
	papermono_rss_get(&current);
	assert(!current.valid);
}

static void test_parse_failure_keeps_cache(void)
{
	static const char broken[] = "<html><body>not a feed</body></html>";

	begin_test();
	corpus_reset();
	ingest_text(xml_buffer);
	assert(papermono_rss_test_ingest(broken, sizeof(broken) - 1U) < 0);
	/* The rejected fetch must not have touched the previous cache. */
	assert(papermono_rss_test_load() == 1);
	papermono_rss_get(&current);
	assert(current.valid);
	assert(current.count == 3U);
	assert(strcmp(current.title, "AI 新闻 & More") == 0);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "Hello world & friends") == 0);
}

static void test_body_cache_total_limit(void)
{
	const size_t item_body = 60000U;
	size_t offset = 0U;

	begin_test();
	offset = append_feed_open(offset, "Big");
	for (unsigned int i = 0U; i < 5U; ++i) {
		char title[32];

		(void)snprintf(title, sizeof(title), "Big %u", i);
		offset = append_text(offset, "<item><title>");
		offset = append_text(offset, title);
		offset = append_text(offset, "</title><description>");
		offset = append_repeat(offset, 'B', item_body);
		offset = append_text(offset, "</description></item>");
	}
	offset = append_feed_close(offset);
	ingest(xml_buffer, offset);
	/* 4 x 60000 bytes fit under the 256 KB cache limit; the fifth item is
	 * skipped instead of being silently truncated.
	 */
	assert(current.count == 4U);
	for (size_t i = 0U; i < current.count; ++i) {
		assert(current.items[i].body_length == item_body);
	}
}

static int copy_file(const char *from, const char *to)
{
	FILE *source = fopen(from, "rb");
	FILE *target;
	char buffer[512];
	size_t got;

	if (source == NULL) {
		return -1;
	}
	target = fopen(to, "wb");
	if (target == NULL) {
		(void)fclose(source);
		return -1;
	}
	while ((got = fread(buffer, 1U, sizeof(buffer), source)) > 0U) {
		if (fwrite(buffer, 1U, got, target) != got) {
			(void)fclose(source);
			(void)fclose(target);
			return -1;
		}
	}
	(void)fclose(source);
	return fclose(target);
}

static void test_commit_recovery(void)
{
	FILE *file;
	char path[] = TEST_CACHE_ROOT "/bodies.bin";
	char backup[] = TEST_CACHE_ROOT "/bodies.bak";
	char index_backup[] = TEST_CACHE_ROOT "/index.bak";

	/* Bodies were replaced before the index was: the boot-time recovery must
	 * roll the bodies back so the old index still matches.
	 */
	begin_test();
	corpus_reset();
	ingest_text(xml_buffer);
	assert(copy_file(path, backup) == 0);
	file = fopen(path, "wb");
	assert(file != NULL);
	assert(fwrite("torn", 1U, 4U, file) == 4U);
	assert(fclose(file) == 0);
	assert(papermono_rss_test_load() == 1);
	papermono_rss_get(&current);
	assert(current.valid);
	assert(current.count == 3U);
	assert(load_body(0U, body_buffer, sizeof(body_buffer)) ==
	       (int)current.items[0].body_length);
	assert(strcmp(body_buffer, "Hello world & friends") == 0);
	assert(remove(backup) != 0); /* recovery consumed the backup */

	/* A reset after both files were replaced leaves two stale backups. */
	begin_test();
	corpus_reset();
	ingest_text(xml_buffer);
	assert(copy_file(TEST_CACHE_ROOT "/bodies.bin", backup) == 0);
	assert(copy_file(TEST_CACHE_ROOT "/index.bin", index_backup) == 0);
	assert(papermono_rss_test_load() == 1);
	papermono_rss_get(&current);
	assert(current.valid);
	assert(current.count == 3U);
	assert(remove(backup) != 0);
	assert(remove(index_backup) != 0);
}

static void test_malformed_input(void)
{
	static const char unclosed[] =
		"<rss><channel><title>Partial<title><item><title>One</title>"
		"<description>broken";
	static const char commented[] =
		"<rss><channel><title>T</title><item><title>visible</title>"
		"</item><!-- <item><title>hidden</title></item> -->"
		"</channel></rss>";
	static const char bad_bytes[] =
		"<rss><channel><title>\xC3\x28 \xFF</title><item><title>"
		"\xE4\xB8</title><description>\x80\x81</description></item>"
		"</channel></rss>";
	static const char self_closed[] =
		"<rss><channel><item><title/></item>"
		"<item><title>kept</title></item></channel></rss>";

	begin_test();
	assert(papermono_rss_test_ingest(unclosed, sizeof(unclosed) - 1U) ==
	       0);
	papermono_rss_get(&current);
	assert(current.count <= 1U);

	begin_test();
	assert(papermono_rss_test_ingest(commented, sizeof(commented) - 1U) ==
	       0);
	papermono_rss_get(&current);
	assert(current.count == 1U);
	assert(strcmp(current.items[0].title, "visible") == 0);

	begin_test();
	assert(papermono_rss_test_ingest(bad_bytes, sizeof(bad_bytes) - 1U) ==
	       0);
	papermono_rss_get(&current);
	assert(current.count <= 1U);

	begin_test();
	assert(papermono_rss_test_ingest(self_closed,
					 sizeof(self_closed) - 1U) == 0);
	papermono_rss_get(&current);
	assert(current.count == 1U);
	assert(strcmp(current.items[0].title, "kept") == 0);

	/* Empty input is refused, and no cache is created from it. */
	begin_test();
	assert(papermono_rss_test_ingest(NULL, 0U) == -EINVAL);
	assert(papermono_rss_test_ingest("", 0U) == -EINVAL);
	assert(papermono_rss_test_load() < 0);
}

static void table_html(const char *html)
{
	size_t offset = append_feed_open(0U, "Tables");

	offset = append_text(offset, "<item><title>Table article</title><description><![CDATA[");
	offset = append_text(offset, html);
	offset = append_text(offset, "]]></description></item>");
	offset = append_feed_close(offset);
	begin_test();
	ingest(xml_buffer, offset);
	assert(papermono_rss_body_load(0U, body_buffer, sizeof(body_buffer)) >= 0);
}

/* Decode independently of the renderer to check the whole cached result,
 * including characters adjoining a skipped malformed source byte.
 */
static void assert_valid_utf8(const char *text, size_t length)
{
	for (size_t i = 0U; i < length;) {
		const unsigned char lead = (unsigned char)text[i++];
		unsigned int code_point;
		unsigned int minimum;
		size_t remaining;

		if (lead < 0x80U) continue;
		if (lead >= 0xC2U && lead <= 0xDFU) {
			code_point = lead & 0x1FU;
			minimum = 0x80U;
			remaining = 1U;
		} else if (lead >= 0xE0U && lead <= 0xEFU) {
			code_point = lead & 0x0FU;
			minimum = 0x800U;
			remaining = 2U;
		} else {
			assert(lead >= 0xF0U && lead <= 0xF4U);
			code_point = lead & 0x07U;
			minimum = 0x10000U;
			remaining = 3U;
		}
		assert(remaining <= length - i);
		while (remaining-- > 0U) {
			const unsigned char continuation = (unsigned char)text[i++];
			assert((continuation & 0xC0U) == 0x80U);
			code_point = (code_point << 6U) | (continuation & 0x3FU);
		}
		assert(code_point >= minimum && code_point <= 0x10FFFFU);
		assert(code_point < 0xD800U || code_point > 0xDFFFU);
	}
}

static void test_utf8_scalars(void)
{
	static const char *const invalid[] = {
		"\x80", "\xBF", "\xC0\x80", "\xC1\xBF",
		"\xC2", "\xDF", "\xE0", "\xE0\xA0", "\xE1\x80",
		"\xED\x9F", "\xEF\xBF", "\xF0", "\xF0\x90",
		"\xF0\x90\x80", "\xF1\x80\x80", "\xF4\x8F\xBF",
		"\xE0\x80\x80", "\xE0\x9F\xBF", "\xED\xA0\x80",
		"\xED\xBF\xBF", "\xF0\x80\x80\x80", "\xF0\x8F\xBF\xBF",
		"\xF4\x90\x80\x80", "\xF4\xBF\xBF\xBF",
		"\xF5\x80\x80\x80", "\xF8\x80\x80\x80\x80", "\xFF",
	};
	static const char boundaries[] =
		"\xC2\x80\xDF\xBF\xE0\xA0\x80\xED\x9F\xBF"
		"\xEE\x80\x80\xEF\xBF\xBF\xF0\x90\x80\x80"
		"\xF3\xBF\xBF\xBF\xF4\x8F\xBF\xBF";
	char html[256];

	for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
		(void)snprintf(html, sizeof(html),
			"<p>A%s中<b>B</b></p><table><tr><td>C%s</td>"
			"<td>右</td></tr></table>", invalid[i], invalid[i]);
		table_html(html);
		assert(strcmp(body_buffer, "A中B\n\n" PAPERMONO_RSS_TABLE_BEGIN
			PAPERMONO_RSS_TABLE_ROW "C\t右\n" PAPERMONO_RSS_TABLE_END) == 0);
		assert_valid_utf8(body_buffer, strlen(body_buffer));

		/* An incomplete source unit at the field boundary is not cached. */
		(void)snprintf(html, sizeof(html), "A%s", invalid[i]);
		table_html(html);
		assert(strcmp(body_buffer, "A") == 0);
	}
	(void)snprintf(html, sizeof(html), "A%s中", boundaries);
	table_html(html);
	assert(strcmp(body_buffer, html) == 0);
	assert_valid_utf8(body_buffer, strlen(body_buffer));

	/* A bad lead cannot swallow a tag boundary, a quote, or the first byte
	 * of a following valid scalar; markup stays hidden and cells stay aligned. */
	table_html("<table><tr><td title='\xC2'><!--\xE1--><script>\xF1</script>"
		"A\xC2</td><td>中</td></tr></table>");
	assert(strcmp(body_buffer, PAPERMONO_RSS_TABLE_BEGIN
		PAPERMONO_RSS_TABLE_ROW "A\t中\n" PAPERMONO_RSS_TABLE_END) == 0);
	table_html("<p>h中 https://example.com/中文。</p>");
	assert(strcmp(body_buffer, "h中 example.com/中文。") == 0);
	assert_valid_utf8(body_buffer, strlen(body_buffer));

	/* URL collection reaching its fixed byte limit also keeps a scalar whole. */
	size_t length = (size_t)snprintf(xml_buffer, sizeof(xml_buffer),
		"<p>https://example.com/");
	for (size_t i = 0U; i < 300U; ++i) {
		memcpy(xml_buffer + length, "中", 3U);
		length += 3U;
	}
	(void)snprintf(xml_buffer + length, sizeof(xml_buffer) - length, "</p>");
	/* table_html writes xml_buffer itself, so keep the source separate. */
	char url_html[1024];
	memcpy(url_html, xml_buffer, length + sizeof("</p>"));
	table_html(url_html);
	assert_valid_utf8(body_buffer, strlen(body_buffer));
}

static void test_utf8_control_boundaries(void)
{
	static const struct {
		unsigned char lead;
		size_t length;
	} leads[] = {
		{ 0xC2U, 2U }, { 0xDFU, 2U }, { 0xE0U, 3U },
		{ 0xE1U, 3U }, { 0xEDU, 3U }, { 0xEFU, 3U },
		{ 0xF0U, 4U }, { 0xF1U, 4U }, { 0xF4U, 4U },
		{ 0xF5U, 4U }, { 0xFFU, 4U },
	};
	size_t offset = append_feed_open(0U, "Controls");
	size_t expected_rows = 0U;

	offset = append_text(offset,
		"<item><title>T</title><description><![CDATA[<table>");
	/* All C0 values, at every continuation position of each lead class.
	 * Explicit lengths retain embedded NUL bytes in the ingest input. */
	for (size_t l = 0U; l < sizeof(leads) / sizeof(leads[0]); ++l) {
		for (size_t position = 1U; position < leads[l].length; ++position) {
			for (unsigned int control = 0U; control < 0x20U; ++control) {
				offset = append_text(offset, "<tr><td>A");
				xml_buffer[offset++] = (char)leads[l].lead;
				for (size_t p = 1U; p < leads[l].length; ++p) {
					xml_buffer[offset++] = p == position ?
						(char)control : (char)0x80U;
				}
				offset = append_text(offset, "B</td><td>中</td></tr>");
				++expected_rows;
			}
		}
	}
	offset = append_text(offset, "</table>]]></description></item>");
	offset = append_feed_close(offset);
	begin_test();
	ingest(xml_buffer, offset);
	assert(papermono_rss_test_load() == 1);
	papermono_rss_get(&current);
	assert(papermono_rss_body_load(0U, body_buffer, sizeof(body_buffer)) > 0);
	assert_valid_utf8(body_buffer, strlen(body_buffer));
	struct papermono_rss_block block;
	size_t cursor = 0U;
	assert(papermono_rss_block_next(body_buffer, strlen(body_buffer), &cursor, &block));
	assert(block.kind == PAPERMONO_RSS_BLOCK_TABLE && block.table.complete);
	assert(block.table.row_count == expected_rows && block.table.column_count == 2U);
	size_t row_cursor = 0U;
	for (size_t l = 0U; l < sizeof(leads) / sizeof(leads[0]); ++l) {
		for (size_t position = 1U; position < leads[l].length; ++position) {
			for (unsigned int control = 0U; control < 0x20U; ++control) {
				struct papermono_rss_table_row row;
				struct papermono_rss_slice cell;
				size_t cell_cursor = 0U;
				const char *expected = control >= 9U && control <= 13U ?
					"A B" : "AB";

				assert(papermono_rss_table_row_next(&block.table, &row_cursor, &row));
				assert(row.column_count == 2U && !row.header);
				assert(papermono_rss_table_cell_next(&row, &cell_cursor, &cell));
				assert(cell.length == strlen(expected));
				assert(memcmp(cell.text, expected, cell.length) == 0);
				assert(papermono_rss_table_cell_next(&row, &cell_cursor, &cell));
				assert(cell.length == strlen("中") && memcmp(cell.text, "中", cell.length) == 0);
				assert(!papermono_rss_table_cell_next(&row, &cell_cursor, &cell));
			}
		}
	}
	assert(!papermono_rss_block_next(body_buffer, strlen(body_buffer), &cursor, &block));

	/* A malformed prefix cannot smuggle any complete codec record into
	 * ordinary text, even when followed by tabs/newlines and valid CJK. */
	table_html("\xC2\x1eT1\n\xE1\x80\x1eH\tInjected\n"
		"\xF1\x80\x80\x1eR\tFake\n\xFF\x1e" "E\n<p>中</p>");
	assert(strchr(body_buffer, '\x1e') == NULL);
	assert(strstr(body_buffer, "中") != NULL);
	assert_valid_utf8(body_buffer, strlen(body_buffer));
	cursor = 0U;
	while (papermono_rss_block_next(body_buffer, strlen(body_buffer), &cursor, &block)) {
		assert(block.kind == PAPERMONO_RSS_BLOCK_TEXT);
	}
}

static void test_utf8_titles(void)
{
	size_t offset = append_feed_open(0U, "A\xC2\t中\xED\xA0\x80Z");

	offset = append_text(offset, "<item><title>A\xF1\x80\x1e中&#30;Z</title>"
		"<description>body</description></item>");
	offset = append_feed_close(offset);
	begin_test();
	ingest(xml_buffer, offset);
	assert(strcmp(current.title, "A 中Z") == 0);
	assert(strcmp(current.items[0].title, "A中Z") == 0);
	assert_valid_utf8(current.title, strlen(current.title));
	assert_valid_utf8(current.items[0].title, strlen(current.items[0].title));

	/* Entity output must be atomic too: a title boundary never stores only
	 * the first one or two bytes of a decoded multi-byte character. */
	offset = append_feed_open(0U, "T");
	offset = append_text(offset, "<item><title>");
	offset = append_repeat(offset, 'A', PAPERMONO_RSS_TITLE_MAX - 1U);
	offset = append_text(offset, "&#x4E2D;Z</title><description>body</description></item>");
	offset = append_feed_close(offset);
	begin_test();
	ingest(xml_buffer, offset);
	assert(strlen(current.items[0].title) == PAPERMONO_RSS_TITLE_MAX - 1U);
	assert_valid_utf8(current.items[0].title, strlen(current.items[0].title));
}

static void test_html_tables(void)
{
	table_html("<p>前文</p><table><caption>行情摘要</caption><thead><tr>"
		"<th>品种</th><th>价&nbsp;格</th><th></th></tr></thead><tbody>"
		"<tr><td><b>AU</b>USD</td><td>1&amp;2<br>3</td><td></td></tr>"
		"</tbody></table><p>后文</p>");
	assert(strcmp(body_buffer, "前文\n\n行情摘要\n\n"
		PAPERMONO_RSS_TABLE_BEGIN PAPERMONO_RSS_TABLE_HEADER "品种\t价格\t\n"
		PAPERMONO_RSS_TABLE_ROW "AUUSD\t1&2 3\t\n"
		PAPERMONO_RSS_TABLE_END "\n\n后文") == 0);

	table_html("before<table><tr><td>one</td></tr></table>between"
		"<table><tr><th>two</th></tr></table>after");
	const char *first = strstr(body_buffer, PAPERMONO_RSS_TABLE_BEGIN);
	assert(first != NULL);
	const char *middle = strstr(first, "between");
	const char *second = strstr(first + 4U, PAPERMONO_RSS_TABLE_BEGIN);
	assert(middle != NULL && second != NULL && first < middle && middle < second);
	assert(strstr(second, "after") != NULL);

	/* Merged cells expand to explicit empty placeholders, preserving the
	 * original columns for subsequent rows rather than shifting values left. */
	table_html("<table><tr><th>A</th><th>B</th><th>C</th></tr>"
		"<tr><td colspan='2' rowspan=2>X</td><td>Y</td></tr>"
		"<tr><td>Z</td></tr></table>");
	assert(strcmp(body_buffer, PAPERMONO_RSS_TABLE_BEGIN
		PAPERMONO_RSS_TABLE_HEADER "A\tB\tC\n"
		PAPERMONO_RSS_TABLE_ROW "X\t\tY\n"
		PAPERMONO_RSS_TABLE_ROW "\t\tZ\n"
		PAPERMONO_RSS_TABLE_END) == 0);

	/* A nested table is only inline content of its outer cell. */
	table_html("<table><tr><td>left<table><tr><td>inside1</td>"
		"<td>inside2</td></tr></table>tail</td><td>right</td></tr></table>");
	assert(strcmp(body_buffer, PAPERMONO_RSS_TABLE_BEGIN
		PAPERMONO_RSS_TABLE_ROW "left inside1 inside2 tail\tright\n"
		PAPERMONO_RSS_TABLE_END) == 0);

	/* Missing cell/row/table end tags are repaired at the body boundary. */
	table_html("<table><tr><td>a<td>b");
	assert(strcmp(body_buffer, PAPERMONO_RSS_TABLE_BEGIN
		PAPERMONO_RSS_TABLE_ROW "a\tb\n" PAPERMONO_RSS_TABLE_END) == 0);

	/* Private record bytes may only originate in the converter. */
	table_html("pre&#30;T1\n&#30;H\tA\n&#30;R\tB\n&#30;Epost\x1e");
	assert(strchr(body_buffer, '\x1e') == NULL);
	assert(strstr(body_buffer, "preT1") != NULL);

	/* Unsupported spans preserve visible content with an explicit sequential
	 * fallback, never silently map it to the wrong grid columns. */
	table_html("<table><tr><td colspan='999999'>preserved</td><td>next</td></tr></table>");
	assert(strstr(body_buffer, "按单元格顺序显示") != NULL);
	assert(strstr(body_buffer, "preserved | next") != NULL);
	assert(strstr(body_buffer, PAPERMONO_RSS_TABLE_BEGIN) == NULL);
	table_html("<table><tr><td rowspan='0'>kept</td></tr></table>");
	assert(strstr(body_buffer, "按单元格顺序显示") != NULL && strstr(body_buffer, "kept") != NULL);

	/* A quoted > in an unrelated attribute does not hide later spans. */
	table_html("<table><tr><td title='> inside' colspan='2'>pair</td><td>third</td></tr></table>");
	assert(strstr(body_buffer, PAPERMONO_RSS_TABLE_ROW "pair\t\tthird\n") != NULL);
	table_html("<table><tr><td title='属性不显示'><!--评论不显示-->"
		"<script>脚本不显示</script><style>样式不显示</style>可见</td></tr></table>");
	assert(strcmp(body_buffer, PAPERMONO_RSS_TABLE_BEGIN
		PAPERMONO_RSS_TABLE_ROW "可见\n" PAPERMONO_RSS_TABLE_END) == 0);

	char html[2048];
	size_t length = (size_t)snprintf(html, sizeof(html), "<table><tr><th>长格</th></tr><tr><td>");
	for (unsigned int i = 0U; i < 180U; ++i) {
		memcpy(html + length, "长", 3U);
		length += 3U;
	}
	(void)snprintf(html + length, sizeof(html) - length, "</td></tr></table>");
	table_html(html);
	const char *long_cell = strstr(body_buffer, PAPERMONO_RSS_TABLE_ROW);
	assert(long_cell != NULL);
	long_cell += 3U;
	assert(strchr(long_cell, '\n') - long_cell == 540);

	/* Ordinary wide tables are not limited by the merged-cell tracking
	 * budget. Every column remains represented in the streamed codec. */
	length = (size_t)snprintf(html, sizeof(html), "<table><tr>");
	for (unsigned int i = 0U; i < 40U; ++i) {
		length += (size_t)snprintf(html + length, sizeof(html) - length,
			"<td>%u</td>", i);
	}
	(void)snprintf(html + length, sizeof(html) - length, "</tr><tr></tr></table>");
	table_html(html);
	assert(strstr(body_buffer, "\t38\t39\n") != NULL);
	assert(strstr(body_buffer, "按单元格顺序显示") == NULL);
	assert(strstr(body_buffer, PAPERMONO_RSS_TABLE_ROW "\n") != NULL);
	table_html("<table><tr><td/><td>kept</td></tr></table>");
	assert(strstr(body_buffer, PAPERMONO_RSS_TABLE_ROW "\tkept\n") != NULL);

	length = (size_t)snprintf(html, sizeof(html), "<table><tr><td title='");
	memset(html + length, 'x', 256U); length += 256U;
	(void)snprintf(html + length, sizeof(html) - length,
		"' colspan='2'>attributes kept</td><td>last</td></tr></table>");
	table_html(html);
	assert(strstr(body_buffer, "按单元格顺序显示") != NULL);
	assert(strstr(body_buffer, "attributes kept | last") != NULL);

	/* Once a UTF-8 unit exceeds the body limit, later ASCII/control records
	 * must not fill the leftover byte and alter a truncated cell's content. */
	static char large_html[PAPERMONO_RSS_BODY_MAX + 128U];
	length = (size_t)snprintf(large_html, sizeof(large_html), "<table><tr><td>");
	memset(large_html + length, 'x', PAPERMONO_RSS_BODY_MAX - 8U);
	length += PAPERMONO_RSS_BODY_MAX - 8U;
	(void)snprintf(large_html + length, sizeof(large_html) - length,
		"长Z</td></tr></table>");
	table_html(large_html);
	assert(strlen(body_buffer) == PAPERMONO_RSS_BODY_MAX - 1U);
	assert(strchr(body_buffer, 'Z') == NULL);
	assert(strstr(body_buffer, PAPERMONO_RSS_TABLE_END) == NULL);
}

int main(void)
{
	test_corpus_index();
	test_entities_and_whitespace();
	test_cdata_literal();
	test_raw_html_stripping();
	test_html_paragraphs();
	test_html_headings();
	test_html_blockquote();
	test_html_lists();
	test_html_blank_line_compression();
	test_html_inline_spacing();
	test_html_url_shortening();
	test_html_url_truncation();
	test_html_heading_dedup();
	test_cache_version_invalid();
	test_content_encoded_fallback();
	test_missing_channel_title();
	test_empty_feed();
	test_item_limit();
	test_title_truncation();
	test_body_truncation_ascii();
	test_body_truncation_utf8();
	test_body_load_capacity();
	test_body_load_missing();
	test_no_cache();
	test_cache_round_trip();
	test_corrupt_index();
	test_missing_bodies();
	test_parse_failure_keeps_cache();
	test_body_cache_total_limit();
	test_commit_recovery();
	test_malformed_input();
	test_html_tables();
	test_utf8_scalars();
	test_utf8_control_boundaries();
	test_utf8_titles();
	(void)system("rm -rf " TEST_CACHE_DIR);
	puts("rss feed parsing and SD cache: PASS");
	return 0;
}
