#ifndef __BLESS_H__
#define __BLESS_H__

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
#include <rte_mbuf.h>
#include <rte_string_fns.h>
#include <rte_pdump.h>
#include <rte_telemetry.h>

struct base;
#include "config.h"

/* Per-port statistics struct */
struct port_statistics {
	rte_atomic64_t tx_pkts;
	rte_atomic64_t tx_bytes;
	rte_atomic64_t rx;
	rte_atomic64_t dropped_pkts;
	rte_atomic64_t dropped_bytes;
	rte_atomic64_t tsc;
} __rte_cache_aligned;

struct distribution {
	uint32_t pos;
	uint32_t size;
	uint32_t mask;
	uint32_t capacity;
	uint8_t data[0];
};

#ifndef DIST_RATIO_DUMP
#define DISTRIBUTION_DUMP(d) \
	do { \
		printf("[%s %d] distribution %p:\n" \
				"   pos\t%u\n" \
				"   size\t%u\n" \
				"   mask\t%u\n" \
				"   capacity\t%u\n", \
				__func__, __LINE__, \
				d, d->pos, d->size, d->mask, d->capacity); \
	} while (0)
#endif

enum BLESS_TYPE {
	TYPE_ARP = 0,
	TYPE_ICMP,
	TYPE_TCP,
	TYPE_UDP,
	TYPE_MAX,
};

enum BLESS_MODE {
	BLESS_MODE_TX_ONLY,
	BLESS_MODE_RX_ONLY,
	BLESS_MODE_FWD,
	BLESS_MODE_MAX,
};

#define OFFLOAD_IPV4(cnode) ((cnode)->offload & OF_IPV4_VAL)
#define OFFLOAD_IPV6(cnode) ((cnode)->offload & OF_IPV4_VAL)
#define OFFLOAD_TCP(cnode) ((cnode)->offload & OF_TCP_VAL)
#define OFFLOAD_UDP(cnode) ((cnode)->offload & OF_UDP_VAL)

struct dist_ratio {
	uint64_t num;
	int32_t weight[TYPE_MAX];
	int32_t quota[TYPE_MAX];
};

// FIXME erroneous
#ifndef DIST_RATIO_DUMP
#define DIST_RATIO_DUMP(r) \
	do { \
		typeof(r) _r = r; \
		printf("[%s %d] dist_ratio %p\n" \
				"   num %lu\n" \
				"   data [\n", \
				__func__, __LINE__, \
				_r, _r->num); \
		for (int i = 0; i < TYPE_MAX - 0; i++) { \
			printf("    %d %s\t%d %d -> %.2f\n", \
					i, BLESS_TYPE_STR[i], _r->weight[i], _r->weight[i], \
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

struct bless_encap_params {
	struct mbuf_conf *inner;
	struct mbuf_conf *outer;
};

struct bless_conf {
	// struct lcore_queue_conf (*lcore_queue_conf)[RTE_MAX_LCORE];
	struct base *base;
	uint32_t enabled_port_mask;
	struct port_statistics **stats;
	uint32_t *dst_ports;
	uint64_t timer_period;
	atomic_int *state;
	pthread_barrier_t *barrier;
	Cnode *cnode;
	struct config_file_map *cfm;

	int64_t num;
	uint8_t auto_start;
	uint8_t mode;
	uint16_t batch;
	uint64_t batch_delay_us;
	struct distribution *dist;
	struct dist_ratio dist_ratio;

	struct bless_encap_params bep;
	// struct mbuf_conf mconf;
	int argc;
	char **argv;
};

#ifndef BLESS_CONF_DUMP
#define BLESS_CONF_DUMP(c) \
	do { \
		typeof(c) _c = c; \
		printf("bless_conf %p\n" \
				"   batch %u\n" \
				"   dist %p\n", \
				_c, _c->batch, _c->dist); \
	} while (0)
#endif

void bless_print_mac(const struct rte_ether_addr *mac);
void bless_print_ipv4(uint32_t ip);
struct rte_mempool *bless_create_pktmbuf_pool(uint32_t n, char *name);
int bless_alloc_mbufs(struct rte_mempool *pktmbuf_pool, struct rte_mbuf **mbufs, int n);
uint64_t bless_mbufs(struct rte_mbuf **mbufs, uint32_t n, enum BLESS_TYPE type, void *data);
uint64_t bless_mbufs_arp(struct rte_mbuf **mbufs, unsigned int n, void *data);
uint64_t bless_mbufs_icmp(struct rte_mbuf **mbufs, unsigned int n, void *data);
uint64_t bless_mbufs_tcp(struct rte_mbuf **mbufs, unsigned int n, void *data);
uint64_t bless_mbufs_udp(struct rte_mbuf **mbufs, unsigned int n, void *data);
uint64_t bless_mbufs_erroneous(struct rte_mbuf **mbufs, unsigned int n, void *data);
struct bless_conf *bless_init();
int bless_set_dist(struct bless_conf* bconf, struct dist_ratio *ratio, struct bless_encap_params *bep);
// void bless_set_config_file(struct bless_conf* bconf, struct config_file_map *cfm);
int32_t bless_parse_type(enum BLESS_TYPE type, char *optarg);
void dist_ratio_init(struct dist_ratio *dr);
int bless_parse_ip_range(char *data, uint32_t *ip, int64_t *range);
int bless_parse_port_range(char *data, uint16_t *port, int32_t *range);
int64_t bless_seperate_ip_range(char *ip_range);
int32_t bless_seperate_port_range(char *port_range);
uint64_t bless_encap_vxlan(struct rte_mbuf **mbufs, unsigned int n, void *data);

#endif
