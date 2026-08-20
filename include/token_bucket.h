#ifndef __TOKEN_BUCKET_H__
#define __TOKEN_BUCKET_H__

#include <stdint.h>
#include <rte_cycles.h>

/**
 * Token bucket rate limiter.
 *
 * Three-layer rate control for bless:
 *   - PPS bucket: packets per second
 *   - BPS bucket: bytes per second
 *   - CPS bucket: handshake SYNs per second
 *
 * Each worker has its own set of buckets.  Refill happens inline on the
 * TX hot path using TSC for precise sub-second accounting.
 *
 * A bucket is disabled when cir == 0 (token_bucket_available returns
 * UINT64_MAX and consume is a no-op).
 */

struct token_bucket {
	uint64_t cir;           /* committed info rate (0 = disabled) */
	uint64_t cbs;           /* committed burst size (max tokens) */
	uint64_t tokens;        /* current token count */
	uint64_t last_tsc;      /* last refill TSC */
	uint64_t timer_hz;      /* cached TSC frequency */
};

/**
 * Initialise a token bucket.
 * Call once per worker during init.  Bucket starts full (cbs tokens).
 */
static inline void
token_bucket_init(struct token_bucket *tb, uint64_t cir, uint64_t cbs)
{
	tb->cir      = cir;
	tb->cbs      = cbs;
	tb->tokens   = cbs;
	tb->last_tsc = rte_rdtsc();
	tb->timer_hz = rte_get_timer_hz();
}

/**
 * Refill and return available tokens.
 *
 * Returns UINT64_MAX when the bucket is disabled (cir == 0), so callers
 * can MIN() the result directly without a branch:
 *   nb = MIN(nb, token_bucket_available(&tb));
 *
 * Safe against 64-bit overflow: the product cir * elapsed is checked
 * against UINT64_MAX before multiplication.
 */
static inline uint64_t
token_bucket_available(struct token_bucket *tb)
{
	if (tb->cir == 0) {
		return UINT64_MAX;
	}

	uint64_t now = rte_rdtsc();
	uint64_t elapsed = now - tb->last_tsc;

	if (elapsed > 0) {
		uint64_t refill;
		if (tb->cir > (UINT64_MAX / elapsed)) {
			refill = tb->cbs;          /* clamp to burst */
		} else {
			refill = (tb->cir * elapsed) / tb->timer_hz;
		}

		if (refill > 0) {
			tb->last_tsc = now;
			uint64_t new_tokens = tb->tokens + refill;
			tb->tokens = (new_tokens > tb->cbs)
				? tb->cbs : new_tokens;
		}
	}

	return tb->tokens;
}

/**
 * Consume tokens after a successful send.
 * No-op when bucket is disabled.
 */
static inline void
token_bucket_consume(struct token_bucket *tb, uint64_t n)
{
	if (tb->cir > 0) {
		if (n >= tb->tokens) {
			tb->tokens = 0;
		} else {
			tb->tokens -= n;
		}
	}
}

/**
 * Change CIR at runtime.  Resets last_tsc to avoid an immediate windfall.
 */
static inline void
token_bucket_set_rate(struct token_bucket *tb, uint64_t cir)
{
	tb->cir      = cir;
	tb->last_tsc = rte_rdtsc();
	if (tb->tokens > tb->cbs) {
		tb->tokens = tb->cbs;
	}
}

#endif /* __TOKEN_BUCKET_H__ */
