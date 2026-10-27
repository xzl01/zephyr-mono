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
	(void)system("rm -rf " TEST_CACHE_DIR);
	puts("rss feed parsing and SD cache: PASS");
	return 0;
}
