/* SPDX-License-Identifier: GPL-3.0-only */

#include <assert.h>
#include <stdio.h>

#include <papermono/refresh_policy.h>

static struct papermono_refresh_policy_state state(bool baseline, uint8_t partials,
						   enum papermono_refresh_quality quality)
{
	struct papermono_refresh_policy_state value = {
		.baseline_ready = baseline,
		.partials = partials,
		.quality = quality,
	};

	return value;
}

int main(void)
{
	const struct papermono_refresh_policy plain = {
		.limit = 10U,
		.opportunistic = false,
	};
	const struct papermono_refresh_policy opportunistic = {
		.limit = 10U,
		.opportunistic = true,
	};
	struct papermono_refresh_policy_state s;

	/* Invalid or missing baseline always needs a full refresh. */
	s = state(false, 0U, PAPERMONO_REFRESH_AUTO);
	assert(papermono_refresh_policy_decide(NULL, &s) ==
		PAPERMONO_REFRESH_ACTION_FULL);
	assert(papermono_refresh_policy_decide(&plain, NULL) ==
		PAPERMONO_REFRESH_ACTION_FULL);
	assert(papermono_refresh_policy_decide(&plain, &s) ==
		PAPERMONO_REFRESH_ACTION_FULL);

	/* Normal partial path below the vendor-recommended limit. */
	s = state(true, 0U, PAPERMONO_REFRESH_AUTO);
	assert(papermono_refresh_policy_decide(&plain, &s) ==
		PAPERMONO_REFRESH_ACTION_PARTIAL);
	s = state(true, 9U, PAPERMONO_REFRESH_FAST);
	assert(papermono_refresh_policy_decide(&plain, &s) ==
		PAPERMONO_REFRESH_ACTION_PARTIAL);

	/* The limit itself is never exceeded, even for FAST page turns. */
	s = state(true, 10U, PAPERMONO_REFRESH_FAST);
	assert(papermono_refresh_policy_decide(&plain, &s) ==
		PAPERMONO_REFRESH_ACTION_FULL);
	s = state(true, 10U, PAPERMONO_REFRESH_AUTO);
	assert(papermono_refresh_policy_decide(&opportunistic, &s) ==
		PAPERMONO_REFRESH_ACTION_FULL);
	s = state(true, 10U, PAPERMONO_REFRESH_FAST);
	assert(papermono_refresh_policy_decide(&opportunistic, &s) ==
		PAPERMONO_REFRESH_ACTION_FULL);

	/* Opportunistic cleanup only fires one refresh below the limit and only
	 * for non-FAST renders; FAST page turns keep the partial path.
	 */
	s = state(true, 9U, PAPERMONO_REFRESH_AUTO);
	assert(papermono_refresh_policy_decide(&opportunistic, &s) ==
		PAPERMONO_REFRESH_ACTION_FULL);
	s = state(true, 9U, PAPERMONO_REFRESH_FULL);
	assert(papermono_refresh_policy_decide(&opportunistic, &s) ==
		PAPERMONO_REFRESH_ACTION_FULL);
	s = state(true, 9U, PAPERMONO_REFRESH_FAST);
	assert(papermono_refresh_policy_decide(&opportunistic, &s) ==
		PAPERMONO_REFRESH_ACTION_PARTIAL);
	s = state(true, 8U, PAPERMONO_REFRESH_AUTO);
	assert(papermono_refresh_policy_decide(&opportunistic, &s) ==
		PAPERMONO_REFRESH_ACTION_PARTIAL);
	s = state(true, 9U, PAPERMONO_REFRESH_AUTO);
	assert(papermono_refresh_policy_decide(&plain, &s) ==
		PAPERMONO_REFRESH_ACTION_PARTIAL);

	/* Defensive handling of a zero limit: one partial refresh. */
	struct papermono_refresh_policy zero = plain;

	zero.limit = 0U;
	s = state(true, 0U, PAPERMONO_REFRESH_AUTO);
	assert(papermono_refresh_policy_decide(&zero, &s) ==
		PAPERMONO_REFRESH_ACTION_PARTIAL);
	s = state(true, 1U, PAPERMONO_REFRESH_AUTO);
	assert(papermono_refresh_policy_decide(&zero, &s) ==
		PAPERMONO_REFRESH_ACTION_FULL);

	/* A limit of one never defers a full refresh to a breakpoint. */
	struct papermono_refresh_policy one = opportunistic;

	one.limit = 1U;
	s = state(true, 0U, PAPERMONO_REFRESH_AUTO);
	assert(papermono_refresh_policy_decide(&one, &s) ==
		PAPERMONO_REFRESH_ACTION_PARTIAL);
	s = state(true, 1U, PAPERMONO_REFRESH_AUTO);
	assert(papermono_refresh_policy_decide(&one, &s) ==
		PAPERMONO_REFRESH_ACTION_FULL);

	puts("refresh policy limit and breakpoint decisions: PASS");
	return 0;
}
