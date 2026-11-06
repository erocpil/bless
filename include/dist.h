#ifndef __BLESS_DIST_H__
#define __BLESS_DIST_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of categories supported by distribute() on-stack arrays.
 *  Tied to BLESS_MAX_TYPES (32) with generous headroom. */
#define DIST_MAX_N  64

/** Error codes returned by distribute(). */
enum {
	DIST_OK           =  0,   /**< success */
	DIST_ERR_NULL     = -1,   /**< NULL weights or result pointer */
	DIST_ERR_ZERO_N   = -2,   /**< n == 0 */
	DIST_ERR_TOTAL_SMALL = -3,/**< total < n (cannot give 1 per category) */
	DIST_ERR_ZERO_SUM = -4,   /**< weight sum is zero */
	DIST_ERR_OVERFLOW = -5,   /**< weight × remaining overflows uint64_t */
	DIST_ERR_N_TOO_LARGE = -6,/**< n > DIST_MAX_N */
};

/** Weighted-proportional distribution with largest-remainder tie-breaking.
 *
 *  Distributes ``total`` units among ``n`` categories proportionally to
 *  ``weights[]`` using pure integer arithmetic.  Each category receives at
 *  least 1 unit (caller must ensure total >= n).  Remaining units after
 *  integer truncation are assigned to the categories with the largest
 *  integer remainders (Hamilton largest-remainder method).
 *
 *  Implementation is deterministic: two calls with identical inputs
 *  produce identical outputs.  No heap allocation — scratch arrays live
 *  on the stack (n ≤ DIST_MAX_N, fixed at 64).
 *
 *  @param weights    Read-only array of n weights (uint32_t, may be zero)
 *  @param n          Number of categories (1 ≤ n ≤ DIST_MAX_N)
 *  @param total      Total units to distribute (must be ≥ n)
 *  @param result[out] Pre-allocated array of n uint64_t results
 *  @return 0 on success, negative error code on failure (see enum above) */
int distribute(const uint32_t *weights, uint32_t n, uint64_t total,
	       uint64_t *result);

/** Round ``n`` up to the next power of 2.
 *
 *  Returns 0 when n == 0, or when n > 2³¹ (overflow — result would
 *  exceed UINT_MAX).  Callers should check for 0 return and treat it as
 *  an error when the input is expected to be non-zero. */
unsigned int make_power_of_2(unsigned int n);

#ifdef __cplusplus
}
#endif

#endif /* __BLESS_DIST_H__ */
