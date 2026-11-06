#ifndef __CONFIG_H__
#define __CONFIG_H__

/**
 * @file config.h
 * @brief Config-file data structures -- mmap-based I/O, YAML node tree.
 *
 * Config files are memory-mapped and parsed into a custom Node tree
 * (not libyaml's native document model).  The tree is then traversed
 * to populate Cnode and bless_conf structures.
 */

#include "config_file.h"
#include "config_node.h"
#include "cnode.h"
#include "cnode_runtime.h"
#include "server.h"
#include "system.h"

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
	OF_OUTER_IPV4,
	OF_OUTER_UDP,
	OF_SCTP,
	OF_MAX,
};

enum OFFLOAD_VALUE {
	OF_IPV4_VAL = 1 << OF_IPV4,
	OF_IPV6_VAL = 1 << OF_IPV6,
	OF_TCP_VAL = 1 << OF_TCP,
	OF_UDP_VAL = 1 << OF_UDP,
	OF_OUTER_IPV4_VAL = 1 << OF_OUTER_IPV4,
	OF_OUTER_UDP_VAL = 1 << OF_OUTER_UDP,
	OF_SCTP_VAL = 1 << OF_SCTP,
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

/** Parse the ``system`` section from the YAML config tree. */
int config_parse_system(Node *root, struct system_cfg *cfg);

/** Parse the ``server`` section (HTTP/WebSocket) from the YAML config tree. */
int config_parse_server(Node *root, struct server *server);

/** Parse the ``dpdk`` section: EAL arguments for rte_eal_init(). */
int config_parse_dpdk(Node *root, int *targc, char ***targv);

/** Parse a YAML sequence node into argv-style EAL arguments. */
int config_parse_generic(Node *node, int *targc, char ***targv,
		int i, const char *prefix);

/** Parse the ``bless`` section into a Cnode configuration tree.
 *  @return  Heap-allocated Cnode, or NULL on error. */
Cnode *config_parse_bless(Node *root);

/** Log the full config structure. */
void config_show(struct config *cfg);

/** Log the root Node tree. */
void config_show_root(struct config *cfg);

/** Deep-copy a Cnode (payloads, ext configs, erroneous tables).
 *  @return  0 on success, -1 on allocation failure. */
int config_clone_cnode(Cnode *src, Cnode *dst);

#endif
