/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_SYNC_H_
#define PAPERMONO_SYNC_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct papermono_sync_state {
	uint64_t expected;
	uint64_t received;
	int last_error;
	bool initialized;
	bool active;
};

int papermono_sync_init(void);
void papermono_sync_get_state(struct papermono_sync_state *state);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_SYNC_H_ */
