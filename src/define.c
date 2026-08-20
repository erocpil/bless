#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <pthread.h>
#include <stdatomic.h>
#include "define.h"

/*
 * xorshift64* -- period 2^64-1, passes BigCrush (PractRand).
 * ~3 cycles/byte, comparable to LCG speed but much higher quality.
 * Source: Vigna, "An experimental exploration of Marsaglia's xorshift generators", 2016.
 *
 * NOTE: seed MUST be non-zero or the generator produces all zeros.
 * Initialised lazily with rdtsc for a per-core pseudo-random seed.
 */
static __thread uint64_t rand_s = 0;
static __thread int rand_seeded = 0;

/* Master seed set by fast_rand_set_seed().  0 = auto (rdtsc-based). */
static _Atomic uint64_t g_master_seed = 0;

static inline void fast_rand_init(void)
{
	uint64_t master_seed = atomic_load_explicit(&g_master_seed,
						    memory_order_relaxed);
	if (master_seed) {
		/* Deterministic: derive per-core seed from master_seed +
		 * rte_lcore_id().  Same seed + same topology = same traffic. */
		unsigned lcore = rte_lcore_id();
		rand_s = master_seed + (lcore != (unsigned)LCORE_ID_ANY ? lcore : 0);
		if (!rand_s) {
			rand_s = 1;
		}
	} else {
		rand_s = rte_get_tsc_cycles() ^ (uint64_t)(uintptr_t)pthread_self();
		if (!rand_s) {
			rand_s = 1;
		}
	}
	rand_seeded = 1;
}

void fast_rand_set_seed(uint64_t master_seed)
{
	atomic_store_explicit(&g_master_seed, master_seed, memory_order_relaxed);
}

uint64_t fast_rand_get_seed(void)
{
	return atomic_load_explicit(&g_master_seed, memory_order_relaxed);
}

void fast_rand_reseed(void)
{
	/* If no master seed is set (auto-seed mode), leave the per-core
	 * state alone -- each core already has its own rdtsc-based seed. */
	if (atomic_load_explicit(&g_master_seed, memory_order_relaxed)) {
		rand_seeded = 0;
	}
}

uint32_t fast_rand_next(void)
{
	if (unlikely(!rand_seeded)) {
		fast_rand_init();
	}
	uint64_t x = rand_s;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	rand_s = x;
	return (uint32_t)(x * 0x2545F4914F6CDD1DULL);
}

uint16_t random_array_elem_uint16_t(uint16_t *array, uint16_t num, int32_t range)
{
	uint32_t rs = fast_rand_next();
	uint16_t r = rs ^ (rs >> 16);

	/* Discrete set */
	if (likely(num)) {
		if ((num & (num - 1)) == 0) {
			return array[r & (num - 1)];
		}
		return array[r % num];
	}

	/* Continuous range */
	if (unlikely(range == 0 || range == 1)) {
		return array[0];
	}

	int32_t abs_range = (range >= 0) ? range : -range;
	if (abs_range == 0) {
		return array[0]; /* safety: unreachable (guarded above), satisfies static analysis */
	}
	uint16_t off = r % abs_range;
	uint16_t port = array[0];

	return (range > 0) ?  (uint16_t)(port + off) : (uint16_t)(port - off);
}

uint32_t random_array_elem_uint32_t(uint32_t *array, uint16_t num, int64_t range)
{
	uint32_t r = fast_rand_next();
	r ^= r >> 16;

	/* Discrete set */
	if (likely(num)) {
		if ((num & (num - 1)) == 0) {
			return array[r & (num - 1)];
		}
		return array[r % num];
	}

	/* Continuous range */
	if (unlikely(range == 0 || range == 1)) {
		return array[0];
	}

	int64_t abs_range = (range >= 0) ? range : -range;
	if (abs_range == 0) {
		return array[0]; /* safety: unreachable (guarded above), satisfies static analysis */
	}
	uint32_t off = r % abs_range;
	uint32_t ipv4 = rte_be_to_cpu_32(array[0]);

	return rte_cpu_to_be_32((range > 0) ?  (uint32_t)(ipv4 + off) : (uint32_t)(ipv4 - off));
}

/* Select an IPv6 address from a discrete array or a consecutive range.
 * Range arithmetic is performed on the wire-order byte representation so
 * carries propagate across the full 128-bit address. */
void random_array_elem_ipv6(uint8_t out[16], const uint8_t array[][16],
		uint16_t num, int64_t range)
{
	uint32_t r = fast_rand_next();

	if (likely(num)) {
		uint16_t index = (num & (num - 1)) == 0
			? (uint16_t)(r & (num - 1)) : (uint16_t)(r % num);
		memcpy(out, array[index], 16);
		return;
	}

	memcpy(out, array[0], 16);
	if (range == 0 || range == 1) {
		return;
	}

	uint64_t magnitude = range < 0 ? -(uint64_t)range : (uint64_t)range;
	// cppcheck-suppress zerodivcond
	uint64_t offset = (uint64_t)r % magnitude;
	if (offset == 0) {
		return;
	}

	if (range > 0) {
		for (int i = 15; i >= 0 && offset; i--) {
			uint64_t sum = (uint64_t)out[i] + (offset & 0xffU);
			out[i] = (uint8_t)sum;
			offset = (offset >> 8) + (sum >> 8);
		}
	} else {
		for (int i = 15; i >= 0 && offset; i--) {
			uint64_t sub = offset & 0xffU;
			uint64_t borrow = out[i] < sub;
			out[i] = (uint8_t)(out[i] - sub);
			offset = (offset >> 8) + borrow;
		}
	}
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

    /* Discrete set: array[i] <-> peer[i] strong binding */
    if (likely(num)) {
        if ((num & (num - 1)) == 0) {
            idx = r & (num - 1);
		} else {
            idx = r % num;
		}

        return ((uint64_t)peer[idx] << 32) | array[idx];
    }

	/* Continuous range: base + offset, network-to-host for arithmetic */
    if (unlikely(range == 0 || range == 1)) {
	    return ((uint64_t)peer[0] << 32) | array[0];
    }

	uint32_t abs_range = (range >= 0) ? range : -range;
	if (abs_range == 0) {
		return ((uint64_t)peer[0] << 32) | array[0]; /* safety: unreachable, satisfies static analysis */
	}
	idx = r % abs_range;
	uint32_t ipv4 = rte_be_to_cpu_32(array[0]);
	uint32_t ip = (range >= 0) ?  (ipv4 + idx) : (ipv4 - idx);
	uint32_t vni = peer[0] + idx;

	return ((uint64_t)vni << 32) | rte_cpu_to_be_32(ip);
}

/* Compute 16-bit one's complement checksum */
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
