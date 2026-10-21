/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_REFRESH_H_
#define PAPERMONO_REFRESH_H_

#ifdef __cplusplus
extern "C" {
#endif

enum papermono_refresh_quality {
	/* Let the display driver choose partial or maintenance full refresh. */
	PAPERMONO_REFRESH_AUTO,
	/* Prefer the normal partial path; safety fallbacks and residue limits win. */
	PAPERMONO_REFRESH_FAST,
	/* Rebuild the panel baseline with a complete refresh. */
	PAPERMONO_REFRESH_FULL,
};

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_REFRESH_H_ */
