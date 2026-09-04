/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_DRIVER_SELFTEST_H_
#define PAPERMONO_DRIVER_SELFTEST_H_

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

int papermono_driver_selftest_run(const struct device *display);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_DRIVER_SELFTEST_H_ */
