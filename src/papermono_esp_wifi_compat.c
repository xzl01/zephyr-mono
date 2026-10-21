/* SPDX-License-Identifier: GPL-3.0-only */
#include <stddef.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(papermono_esp_wifi_compat, LOG_LEVEL_INF);

/* hal_espressif 19f979c's ESP32-S3 net80211 blob can dispatch an AP SA Query
 * timer without a station node. Its ROM cnx_node_is_existing(NULL) check
 * accepts an empty slot, then ap_try_sa_query_process dereferences node+0x300.
 * Keep valid PMF exchanges unchanged and discard this invalid timer argument.
 * The link wrap is restricted to ESP32-S3 Wi-Fi builds in CMakeLists.txt.
 */
void __real_ap_try_sa_query_process(void *node);

void __wrap_ap_try_sa_query_process(void *node)
{
	if (node == NULL) {
		LOG_WRN("Discarded Wi-Fi SA Query timer without a station node");
		return;
	}
	__real_ap_try_sa_query_process(node);
}
