/* SPDX-License-Identifier: GPL-3.0-only */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <papermono/adpcm.h>

#define FRAME_SAMPLES 240U
#define TEST_FRAMES 8U
#define WARMUP_SAMPLES 480U

static void fail(const char *message)
{
	fprintf(stderr, "test_adpcm: %s\n", message);
	exit(1);
}

static int16_t sine_sample(unsigned index, unsigned period, int amplitude)
{
	const double phase = 2.0 * 3.14159265358979323846 * (double)(index % period) /
		(double)period;

	return (int16_t)lrint(sin(phase) * amplitude);
}

static double signal_to_noise(const int16_t *reference, const int16_t *decoded,
	size_t count)
{
	double signal = 0.0;
	double noise = 0.0;

	for (size_t index = 0U; index < count; ++index) {
		const double value = reference[index];
		const double error = value - decoded[index];

		signal += value * value;
		noise += error * error;
	}
	if (noise == 0.0) {
		return 200.0;
	}
	return 10.0 * log10(signal / noise);
}

static void test_round_trip(void)
{
	struct papermono_adpcm_state encoder;
	struct papermono_adpcm_state decoder;
	int16_t reference[FRAME_SAMPLES * TEST_FRAMES];
	int16_t decoded[FRAME_SAMPLES * TEST_FRAMES];
	uint8_t encoded[FRAME_SAMPLES / 2U];
	double snr;

	for (size_t index = 0U; index < FRAME_SAMPLES * TEST_FRAMES; ++index) {
		reference[index] = sine_sample((unsigned)index, 32U, 12000);
	}
	papermono_adpcm_reset(&encoder);
	papermono_adpcm_reset(&decoder);
	for (size_t frame = 0U; frame < TEST_FRAMES; ++frame) {
		papermono_adpcm_encode(&encoder, &reference[frame * FRAME_SAMPLES],
			FRAME_SAMPLES, encoded);
		papermono_adpcm_decode(&decoder, encoded, sizeof(encoded),
			&decoded[frame * FRAME_SAMPLES]);
	}
	snr = signal_to_noise(&reference[WARMUP_SAMPLES],
		&decoded[WARMUP_SAMPLES],
		FRAME_SAMPLES * TEST_FRAMES - WARMUP_SAMPLES);
	if (snr < 30.0) {
		fprintf(stderr, "test_adpcm: round-trip SNR %.2f dB is too low\n", snr);
		exit(1);
	}
	if (encoder.predictor != decoder.predictor ||
	    encoder.step_index != decoder.step_index) {
		fail("encoder and decoder state diverged");
	}
}

static void test_high_nibble_first(void)
{
	struct papermono_adpcm_state state;
	const int16_t samples[2] = { 0, 0 };
	uint8_t encoded = 0U;

	papermono_adpcm_reset(&state);
	papermono_adpcm_encode(&state, samples, 2U, &encoded);
	if (encoded != 0U) {
		fail("silence should encode as two zero nibbles");
	}
}

static void test_state_carries_across_frames(void)
{
	struct papermono_adpcm_state encoder;
	struct papermono_adpcm_state decoder;
	struct papermono_adpcm_state checkpoint;
	int16_t reference[FRAME_SAMPLES * 2U];
	int16_t decoded[FRAME_SAMPLES * 2U];
	uint8_t encoded[FRAME_SAMPLES];

	for (size_t index = 0U; index < FRAME_SAMPLES * 2U; ++index) {
		reference[index] = sine_sample((unsigned)index, 48U, 16000);
	}
	papermono_adpcm_reset(&encoder);
	papermono_adpcm_reset(&decoder);
	papermono_adpcm_encode(&encoder, reference, FRAME_SAMPLES, encoded);
	papermono_adpcm_decode(&decoder, encoded, FRAME_SAMPLES / 2U, decoded);
	if (encoder.predictor != decoder.predictor ||
	    encoder.step_index != decoder.step_index) {
		fail("state mismatch after first frame");
	}
	checkpoint = encoder;
	papermono_adpcm_encode(&encoder, &reference[FRAME_SAMPLES],
		FRAME_SAMPLES, encoded);
	papermono_adpcm_decode(&decoder, encoded, FRAME_SAMPLES / 2U,
		&decoded[FRAME_SAMPLES]);
	if (checkpoint.predictor == encoder.predictor &&
	    checkpoint.step_index == encoder.step_index) {
		fail("encoder did not advance across frames");
	}
	if (decoder.predictor != encoder.predictor ||
	    decoder.step_index != encoder.step_index) {
		fail("state mismatch after second frame");
	}
}

static void test_step_index_bounds(void)
{
	struct papermono_adpcm_state state;
	int16_t samples[FRAME_SAMPLES];
	uint8_t encoded[FRAME_SAMPLES / 2U];

	for (size_t index = 0U; index < FRAME_SAMPLES; ++index) {
		samples[index] = (index & 1U) != 0U ? INT16_MAX : INT16_MIN;
	}
	papermono_adpcm_reset(&state);
	for (unsigned frame = 0U; frame < 64U; ++frame) {
		papermono_adpcm_encode(&state, samples, FRAME_SAMPLES, encoded);
		if (state.step_index > 88U) {
			fail("step index exceeded the table");
		}
		if (state.predictor > INT16_MAX || state.predictor < INT16_MIN) {
			fail("predictor left the signed 16-bit range");
		}
	}
}

int main(void)
{
	test_round_trip();
	test_high_nibble_first();
	test_state_carries_across_frames();
	test_step_index_bounds();
	printf("test_adpcm: PASS\n");
	return 0;
}
