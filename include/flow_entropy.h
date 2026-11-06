#ifndef __FLOW_ENTROPY_H__
#define __FLOW_ENTROPY_H__

#include <stdint.h>
#include <string.h>
#include <math.h>

#include <rte_cycles.h>
#include <rte_lcore.h>

/**
 * Per-flow entropy: samples connection lifecycle events from the
 * handshake hash table, then computes Shannon entropy over:
 *   - Flow 5-tuple distribution
 *   - Connection lifetime distribution
 *   - Event type distribution (CREATED / ESTABLISHED / TIMEOUT / RST)
 *
 * Flow events are rate-limited via sample_interval (sample every Nth
 * event), mirroring the packet-level entropy_sampler.
 *
 * Naming: flow_entropy_* / flow_sampler_* prefix, matching the module
 * convention established by entropy.h.
 */

/* ring buffer */
#define FLOW_RING_SIZE  4096

/** A single flow lifecycle event sample. */
struct flow_sample {
	uint32_t src_ip;
	uint32_t dst_ip;
	uint16_t src_port;
	uint16_t dst_port;
	uint8_t  event;          /* 0=CREATED, 1=ESTABLISHED, 2=TIMEOUT, 3=RST */
	uint8_t  pad[3];
	uint64_t lifetime_tsc;   /* TSC since creation (0 for CREATED event) */
} __attribute__((packed));

/** Per-worker flow sampler ring buffer. */
struct flow_sampler {
	struct flow_sample ring[FLOW_RING_SIZE] __rte_cache_aligned;
	volatile uint32_t write_idx;
	uint32_t seen;
	uint32_t sample_interval;
	uint32_t last_read_idx;
} __rte_cache_aligned;

/* hot-path helpers */
/** Initialise a flow sampler.  Default: sample every 4th event. */
static inline void
flow_sampler_init(struct flow_sampler *fs, uint32_t interval)
{
	memset(fs, 0, sizeof(*fs));
	fs->sample_interval = interval ? interval : 4;
}

/** Record one flow lifecycle event.  Returns 1 if sampled, 0 if skipped. */
static inline int
flow_sampler_record(struct flow_sampler *fs,
		    uint32_t src_ip, uint32_t dst_ip,
		    uint16_t src_port, uint16_t dst_port,
		    uint8_t event, uint64_t lifetime_tsc)
{
	uint32_t seen = __atomic_fetch_add(&fs->seen, 1,
					  __ATOMIC_RELAXED);
	if ((seen % fs->sample_interval) != 0) {
		return 0;
	}

	uint32_t idx = __atomic_load_n(&fs->write_idx, __ATOMIC_RELAXED);
	uint32_t slot = idx % FLOW_RING_SIZE;
	fs->ring[slot].src_ip       = src_ip;
	fs->ring[slot].dst_ip       = dst_ip;
	fs->ring[slot].src_port     = src_port;
	fs->ring[slot].dst_port     = dst_port;
	fs->ring[slot].event        = event;
	fs->ring[slot].lifetime_tsc = lifetime_tsc;
	__atomic_store_n(&fs->write_idx, idx + 1, __ATOMIC_RELEASE);
	return 1;
}

/* stats path (non-inline, defined in .c) */
/**
 * Compute flow-level entropy stats from all workers' flow samplers.
 * Called from the master lcore during stats generation.
 * Fills:
 *   s->flow_entropy_5tuple
 *   s->flow_entropy_lifetime
 *   s->flow_entropy_event
 *   s->flow_count
 */
void flow_entropy_compute(struct stats_snapshot *s);

#endif /* __FLOW_ENTROPY_H__ */
