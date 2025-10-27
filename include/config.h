#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <yaml.h>
#include "define.h"

typedef enum {
	NODE_SCALAR,
	NODE_MAPPING,
	NODE_SEQUENCE
} NodeType;

typedef struct Node {
	char *key;
	char *value;
	NodeType type;
	struct Node *child;
	struct Node *next;
} Node;

#define BLESS_CONFIG_MAX 1024
#define BLESS_ERRNONEOUS_CLASS_TYPE_MAX 32
#define BLESS_ERRNONEOUS_CLASS_TYPE_NAME_MAX 32
#define ETHER_ADDR_LEN 6
#define ETHER_ADDR_MAX BLESS_CONFIG_MAX
#define IP_PROTO_MAX 256
#define IP_ADDR_MAX BLESS_CONFIG_MAX
#define PORT_MAX BLESS_CONFIG_MAX

#define RANDOM_VXLAN_IP_SRC(p) \
	random_array_elem_uint32_t(p->vxlan.ether.type.ipv4.src, \
			p->vxlan.ether.type.ipv4.n_src, \
			p->vxlan.ether.type.ipv4.src_range);

#define RANDOM_VXLAN_IP_DST(p) \
	random_array_elem_uint32_t(p->vxlan.ether.type.ipv4.dst, \
			p->vxlan.ether.type.ipv4.n_dst, \
			p->vxlan.ether.type.ipv4.dst_range);

#define RANDOM_VXLAN_IP_VNI(p) \
	random_array_elem_uint32_t_with_peer(p->vxlan.ether.type.ipv4.src, \
			p->vxlan.ether.type.ipv4.vni, \
			p->vxlan.ether.type.ipv4.n_src, \
			p->vxlan.ether.type.ipv4.src_range);

#define RANDOM_VXLAN_UDP_SRC(p) \
	random_array_elem_uint16_t(p->vxlan.ether.type.ipv4.udp.src, \
			p->vxlan.ether.type.ipv4.udp.n_src, \
			p->vxlan.ether.type.ipv4.udp.src_range);

#define RANDOM_VXLAN_UDP_DST(p) \
	random_array_elem_uint16_t(p->vxlan.ether.type.ipv4.udp.dst, \
			p->vxlan.ether.type.ipv4.udp.n_dst, \
			p->vxlan.ether.type.ipv4.udp.dst_range);

#define RANDOM_IP_SRC(p) \
	random_array_elem_uint32_t(p->ether.type.ipv4.src, \
			p->ether.type.ipv4.n_src, \
			p->ether.type.ipv4.src_range);

#define RANDOM_IP_DST(p) \
	random_array_elem_uint32_t(p->ether.type.ipv4.dst, \
			p->ether.type.ipv4.n_dst, \
			p->ether.type.ipv4.dst_range);

#define RANDOM_TCP_SRC(p) \
	random_array_elem_uint16_t(p->ether.type.ipv4.tcp.src, \
			p->ether.type.ipv4.tcp.n_src, \
			p->ether.type.ipv4.tcp.src_range);

#define RANDOM_TCP_DST(p) \
	random_array_elem_uint16_t(p->ether.type.ipv4.tcp.dst, \
			p->ether.type.ipv4.tcp.n_dst, \
			p->ether.type.ipv4.tcp.dst_range);

#define RANDOM_UDP_SRC(p) \
	random_array_elem_uint16_t(p->ether.type.ipv4.udp.src, \
			p->ether.type.ipv4.udp.n_src, \
			p->ether.type.ipv4.udp.src_range);

#define RANDOM_UDP_DST(p) \
	random_array_elem_uint16_t(p->ether.type.ipv4.udp.dst, \
			p->ether.type.ipv4.udp.n_dst, \
			p->ether.type.ipv4.udp.dst_range);

enum BLESS_ERRNONEOUS_CLASS_TYPE_OFFSET {
	EMAC = 0,
	EARP,
	EIP,
	EICMP,
	ETCP,
	EUDP,
};

enum OFFLOAD_TYPE {
	OF_IPV4 = 0,
	OF_IPV6,
	OF_TCP,
	OF_UDP,
	OF_MAX,
};

enum OFFLOAD_VALUE {
	OF_IPV4_VAL = 1 << OF_IPV4,
	OF_IPV6_VAL = 1 << OF_IPV6,
	OF_TCP_VAL = 1 << OF_TCP,
	OF_UDP_VAL = 1 << OF_UDP,
	OF_MAX_VAL = (uint64_t)-1,
};

struct offload_table_item {
	char *name;
	int type;
};

static struct offload_table_item offload_table[] = {
	{ "ipv4", OF_IPV4 },
	{ "ipv6", OF_IPV6 },
	{ "tcp", OF_TCP },
	{ "udp", OF_UDP },
};

typedef struct Cnode {
	uint64_t offload;
	struct {
		uint16_t mtu;
		uint8_t dst[ETHER_ADDR_LEN];
		uint8_t src[ETHER_ADDR_LEN];
		// uint16_t n_dst;
		// uint16_t n_src;
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
				/* TODO */
				uint16_t proto[IP_PROTO_MAX];
				uint16_t n_dst;
				uint16_t n_src;
				int64_t dst_range;
				int64_t src_range;
				struct {
					uint16_t ident[BLESS_CONFIG_MAX];
					uint16_t n_ident;
					char *payload;
					uint16_t payload_len;
				} icmp;
				struct {
					uint16_t src[PORT_MAX];
					uint16_t dst[PORT_MAX];
					uint16_t n_dst;
					uint16_t n_src;
					int32_t dst_range;
					int32_t src_range;
					char *payload;
					uint16_t payload_len;
				} tcp, udp;
			} ipv4;
		} type;
	} ether;
	struct {
		uint8_t enable;
		uint8_t ratio;
		struct {
			uint8_t dst[ETHER_ADDR_LEN];
			uint8_t src[ETHER_ADDR_LEN];
			uint16_t n_dst;
			uint16_t n_src;
			struct {
				struct {
					uint32_t src[IP_ADDR_MAX];
					uint32_t dst[IP_ADDR_MAX]; /* TODO uint64_t dst[] = { vni:ipv4 } */
					uint32_t vni[IP_ADDR_MAX];
					uint16_t n_src;
					uint16_t n_dst;
					int64_t src_range; // xxx
					int64_t dst_range; // xxx
					struct {
						uint16_t src[PORT_MAX];
						uint16_t dst[PORT_MAX];
						uint16_t n_src;
						uint16_t n_dst;
						int32_t src_range; // xxx
						int32_t dst_range; // xxx
						char *payload;
					uint16_t payload_len;
					} udp;
				} ipv4;
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
} __attribute__((__aligned__(sizeof(char)))) Cnode;

int config_check_file(char *file);
Node *config_init(int argc, char **argv);
int config_exit(Node *root);
int config_parse_dpdk(Node *root, int *targc, char ***targv);
int config_parse_generic(Node *node, int *targc, char ***targv, int i, const char *prefix);
Cnode *config_parse_bless(Node *root);
Node *parse_node(yaml_parser_t *parser);
uint16_t random_array_elem_uint16_t(uint16_t *array, uint16_t num, int32_t range);
uint32_t random_array_elem_uint32_t(uint32_t *array, uint16_t num, int64_t range);
uint64_t random_array_elem_uint32_t_with_peer(uint32_t *array, uint32_t *peer, uint16_t num, int64_t range);

#endif
