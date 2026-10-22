/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_ADPCM_H_
#define PAPERMONO_ADPCM_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** IMA/DVI ADPCM state shared by the encoder and the reference decoder.
 *
 * The Bluetooth voice stream sends four-bit nibbles high-first, two samples
 * per byte, with one continuous predictor across frames. The host resets its
 * decoder from an AUDIO_SYNC packet carrying @ref predictor and
 * @ref step_index, so both values must describe the state *before* the next
 * encoded frame.
 */
struct papermono_adpcm_state {
	int32_t predictor;
	uint8_t step_index;
};

void papermono_adpcm_reset(struct papermono_adpcm_state *state);

/**
 * Encode signed 16-bit PCM into IMA ADPCM.
 *
 * @param state Encoder state; must be reset once per stream.
 * @param samples Input samples. @p count must be even.
 * @param count Number of input samples.
 * @param output Destination for @p count / 2 bytes.
 */
void papermono_adpcm_encode(struct papermono_adpcm_state *state,
	const int16_t *samples, size_t count, uint8_t *output);

/**
 * Decode IMA ADPCM produced by @ref papermono_adpcm_encode.
 *
 * This reference decoder exists for host tests and diagnostics. The device
 * only encodes; keeping both directions here makes a round-trip test possible
 * without duplicating the step tables.
 *
 * @param state Decoder state; reset to the same predictor/step as the encoder.
 * @param input Encoded bytes.
 * @param count Number of encoded bytes.
 * @param output Destination for @p count * 2 samples.
 */
void papermono_adpcm_decode(struct papermono_adpcm_state *state,
	const uint8_t *input, size_t count, int16_t *output);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_ADPCM_H_ */
