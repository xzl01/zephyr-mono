/* SPDX-License-Identifier: GPL-3.0-only */

/* Preview shim: zephyr/devicetree.h */
#ifndef PAPERMONO_PREVIEW_ZEPHYR_DEVICETREE_H_
#define PAPERMONO_PREVIEW_ZEPHYR_DEVICETREE_H_

#include <papermono_preview_nodes.h>

/* Unknown node labels resolve through papermono_preview_nodes.h; anything not
 * listed there fails loudly at compile time, as it should.
 */
#define DT_NODELABEL(name) PAPERMONO_DT_ID_##name
#define DT_PATH(name) PAPERMONO_DT_ID_##name
#define DT_CHOSEN(name) PAPERMONO_DT_ID_##name
#define DT_ALIAS(name) PAPERMONO_DT_ID_##name
#define DT_HAS_NODELABEL(name) 1
#define DT_HAS_CHOSEN(name) 1
#define DT_HAS_ALIAS(name) 1
#define DT_HAS_COMPAT_STATUS_OKAY(compat) 1
#define DT_NODE_EXISTS(node_id) 0
#define DT_NODE_HAS_STATUS(node_id, status) 1
#define DT_NODE_HAS_STATUS_OKAY(node_id) 1
#define DT_PROP(node_id, prop) 0
#define DT_PROP_OR(node_id, prop, default) (default)
#define DT_REG_ADDR(node_id) 0
#define DT_STRING_UPPER_TOKEN(node_id, prop) 0
#define DT_ENUM_IDX(node_id, prop) 0
#define DT_INST_FOREACH_STATUS_OKAY(fn)
#define DT_FOREACH_STATUS_OKAY(compat, fn)

#endif /* PAPERMONO_PREVIEW_ZEPHYR_DEVICETREE_H_ */
