/* SPDX-License-Identifier: GPL-3.0-only */

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <papermono/rss.h>

/* Internal seam of src/papermono_rss.c; the XML scanner is pure C so the host
 * build does not need the Zephyr network stack.
 */
extern int papermono_rss_parse_xml(const char *xml, size_t length,
				   struct papermono_rss_feed *out);

static const char sample_feed[] =
	"<?xml version='1.0' encoding='utf-8'?>\n"
	"<rss version=\"2.0\" xmlns:content=\"http://purl.org/rss/1.0/"
	"modules/content/\">"
	"<channel>"
	"<title>AI 新闻 &amp; More</title>"
	"<link>http://192.168.100.122:8080/</link>"
	"<description>Channel description</description>"
	"<item>"
	"<title>First &lt;post&gt;</title>"
	"<link>http://192.168.100.122:8080/entries/1</link>"
	"<pubDate>Tue, 27 Oct 2026 01:00:00 GMT</pubDate>"
	"<description>&lt;p&gt;Hello &lt;b&gt;world&lt;/b&gt; "
	"&amp;amp; friends&lt;/p&gt;</description>"
	"</item>"
	"<item>"
	"<title><![CDATA[Second <raw> title]]></title>"
	"<description><![CDATA[<p>CDATA paragraph</p>]]></description>"
	"</item>"
	"<item>"
	"<title>Third</title>"
	"<content:encoded><![CDATA[<p>Encoded fallback</p>]]>"
	"</content:encoded>"
	"</item>"
	"</channel></rss>";

static void test_sample_feed(void)
{
	struct papermono_rss_feed feed;

	assert(papermono_rss_parse_xml(sample_feed, sizeof(sample_feed) - 1U,
				       &feed) == 0);
	assert(feed.valid);
	assert(feed.count == 3U);
	assert(strcmp(feed.title, "AI 新闻 & More") == 0);

	assert(strcmp(feed.items[0].title, "First <post>") == 0);
	assert(strcmp(feed.items[0].published,
		      "Tue, 27 Oct 2026 01:00:00 GMT") == 0);
	/* HTML-escaped description: tags become separators, then the entities
	 * hidden by the XML escaping are decoded a second time.
	 */
	assert(strcmp(feed.items[0].body, "Hello world & friends") == 0);

	/* CDATA is literal XML text: no tag stripping for titles, no entity
	 * decoding.
	 */
	assert(strcmp(feed.items[1].title, "Second <raw> title") == 0);
	assert(strcmp(feed.items[1].body, "CDATA paragraph") == 0);
	/* A missing <description> falls back to <content:encoded>. */
	assert(strcmp(feed.items[2].title, "Third") == 0);
	assert(strcmp(feed.items[2].body, "Encoded fallback") == 0);
}

static void test_entities_and_whitespace(void)
{
	static const char feed_xml[] =
		"<rss><channel><title>x</title><item><title>"
		"A&amp;B &lt;tag&gt; &quot;q&quot; &#39; &#20013; &#x4E2D; "
		"&bogus; &amp;amp;</title>"
		"<description>&lt;p&gt;  alpha \n\t beta   gamma &lt;/p&gt;"
		"</description></item></channel></rss>";
	struct papermono_rss_feed feed;

	assert(papermono_rss_parse_xml(feed_xml, sizeof(feed_xml) - 1U,
				       &feed) == 0);
	assert(feed.count == 1U);
	/* Named, decimal and hexadecimal entities decode; unknown ones and a
	 * literal ampersand survive verbatim; a title keeps its angle brackets.
	 */
	assert(strcmp(feed.items[0].title,
		      "A&B <tag> \"q\" ' 中 中 &bogus; &amp;") == 0);
	assert(strcmp(feed.items[0].body, "alpha beta gamma") == 0);
}

static void test_cdata_literal(void)
{
	static const char feed_xml[] =
		"<rss><channel><item>"
		"<title><![CDATA[A &amp; B <b>raw</b>]]></title>"
		"<description>  <![CDATA[x &lt; y]]>  </description>"
		"</item></channel></rss>";
	struct papermono_rss_feed feed;

	assert(papermono_rss_parse_xml(feed_xml, sizeof(feed_xml) - 1U,
				       &feed) == 0);
	assert(feed.count == 1U);
	assert(strcmp(feed.items[0].title, "A &amp; B <b>raw</b>") == 0);
	/* A CDATA body is HTML: its tags are stripped and its entities decoded. */
	assert(strcmp(feed.items[0].body, "x < y") == 0);
}

static void test_raw_html_stripping(void)
{
	static const char feed_xml[] =
		"<rss><channel><item><title>Raw HTML</title>"
		"<description><p>Raw <b>bold</b> text</p>"
		"<a href=\"http://example.com/?a=1&amp;b=2\">link</a>"
		"<script>ignored()</script></description></item>"
		"</channel></rss>";
	struct papermono_rss_feed feed;

	assert(papermono_rss_parse_xml(feed_xml, sizeof(feed_xml) - 1U,
				       &feed) == 0);
	assert(feed.count == 1U);
	assert(strcmp(feed.items[0].body, "Raw bold text link ignored()") == 0);
}

static void test_missing_channel_title(void)
{
	static const char feed_xml[] =
		"<rss><channel><item><title>Item only</title></item>"
		"</channel></rss>";
	struct papermono_rss_feed feed;

	assert(papermono_rss_parse_xml(feed_xml, sizeof(feed_xml) - 1U,
				       &feed) == 0);
	assert(feed.title[0] == '\0');
	assert(feed.count == 1U);
	assert(strcmp(feed.items[0].title, "Item only") == 0);
}

static void test_empty_feed(void)
{
	static const char feed_xml[] = "<rss><channel></channel></rss>";
	struct papermono_rss_feed feed;

	assert(papermono_rss_parse_xml(feed_xml, sizeof(feed_xml) - 1U,
				       &feed) == 0);
	assert(feed.valid);
	assert(feed.count == 0U);
	assert(strcmp(feed.title, "") == 0);
}

static void test_item_limit(void)
{
	char feed_xml[2048];
	size_t used = 0U;
	struct papermono_rss_feed feed;

	used += (size_t)snprintf(feed_xml + used, sizeof(feed_xml) - used,
				 "<rss><channel><title>Limit</title>");
	for (unsigned int i = 0U; i < 15U; ++i) {
		used += (size_t)snprintf(feed_xml + used,
					 sizeof(feed_xml) - used,
					 "<item><title>Item %u</title></item>",
					 i);
	}
	used += (size_t)snprintf(feed_xml + used, sizeof(feed_xml) - used,
				 "</channel></rss>");
	assert(used < sizeof(feed_xml));

	assert(papermono_rss_parse_xml(feed_xml, used, &feed) == 0);
	assert(feed.count == PAPERMONO_RSS_ITEM_MAX);
	assert(strcmp(feed.items[0].title, "Item 0") == 0);
	assert(strcmp(feed.items[PAPERMONO_RSS_ITEM_MAX - 1U].title,
		      "Item 11") == 0);
}

static void test_truncation_ascii(void)
{
	/* BODY_MAX is large enough for a full digest, so keep these buffers off
	 * the stack and size them for the doubled input.
	 */
	static char feed_xml[PAPERMONO_RSS_BODY_MAX * 2U + 512U];
	static char long_text[PAPERMONO_RSS_BODY_MAX * 2U + 1U];
	struct papermono_rss_feed feed;
	size_t used;

	memset(long_text, 'A', sizeof(long_text) - 1U);
	long_text[sizeof(long_text) - 1U] = '\0';
	used = (size_t)snprintf(feed_xml, sizeof(feed_xml),
				"<rss><channel><item><title>%s</title>"
				"</item></channel></rss>", long_text);
	assert(used < sizeof(feed_xml));
	assert(papermono_rss_parse_xml(feed_xml, used, &feed) == 0);
	assert(feed.count == 1U);
	assert(strlen(feed.items[0].title) == PAPERMONO_RSS_TITLE_MAX);
	assert(feed.items[0].title[PAPERMONO_RSS_TITLE_MAX] == '\0');

	memset(long_text, 'B', sizeof(long_text) - 1U);
	long_text[sizeof(long_text) - 1U] = '\0';
	used = (size_t)snprintf(feed_xml, sizeof(feed_xml),
				"<rss><channel><item><title>t</title>"
				"<description>%s</description></item>"
				"</channel></rss>", long_text);
	assert(used < sizeof(feed_xml));
	assert(papermono_rss_parse_xml(feed_xml, used, &feed) == 0);
	assert(feed.count == 1U);
	assert(strlen(feed.items[0].body) == PAPERMONO_RSS_BODY_MAX);
	assert(feed.items[0].body[PAPERMONO_RSS_BODY_MAX] == '\0');
}

static void test_truncation_utf8(void)
{
	char feed_xml[512];
	char title[PAPERMONO_RSS_TITLE_MAX + 8U];
	struct papermono_rss_feed feed;
	size_t used;
	size_t title_len = 0U;

	/* 126 ASCII bytes then a three-byte character: 129 bytes do not fit in
	 * 127, and the character must not be split.
	 */
	memset(title, 'A', PAPERMONO_RSS_TITLE_MAX - 1U);
	title_len = PAPERMONO_RSS_TITLE_MAX - 1U;
	memcpy(title + title_len, "中", 3U);
	title_len += 3U;
	title[title_len] = '\0';
	used = (size_t)snprintf(feed_xml, sizeof(feed_xml),
				"<rss><channel><item><title>%s</title>"
				"</item></channel></rss>", title);
	assert(used < sizeof(feed_xml));
	assert(papermono_rss_parse_xml(feed_xml, used, &feed) == 0);
	assert(feed.count == 1U);
	assert(strlen(feed.items[0].title) == PAPERMONO_RSS_TITLE_MAX - 1U);
	for (size_t i = 0U; i < strlen(feed.items[0].title); ++i) {
		assert(feed.items[0].title[i] == 'A');
	}
}

static void test_utf8_round_trip(void)
{
	static const char feed_xml[] =
		"<rss><channel><title>中文标题</title><item>"
		"<title>早报 · 2026-10-27</title>"
		"<description>今日头条：代理开始替人办事。</description>"
		"</item></channel></rss>";
	struct papermono_rss_feed feed;

	assert(papermono_rss_parse_xml(feed_xml, sizeof(feed_xml) - 1U,
				       &feed) == 0);
	assert(strcmp(feed.title, "中文标题") == 0);
	assert(strcmp(feed.items[0].title, "早报 · 2026-10-27") == 0);
	assert(strcmp(feed.items[0].body,
		      "今日头条：代理开始替人办事。") == 0);
}

static void test_malformed_input(void)
{
	struct papermono_rss_feed feed;
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

	/* No crash and best-effort extraction from a truncated feed. */
	assert(papermono_rss_parse_xml(unclosed, sizeof(unclosed) - 1U,
				       &feed) == 0);
	assert(feed.count <= 1U);

	/* A commented-out item is not a real item. */
	assert(papermono_rss_parse_xml(commented, sizeof(commented) - 1U,
				       &feed) == 0);
	assert(feed.count == 1U);
	assert(strcmp(feed.items[0].title, "visible") == 0);

	/* Invalid UTF-8 is copied through, never read out of bounds. */
	assert(papermono_rss_parse_xml(bad_bytes, sizeof(bad_bytes) - 1U,
				       &feed) == 0);
	assert(feed.title[0] != '\0');

	/* Self-closing fields yield no text and are skipped. */
	assert(papermono_rss_parse_xml(self_closed,
				       sizeof(self_closed) - 1U, &feed) == 0);
	assert(feed.count == 1U);
	assert(strcmp(feed.items[0].title, "kept") == 0);
}

static void test_rejects_non_feed(void)
{
	static const char html[] = "<html><body>not a feed</body></html>";
	static const char no_close[] = "<rss><channel><title>x</title>";
	struct papermono_rss_feed feed;

	/* Empty input is an error and the output is cleared. */
	memset(&feed, 0x5A, sizeof(feed));
	assert(papermono_rss_parse_xml(NULL, 0U, &feed) == -EINVAL);
	assert(!feed.valid);
	assert(feed.count == 0U);
	assert(papermono_rss_parse_xml("", 0U, &feed) == -EINVAL);
	assert(papermono_rss_parse_xml(html, sizeof(html) - 1U, &feed) ==
	       -EBADMSG);
	assert(!feed.valid);
	assert(feed.count == 0U);
	/* A NULL output pointer is refused instead of dereferenced. */
	assert(papermono_rss_parse_xml(html, sizeof(html) - 1U, NULL) ==
	       -EINVAL);

	/* A channel without a closing tag is still a usable feed. */
	assert(papermono_rss_parse_xml(no_close, sizeof(no_close) - 1U,
				       &feed) == 0);
	assert(feed.valid);
	assert(strcmp(feed.title, "x") == 0);
	assert(feed.count == 0U);
}

int main(void)
{
	test_sample_feed();
	test_entities_and_whitespace();
	test_cdata_literal();
	test_raw_html_stripping();
	test_missing_channel_title();
	test_empty_feed();
	test_item_limit();
	test_truncation_ascii();
	test_truncation_utf8();
	test_utf8_round_trip();
	test_malformed_input();
	test_rejects_non_feed();
	puts("rss feed parsing: PASS");
	return 0;
}
