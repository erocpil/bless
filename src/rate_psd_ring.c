/* SPDX-License-Identifier: BSD-3-Clause */

#include "rate_psd.h"

#include <string.h>

void
rate_psd_init(struct rate_psd *psd)
{
	memset(psd, 0, sizeof(*psd));
	for (unsigned i = 0; i < PSD_RING_SIZE; i++) {
		atomic_init(&psd->ring[i].count, 0);
		atomic_init(&psd->ring[i].bucket, UINT64_MAX);
	}
}

void
rate_psd_account_bucket(struct rate_psd *psd, uint64_t count, uint64_t bucket)
{
	struct rate_psd_slot *slot = &psd->ring[bucket % PSD_RING_SIZE];
	uint64_t published = atomic_load_explicit(&slot->bucket,
		memory_order_relaxed);

	if (published == bucket) {
		atomic_fetch_add_explicit(&slot->count, count, memory_order_relaxed);
		return;
	}

	/* Invalidate before replacing a wrapped slot so a concurrent reader cannot
	 * pair the old bucket number with the new count. */
	atomic_store_explicit(&slot->bucket, UINT64_MAX, memory_order_release);
	atomic_store_explicit(&slot->count, count, memory_order_relaxed);
	atomic_store_explicit(&slot->bucket, bucket, memory_order_release);
}

uint64_t
rate_psd_read_bucket(const struct rate_psd *psd, uint64_t bucket)
{
	const struct rate_psd_slot *slot = &psd->ring[bucket % PSD_RING_SIZE];
	uint64_t before = atomic_load_explicit(&slot->bucket,
		memory_order_acquire);
	if (before != bucket) {
		return 0;
	}

	uint64_t count = atomic_load_explicit(&slot->count, memory_order_relaxed);
	uint64_t after = atomic_load_explicit(&slot->bucket, memory_order_acquire);
	return after == bucket ? count : 0;
}

void
rate_psd_merge_window(const struct rate_psd *const *samplers,
		      unsigned n_samplers, uint64_t last_complete,
		      uint64_t *merged, unsigned window)
{
	if (window > PSD_RING_SIZE) {
		window = PSD_RING_SIZE;
	}
	memset(merged, 0, (size_t)window * sizeof(*merged));

	for (unsigned worker = 0; worker < n_samplers; worker++) {
		const struct rate_psd *psd = samplers[worker];
		if (!psd) {
			continue;
		}
		for (unsigned i = 0; i < window; i++) {
			uint64_t age = (uint64_t)(window - 1 - i);
			if (last_complete >= age) {
				merged[i] += rate_psd_read_bucket(psd,
								  last_complete - age);
			}
		}
	}
}

void
rate_psd_find_peak_bins(const double *power, unsigned first_bin,
			unsigned end_bin, unsigned *strongest_bin,
			unsigned *fundamental_bin)
{
	double max_power = 0.0;
	unsigned strongest = 0;

	for (unsigned i = first_bin; i < end_bin; i++) {
		if (power[i] > max_power) {
			max_power = power[i];
			strongest = i;
		}
	}

	unsigned fundamental = strongest;
	double threshold = max_power * 0.05;
	for (unsigned base = first_bin; max_power > 0.0 && base < end_bin; base++) {
		double left = base > first_bin ? power[base - 1] : 0.0;
		double right = base + 1 < end_bin ? power[base + 1] : 0.0;
		if (power[base] < threshold || power[base] < left || power[base] < right) {
			continue;
		}

		int harmonic_found = 0;
		for (unsigned multiple = 2; multiple * base < end_bin; multiple++) {
			unsigned expected = multiple * base;
			unsigned lo = expected > first_bin ? expected - 1 : expected;
			unsigned hi = expected + 1 < end_bin ? expected + 1 : end_bin - 1;
			for (unsigned bin = lo; bin <= hi; bin++) {
				double bin_left = bin > first_bin ? power[bin - 1] : 0.0;
				double bin_right = bin + 1 < end_bin ? power[bin + 1] : 0.0;
				if (power[bin] >= threshold && power[bin] >= bin_left &&
				    power[bin] >= bin_right) {
					harmonic_found = 1;
					break;
				}
			}
			if (harmonic_found) {
				break;
			}
		}
		if (harmonic_found) {
			fundamental = base;
			break;
		}
	}

	*strongest_bin = strongest;
	*fundamental_bin = fundamental;
}
