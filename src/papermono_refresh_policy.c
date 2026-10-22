/* SPDX-License-Identifier: GPL-3.0-only */

#include <stddef.h>

#include <papermono/refresh_policy.h>

enum papermono_refresh_action papermono_refresh_policy_decide(
	const struct papermono_refresh_policy *policy,
	const struct papermono_refresh_policy_state *state)
{
	uint8_t limit;

	if (policy == NULL || state == NULL || !state->baseline_ready) {
		return PAPERMONO_REFRESH_ACTION_FULL;
	}

	/* Kconfig enforces 1..20; stay defensive for direct host callers. */
	limit = policy->limit == 0U ? 1U : policy->limit;
	if (state->partials >= limit) {
		/* The vendor-recommended interval is exhausted. */
		return PAPERMONO_REFRESH_ACTION_FULL;
	}

	if (!policy->opportunistic || limit <= 1U ||
	    state->quality == PAPERMONO_REFRESH_FAST) {
		return PAPERMONO_REFRESH_ACTION_PARTIAL;
	}

	/* Full refresh at a natural page transition instead of one refresh later.
	 * FAST page turns keep the partial path so reading is not interrupted.
	 */
	if (state->partials >= (uint8_t)(limit - 1U)) {
		return PAPERMONO_REFRESH_ACTION_FULL;
	}

	return PAPERMONO_REFRESH_ACTION_PARTIAL;
}
