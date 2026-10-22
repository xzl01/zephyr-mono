/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_BLE_MIC_H_
#define PAPERMONO_BLE_MIC_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Lifecycle of one device-initiated voice transfer. */
enum papermono_ble_mic_phase {
	/** No capture is requested and the microphone is powered down. */
	PAPERMONO_BLE_MIC_IDLE = 0,
	/** Start was accepted; the worker is powering and configuring the mic. */
	PAPERMONO_BLE_MIC_STARTING,
	/** PCM is being captured, encoded and notified to the subscribed host. */
	PAPERMONO_BLE_MIC_STREAMING,
	/** Stop was requested; the worker is draining and powering the mic down. */
	PAPERMONO_BLE_MIC_STOPPING,
};

struct papermono_ble_mic_state {
	enum papermono_ble_mic_phase phase;
	int last_error;
	/** Duration of the current session, or of the last completed session. */
	uint32_t session_ms;
	/** Encoded ADPCM bytes accepted by the Bluetooth stack in that session. */
	uint32_t session_bytes;
	/** Completed or currently running sessions since boot. */
	uint32_t sessions;
	uint32_t frames_sent;
	uint32_t frames_dropped;
	bool initialized;
	bool connected;
	bool secured;
	bool subscribed;
	bool streaming;
};

/**
 * Create the capture worker and connection/security tracking.
 *
 * The GATT service itself is defined statically, so this must run before the
 * host can use it. Call after @ref papermono_hid_init() has enabled Bluetooth.
 */
int papermono_ble_mic_init(void);

/**
 * Ask the worker to power the microphone and start an ATVV voice stream.
 *
 * The call only validates the current link state and wakes the worker; it
 * never blocks on microphone power sequencing or Bluetooth notifications.
 *
 * @retval 0 The request was accepted, or a stream is already running.
 * @retval -ENODEV The feature or the PDM microphone is unavailable.
 * @retval -ENOTCONN No host is connected.
 * @retval -EACCES The link is not encrypted or the host has not subscribed.
 */
int papermono_ble_mic_request_start(void);

/** Ask the worker to stop the stream and power the microphone down. */
int papermono_ble_mic_request_stop(void);

/**
 * Stop immediately for sleep/power transitions.
 *
 * Sends AUDIO_STOP when possible and waits up to one second for the worker to
 * release the PDM stream. If the worker does not converge in time, the
 * microphone is force-stopped and the worker settles on its next read.
 * Unlike @ref papermono_ble_mic_request_stop(), this call may block briefly
 * and must not be used from an ISR.
 */
void papermono_ble_mic_abort(void);

void papermono_ble_mic_get_state(struct papermono_ble_mic_state *state);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_BLE_MIC_H_ */
