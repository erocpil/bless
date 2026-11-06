#ifndef BLESS_ENTROPY_SAMPLER_POLICY_H
#define BLESS_ENTROPY_SAMPLER_POLICY_H

#include <math.h>
#include <stdint.h>

#define ENTROPY_FLOW_HLL_BITS 12U
#define ENTROPY_FLOW_HLL_REGISTERS (1U << ENTROPY_FLOW_HLL_BITS)

/* SplitMix64 finalizer. It maps consecutive packet sequence numbers to a
 * stable pseudo-random order without keeping mutable RNG state. */
static inline uint64_t
entropy_sampler_mix64(uint64_t x)
{
	x ^= x >> 30;
	x *= UINT64_C(0xbf58476d1ce4e5b9);
	x ^= x >> 27;
	x *= UINT64_C(0x94d049bb133111eb);
	return x ^ (x >> 31);
}

static inline int
entropy_sampler_select(uint64_t sequence, uint64_t seed, uint32_t interval)
{
	return interval != 0 &&
		entropy_sampler_mix64(sequence ^ seed) % interval == 0;
}

static inline uint8_t
entropy_flow_hll_rank(uint64_t hash)
{
	uint64_t tail = hash >> ENTROPY_FLOW_HLL_BITS;
	return tail ? (uint8_t)(__builtin_clzll(tail) -
		(ENTROPY_FLOW_HLL_BITS - 1U))
		: (uint8_t)(65U - ENTROPY_FLOW_HLL_BITS);
}

static inline double
entropy_flow_hll_estimate(const uint8_t *registers)
{
	double m = ENTROPY_FLOW_HLL_REGISTERS;
	double sum = 0.0;
	unsigned zero = 0;

	for (unsigned i = 0; i < ENTROPY_FLOW_HLL_REGISTERS; i++) {
		sum += ldexp(1.0, -(int)registers[i]);
		if (!registers[i]) {
			zero++;
		}
	}

	if (zero) {
		return m * log(m / (double)zero);
	}
	return 0.7213 / (1.0 + 1.079 / m) * m * m / sum;
}

static inline double
entropy_flow_hll_bounded_estimate(const uint8_t *registers, uint64_t total)
{
	double estimate = entropy_flow_hll_estimate(registers);
	return estimate < (double)total ? estimate : (double)total;
}

/* Select the newest readable ring window. Unsigned subtraction preserves the
 * distance when the 32-bit producer index wraps. */
static inline uint32_t
entropy_sampler_read_window(uint32_t write_idx, uint32_t *first_idx,
			    uint32_t capacity, uint64_t *overwritten)
{
	uint32_t available = write_idx - *first_idx;
	*overwritten = 0;
	if (available > capacity) {
		*overwritten = (uint64_t)available - capacity;
		*first_idx = write_idx - capacity;
		available = capacity;
	}
	return available;
}

#endif
