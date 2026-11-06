#ifndef __RATE_PSD_H__
#define __RATE_PSD_H__

/**
 * @file rate_psd.h
 * @brief Per-worker rate PSD (Power Spectral Density) sampling.
 *
 * Each worker records packet counts in absolute 100-microsecond TSC buckets.
 * The statistics thread aligns those buckets across workers before applying a
 * 256-point FFT and reporting the dominant frequency and spectral flatness.
 */

#include <stdint.h>
#include <stdatomic.h>

/* Forward declaration: struct stats_snapshot from server.h */
struct stats_snapshot;

/** Sampling period and rate. 100 us resolves pacing frequencies up to 5 kHz. */
#define PSD_BUCKET_US   100
#define PSD_SAMPLE_HZ   (1000000 / PSD_BUCKET_US)

/** 51.2 ms of packet counts at 100 us resolution. */
#define PSD_RING_SIZE  512

/** FFT window size: 256 points (25.6 ms), dyadic for Cooley-Tukey. */
#define PSD_FFT_SIZE   256

/**
 * Per-lcore rate sampler.
 *
 * Each slot is identified by its absolute bucket number. Writes are
 * single-writer; the bucket number release-publishes a newly reused slot.
 */
struct rate_psd_slot {
	atomic_uint_fast64_t count;
	atomic_uint_fast64_t bucket;
};

struct rate_psd {
	struct rate_psd_slot ring[PSD_RING_SIZE];
};

/* Bucket-level helpers are public so the alignment logic can be unit tested
 * without a live DPDK clock. */
void rate_psd_account_bucket(struct rate_psd *psd, uint64_t count,
			     uint64_t bucket);
uint64_t rate_psd_read_bucket(const struct rate_psd *psd, uint64_t bucket);
void rate_psd_merge_window(const struct rate_psd *const *samplers,
			   unsigned n_samplers, uint64_t last_complete,
			   uint64_t *merged, unsigned window);
void rate_psd_find_peak_bins(const double *power, unsigned first_bin,
			     unsigned end_bin, unsigned *strongest_bin,
			     unsigned *fundamental_bin);

/**
 * Record a TX burst on the hot path.
 *
 * Called by worker_func_tx_only / worker_func_fwd after every
 * rte_eth_tx_burst(). The TSC value selects a global 100 us bucket.
 *
 * @param psd    per-worker PSD state (non-NULL)
 * @param count  number of packets sent in this burst
 * @param tsc    current TSC value (rte_rdtsc())
 */
void rate_psd_account(struct rate_psd *psd, uint64_t count, uint64_t tsc);

/**
 * One-time initialisation (zeroes state, records init timestamp).
 */
void rate_psd_init(struct rate_psd *psd);

/**
 * Master-side: drain all registered workers, merge time series,
 * run 256-point Hann-windowed FFT, and cache results in static
 * storage for rate_psd_fill_snapshot().
 */
void rate_psd_compute(void);

/**
 * Copy the most recent PSD results into a stats snapshot.
 *
 * @param s  snapshot to fill (psd_dominant_hz, psd_spectral_flatness,
 *           psd_bins[256])
 */
void rate_psd_fill_snapshot(struct stats_snapshot *s);

/**
 * Per-lcore registry of active PSD samplers.
 *
 * Populated by worker_loop() during initialisation, consumed by
 * rate_psd_compute() in the stats (master) thread.  Slots belonging
 * to the main lcore or never-initialised lcores are NULL.
 */
extern struct rate_psd *rate_psd_samplers[];

#endif /* __RATE_PSD_H__ */
