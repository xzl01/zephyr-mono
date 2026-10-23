/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PAPERMONO_WEATHER_H_
#define PAPERMONO_WEATHER_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct papermono_weather {
	bool valid;
	int8_t current_temp;
	uint8_t weather_code;
	int8_t hourly_temp[8];
	uint8_t hourly_count;
	int64_t updated_at;
};

int papermono_weather_init(void);
void papermono_weather_get(struct papermono_weather *snapshot);
int papermono_weather_request(void);

#ifdef __cplusplus
}
#endif

#endif
