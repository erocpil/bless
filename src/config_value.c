#include "config_value.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

int
config_value_uint16(const char *text, uint16_t *value)
{
	char *end;
	unsigned long parsed;

	if (!text || !*text || !value) {
		return -1;
	}
	errno = 0;
	parsed = strtoul(text, &end, 0);
	if (errno || *end || parsed > UINT16_MAX) {
		return -1;
	}
	*value = (uint16_t)parsed;
	return 0;
}

int
config_value_uint32(const char *text, uint32_t *value)
{
	char *end;
	unsigned long long parsed;

	if (!text || !*text || !value) {
		return -1;
	}
	errno = 0;
	parsed = strtoull(text, &end, 0);
	if (errno || *end || parsed > UINT32_MAX) {
		return -1;
	}
	*value = (uint32_t)parsed;
	return 0;
}
