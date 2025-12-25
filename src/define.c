#include "define.h"

/* 计算 16-bit one's complement checksum */
uint16_t icmp_calc_cksum(const void *buf, size_t len)
{
	const uint16_t *data = buf;
	uint32_t sum = 0;

	while (len > 1) {
		sum += *data++;
		len -= 2;
	}
	if (len == 1) {
		sum += *((const uint8_t *)data) << 8;
	}
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return (uint16_t)(~sum);
}
