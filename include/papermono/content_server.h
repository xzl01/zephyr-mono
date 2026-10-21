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

int papermono_content_server_init(void);
void papermono_content_server_get_state(
	struct papermono_content_server_state *state);
bool papermono_content_server_busy(void);
int papermono_content_server_reset_wifi(void);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_CONTENT_SERVER_H_ */
