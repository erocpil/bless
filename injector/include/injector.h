#ifndef __INJECTOR_H__
#define __INJECTOR_H__

#include <pthread.h>
// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
// #include <setjmp.h>
#include <stdarg.h>
// #include <ctype.h>
// #include <errno.h>
#include <getopt.h>
// #include <signal.h>
#include <stdbool.h>
#include <sys/param.h>
#include <stdatomic.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>

#include "injector_parse.h"
#include <rte_mbuf.h>
#include <rte_string_fns.h>

/* VXLAN outer encapsulation overhead: ETH(14) + IPv4(20) + UDP(8) + VNI(8) */
#define INJECTOR_SIZEOF_VXLAN (sizeof(struct rte_ether_hdr) + \
		sizeof(struct rte_ipv4_hdr) + \
		sizeof(struct rte_udp_hdr) + \
		8)

struct base;
#include "config.h"
#include "injector_plugin.h"



struct distribution {
	uint32_t pos;
	uint32_t size;
	uint32_t mask;
	uint32_t capacity;
	uint8_t data[0];
};

/* Built-in protocol type indices -- must match type_idx in proto_*.c */
enum INJECTOR_TYPE {
	TYPE_ARP = 0,
	TYPE_ICMP,
	TYPE_TCP,
	TYPE_UDP,
	TYPE_MAX,     /* # of built-in types = INJECTOR_NUM_BUILTIN */
};

#define OFFLOAD_IPV4(cnode) ((cnode)->offload & OF_IPV4_VAL)
#define OFFLOAD_TCP(cnode) ((cnode)->offload & OF_TCP_VAL)
#define OFFLOAD_UDP(cnode) ((cnode)->offload & OF_UDP_VAL)
#define OFFLOAD_OUTER_IPV4(cnode) ((cnode)->offload & OF_OUTER_IPV4_VAL)
#define OFFLOAD_OUTER_UDP(cnode) ((cnode)->offload & OF_OUTER_UDP_VAL)

struct dist_ratio {
	uint64_t num;
	int32_t weight[TYPE_MAX];
	int32_t quota[TYPE_MAX];
};

#ifndef DIST_RATIO_DUMP
#define DIST_RATIO_DUMP(r) \
	do { \
		typeof(r) _r = r; \
		printf("[%s %d] dist_ratio %p\n" \
				"   num %lu\n" \
				"   data [\n", \
				__func__, __LINE__, \
				_r, _r->num); \
		for (int i = 0; i < TYPE_MAX; i++) { \
			printf("    %d %s	%d %d -> %.2f\n", \
					i, injector_get_type_name(i), _r->weight[i], _r->weight[i], \
					_r->weight[i] > 0 ? (float)_r->weight[i] * 100 / _r->num : 0); \
		} \
		printf("   ]\n"); \
	} while (0)
#endif

struct mbuf_conf {
	union {
		struct rte_ether_addr dst_addr;
		uint8_t dst_addr_array[RTE_ETHER_ADDR_LEN];
	};
	union {
		struct rte_ether_addr src_addr;
		uint8_t src_addr_array[RTE_ETHER_ADDR_LEN];
	};
	uint32_t src_ip;
	uint32_t dst_ip;
	struct {
		unsigned char dst_addr[18];
		unsigned char src_addr[18];
		unsigned char src_ip[16];
		unsigned char dst_ip[16];
	} str;
	uint16_t src_port;
	uint16_t dst_port;

	/* for inner only */
	int64_t src_ip_range;
	int64_t dst_ip_range;
	int32_t src_port_range;
	int32_t dst_port_range;

	union {
		uint32_t ratio_vni;
		struct {
			uint32_t vni: 24;
			uint32_t ratio: 8;
		} fields;
	};
	union {
		uint16_t offset; /* outer eth + offset == inner eth */
		uint16_t range; /* vni range */
	};

	uint8_t *payload;
	uint16_t payload_len;
} __rte_aligned(1);

struct injector_encap_params {
	struct mbuf_conf *inner;
	struct mbuf_conf *outer;
};

struct injector_conf {
	struct base *base;
	uint32_t enabled_port_mask;
	atomic_int *state;
	pthread_barrier_t *barrier;
	Cnode *cnode;
	struct config_file_map *cfm;

	int64_t num;
	uint16_t batch;
	uint64_t batch_delay_us;
	uint64_t batch_jitter_us;  /* ±jitter applied to batch_delay_us */
	uint8_t traffic_model;      /* 0=uniform, 1=Poisson (exp inter-arrival), 2=Pareto ON-OFF */
	double   pareto_alpha;      /* Pareto shape parameter α (>0, typ 1.0-2.0 for heavy tail), model=2 only */
	uint8_t interleave;         /* 1=Fisher-Yates shuffle mbufs before tx_burst to break spatial locality */
	uint8_t interleave_depth;   /* shuffle ratio percentage (1-100, 100=full shuffle) */
	uint8_t  bench_mode;       /* 0=normal, 1=bench-template (copy pre-built pkt) */
	/* token bucket rate limits (0 = disabled) */
	uint32_t pps_rate;         /* PPS target */
	uint32_t pps_burst;        /* PPS burst size (0 = auto = batch * 4) */
	uint32_t bps_rate;         /* BPS target (bytes/sec) */
	uint32_t bps_burst;        /* BPS burst size (0 = auto = 65536) */
	struct distribution *dist;
	struct dist_ratio dist_ratio;

	struct injector_encap_params bep;
	// struct mbuf_conf mconf;
	int argc;
	char **argv;
};

#ifndef INJECTOR_CONF_DUMP
#define INJECTOR_CONF_DUMP(c) \
	do { \
		typeof(c) _c = c; \
		printf("injector_conf %p\n" \
				"   batch %u\n" \
				"   dist %p\n", \
				_c, _c->batch, _c->dist); \
	} while (0)
#endif

/** Print a MAC address in xx:xx:xx:xx:xx:xx format. */
void injector_print_mac(const struct rte_ether_addr *mac);

/** Print an IPv4 address in dotted-quad format. */
void injector_print_ipv4(uint32_t ip);

/** Allocate a DPDK mbuf pool with the given element count.
 *
 *  @param n   Number of mbufs in the pool
 *  @param name  Pool name (may be NULL for unnamed)
 *  @return  Pointer to the new pool, or NULL on failure */

/** Allocate a contiguous array of n mbufs from the pool.
 *
 *  @param pktmbuf_pool  Source pool
 *  @param mbufs[out]    Array to fill (pre-allocated, n elements)
 *  @param n             Number of mbufs to allocate
 *  @return  0 on success, -1 on failure */
int injector_alloc_mbufs(struct rte_mempool *pktmbuf_pool,
		      struct rte_mbuf **mbufs, int n);

/** Dispatch mbuf construction by type index.
 *
 *  Delegates to the per-type builder (ARP/ICMP/TCP/UDP/etc.)
 *  according to type_idx.  Applies VXLAN encap if configured
 *  and within the VXLAN ratio.
 *
 *  @param mbufs  Pre-allocated mbuf array
 *  @param n      Number of mbufs to construct
 *  @param type_idx  Packet-type index from the distribution table
 *  @param data   Cnode config (cast to Cnode *)
 *  @return  Number of bytes of payload generated */
uint64_t injector_mbufs(struct rte_mbuf **mbufs, uint32_t n,
		     unsigned int type_idx, void *data);

/** Construct a burst of ARP packets (Request/Reply alternating). */
uint64_t injector_mbufs_arp(struct rte_mbuf **mbufs,
			 unsigned int n, void *data);

/** Construct a burst of ICMP Echo packets (Request/Reply alternating). */
uint64_t injector_mbufs_icmp(struct rte_mbuf **mbufs,
			  unsigned int n, void *data);

/** Construct a burst of TCP PSH+ACK segments (sequence number increments). */
uint64_t injector_mbufs_tcp(struct rte_mbuf **mbufs,
			 unsigned int n, void *data);

/** Construct a burst of UDP datagrams. */
uint64_t injector_mbufs_udp(struct rte_mbuf **mbufs,
			 unsigned int n, void *data);




/** Construct a burst of erroneous/malformed packets for packet mutation.
 *
 *  Applies random mutations per the erroneous config class table. */
uint64_t injector_mbufs_erroneous(struct rte_mbuf **mbufs,
			       unsigned int n, void *data);

/** Allocate and initialise a injector_conf.
 *
 *  @return  Heap-allocated zeroed injector_conf, or NULL on malloc failure */
struct injector_conf *injector_init(void);

/** Free a injector_conf and all owned resources.
 *
 *  Calls free() on heap fields and rte_free() on DPDK-allocated
 *  fields (dist).   */
void injector_free(struct injector_conf *bconf);

/** Build the O(1) random-type dispatch table from configured weights.
 *
 *  Stores the result in bconf->dist.  Exits on memory failure.
 *
 *  @param bconf  Target conf (receives dist)
 *  @param ratio  Per-type weight ratios
 *  @param bep    Inner/outer encap parameters (ownership transferred)
 *  @return  0 on success (never returns on error) */
int injector_set_dist(struct injector_conf *bconf, struct dist_ratio *ratio,
		   struct injector_encap_params *bep);

/** Parse a protocol-type CLI argument (e.g. "--udp 100") into a weight.
 *
 *  @param type_idx  Internal type index (e.g. TYPE_ARP=0)
 *  @param optarg    CLI option value string
 *  @return  Parsed weight, or -1 on error */
int32_t injector_parse_type(unsigned int type_idx, char *optarg);

/** Initialise a dist_ratio struct to all-zeros with num=1. */
void dist_ratio_init(struct dist_ratio *dr);

/** Prepend a VXLAN tunnel header to each mbuf in the burst.
 *
 *  Stacks Outer Eth -> Outer IPv4 -> Outer UDP -> VXLAN -> inner frame.
 *  Sets HW offload flags for tunnel checksum offload.
 *
 *  @return  Number of bytes prepended, or -1 on mbuf prepend failure */
uint64_t injector_encap_vxlan(struct rte_mbuf **mbufs,
			   unsigned int n, void *data);

/** Log the internal state of a distribution table for debugging. */
void injector_show_dist(const struct distribution *dist);

#endif
