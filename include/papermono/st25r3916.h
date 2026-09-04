/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_ST25R3916_H_
#define PAPERMONO_ST25R3916_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

struct papermono_st25r3916_info {
	uint8_t identity;
	uint8_t type;
	uint8_t revision;
};

#define PAPERMONO_NFCA_UID_MAX_LENGTH 10U

struct papermono_nfca_tag {
	uint8_t uid[PAPERMONO_NFCA_UID_MAX_LENGTH];
	uint8_t uid_length;
	uint16_t atqa;
	uint8_t sak;
};

struct papermono_nfcb_tag {
	uint8_t pupi[4];
	uint8_t application_data[4];
	uint8_t protocol_info[3];
};

struct papermono_nfcf_tag {
	uint8_t idm[8];
	uint8_t pmm[8];
};

struct papermono_nfcv_tag {
	uint8_t uid[8];
	uint8_t dsfid;
};

typedef void (*papermono_st25r3916_irq_callback_t)(const struct device *dev,
						   void *user_data);

int papermono_st25r3916_set_power(const struct device *dev, bool enable);
int papermono_st25r3916_get_info(const struct device *dev,
					 struct papermono_st25r3916_info *info);
int papermono_st25r3916_read(const struct device *dev, uint8_t reg,
				     uint8_t *data, size_t length);
int papermono_st25r3916_write(const struct device *dev, uint8_t reg,
				      const uint8_t *data, size_t length);
int papermono_st25r3916_direct_command(const struct device *dev,
					       uint8_t command);
int papermono_st25r3916_configure_nfca(const struct device *dev);
int papermono_st25r3916_nfca_scan(const struct device *dev,
				  struct papermono_nfca_tag *tag);
int papermono_st25r3916_configure_nfcb(const struct device *dev);
int papermono_st25r3916_nfcb_scan(const struct device *dev,
				  struct papermono_nfcb_tag *tag);
int papermono_st25r3916_configure_nfcf(const struct device *dev);
int papermono_st25r3916_nfcf_scan(const struct device *dev,
				  struct papermono_nfcf_tag *tag);
int papermono_st25r3916_configure_nfcv(const struct device *dev);
int papermono_st25r3916_nfcv_scan(const struct device *dev,
				  struct papermono_nfcv_tag *tag);
int papermono_st25r3916_disable_field(const struct device *dev);
int papermono_st25r3916_set_irq_callback(
	const struct device *dev, papermono_st25r3916_irq_callback_t callback,
	void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_ST25R3916_H_ */
