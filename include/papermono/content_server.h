/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_CONTENT_SERVER_H_
#define PAPERMONO_CONTENT_SERVER_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAPERMONO_PAIRING_CODE_LENGTH 8U

struct papermono_content_server_state {
	char ap_ssid[24];
	char ap_password[24];
	char pairing_code[PAPERMONO_PAIRING_CODE_LENGTH + 1U];
	char station_ipv4[16];
	bool ap_enabled;
	bool ap_verified;
	bool station_connecting;
	bool station_connected;
	bool client_paired;
	uint32_t content_generation;
};

#define PAPERMONO_WIFI_NETWORK_MAX 16U
#define PAPERMONO_WIFI_SSID_LENGTH 32U

enum papermono_wifi_security {
	PAPERMONO_WIFI_SECURITY_OPEN,
	PAPERMONO_WIFI_SECURITY_WPA2_PSK,
	PAPERMONO_WIFI_SECURITY_UNSUPPORTED,
};

enum papermono_wifi_connection_state {
	PAPERMONO_WIFI_DISCONNECTED,
	PAPERMONO_WIFI_CONNECTING,
	PAPERMONO_WIFI_CONNECTED,
	PAPERMONO_WIFI_CONNECTION_ERROR,
};

struct papermono_wifi_network {
	/* Always terminated; ssid_length is authoritative (scan SSIDs need not
	 * contain a NUL). Hidden/embedded-NUL/non-2.4GHz entries cannot connect.
	 */
	char ssid[PAPERMONO_WIFI_SSID_LENGTH + 1U];
	uint8_t ssid_length;
	int16_t rssi;
	uint8_t channel;
	enum papermono_wifi_security security;
	bool hidden;
	bool supported;
};

struct papermono_wifi_state {
	/* Public snapshots never contain router passwords. */
	char saved_ssid[PAPERMONO_WIFI_SSID_LENGTH + 1U];
	bool configured;
	enum papermono_wifi_connection_state connection;
	int last_connection_error;
	bool request_busy;
	uint32_t scan_generation;
	bool scan_busy;
	int scan_error;
	uint8_t count;
	struct papermono_wifi_network networks[PAPERMONO_WIFI_NETWORK_MAX];
};

/* Thread-context APIs. Requests only copy/validate and enqueue; zero means
 * accepted, NOT connected/persisted. Poll the snapshot after background refresh.
 * -EAGAIN: not initialized; -EBUSY: another operation/scan; -ENOTSUP: unsupported
 * network; -EINVAL: malformed SSID/password. WPA2 accepts 8..63 printable ASCII.
 * OPEN requires an empty password. Callers may release password memory on return.
 */
void papermono_content_server_get_wifi_state(struct papermono_wifi_state *state);
int papermono_content_server_scan_wifi(void);
int papermono_content_server_connect_wifi(
	const struct papermono_wifi_network *network, const char *password);
int papermono_content_server_connect_saved(void);
/* Forget/reset also revoke client pairing and rotate the provisioning AP
 * credentials after its old beacon has stopped. Asynchronous, like connect.
 */
int papermono_content_server_forget_wifi(void);

int papermono_content_server_init(void);
void papermono_content_server_get_state(
	struct papermono_content_server_state *state);
bool papermono_content_server_busy(void);
int papermono_content_server_reset_wifi(void);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_CONTENT_SERVER_H_ */
