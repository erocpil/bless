#ifndef __DEFINE_H__
#define __DEFINE_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   // access()
#include <stdatomic.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <bits/socket.h>
#include <x86intrin.h>
#include <math.h>

#define RTE_LOGTYPE_BLESS RTE_LOGTYPE_USER1

#define MAX_PKT_BURST 64
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
#define MEMPOOL_CACHE_SIZE 256

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RX_DESC_DEFAULT 4096
#define TX_DESC_DEFAULT 4096

#define MAX_RX_QUEUE_PER_LCORE 512
#define MAX_TX_QUEUE_PER_PORT 512

#ifndef NELEMS
#define NELEMS(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define MBUF_DYNFIELDS_MAX  8

#ifndef max
#define max(a, b) (a) > (b) ? (a) : (b)
#endif

#ifndef min
#define min(a, b) (a) < (b) ? (a) : (b)
#endif

enum {
	STATE_INIT = 0,
	STATE_RUNNING = 1,
	STATE_STOPPED = 2,
	STATE_EXIT = 3,
};

typedef uint64_t (*mutation_func)(void **mbufs, unsigned int n, void *data);

#ifndef likely
#define likely(x)	__builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x)	__builtin_expect(!!(x), 0)
#endif

uint32_t fast_rand_next(void);

/**
 * Override the per-core PRNG seed with a user-provided value.
 * Call before any worker threads start (in parse_args or early in main).
 * Each core derives its seed as master_seed + lcore_id for determinism
 * across different runs with the same --seed value.
 * Pass 0 to restore auto-seeding (rdtsc-based).
 */
void fast_rand_set_seed(uint64_t master_seed);

/** Return the current master seed (0 = auto, non-zero = user override). */
uint64_t fast_rand_get_seed(void);

/** Re-derive per-core PRNG state from the master seed on next call to
 *  fast_rand_next().  Call from each worker after a runtime seed change
 *  (e.g. via WebSocket {"cmd":"set","key":"seed","value":12345}).
 *  No-op if g_master_seed == 0 (auto-seed mode). */
void fast_rand_reseed(void);

/* Compute 16-bit one's complement checksum */
uint16_t icmp_calc_cksum(const void *buf, size_t len);

/*
 * random_delay_jitter -- apply uniform ±jitter around base delay.
 * When jitter == 0, returns base unchanged. Thread-safe (per-lcore PRNG).
 */
static inline uint64_t random_delay_jitter(uint64_t base, uint64_t jitter)
{
	if (unlikely(!jitter || !base)) {
		return base;
	}
	uint64_t r = fast_rand_next();
	return base + (r % (2 * jitter + 1)) - jitter;
}

/*
 * exp_random -- exponential random delay using inverse transform sampling.
 * Returns mean_us * (-ln U) where U ~ (0, 1].
 * mean_us: mean delay in microseconds; 0 returns 0.
 */
static inline uint64_t exp_random(uint64_t mean_us)
{
	if (unlikely(!mean_us)) {
		return 0;
	}
	uint64_t r = fast_rand_next();
	double u = (r & 0x7FFFFFFF) / (double)(1u << 31);
	if (u < 1e-10) {
		u = 1e-10; /* avoid log(0) */
	}
	double delay = -(double)mean_us * log(u);
	return (uint64_t)(delay + 0.5);  /* round */
}

/**
 * pareto_random -- Pareto-distributed random value.
 * Returns scale / (U^(1/alpha)) where U ~ (0, 1].
 * scale: minimum value (x_m), alpha: shape parameter (> 0).
 * alpha < 2.0 gives infinite variance (heavy tail).
 * alpha <= 1.0 gives infinite mean (use with caution).
 */
static inline uint64_t pareto_random(uint64_t scale, double alpha)
{
	if (unlikely(!scale || alpha <= 0.0)) {
		return scale;
	}
	uint64_t r = fast_rand_next();
	double u = (r & 0x7FFFFFFF) / (double)(1u << 31);
	if (u < 1e-10) {
		u = 1e-10;
	}
	return (uint64_t)((double)scale / pow(u, 1.0 / alpha) + 0.5);
}

static inline uint64_t rdtsc_serialized()
{
	// _mm_lfence();
	return __rdtsc();
}

static inline uint8_t rdtsc8()
{
	return rdtsc_serialized();
}

static inline uint16_t rdtsc16()
{
	return rdtsc_serialized();
}

static inline uint32_t rdtsc32()
{
	return rdtsc_serialized();
}

static inline uint64_t rdtsc64()
{
	return rdtsc_serialized();
}

uint16_t random_array_elem_uint16_t(uint16_t *array, uint16_t num, int32_t range);
uint32_t random_array_elem_uint32_t(uint32_t *array, uint16_t num, int64_t range);
uint64_t random_array_elem_uint32_t_with_peer(uint32_t *array, uint32_t *peer, uint16_t num, int64_t range);
void random_array_elem_ipv6(uint8_t out[16], const uint8_t array[][16],
		uint16_t num, int64_t range);

#endif
