#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "timing_policy.h"
#include "rate_psd.h"

static int passed;
static int failed;

#define CHECK(name, expr) do { \
	if (expr) { passed++; } \
	else { fprintf(stderr, "FAIL: %s\n", name); failed++; } \
} while (0)

int main(void)
{
	uint64_t bins[PACING_HIST_BUCKETS] = {0};
	CHECK("disabled pacing has no deadline",
	      timing_next_deadline(100, 200, 0) == 0);
	CHECK("first deadline starts from now",
	      timing_next_deadline(0, 1000, 250) == 1250);
	CHECK("future schedule remains absolute",
	      timing_next_deadline(1200, 1000, 250) == 1450);
	CHECK("small miss preserves absolute schedule",
	      timing_next_deadline(900, 1000, 250) == 1150);
	CHECK("stale schedule rebases on now",
	      timing_next_deadline(700, 1000, 250) == 1250);

	CHECK("no limits means immediate",
	      timing_effective_interval_us(0, 0, 64) == 0);
	CHECK("batch delay alone",
	      timing_effective_interval_us(1000, 0, 64) == 1000);
	CHECK("pps rate alone",
	      timing_effective_interval_us(0, 64000, 64) == 1000);
	CHECK("slower batch delay wins",
	      timing_effective_interval_us(2000, 64000, 64) == 2000);
	CHECK("slower pps interval wins",
	      timing_effective_interval_us(500, 32000, 64) == 2000);
	CHECK("PSD samples at 10 kHz", PSD_SAMPLE_HZ == 10000);
	CHECK("PSD resolves a 1 ms cycle below Nyquist",
	      1000 < PSD_SAMPLE_HZ / 2);

	CHECK("fine histogram keeps one-us buckets",
	      timing_histogram_bucket(42 * 2500, 2500) == 42);
	CHECK("middle histogram keeps eight-us buckets",
	      timing_histogram_bucket(300 * 2500, 2500) == 261);
	CHECK("large values use logarithmic buckets",
	      timing_histogram_bucket(4096 * 2500, 2500) ==
	      PACING_HIST_FINE_US + PACING_HIST_MID_BUCKETS + 2);
	bins[timing_histogram_bucket(15 * 2500, 2500)] = 50;
	bins[timing_histogram_bucket(136 * 2500, 2500)] = 50;
	CHECK("histogram p50 has microsecond resolution",
	      timing_histogram_percentile(bins, 100, 50, 2500,
		300 * 2500) == 16 * 2500 - 1);
	CHECK("histogram p99 is capped by observed max",
	      timing_histogram_percentile(bins, 100, 99, 2500,
		136 * 2500) == 136 * 2500);
	CHECK("empty histogram percentile is zero",
	      timing_histogram_percentile(bins, 0, 99, 2500, 171) == 0);

	printf("timing policy: %d passed, %d failed\n", passed, failed);
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
