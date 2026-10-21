/* SPDX-License-Identifier: GPL-3.0-only */

/* Preview shim: zephyr/device.h
 *
 * DEVICE_DT_GET() expands to the address of a distinct static struct device so
 * it is a compile-time constant (the UI file uses it to initialise file-scope
 * const pointers) and so device_is_ready() returns true, putting the UI on its
 * "hardware present" paths.
 */
#ifndef PAPERMONO_PREVIEW_ZEPHYR_DEVICE_H_
#define PAPERMONO_PREVIEW_ZEPHYR_DEVICE_H_

#include <stdbool.h>

struct device {
	const char *name;
};

#define PAPERMONO_PREVIEW_DEVICE_SLOTS 16

extern struct device papermono_preview_device_slots[PAPERMONO_PREVIEW_DEVICE_SLOTS];

#define DEVICE_DT_GET(node_id) (&papermono_preview_device_slots[(node_id) % PAPERMONO_PREVIEW_DEVICE_SLOTS])
#define DEVICE_DT_GET_OR_NULL(node_id) DEVICE_DT_GET(node_id)
#define DEVICE_DT_GET_ANY(binding) (&papermono_preview_device_slots[0])
#define DEVICE_DT_GET_ONE(binding) (&papermono_preview_device_slots[0])

static inline bool device_is_ready(const struct device *dev)
{
	return dev != NULL;
}

static inline int device_init(const struct device *dev)
{
	(void)dev;
	return 0;
}

#endif /* PAPERMONO_PREVIEW_ZEPHYR_DEVICE_H_ */
