/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_ARCHIVE_SAFETY_H_
#define PAPERMONO_ARCHIVE_SAFETY_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int papermono_archive_validate_limits(uint32_t entry_count,
	uint32_t central_size, uint64_t expanded_total);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_ARCHIVE_SAFETY_H_ */
