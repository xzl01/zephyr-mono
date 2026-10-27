/* SPDX-License-Identifier: GPL-3.0-only */

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

#include <papermono/settings.h>
#include <papermono/ui.h>

LOG_MODULE_REGISTER(papermono_rss, LOG_LEVEL_INF);
#endif

/*
 * RSS 2.0 scanner.
 *
 * The scanner is deliberately dependency free: it never allocates, never
 * recurses, and always advances through the input, so a truncated or malformed
 * feed can only produce fewer fields, not a crash or an infinite loop. It is
 * shared with tests/host/test_rss.c through papermono_rss_parse_xml().
 */

/* Internal seam for the host tests; the firmware only needs the worker. */
int papermono_rss_parse_xml(const char *xml, size_t length,
			    struct papermono_rss_feed *out);

static bool rss_is_space(char ch)
{
	return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
	       ch == '\f' || ch == '\v';
}

static bool rss_is_tag_start(char ch)
{
	return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
	       ch == '/' || ch == '!' || ch == '?';
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
		/* The name runs to the end of the buffer: tolerate the feed. */
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
		/* Invalid scalar value: show a placeholder instead of the entity
		 * source text.
		 */
		out[0] = '?';
		written = 1U;
	}
	*consumed = name_len + 2U;
	return written;
}

struct rss_sanitizer {
	char *out;
	size_t out_size; /* sizeof(out), including the trailing NUL */
	size_t length;
	bool pending_space;
};

/* Append @p length bytes, inserting a single word separator when one is
 * pending. Returns false once the output buffer is full.
 */
static bool rss_append(struct rss_sanitizer *s, const char *text, size_t length)
{
	if (s->pending_space) {
		if (s->length + 1U + length >= s->out_size) {
			return false;
		}
		s->out[s->length] = ' ';
		++s->length;
		s->pending_space = false;
	} else if (s->length + length >= s->out_size) {
		return false;
	}
	memcpy(s->out + s->length, text, length);
	s->length += length;
	return true;
}

/*
 * Normalise one element payload: optionally decode entities, optionally strip
 * HTML tags (replaced by word separators), then collapse whitespace. UTF-8 is
 * copied verbatim, and truncation never splits a multi-byte character. src and
 * out may alias: the decoded stream is never longer than what has been read, so
 * a forward in-place pass is safe.
 */
static void rss_sanitize(const char *src, size_t length, bool decode_entities,
			 bool strip_html, char *out, size_t out_size)
{
	struct rss_sanitizer s = {
		.out = out,
		.out_size = out_size,
		.length = 0U,
		.pending_space = false,
	};
	bool in_tag = false;
	bool pending_lt = false;
	char tag_quote = '\0';
	size_t index = 0U;

	if (out == NULL || out_size == 0U) {
		return;
	}
	while (index < length) {
		char decoded[4];
		size_t decoded_len;
		size_t consumed = 1U;

		if (decode_entities && src[index] == '&') {
			decoded_len = rss_decode_entity(src + index,
							length - index, decoded,
							&consumed);
			if (decoded_len == 0U) {
				decoded[0] = '&';
				decoded_len = 1U;
				consumed = 1U;
			}
		} else if ((unsigned char)src[index] < 0x80U) {
			decoded[0] = src[index];
			decoded_len = 1U;
		} else {
			const unsigned char lead = (unsigned char)src[index];
			size_t sequence = lead >= 0xF0U ? 4U :
					  lead >= 0xE0U ? 3U :
					  lead >= 0xC0U ? 2U : 1U;

			if (sequence > length - index) {
				sequence = length - index;
			}
			memcpy(decoded, src + index, sequence);
			decoded_len = sequence;
			consumed = sequence;
		}
		index += consumed;

		if (decoded_len != 1U || (unsigned char)decoded[0] >= 0x80U) {
			/* Multi-byte character: copy it as a unit so the output
			 * never ends in the middle of a UTF-8 sequence.
			 */
			if (in_tag) {
				continue;
			}
			if (pending_lt) {
				if (!rss_append(&s, "<", 1U)) {
					break;
				}
				pending_lt = false;
			}
			if (!rss_append(&s, decoded, decoded_len)) {
				break;
			}
			continue;
		}
		const char ch = decoded[0];

		if (in_tag) {
			if (tag_quote != '\0') {
				if (ch == tag_quote) {
					tag_quote = '\0';
				}
				continue;
			}
			if (ch == '"' || ch == '\'') {
				tag_quote = ch;
				continue;
			}
			if (ch == '>') {
				in_tag = false;
				if (s.length > 0U) {
					/* Inline and block tags alike separate
					 * words; the whitespace collapse above
					 * removes the extra spaces.
					 */
					s.pending_space = true;
				}
			}
			continue;
		}
		if (pending_lt) {
			if (rss_is_tag_start(ch)) {
				pending_lt = false;
				in_tag = true;
				tag_quote = '\0';
				continue;
			}
			if (!rss_append(&s, "<", 1U)) {
				break;
			}
			pending_lt = false;
		}
		if (strip_html && ch == '<') {
			pending_lt = true;
			continue;
		}
		if (rss_is_space(ch)) {
			if (s.length > 0U) {
				s.pending_space = true;
			}
			continue;
		}
		if (!rss_append(&s, &ch, 1U)) {
			break;
		}
	}
	if (pending_lt && s.length + 1U < s.out_size) {
		s.out[s.length] = '<';
		++s.length;
	}
	s.out[s.length] = '\0';
}

/*
 * Extract the text of the first <name> element in [start, end). CDATA payloads
 * are literal XML text, so entities inside them are not decoded; for item
 * descriptions a second pass decodes the HTML entities that were hidden behind
 * the XML escaping of the feed.
 */
static bool rss_extract_text(const char *start, const char *end,
			     const char *name, char *out, size_t out_size,
			     bool strip_html)
{
	static const char cdata_open[] = "<![CDATA[";
	const size_t name_len = strlen(name);
	const char *content;
	const char *limit;
	const char *raw;
	size_t raw_len;
	bool literal = false;
	bool self_closed = false;

	if (out == NULL || out_size == 0U) {
		return false;
	}
	out[0] = '\0';
	content = rss_xml_find_open(start, end, name, name_len, &self_closed);
	if (content == NULL || self_closed) {
		return false;
	}
	limit = rss_xml_find_close(content, end, name, name_len);
	if (limit == NULL) {
		limit = end;
	}
	raw = content;
	raw_len = (size_t)(limit - content);
	{
		const char *p = content;

		while (p < limit && rss_is_space(*p)) {
			++p;
		}
		if ((size_t)(limit - p) >= sizeof(cdata_open) - 1U &&
		    memcmp(p, cdata_open, sizeof(cdata_open) - 1U) == 0) {
			const char *cdata_end =
				rss_mem_find(p + sizeof(cdata_open) - 1U, limit,
					     "]]>", 3U);

			raw = p + sizeof(cdata_open) - 1U;
			raw_len = cdata_end != NULL ?
				(size_t)(cdata_end - raw) :
				(size_t)(limit - raw);
			literal = true;
		}
	}
	rss_sanitize(raw, raw_len, !literal, strip_html, out, out_size);
	if (strip_html && out[0] != '\0') {
		/* Feeds escape their HTML, so &amp;amp; arrives here as the
		 * literal text "&amp;". Decode the remaining entities and
		 * collapse the whitespace they may have introduced.
		 */
		rss_sanitize(out, strlen(out), true, false, out, out_size);
	}
	return out[0] != '\0';
}

int papermono_rss_parse_xml(const char *xml, size_t length,
			    struct papermono_rss_feed *out)
{
	const char *end;
	const char *channel;
	const char *channel_end;
	const char *first_item;
	bool self_closed = false;
	size_t count = 0U;
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
	/* The channel title is the first title before any item; item titles must
	 * not win when the channel omits its own title.
	 */
	first_item = rss_xml_find_open(channel, channel_end, "item", 4U,
				       &self_closed);
	(void)rss_extract_text(channel,
			       first_item != NULL ? first_item : channel_end,
			       "title", out->title, sizeof(out->title), false);

	cursor = channel;
	while (count < PAPERMONO_RSS_ITEM_MAX) {
		struct papermono_rss_item *entry;
		const char *item;
		const char *item_end;
		bool item_self = false;
		bool have_title;
		bool have_body;

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
		entry = &out->items[count];
		have_title = rss_extract_text(item, item_end, "title",
					      entry->title,
					      sizeof(entry->title), false);
		(void)rss_extract_text(item, item_end, "pubDate",
				       entry->published,
				       sizeof(entry->published), false);
		have_body = rss_extract_text(item, item_end, "description",
					     entry->body, sizeof(entry->body),
					     true);
		if (!have_body) {
			have_body = rss_extract_text(item, item_end,
						     "content:encoded",
						     entry->body,
						     sizeof(entry->body), true);
		}
		if (have_title || have_body) {
			++count;
		}
		if (item_end <= cursor) {
			break;
		}
		cursor = item_end;
	}
	out->count = count;
	out->valid = true;
	return 0;
}

#if defined(__ZEPHYR__)

#define RSS_REFRESH_MS (30U * 60U * 1000U)
#define RSS_RETRY_MS (2U * 60U * 1000U)
#define RSS_FETCH_ATTEMPTS 10U
#define RSS_FETCH_RETRY_MS 20000U
#define RSS_IPV4_WAIT_MS 90000U
#define RSS_RESPONSE_MAX (72U * 1024U)
#define RSS_HOST_MAX 63U
#define RSS_PATH_MAX 191U
#define RSS_THREAD_STACK_SIZE 5120

#if defined(CONFIG_ESP_SPIRAM)
#define RSS_PSRAM __attribute__((section(".ext_ram.bss")))
#else
#define RSS_PSRAM
#endif

struct rss_url {
	char host[RSS_HOST_MAX + 1U];
	char path[RSS_PATH_MAX + 1U];
	uint16_t port;
};

/* The snapshot and both staging buffers live in PSRAM: a 60 KB feed plus the
 * parsed copy would be a large chunk of the internal SRAM budget. */
static struct papermono_rss_feed snapshot RSS_PSRAM;
static struct papermono_rss_feed staging RSS_PSRAM;
static char response[RSS_RESPONSE_MAX] RSS_PSRAM;

static K_MUTEX_DEFINE(snapshot_lock);
static K_SEM_DEFINE(refresh_request, 0, 1);
static atomic_t fetch_busy;
static atomic_t worker_started;
static struct k_thread worker_thread;

/*
 * The worker stack is also placed in PSRAM when it is available. The RSS
 * parser never needs more than a few hundred bytes of stack, and moving it out
 * of the internal DRAM budget keeps room for the UI and radio buffers. The
 * thread is low priority and only runs while a fetch is in flight.
 */
#if defined(CONFIG_ESP_SPIRAM)
/* Z_KERNEL_STACK_DEFINE_IN is available without CONFIG_USERSPACE, unlike the
 * thread-flavoured variant, and the worker never runs in user mode.
 */
static Z_KERNEL_STACK_DEFINE_IN(worker_stack, RSS_THREAD_STACK_SIZE,
				RSS_PSRAM);
#else
static K_THREAD_STACK_DEFINE(worker_stack, RSS_THREAD_STACK_SIZE);
#endif

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
 * Promote the DHCP gateway to a default router once the address is up.
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
	/* A default router left over from a previous network has lifetime 0 and
	 * never expires, so drop it before installing the gateway we just got
	 * from DHCP. Otherwise moving between networks keeps routing to the old
	 * gateway.
	 */
	struct net_if_router *stale = net_if_ipv4_router_find_default(iface,
									NULL);

	if (stale != NULL) {
		(void)net_if_ipv4_router_rm(stale);
	}
	if (net_if_ipv4_router_add(iface, &gw, true, 0) != NULL) {
		LOG_INF("RSS using DHCP default router %u.%u.%u.%u",
			gw.s4_addr[0], gw.s4_addr[1], gw.s4_addr[2],
			gw.s4_addr[3]);
	} else {
		LOG_WRN("RSS could not install DHCP default router");
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

/* Decode a chunked transfer-encoding body in place. Returns the decoded
 * length; a malformed chunk stream keeps the chunks decoded so far.
 */
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
		/* Skip chunk extensions and the CRLF that ends the size line. */
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

/*
 * Fetch @p url and return the response body inside the shared response buffer.
 * The body is only valid until the next fetch.
 */
static int rss_http_get(const char *url, size_t *body_offset,
			size_t *body_length)
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
			LOG_WRN("RSS DNS lookup failed for %s (%d)", parts.host,
				ret);
			return -EHOSTUNREACH;
		}
		memcpy(&address, result->ai_addr, sizeof(address));
		zsock_freeaddrinfo(result);
	}
	sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		return -errno;
	}
	if (zsock_connect(sock, (struct sockaddr *)&address,
			  sizeof(address)) < 0) {
		ret = -errno;
		LOG_WRN("RSS connect failed (%d) to %s", ret,
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
	while (used + 1U < sizeof(response)) {
		const ssize_t received = zsock_recv(sock, response + used,
						    sizeof(response) - 1U - used,
						    0);

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
	if (used + 1U == sizeof(response)) {
		LOG_WRN("RSS response truncated at %u bytes", (unsigned int)used);
	}
	header_end = rss_mem_find(response, response + used, "\r\n\r\n", 4U);
	if (header_end == NULL) {
		LOG_WRN("RSS response has no HTTP header terminator");
		return -EBADMSG;
	}
	header_len = (size_t)(header_end - response);
	ret = rss_http_status(response, header_len);
	if (ret < 0) {
		return ret;
	}
	if (ret != 200) {
		LOG_WRN("RSS HTTP status %d", ret);
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

static int rss_refresh_once(const char *url)
{
	size_t body_offset = 0U;
	size_t body_length = 0U;
	int ret;

	ret = rss_http_get(url, &body_offset, &body_length);
	if (ret < 0) {
		return ret;
	}
	memset(&staging, 0, sizeof(staging));
	ret = papermono_rss_parse_xml(response + body_offset, body_length,
				      &staging);
	if (ret < 0) {
		LOG_WRN("RSS parse failed (%d) after %u bytes", ret,
			(unsigned int)body_length);
		return ret;
	}
	const time_t now = time(NULL);

	staging.updated_at = now > 1600000000 ? (int64_t)now : 0;
	k_mutex_lock(&snapshot_lock, K_FOREVER);
	snapshot = staging;
	k_mutex_unlock(&snapshot_lock);
	LOG_INF("RSS feed \"%s\": %u items (%u bytes)",
		snapshot.title, (unsigned int)snapshot.count,
		(unsigned int)body_length);
	return 0;
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

static void rss_worker(void *a, void *b, void *c)
{
	char url[PAPERMONO_RSS_URL_MAX + 1U];
	uint32_t delay_ms = 0U;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	LOG_INF("RSS thread started");
	for (;;) {
		int ret = -EIO;

		(void)k_sem_take(&refresh_request, K_MSEC(delay_ms));
		if (!atomic_cas(&fetch_busy, 0, 1)) {
			continue;
		}
		(void)snprintf(url, sizeof(url), "%s",
			       papermono_settings_get()->rss_url);
		if (url[0] == '\0' || strncmp(url, "http://", 7U) != 0) {
			LOG_WRN("RSS URL is not a plain http:// URL");
			ret = -EINVAL;
		} else if (!rss_wait_ipv4()) {
			LOG_WRN("RSS fetch skipped: IPv4 is not ready");
			ret = -ENETUNREACH;
		} else {
			for (uint8_t attempt = 0U;
			     attempt < RSS_FETCH_ATTEMPTS; ++attempt) {
				ensure_default_router();
				ret = rss_refresh_once(url);
				if (ret == 0) {
					break;
				}
				LOG_INF("RSS attempt %u/%u failed (%d)",
					attempt + 1U, RSS_FETCH_ATTEMPTS,
					ret);
				/* A rejected URL or scheme cannot heal by
				 * retrying; wait for the next refresh cycle.
				 */
				if (ret == -EINVAL ||
				    ret == -EPROTONOSUPPORT) {
					break;
				}
				k_msleep(RSS_FETCH_RETRY_MS);
			}
		}
		if (ret == 0) {
			papermono_ui_request_background_refresh();
			delay_ms = RSS_REFRESH_MS;
		} else {
			LOG_WRN("RSS refresh failed (%d); retrying in %u s", ret,
				(unsigned int)(RSS_RETRY_MS / 1000U));
			delay_ms = RSS_RETRY_MS;
		}
		atomic_set(&fetch_busy, 0);
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
	return 0;
}

void papermono_rss_get(struct papermono_rss_feed *out)
{
	if (out == NULL) {
		return;
	}
	k_mutex_lock(&snapshot_lock, K_FOREVER);
	*out = snapshot;
	k_mutex_unlock(&snapshot_lock);
}

int papermono_rss_request(void)
{
	if (atomic_get(&fetch_busy) != 0) {
		return 0;
	}
	k_sem_give(&refresh_request);
	return 0;
}

#endif /* __ZEPHYR__ */
