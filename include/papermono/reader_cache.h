/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_READER_CACHE_H_
#define PAPERMONO_READER_CACHE_H_

#include <stdbool.h>
#include <stdint.h>

#include <papermono/content_store.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAPERMONO_READER_CACHE_FORMAT_VERSION 2U

struct papermono_reader_cache_key {
	char id[PAPERMONO_CONTENT_ID_LENGTH + 1U];
	uint64_t book_size;
	uint32_t book_modified;
	uint8_t kind;
	uint8_t encoding;
	uint8_t density;
	uint8_t columns;
	uint8_t lines;
};

struct papermono_reader_cache_entry {
	uint32_t offset;
	uint16_t chapter;
};

struct papermono_reader_cache {
	struct papermono_reader_cache_key key;
	char path[PAPERMONO_CONTENT_PATH_MAX];
	uint32_t count;
	bool ready;
};

int papermono_reader_cache_open(struct papermono_reader_cache *cache,
	const struct papermono_reader_cache_key *key,
	const struct papermono_reader_cache_entry *initial);
int papermono_reader_cache_reset(struct papermono_reader_cache *cache,
	const struct papermono_reader_cache_key *key,
	const struct papermono_reader_cache_entry *initial);
int papermono_reader_cache_record(struct papermono_reader_cache *cache,
	uint32_t index, const struct papermono_reader_cache_entry *entry);
int papermono_reader_cache_read(const struct papermono_reader_cache *cache,
	uint32_t index, struct papermono_reader_cache_entry *entry);
int papermono_reader_cache_find(const struct papermono_reader_cache *cache,
	const struct papermono_reader_cache_entry *entry, uint32_t *index);
bool papermono_reader_cache_matches(const struct papermono_reader_cache *cache,
	const struct papermono_reader_cache_key *key);
void papermono_reader_cache_disable(struct papermono_reader_cache *cache);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_READER_CACHE_H_ */
