#ifndef __BLESS_CONFIG_VALUE_H__
#define __BLESS_CONFIG_VALUE_H__

#include <stdint.h>

/* Strict scalar conversions used by built-in and extension config parsers.
 * Return 0 on success and -1 for empty, malformed, or out-of-range input. */
int config_value_uint16(const char *text, uint16_t *value);
int config_value_uint32(const char *text, uint32_t *value);

#endif
