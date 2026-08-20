#ifndef __BLESS_CONFIG_BLESS_INTERNAL_H__
#define __BLESS_CONFIG_BLESS_INTERNAL_H__

#include "cnode.h"
#include "config_yaml.h"

int config_parse_bless_erroneous(Node *bless_node, Cnode *cnode);
int config_parse_bless_ether_all(Node *bless_node, Node *ether_node,
	Cnode *cnode);
int config_parse_bless_vxlan(Node *bless_node, Cnode *cnode);
int config_parse_bless_plugins(Node *ether_node, Cnode *cnode);

#endif
