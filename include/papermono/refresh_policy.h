/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_REFRESH_POLICY_H_
#define PAPERMONO_REFRESH_POLICY_H_

#include <stdbool.h>
#include <stdint.h>

#include <papermono/refresh.h>

#ifdef __cplusplus
extern "C" {
#endif

struct papermono_refresh_policy {
	/* Consecutive partial refreshes allowed before a maintenance full. */
	uint8_t limit;
	/* Perform the maintenance full at the first non-FAST render once the
	 * partial count is one below the limit. The limit itself is not raised.
	 */
	bool opportunistic;
};

struct papermono_refresh_policy_state {
	bool baseline_ready;
	uint8_t partials;
	enum papermono_refresh_quality quality;
};

enum papermono_refresh_action {
	PAPERMONO_REFRESH_ACTION_PARTIAL = 0,
	PAPERMONO_REFRESH_ACTION_FULL,
};

/**
 * Decide between the partial and full-refresh paths.
 *
 * A missing baseline always needs a full refresh. Otherwise the maintenance
 * full refresh is taken when the partial count reaches the configured limit,
 * or - when @c opportunistic is set - at the first non-FAST render once the
 * count is one below the limit.
 */
enum papermono_refresh_action papermono_refresh_policy_decide(
	const struct papermono_refresh_policy *policy,
	const struct papermono_refresh_policy_state *state);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_REFRESH_POLICY_H_ */
