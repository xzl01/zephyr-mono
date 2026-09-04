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

	puts("HTTP metadata codec tests: PASS");
	return 0;
}
