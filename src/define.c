#include <rte_byteorder.h>
#include "define.h"

static __thread uint32_t rand_s;

uint32_t fast_rand_next()
{
	return rand_s = rand_s * 1664525u + 1013904223u;
}

uint16_t random_array_elem_uint16_t(uint16_t *array, uint16_t num, int32_t range)
{
	uint32_t rs = fast_rand_next();
	uint16_t r = rs ^ (rs >> 16);

	/* 离散集合 */
	if (likely(num)) {
		if ((num & (num - 1)) == 0) {
			return array[r & (num - 1)];
		}
		return array[r % num];
	}

	/* 连续区间 */
	if (unlikely(range == 0 || range == 1)) {
		return array[0];
	}

	int32_t abs_range = (range >= 0) ? range : -range;
	uint16_t off = r % abs_range;
	uint16_t port = array[0];

	return (range > 0) ?  (uint16_t)(port + off) : (uint16_t)(port - off);
}

uint32_t random_array_elem_uint32_t(uint32_t *array, uint16_t num, int64_t range)
{
	uint32_t r = fast_rand_next();
	r ^= r >> 16;

	/* 离散集合 */
	if (likely(num)) {
		if ((num & (num - 1)) == 0) {
			return array[r & (num - 1)];
		}
		return array[r % num];
	}

	/* 连续区间 */
	if (unlikely(range == 0 || range == 1)) {
		return array[0];
	}

	int64_t abs_range = (range >= 0) ? range : -range;
	uint32_t off = r % abs_range;
	uint32_t ipv4 = rte_be_to_cpu_32(array[0]);

	return rte_cpu_to_be_32((range > 0) ?  (uint32_t)(ipv4 + off) : (uint32_t)(ipv4 - off));
}

/** random_array_elem_uint32_t_with_peer - special case for ipv4:vni
 * @return: the lower 32 bits is ipv4 address with net order, the upper 32 bits is vni
 * with host order.
 */
uint64_t random_array_elem_uint32_t_with_peer(uint32_t *array, uint32_t *peer, uint16_t num, int64_t range)
{
    uint32_t r = fast_rand_next();
    r ^= r >> 16;

    uint32_t idx;

    /* 离散集合：array[i] <-> peer[i] 强绑定 */
    if (likely(num)) {
        if ((num & (num - 1)) == 0) {
            idx = r & (num - 1);
		} else {
            idx = r % num;
		}

        return ((uint64_t)peer[idx] << 32) | array[idx];
    }

	/* TODO range */
    /* 连续区间 */
    if (unlikely(range == 0 || range == 1))
        return ((uint64_t)peer[0] << 32) | array[0];

    uint32_t abs_range = (range >= 0) ? range : -range;
    idx = r % abs_range;
	uint32_t ipv4 = array[0];
    uint32_t ip = (range >= 0) ?  (ipv4 + idx) : (ipv4 - idx);
    uint32_t vni = peer[0] + idx;

    return ((uint64_t)vni << 32) | ip;
}

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
	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}

	return (uint16_t)(~sum);
}
