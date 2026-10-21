/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <papermono/info_provider.h>
#include <papermono/ui.h>

LOG_MODULE_REGISTER(papermono_info, LOG_LEVEL_INF);

#define INFO_CACHE_VERSION 1U
#define INFO_PROVIDER_MAX 4U

#if defined(CONFIG_ESP_SPIRAM)
#define PAPERMONO_INFO_BSS __attribute__((section(".ext_ram.bss")))
#else
#define PAPERMONO_INFO_BSS
#endif

struct cached_card {
	uint16_t version;
	uint16_t reserved;
	struct papermono_info_card card;
};

struct load_card_context {
	struct cached_card card;
	bool loaded;
};

static struct papermono_info_card cards[PAPERMONO_INFO_CARD_MAX];
static struct papermono_info_card fetch_buffer[PAPERMONO_INFO_CARD_MAX]
	PAPERMONO_INFO_BSS;
static const struct papermono_info_provider *providers[INFO_PROVIDER_MAX];
static size_t card_count;
static size_t provider_count;
static bool initialized;
K_MUTEX_DEFINE(info_lock);
K_MUTEX_DEFINE(info_refresh_lock);

static int load_card(const char *key, size_t length, settings_read_cb read_cb,
	void *cb_arg, void *parameter)
{
	struct load_card_context *context = parameter;

	if ((key == NULL || key[0] == '\0') &&
	    length == sizeof(context->card) &&
	    read_cb(cb_arg, &context->card, sizeof(context->card)) ==
		    sizeof(context->card) &&
	    context->card.version == INFO_CACHE_VERSION) {
		context->loaded = true;
	}
	return 0;
}

static int save_card(size_t index)
{
	struct cached_card cached = {
		.version = INFO_CACHE_VERSION,
		.card = cards[index],
	};
	char key[24];

	(void)snprintf(key, sizeof(key), "info_cache/%02u", (unsigned int)index);
	return settings_save_one(key, &cached, sizeof(cached));
}

int papermono_info_init(void)
{
	if (initialized) {
		return 0;
	}
	for (size_t index = 0U; index < PAPERMONO_INFO_CARD_MAX; ++index) {
		struct load_card_context loaded = { 0 };
		char key[24];

		(void)snprintf(key, sizeof(key), "info_cache/%02u",
			(unsigned int)index);
		(void)settings_load_subtree_direct(key, load_card, &loaded);
		if (!loaded.loaded || loaded.card.card.id[0] == '\0') {
			continue;
		}
		loaded.card.card.status = PAPERMONO_INFO_STALE;
		cards[card_count++] = loaded.card.card;
	}
	initialized = true;
	LOG_INF("Information cache loaded: %zu cards", card_count);
	return 0;
}

int papermono_info_register(const struct papermono_info_provider *provider)
{
	if (provider == NULL || provider->id == NULL || provider->fetch == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&info_lock, K_FOREVER);
	if (provider_count >= INFO_PROVIDER_MAX) {
		k_mutex_unlock(&info_lock);
		return -ENOSPC;
	}
	for (size_t index = 0U; index < provider_count; ++index) {
		if (strcmp(providers[index]->id, provider->id) == 0) {
			k_mutex_unlock(&info_lock);
			return -EALREADY;
		}
	}
	providers[provider_count++] = provider;
	k_mutex_unlock(&info_lock);
	return 0;
}

int papermono_info_publish(const struct papermono_info_card *published,
	size_t count)
{
	int first_error = 0;

	if ((published == NULL && count > 0U) || count > PAPERMONO_INFO_CARD_MAX) {
		return -EINVAL;
	}
	k_mutex_lock(&info_lock, K_FOREVER);
	for (size_t incoming = 0U; incoming < count; ++incoming) {
		if (published[incoming].id[0] == '\0' ||
		    published[incoming].title[0] == '\0') {
			first_error = first_error ?: -EINVAL;
			continue;
		}
		size_t target = card_count;
		for (size_t index = 0U; index < card_count; ++index) {
			if (strcmp(cards[index].id, published[incoming].id) == 0) {
				target = index;
				break;
			}
		}
		if (target == card_count) {
			if (card_count >= PAPERMONO_INFO_CARD_MAX) {
				first_error = first_error ?: -ENOSPC;
				continue;
			}
			++card_count;
		}
		cards[target] = published[incoming];
		cards[target].status = published[incoming].status ==
			PAPERMONO_INFO_ERROR ? PAPERMONO_INFO_ERROR :
			PAPERMONO_INFO_FRESH;
		int ret = save_card(target);
		if (ret < 0 && first_error == 0) {
			first_error = ret;
		}
	}
	k_mutex_unlock(&info_lock);
	papermono_ui_request_background_refresh();
	return first_error;
}

int papermono_info_refresh_all(void)
{
	const struct papermono_info_provider *local[INFO_PROVIDER_MAX];
	size_t count;
	int first_error = 0;

	k_mutex_lock(&info_lock, K_FOREVER);
	count = provider_count;
	memcpy(local, providers, count * sizeof(local[0]));
	k_mutex_unlock(&info_lock);
	k_mutex_lock(&info_refresh_lock, K_FOREVER);
	for (size_t index = 0U; index < count; ++index) {
		size_t fetched_count = 0U;
		int ret = local[index]->fetch(local[index]->context, fetch_buffer,
			ARRAY_SIZE(fetch_buffer), &fetched_count);

		if (ret == 0) {
			ret = papermono_info_publish(fetch_buffer, fetched_count);
		}
		if (ret < 0 && first_error == 0) {
			first_error = ret;
		}
	}
	k_mutex_unlock(&info_refresh_lock);
	return first_error;
}

void papermono_info_mark_stale(void)
{
	k_mutex_lock(&info_lock, K_FOREVER);
	for (size_t index = 0U; index < card_count; ++index) {
		if (cards[index].status == PAPERMONO_INFO_FRESH) {
			cards[index].status = PAPERMONO_INFO_STALE;
		}
	}
	k_mutex_unlock(&info_lock);
}

size_t papermono_info_count(void)
{
	size_t count;
	k_mutex_lock(&info_lock, K_FOREVER);
	count = card_count;
	k_mutex_unlock(&info_lock);
	return count;
}

int papermono_info_get(size_t index, struct papermono_info_card *card)
{
	if (card == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&info_lock, K_FOREVER);
	if (index >= card_count) {
		k_mutex_unlock(&info_lock);
		return -ENOENT;
	}
	*card = cards[index];
	k_mutex_unlock(&info_lock);
	return 0;
}
