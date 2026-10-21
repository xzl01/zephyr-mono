/* SPDX-License-Identifier: GPL-3.0-only */
/* Standalone host test: cc -std=c17 -Wall -Wextra -Werror
 * -Itests/host/stubs -Iinclude tests/host/test_info_provider.c -o /tmp/test_info_provider
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/papermono_info_provider.c"

static const char *fake_suffix;
static unsigned int read_count;
static struct cached_card saved_card = {
	.version = INFO_CACHE_VERSION,
	.card = { .id = "cached", .title = "Saved card",
		.status = PAPERMONO_INFO_FRESH },
};

static int read_card(void *arg, void *data, size_t length)
{
	++read_count;
	memcpy(data, arg, length);
	return (int)length;
}

int settings_load_subtree_direct(const char *subtree,
	settings_load_direct_cb callback, void *param)
{
	if (strcmp(subtree, "info_cache/00") != 0) {
		return 0;
	}
	return callback(fake_suffix, sizeof(saved_card), read_card,
		&saved_card, param);
}

int settings_save_one(const char *key, const void *value, size_t length)
{
	(void)key;
	(void)value;
	(void)length;
	return 0;
}

void papermono_ui_request_background_refresh(void)
{
}

int main(void)
{
	static const char *const suffixes[] = { NULL, "", "child" };

	for (size_t index = 0U; index < 3U; ++index) {
		struct papermono_info_card card;

		initialized = false;
		card_count = 0U;
		read_count = 0U;
		fake_suffix = suffixes[index];
		assert(papermono_info_init() == 0);
		assert(papermono_info_count() == (index < 2U ? 1U : 0U));
		assert(read_count == (index < 2U ? 1U : 0U));
		if (index < 2U) {
			assert(papermono_info_get(0U, &card) == 0);
			assert(strcmp(card.id, "cached") == 0);
			assert(card.status == PAPERMONO_INFO_STALE);
		}
	}
	puts("info provider exact-root settings tests: PASS");
	return 0;
}
