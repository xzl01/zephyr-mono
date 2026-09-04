/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <papermono/text_layout.h>

int main(void)
{
	assert(papermono_text_prohibits_line_start(0x3002U));
	assert(papermono_text_prohibits_line_start(0xFF01U));
	assert(!papermono_text_prohibits_line_start(0x4E2DU));
	assert(papermono_text_prohibits_line_end(0x300CU));
	assert(papermono_text_prohibits_line_end('('));
	assert(!papermono_text_prohibits_line_end(0x6587U));

	assert(!papermono_text_should_wrap_before(0x4E2DU, 25U, 26U));
	assert(papermono_text_should_wrap_before(0x300CU, 25U, 26U));
	assert(!papermono_text_should_wrap_before(0x3002U, 26U, 26U));
	assert(!papermono_text_should_wrap_before(0x201DU, 27U, 26U));
	assert(papermono_text_should_wrap_before(0x3002U, 28U, 26U));
	assert(papermono_text_should_wrap_before(0x4E2DU, 26U, 26U));

	puts("text layout tests: PASS");
	return 0;
}
