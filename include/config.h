#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <stddef.h>

#include <yaml.h>
#include "cnode.h"
#include "server.h"
#include "system.h"

struct config_file_map {
	char *name;
	int fd;
	size_t len;
	void *addr;
};

int config_file_map_open(struct config_file_map *cfm);
void config_file_unmap_close(struct config_file_map *fm);

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
	rte_cpu_to_be_16(random_array_elem_uint16_t(p->vxlan.ether.type.ipv4.udp.src, \
				p->vxlan.ether.type.ipv4.udp.n_src, \
				p->vxlan.ether.type.ipv4.udp.src_range));

#define RANDOM_VXLAN_UDP_DST(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t(p->vxlan.ether.type.ipv4.udp.dst, \
				p->vxlan.ether.type.ipv4.udp.n_dst, \
				p->vxlan.ether.type.ipv4.udp.dst_range));

#define RANDOM_IP_SRC(p) \
	random_array_elem_uint32_t(p->ether.type.ipv4.src, \
				p->ether.type.ipv4.n_src, \
				p->ether.type.ipv4.src_range);

#define RANDOM_IP_DST(p) \
	random_array_elem_uint32_t(p->ether.type.ipv4.dst, \
				p->ether.type.ipv4.n_dst, \
				p->ether.type.ipv4.dst_range);

#define RANDOM_TCP_SRC(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t(p->ether.type.ipv4.tcp.src, \
				p->ether.type.ipv4.tcp.n_src, \
				p->ether.type.ipv4.tcp.src_range));

#define RANDOM_TCP_DST(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t(p->ether.type.ipv4.tcp.dst, \
				p->ether.type.ipv4.tcp.n_dst, \
				p->ether.type.ipv4.tcp.dst_range));

#define RANDOM_UDP_SRC(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t(p->ether.type.ipv4.udp.src, \
				p->ether.type.ipv4.udp.n_src, \
				p->ether.type.ipv4.udp.src_range));

#define RANDOM_UDP_DST(p) \
	rte_cpu_to_be_16(random_array_elem_uint16_t(p->ether.type.ipv4.udp.dst, \
				p->ether.type.ipv4.udp.n_dst, \
				p->ether.type.ipv4.udp.dst_range));

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

struct config {
	struct config_file_map cfm;
	Node *root;
	Cnode *cnode;
};

int config_check_file_map(struct config_file_map *cfm);
Node *config_init(char *f);
int config_exit(Node *root);
int config_parse_system(Node *root, struct system_cfg *cfg);
int config_parse_server(Node *root, struct server *server);
int config_parse_dpdk(Node *root, int *targc, char ***targv);
int config_parse_generic(Node *node, int *targc, char ***targv, int i, const char *prefix);
Cnode *config_parse_bless(Node *root);
Node *parse_node(yaml_parser_t *parser);
void config_show(struct config *cfg);
void config_show_root(struct config *cfg);
int config_clone_cnode(Cnode *src, Cnode *dst);

#endif
