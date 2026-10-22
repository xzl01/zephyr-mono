/* SPDX-License-Identifier: GPL-3.0-only */

#include <papermono/adpcm.h>

/* IMA/DVI ADPCM quantization step table. The encoder and the reference
 * decoder share it so a round-trip test exercises the exact wire format.
 */
static const int16_t adpcm_step_table[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
	34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
	143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
	494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
	1552, 1707, 1878, 2066, 2272, 2494, 2740, 3008, 3307, 3638, 4002,
	4402, 4842, 5327, 5860, 6446, 7091, 7800, 8580, 9438, 10382,
	11420, 12562, 13818, 15200, 16720, 18392, 20231, 22254, 24479,
	26927, 29620, 32767,
};

static const int8_t adpcm_index_table[8] = {
	-1, -1, -1, -1, 2, 4, 6, 8,
};

static int32_t clamp_predictor(int32_t value)
{
	if (value > INT16_MAX) {
		return INT16_MAX;
	}
	if (value < INT16_MIN) {
		return INT16_MIN;
	}
	return value;
}

static uint8_t clamp_step_index(int32_t value)
{
	if (value < 0) {
		return 0U;
	}
	if (value > 88) {
		return 88U;
	}
	return (uint8_t)value;
}

void papermono_adpcm_reset(struct papermono_adpcm_state *state)
{
	if (state == NULL) {
		return;
	}
	state->predictor = 0;
	state->step_index = 0U;
}

static uint8_t encode_nibble(struct papermono_adpcm_state *state,
	int16_t sample)
{
	const int32_t step = adpcm_step_table[state->step_index];
	int32_t difference = (int32_t)sample - state->predictor;
	uint8_t nibble = 0U;
	int32_t quantized;
	int32_t shifted = step;

	if (difference < 0) {
		nibble = 8U;
		difference = -difference;
	}

	quantized = shifted >> 3;
	if (difference >= shifted) {
		nibble |= 4U;
		difference -= shifted;
		quantized += shifted;
	}
	shifted >>= 1;
	if (difference >= shifted) {
		nibble |= 2U;
		difference -= shifted;
		quantized += shifted;
	}
	shifted >>= 1;
	if (difference >= shifted) {
		nibble |= 1U;
		quantized += shifted;
	}

	state->predictor = (nibble & 8U) != 0U ?
		state->predictor - quantized : state->predictor + quantized;
	state->predictor = clamp_predictor(state->predictor);
	state->step_index = clamp_step_index(
		(int32_t)state->step_index + adpcm_index_table[nibble & 7U]);
	return nibble;
}

static int16_t decode_nibble(struct papermono_adpcm_state *state,
	uint8_t nibble)
{
	const int32_t step = adpcm_step_table[state->step_index];
	int32_t difference = step >> 3;

	if ((nibble & 4U) != 0U) {
		difference += step;
	}
	if ((nibble & 2U) != 0U) {
		difference += step >> 1;
	}
	if ((nibble & 1U) != 0U) {
		difference += step >> 2;
	}

	state->predictor = (nibble & 8U) != 0U ?
		state->predictor - difference : state->predictor + difference;
	state->predictor = clamp_predictor(state->predictor);
	state->step_index = clamp_step_index(
		(int32_t)state->step_index + adpcm_index_table[nibble & 7U]);
	return (int16_t)state->predictor;
}

void papermono_adpcm_encode(struct papermono_adpcm_state *state,
	const int16_t *samples, size_t count, uint8_t *output)
{
	size_t index;

	if (state == NULL || samples == NULL || output == NULL) {
		return;
	}
	for (index = 0U; index + 1U < count; index += 2U) {
		const uint8_t high = encode_nibble(state, samples[index]);
		const uint8_t low = encode_nibble(state, samples[index + 1U]);

		output[index / 2U] = (uint8_t)((high << 4) | low);
	}
}

void papermono_adpcm_decode(struct papermono_adpcm_state *state,
	const uint8_t *input, size_t count, int16_t *output)
{
	size_t index;

	if (state == NULL || input == NULL || output == NULL) {
		return;
	}
	for (index = 0U; index < count; ++index) {
		const uint8_t byte = input[index];

		output[index * 2U] = decode_nibble(state, byte >> 4);
		output[index * 2U + 1U] = decode_nibble(state, byte & 0x0FU);
	}
}
