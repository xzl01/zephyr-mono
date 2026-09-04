/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_PDM_MIC_H_
#define PAPERMONO_PDM_MIC_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Power, configure and start the PaperMono PDM microphone stream. */
int papermono_pdm_mic_start(const struct device *dev);

/**
 * Read one DMA block of signed 16-bit PCM samples.
 *
 * @param dev Microphone device.
 * @param samples Destination sample buffer.
 * @param capacity Maximum number of samples that fit in @p samples.
 * @param count Number of samples copied on success.
 */
int papermono_pdm_mic_read(const struct device *dev, int16_t *samples,
			   size_t capacity, size_t *count);

/** Stop the stream, release queued DMA blocks and turn microphone power off. */
int papermono_pdm_mic_stop(const struct device *dev);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_PDM_MIC_H_ */
