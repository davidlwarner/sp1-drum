#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

bool cs42l42_codec_read_plug_status(const struct device *dev, uint8_t *out_status);
bool cs42l42_codec_plug_status_is_connected(uint8_t status);
bool cs42l42_codec_is_headphone_connected(const struct device *dev);
void cs42l42_codec_log_state(const struct device *dev, const char *tag);

#ifdef __cplusplus
}
#endif
