/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_CONTENT_VALIDATION_H_
#define PAPERMONO_CONTENT_VALIDATION_H_

#include <stdint.h>

#include <papermono/content_store.h>

#ifdef __cplusplus
extern "C" {
#endif

int papermono_content_validate(enum papermono_content_kind kind,
	const char *path, uint64_t size);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_CONTENT_VALIDATION_H_ */
