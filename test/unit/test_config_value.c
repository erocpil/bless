#include "config_value.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int
main(void)
{
	uint16_t u16 = 0;
	uint32_t u32 = 0;
	assert(config_value_uint16("0", &u16) == 0 && u16 == 0);
	assert(config_value_uint16("0xffff", &u16) == 0 && u16 == UINT16_MAX);
	assert(config_value_uint16("65536", &u16) < 0);
	assert(config_value_uint16("12x", &u16) < 0);
	assert(config_value_uint16("", &u16) < 0);
	assert(config_value_uint32("0xffffffff", &u32) == 0 && u32 == UINT32_MAX);
	assert(config_value_uint32("4294967296", &u32) < 0);
	assert(config_value_uint32("-1", &u32) < 0);
	puts("config scalar values: PASS");
	return 0;
}
