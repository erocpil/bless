/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * rate_psd.c -- per-worker rate PSD (Power Spectral Density) sampler.
 *
 * Each worker accumulates per-100-microsecond TX packet counts into a
 * 512-slot ring buffer.  The master stats thread drains all workers,
 * merges the time series, applies a 256-point Hann-windowed
 * Cooley-Tukey FFT, and reports the dominant frequency and spectral
 * flatness.
 *
 * This is the beta metric of the frequency-domain observability
 * pipeline -- it detects periodic structure in the TX rate (DUT
 * rate-limiter cycles, token-bucket fill periods, or unintended
 * traffic-construction artefacts).
 */

#include "rate_psd.h"
#include "server.h"   /* struct stats_snapshot */

#include <rte_lcore.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_branch_prediction.h>

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Global registry -- populated by worker_loop(), consumed by master  */
/* ------------------------------------------------------------------ */

struct rate_psd *rate_psd_samplers[RTE_MAX_LCORE];

/* ------------------------------------------------------------------ */
/*  ring-buffer accounting (worker hot path)                           */
/* ------------------------------------------------------------------ */

void
rate_psd_account(struct rate_psd *psd, uint64_t count, uint64_t tsc)
{
	uint64_t bucket_cycles = rte_get_tsc_hz() * PSD_BUCKET_US / 1000000;
	if (unlikely(!bucket_cycles)) {
		bucket_cycles = 1;
	}
	rate_psd_account_bucket(psd, count, tsc / bucket_cycles);
}

/* ------------------------------------------------------------------ */
/*  Cooley-Tukey radix-2 DIT FFT (in-place complex)                   */
/* ------------------------------------------------------------------ */

/**
 * 256-point DIT FFT.
 *
 * Transforms PSD_FFT_SIZE real samples (with zeroed imaginary parts)
 * into complex frequency bins.  The output is in natural order
 * (bit-reversal undone at the end).
 *
 * @param real  [in/out] real parts, size PSD_FFT_SIZE
 * @param imag  [in/out] imag parts, size PSD_FFT_SIZE
 */
static void
fft_256(double *real, double *imag)
{
	const int n = PSD_FFT_SIZE;

	/* --- bit-reversal permutation --- */
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; (j & bit) != 0; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			double tr = real[i];
			double ti = imag[i];
			real[i] = real[j];
			imag[i] = imag[j];
			real[j] = tr;
			imag[j] = ti;
		}
	}

	/* --- Danielson-Lanczos butterflies --- */
	for (int len = 2; len <= n; len <<= 1) {
		double ang = 2.0 * M_PI / (double)len;
		double wlen_r = cos(ang);
		double wlen_i = -sin(ang);

		for (int i = 0; i < n; i += len) {
			double wr = 1.0;
			double wi = 0.0;

			for (int j = 0; j < len / 2; j++) {
				int u = i + j;
				int v = u + len / 2;

				/* t = w * x[v] */
				double tr = wr * real[v] - wi * imag[v];
				double ti = wr * imag[v] + wi * real[v];

				/* butterfly */
				real[v] = real[u] - tr;
				imag[v] = imag[u] - ti;
				real[u] += tr;
				imag[u] += ti;

				/* advance twiddle */
				double wr_next = wr * wlen_r - wi * wlen_i;
				wi = wr * wlen_i + wi * wlen_r;
				wr = wr_next;
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/*  master-side compute + fill                                        */
/* ------------------------------------------------------------------ */

/**
 * Static cache for the most recent PSD computation.
 *
 * rate_psd_compute() writes these; rate_psd_fill_snapshot() copies
 * them out.  One writer (stats thread), one reader (stats thread) --
 * no synchronisation needed.
 */
static double  cached_psd_bins[PSD_FFT_SIZE];
static double  cached_dominant_hz;
static double  cached_strongest_peak_hz;
static double  cached_fundamental_hz;
static double  cached_spectral_flatness;
static double  cached_mean_ppms;
static double  cached_variation_rms_ppms;
static int     cached_signal_valid;

void
rate_psd_compute(void)
{
	double merged[PSD_RING_SIZE];
	uint64_t merged_counts[PSD_RING_SIZE];
	const struct rate_psd *active[RTE_MAX_LCORE];
	unsigned n_active = 0;
	uint64_t bucket_cycles = rte_get_tsc_hz() * PSD_BUCKET_US / 1000000;
	if (unlikely(!bucket_cycles)) {
		bucket_cycles = 1;
	}
	uint64_t now_bucket = rte_get_tsc_cycles() / bucket_cycles;
	uint64_t last_complete = now_bucket ? now_bucket - 1 : 0;

	unsigned main_lc = rte_get_main_lcore();

	for (unsigned lc = 0; lc < RTE_MAX_LCORE; lc++) {
		struct rate_psd *psd = rate_psd_samplers[lc];

		if (psd == NULL || lc == main_lc) {
			continue;
		}
		active[n_active++] = psd;
	}
	rate_psd_merge_window(active, n_active, last_complete, merged_counts,
		PSD_RING_SIZE);
	for (unsigned i = 0; i < PSD_RING_SIZE; i++)
		merged[i] = (double)merged_counts[i];

	/* --- extract last 256 samples, remove DC, and apply Hann window --- */
	double real[PSD_FFT_SIZE];
	double imag[PSD_FFT_SIZE];
	double mean = 0.0;

	for (int i = 0; i < PSD_FFT_SIZE; i++)
		mean += merged[PSD_RING_SIZE - PSD_FFT_SIZE + i];
	mean /= (double)PSD_FFT_SIZE;
	/* Preserve the public packets/ms (Kpps) unit after moving the internal
	 * sampler from 1 ms to 100 us buckets. */
	const double buckets_per_ms = 1000.0 / (double)PSD_BUCKET_US;
	cached_mean_ppms = mean * buckets_per_ms;
	double variation_sum_sq = 0.0;

	for (int i = 0; i < PSD_FFT_SIZE; i++) {
		/* most recent 25.6 ms */
		double val = merged[PSD_RING_SIZE - PSD_FFT_SIZE + i] - mean;
		variation_sum_sq += val * val;

		/* Hann window: w[n] = 0.5 * (1 - cos(2*pi*n/(N-1))) */
		double w = 0.5 * (1.0 - cos(2.0 * M_PI * (double)i
					       / (double)(PSD_FFT_SIZE - 1)));

		real[i] = val * w;
		imag[i] = 0.0;
	}
	cached_variation_rms_ppms =
		sqrt(variation_sum_sq / (double)PSD_FFT_SIZE) * buckets_per_ms;

	/* --- FFT --- */
	fft_256(real, imag);

	/* --- power spectrum + metrics --- */
	double max_power = 0.0;
	double sum_power = 0.0;
	double sum_log   = 0.0;
	const int first_bin = 1; /* DC is mean TX rate, not periodic variation. */
	const int end_bin = PSD_FFT_SIZE / 2; /* exclude the Nyquist bin */

	memset(cached_psd_bins, 0, sizeof(cached_psd_bins));

	for (int i = first_bin; i < end_bin; i++) {
		double power = real[i] * real[i] + imag[i] * imag[i];

		cached_psd_bins[i] = power;
		sum_power += power;

		if (power > max_power) {
			max_power = power;
		}
	}

	/*
	 * Dominant frequency: bin index * bin width.
	 * sample rate = 10000 Hz (100 us sampling)
	 * bin width   = 10000 / 256 = 39.0625 Hz
	 */
	unsigned strongest_bin = 0, fundamental_bin = 0;
	rate_psd_find_peak_bins(cached_psd_bins, first_bin, end_bin,
		&strongest_bin, &fundamental_bin);
	cached_strongest_peak_hz = max_power > 1e-15
		? (double)strongest_bin * (double)PSD_SAMPLE_HZ
			/ (double)PSD_FFT_SIZE : 0.0;
	cached_fundamental_hz = max_power > 1e-15
		? (double)fundamental_bin * (double)PSD_SAMPLE_HZ
			/ (double)PSD_FFT_SIZE : 0.0;
	cached_dominant_hz = cached_strongest_peak_hz;
	cached_signal_valid = sum_power > 1e-15;

	/*
	 * Spectral flatness (Wiener entropy):
	 *   flatness = exp(mean(log(P))) / mean(P)
	 *   0 = pure tone (all power in one bin)
	 *   1 = white noise (flat spectrum)
	 */
	const int analyzed_bins = end_bin - first_bin;
	if (sum_power > 1e-15 && max_power > 0.0) {
		/* A relative floor keeps log(0) finite without dropping quiet bins,
		 * which would artificially inflate flatness for sparse spectra. */
		double floor_power = max_power * 1e-12;
		for (int i = first_bin; i < end_bin; i++)
			sum_log += log(fmax(cached_psd_bins[i], floor_power));
		double geom_mean = exp(sum_log / (double)analyzed_bins);
		double arith_mean = sum_power / (double)analyzed_bins;
		cached_spectral_flatness = fmin(1.0, geom_mean / arith_mean);
	} else {
		cached_spectral_flatness = 0.0;
	}

}

void
rate_psd_fill_snapshot(struct stats_snapshot *s)
{
	s->psd_dominant_hz      = cached_dominant_hz;
	s->psd_strongest_peak_hz = cached_strongest_peak_hz;
	s->psd_fundamental_hz = cached_fundamental_hz;
	s->psd_spectral_flatness = cached_spectral_flatness;
	s->psd_mean_ppms = cached_mean_ppms;
	s->psd_variation_rms_ppms = cached_variation_rms_ppms;
	s->psd_signal_valid = cached_signal_valid;

	for (int i = 0; i < PSD_FFT_SIZE; i++)
		s->psd_bins[i] = cached_psd_bins[i];
}
