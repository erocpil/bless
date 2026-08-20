#ifndef __CNODE_H__
#define __CNODE_H__

#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include "define.h"
#include "bless_cfg.h"

#define BLESS_CONFIG_MAX 1024
#define BLESS_ERRNONEOUS_CLASS_TYPE_MAX 32
#define BLESS_ERRNONEOUS_CLASS_TYPE_NAME_MAX 32
#define ETHER_ADDR_LEN 6
#define ETHER_ADDR_MAX BLESS_CONFIG_MAX
#define IP_PROTO_MAX 256
#define IP_ADDR_MAX BLESS_CONFIG_MAX
#define PORT_MAX BLESS_CONFIG_MAX
#define BLESS_PLUGIN_MAX 16

typedef struct Cnode {
	uint64_t offload;
	struct {
		uint16_t mtu;
		uint8_t copy_payload;
		uint8_t dst[ETHER_ADDR_LEN];
		uint8_t src[ETHER_ADDR_LEN];
		uint16_t n_dst;
		uint16_t n_src;
		/* IMIX (Internet Mix) -- packet sizes for size entropy */
		uint16_t imix[BLESS_CONFIG_MAX];
		uint16_t n_imix;
		struct {
			struct {
				uint32_t src[IP_ADDR_MAX];
				uint32_t dst[IP_ADDR_MAX];
				uint16_t n_dst;
				uint16_t n_src;
			} arp;
			struct {
				uint32_t src[IP_ADDR_MAX];
				uint32_t dst[IP_ADDR_MAX];
				uint16_t _reserved_proto[IP_PROTO_MAX]; /* unused -- protocol selection via dist */
				uint16_t n_src;
				uint16_t n_dst;
				int64_t src_range;
				int64_t dst_range;
				struct {
					uint16_t ident[BLESS_CONFIG_MAX];
					uint16_t n_ident;
					char *payload;
					uint16_t payload_len;
				} icmp;
				struct {
					uint16_t src[PORT_MAX];
					uint16_t dst[PORT_MAX];
					uint16_t n_src;
					uint16_t n_dst;
					int32_t src_range;
					int32_t dst_range;
					char *payload;
					uint16_t payload_len;
				} tcp, udp;
			} ipv4;
			struct {
				uint8_t src[IP_ADDR_MAX][16];
				uint8_t dst[IP_ADDR_MAX][16];
				uint16_t n_src;
				uint16_t n_dst;
				int64_t src_range;
				int64_t dst_range;
				struct {
					uint16_t src[PORT_MAX];
					uint16_t dst[PORT_MAX];
					uint16_t n_src;
					uint16_t n_dst;
					int32_t src_range;
					int32_t dst_range;
				} tcp, udp;
			} ipv6;
		} type;
	} ether;
	struct {
		uint8_t enable;
		uint8_t ratio;
		uint8_t outer_ipv6;      /* 0=IPv4, 1=IPv6 outer header */
		uint16_t wire_mtu;
		struct {
			uint8_t dst[ETHER_ADDR_LEN];
			uint8_t src[ETHER_ADDR_LEN];
			uint16_t n_dst;
			uint16_t n_src;
			struct {
				struct {
					uint32_t src[IP_ADDR_MAX];
					uint32_t dst[IP_ADDR_MAX];
					uint32_t vni[IP_ADDR_MAX];
					uint16_t n_src;
					uint16_t n_dst;
					int64_t src_range;
					int64_t dst_range;
					struct {
						uint16_t src[PORT_MAX];
						uint16_t dst[PORT_MAX];
						uint16_t n_src;
						uint16_t n_dst;
						int32_t src_range;
						int32_t dst_range;
						char *payload;
						uint16_t payload_len;
					} udp;
				} ipv4;
				struct {
					uint8_t src[IP_ADDR_MAX][16];
					uint8_t dst[IP_ADDR_MAX][16];
					uint16_t n_src;
					uint16_t n_dst;
					int64_t src_range;
					int64_t dst_range;
					struct {
						uint16_t src[PORT_MAX];
						uint16_t dst[PORT_MAX];
						uint16_t n_src;
						uint16_t n_dst;
						int32_t src_range;
						int32_t dst_range;
					} udp;
				} ipv6;
			} type;
		} ether;
	} vxlan;
	struct {
		uint8_t ratio;
		mutation_func *func;
		uint16_t n_mutation;
		// mac, arp, ipv4, icmp, tcp, udp, etc ...
		struct ec_clas {
			char *name;
			char *type[BLESS_CONFIG_MAX];
			uint16_t n_type;
		} clas[BLESS_ERRNONEOUS_CLASS_TYPE_MAX];
		uint16_t n_clas;
	} erroneous;
	/* Extension slots (plugin-registered protocol configs) */	uint8_t n_ext;
	uint8_t latency_hist_enable;   /* 1=workers should embed TSC in UDP payload */
	struct {
		const struct bless_ext_cfg *desc;
		void                      *cfg;   /* malloc'd config blob */
	} ext[BLESS_PLUGIN_MAX];
} __attribute__((__aligned__(sizeof(char)))) Cnode;

/**
 * dump Cnode struct
 * @param node - Cnode ptr
 * @param depth - initial depth(normally 0)
 */
void cnode_show(const Cnode *node, int depth);

/**
 * simplified dump, show stats only
 * @param node - Cnode ptr
 */
void cnode_show_summary(const Cnode *node);

/**
 * Dump specific part
 */
/*
void dump_cnode_ether(const Cnode *node, int depth);
void dump_cnode_vxlan(const Cnode *node, int depth);
void dump_cnode_erroneous(const Cnode *node, int depth);
*/

#endif
