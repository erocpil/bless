#ifndef __CONFIG_H__
#define __CONFIG_H__

/**
 * @file config.h
 * @brief Config-file data structures -- mmap-based I/O, YAML node tree.
 *
 * Config files are memory-mapped and parsed into a custom Node tree
 * (not libyaml's native document model).  The tree is then traversed
 * to populate Cnode and injector_conf structures.
 */

#include <stddef.h>

#include <yaml.h>
#include "cnode.h"
#include "system.h"

struct config_file_map {
	char *name;
	int fd;
	size_t len;
	void *addr;
};

/** Memory-map a config file and validate its existence.
 *  @return  0 on success, -1 on error. */
int config_file_map_open(struct config_file_map *cfm);

/** Unmap and close a file previously opened by config_file_map_open(). */
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


/*
 * IMIX helpers -- pick a random packet size from the configured IMIX array.
 * When n_imix == 0, falls back to the caller's default (no-op).
 */
#define IMIX_SIZE(cnode) \
	((cnode)->ether.n_imix \
		? random_array_elem_uint16_t((cnode)->ether.imix, \
			(cnode)->ether.n_imix, 0) \
		: 0)

#define IMIX_PAYLOAD_LEN(cnode, l3, l4) \
	({ \
		uint16_t _total = IMIX_SIZE(cnode); \
		_total ? (_total > (l3)+(l4) ? _total - (l3) - (l4) : 0) : 0; \
	})

enum INJECTOR_ERRNONEOUS_CLASS_TYPE_OFFSET {
	EMAC = 0,
	EARP,
	EIP,
	EICMP,
	ETCP,
	EUDP,
};

enum OFFLOAD_TYPE {
	OF_IPV4 = 0,
	OF_TCP,
	OF_UDP,
	OF_OUTER_IPV4,
	OF_OUTER_UDP,
	OF_MAX,
};

enum OFFLOAD_VALUE {
	OF_IPV4_VAL = 1 << OF_IPV4,
	OF_TCP_VAL = 1 << OF_TCP,
	OF_UDP_VAL = 1 << OF_UDP,
	OF_OUTER_IPV4_VAL = 1 << OF_OUTER_IPV4,
	OF_OUTER_UDP_VAL = 1 << OF_OUTER_UDP,
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

/** Validate that a config_file_map describes a readable, non-empty file.
 *  @return  0 on success, -1 if file missing or empty. */
int config_check_file_map(struct config_file_map *cfm);

/** Parse a YAML config file into a Node tree.
 *  @param cfm  File map (must be open and validated)
 *  @return  Root Node, or NULL on parse failure. */
Node *config_init(struct config_file_map *cfm);

/** Free the config root Node tree allocated by config_init(). */
int config_exit(Node *root);

/** Parse the ``system`` section from the YAML config tree. */
int config_parse_system(Node *root, struct system_cfg *cfg);

/** Parse the ``dpdk`` section: EAL arguments for rte_eal_init(). */
int config_parse_dpdk(Node *root, int *targc, char ***targv);

/** Parse a YAML sequence node into argv-style EAL arguments. */
int config_parse_generic(Node *node, int *targc, char ***targv,
		int i, const char *prefix);

/** Parse the ``injector`` section into a Cnode configuration tree.
 *  @return  Heap-allocated Cnode, or NULL on error. */
Cnode *config_parse_injector(Node *root);

/** Parse a single YAML node into our internal Node representation. */
Node *parse_node(yaml_parser_t *parser);

/** Search for a Node by dotted path (e.g. "system.server.port"). */
Node *find_by_path(Node *root, const char *path);

/** Log the full config structure. */
void config_show(struct config *cfg);

/** Log the root Node tree. */
void config_show_root(struct config *cfg);

/** Deep-copy a Cnode (payloads, ext configs, erroneous tables).
 *  @return  0 on success, -1 on allocation failure. */
int config_clone_cnode(Cnode *src, Cnode *dst);

#endif
