/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_INFO_PROVIDER_H_
#define PAPERMONO_INFO_PROVIDER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAPERMONO_INFO_CARD_MAX 8U

enum papermono_info_status {
	PAPERMONO_INFO_EMPTY,
	PAPERMONO_INFO_FRESH,
	PAPERMONO_INFO_STALE,
	PAPERMONO_INFO_ERROR,
};

struct papermono_info_card {
	char id[33];
	char title[97];
	char summary[241];
	char source[49];
	char value[33];
	int64_t updated_at;
	int32_t change_milli_percent;
	enum papermono_info_status status;
	bool has_value;
	bool has_change;
};

typedef int (*papermono_info_fetch_cb)(void *context,
	struct papermono_info_card *cards, size_t capacity, size_t *count);

struct papermono_info_provider {
	const char *id;
	papermono_info_fetch_cb fetch;
	void *context;
};

int papermono_info_init(void);
int papermono_info_register(const struct papermono_info_provider *provider);
int papermono_info_refresh_all(void);
int papermono_info_publish(const struct papermono_info_card *cards,
	size_t count);
void papermono_info_mark_stale(void);
size_t papermono_info_count(void);
int papermono_info_get(size_t index, struct papermono_info_card *card);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_INFO_PROVIDER_H_ */
