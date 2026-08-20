/* SPDX-License-Identifier: BSD-3-Clause */

#include "rate_psd.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
	struct rate_psd psd;
	struct rate_psd peer;

	rate_psd_init(&psd);
	assert(rate_psd_read_bucket(&psd, 10) == 0);

	rate_psd_account_bucket(&psd, 12, 10);
	rate_psd_account_bucket(&psd, 5, 10);
	assert(rate_psd_read_bucket(&psd, 10) == 17);
	assert(rate_psd_read_bucket(&psd, 11) == 0);

	/* Reusing a ring slot replaces the old epoch and count. */
	rate_psd_account_bucket(&psd, 3, 10 + PSD_RING_SIZE);
	assert(rate_psd_read_bucket(&psd, 10) == 0);
	assert(rate_psd_read_bucket(&psd, 10 + PSD_RING_SIZE) == 3);

	/* Missing epochs read as idle buckets without explicit zero filling. */
	assert(rate_psd_read_bucket(&psd, 4000) == 0);

	/* Workers are merged by absolute epoch, not by ring-head position. */
	rate_psd_init(&psd);
	rate_psd_init(&peer);
	rate_psd_account_bucket(&psd, 10, 100);
	rate_psd_account_bucket(&psd, 20, 102);
	rate_psd_account_bucket(&peer, 3, 101);
	rate_psd_account_bucket(&peer, 4, 102);
	const struct rate_psd *workers[] = { &psd, &peer };
	uint64_t merged[4];
	rate_psd_merge_window(workers, 2, 103, merged, 4);
	assert(merged[0] == 10);
	assert(merged[1] == 3);
	assert(merged[2] == 24);
	assert(merged[3] == 0);

	double power[128] = {0};
	unsigned strongest, fundamental;
	power[26] = 60.0;
	power[51] = 100.0;
	power[77] = 40.0;
	rate_psd_find_peak_bins(power, 1, 128, &strongest, &fundamental);
	assert(strongest == 51);
	assert(fundamental == 26);

	memset(power, 0, sizeof(power));
	power[43] = 100.0;
	rate_psd_find_peak_bins(power, 1, 128, &strongest, &fundamental);
	assert(strongest == 43);
	assert(fundamental == 43);

	puts("rate PSD absolute buckets: PASS");
	return 0;
}
