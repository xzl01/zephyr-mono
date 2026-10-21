/* SPDX-License-Identifier: GPL-3.0-only */
/* Host-test stand-in for miniz's tinfl API. Only symbol shapes matter: the
 * ZIP reader's open-failure paths never inflate, and the positive case below
 * supplies its own statuses. No decompression behaviour is claimed. */
#ifndef HOST_MINIZ_H_
#define HOST_MINIZ_H_

#include <stddef.h>
#include <stdint.h>

#define TINFL_LZ_DICT_SIZE 32768
#define TINFL_FLAG_HAS_MORE_INPUT 0x2

typedef enum {
	TINFL_STATUS_FAILED = -1,
	TINFL_STATUS_DONE = 0,
	TINFL_STATUS_NEEDS_MORE_INPUT = 1,
	TINFL_STATUS_HAS_MORE_OUTPUT = 2,
} tinfl_status;

typedef struct {
	int unused;
} tinfl_decompressor;

void tinfl_init(tinfl_decompressor *decompressor);
tinfl_status tinfl_decompress(tinfl_decompressor *decompressor,
	const uint8_t *input, size_t *input_length,
	uint8_t *output, uint8_t *output_end, size_t *output_length,
	uint32_t flags);

#endif
