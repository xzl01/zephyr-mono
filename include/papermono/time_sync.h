/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_TIME_SYNC_H_
#define PAPERMONO_TIME_SYNC_H_

#ifdef __cplusplus
extern "C" {
#endif

int papermono_time_sync_init(void);
void papermono_time_sync_request(void);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_TIME_SYNC_H_ */
