/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_RX8130_H_
#define PAPERMONO_RX8130_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int papermono_rx8130_set_timer_irq(uint32_t requested_ms,
				    uint32_t *actual_ms);
int papermono_rx8130_disable_timer_irq(void);
int papermono_rx8130_get_irq_status(bool *timer, bool *alarm);
int papermono_rx8130_clear_irq_status(bool timer, bool alarm);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_RX8130_H_ */
