/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <papermono/http_codec.h>

int main(void)
{
	char name[256];
	uint32_t crc = 0U;
	const char *long_name =
		"长文件名长文件名长文件名长文件名长文件名长文件名长文件名长文件名"
		"长文件名长文件名长文件名长文件名长文件名长文件名长文件名长文件名"
		"长文件名长文件名长文件名长文件名.epub";
	const char *long_encoded =
		"b64:6ZW_5paH5Lu25ZCN6ZW_5paH5Lu25ZCN6ZW_5paH5Lu25ZCN6ZW_5paH5Lu25ZCN"
		"6ZW_5paH5Lu25ZCN6ZW_5paH5Lu25ZCN6ZW_5paH5Lu25ZCN6ZW_5paH5Lu25ZCN"
		"6ZW_5paH5Lu25ZCN6ZW_5paH5Lu25ZCN6ZW_5paH5Lu25ZCN6ZW_5paH5Lu25ZCN"
		"6ZW_5paH5Lu25ZCN6ZW_5paH5Lu25ZCN6ZW_5paH5Lu25ZCN6ZW_5paH5Lu25ZCN"
		"6ZW_5paH5Lu25ZCN6ZW_5paH5Lu25ZCN6ZW_5paH5Lu25ZCN6ZW_5paH5Lu25ZCN"
		"LmVwdWI";

	assert(papermono_http_decode_name(
		"%E4%B9%A6%E5%90%8D.txt", name, sizeof(name)) == 0);
	assert(strcmp(name, "书名.txt") == 0);
	assert(papermono_http_decode_name(
		"b64:5Lmm5ZCNLnR4dA", name, sizeof(name)) == 0);
	assert(strcmp(name, "书名.txt") == 0);
	assert(papermono_http_decode_name(long_encoded, name, sizeof(name)) == 0);
	assert(strcmp(name, long_name) == 0);
	assert(papermono_http_decode_name("b64:A", name, sizeof(name)) == -EINVAL);
	assert(papermono_http_decode_name("b64:AB", name, sizeof(name)) == -EINVAL);
	assert(papermono_http_decode_name("bad%2", name, sizeof(name)) == -EINVAL);
	assert(papermono_http_decode_name("b64:5Lmm5ZCNLnR4dA", name, 4U) ==
		-ENAMETOOLONG);

	assert(papermono_http_parse_crc32("89ABCDEF", &crc));
	assert(crc == UINT32_C(0x89ABCDEF));
	assert(papermono_http_parse_crc32("0", &crc) && crc == 0U);
	assert(!papermono_http_parse_crc32("", &crc));
	assert(!papermono_http_parse_crc32("123456789", &crc));
	assert(!papermono_http_parse_crc32("12xz", &crc));

	/* Content-Length. Zephyr never fills in http_client_ctx.content_len on the
	 * HTTP/1 path this firmware uses, so the declared body length has to be
	 * parsed from the header itself. An absent or unparsable value must be an
	 * error: treating it as zero is what let a zero-length body overwrite an
	 * existing file with nothing.
	 */
	size_t length = SIZE_MAX;

	assert(papermono_http_parse_content_length("0", &length) == 0 &&
		length == 0U);
	assert(papermono_http_parse_content_length("1234", &length) == 0 &&
		length == 1234U);
	/* RFC 9110 permits leading zeros. */
	assert(papermono_http_parse_content_length("007", &length) == 0 &&
		length == 7U);
	/* Exactly at the limit is accepted, one above is not. */
	assert(papermono_http_parse_content_length("67108864", &length) == 0 &&
		length == 67108864U);
	assert(papermono_http_parse_content_length("67108865", &length) ==
		-ERANGE);
	assert(papermono_http_parse_content_length("99999999999", &length) ==
		-ERANGE);
	assert(papermono_http_parse_content_length("", &length) == -EINVAL);
	assert(papermono_http_parse_content_length("-1", &length) == -EINVAL);
	assert(papermono_http_parse_content_length("+1", &length) == -EINVAL);
	assert(papermono_http_parse_content_length(" 1", &length) == -EINVAL);
	assert(papermono_http_parse_content_length("1 ", &length) == -EINVAL);
	assert(papermono_http_parse_content_length("1,1", &length) == -EINVAL);
	assert(papermono_http_parse_content_length("abc", &length) == -EINVAL);
	assert(papermono_http_parse_content_length(NULL, &length) == -EINVAL);
	assert(papermono_http_parse_content_length("1", NULL) == -EINVAL);

	puts("HTTP metadata codec tests: PASS");
	return 0;
}
