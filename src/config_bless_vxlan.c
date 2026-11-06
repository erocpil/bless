#include "config_bless_internal.h"

#include "bless_parse.h"
#include "config_array.h"
#include "config_node.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <rte_ether.h>

static int config_parse_bless_vxlan_ether(Node *vxlan_node, Cnode *cnode)
{
	Node *node = find_by_path(vxlan_node, "ether.dst");
	if (!node) {
		return 0;  /* VXLAN ether section not configured -- skip */
	}

	cnode->vxlan.ether.n_dst = 0;
	if (node->value && strlen(node->value) &&
			(rte_ether_unformat_addr(node->value,
									 (struct rte_ether_addr*)&
									 (cnode->vxlan.ether.dst)) == 0)) {
		// bless_print_mac((struct rte_ether_addr*)cnode->vxlan.ether.dst);
		cnode->vxlan.ether.n_dst = 1;
	} else {
		goto ERROR;
	}

	node = find_by_path(vxlan_node, "ether.src");
	if (!node) {
		LOG_INFO("no vxlan src mac address node provided");
		cnode->vxlan.ether.n_src = 0;
	} else {
		if (node->value && strlen(node->value) &&
				(rte_ether_unformat_addr(node->value,
										 (struct rte_ether_addr*)&
										 (cnode->vxlan.ether.src)) == 0)) {
			// bless_print_mac((struct rte_ether_addr*)cnode->vxlan.ether.src);
			cnode->vxlan.ether.n_src = 1;
		} else {
			LOG_WARN("Invalid vxlan src mac address, "
					"injector will try local port");
			cnode->vxlan.ether.n_src = 0;
		}
	}

	return 1;

ERROR:
	LOG_ERR("yaml config parse error: vxlan.ether.dst");
	return -1;
}


int config_parse_bless_vxlan(Node *bless_node, Cnode *cnode)
{
	int n = 0;
	Node *vxlan_node = find_by_path(bless_node, "vxlan");
	if (!vxlan_node) {
		return 0;
	}

	/* enable */
	cnode->vxlan.enable = 1;
	Node *node = find_by_path(vxlan_node, "enable");
	if (!node || !strlen(node->value) || (
				strcmp("true", node->value) &&
				strcmp("TRUE", node->value)
				)
	   ) {
		LOG_HINT("vxlan disabled");
		cnode->vxlan.enable = 0;
	}

	/* ratio */
	node = find_by_path(vxlan_node, "ratio");
	if (!node || !strlen(node->value)) {
		LOG_HINT("vxlan disabled");
		cnode->vxlan.enable = 0;
		cnode->vxlan.ratio = 0;
	} else {
		/* No early return here: wire-mtu and outer_ipv6 are still
		 * parsed below (needed by mutation_udp_vxlan even when
		 * VXLAN traffic generation is disabled).
		 * The full outer config (ether/IP/UDP) is skipped at the
		 * !enable guard below -- mutation gets zeroed defaults there,
		 * which is acceptable for corner-case usage. */
		char *endptr;
		errno = 0;
		long r = strtol(node->value, &endptr, 0);
		if (errno || endptr == node->value || *endptr != '\0' ||
		    r < 0 || r > 100) {
			LOG_WARN("invalid vxlan ratio \"%s\", disabling", node->value);
			cnode->vxlan.enable = 0;
			cnode->vxlan.ratio = 0;
		} else {
			cnode->vxlan.ratio = (uint16_t)r;
		}
	}

	/* wire-mtu -- VXLAN-aware wire-level MTU constraint (0 = disabled) */
	cnode->vxlan.wire_mtu = 0;
	node = find_by_path(vxlan_node, "wire-mtu");
	if (node && strlen(node->value)) {
		char *endptr;
		errno = 0;
		long wm = strtol(node->value, &endptr, 0);
		if (errno || endptr == node->value || *endptr != '\0') {
			LOG_WARN("invalid vxlan wire-mtu \"%s\", ignoring", node->value);
		} else if (wm > 0 && wm < 68) {
			LOG_WARN("vxlan wire-mtu %ld too small, setting to 0 (disabled)", wm);
		} else if (wm > 9000) {
			LOG_INFO("vxlan wire-mtu %ld clamped to 9000", wm);
			cnode->vxlan.wire_mtu = 9000;
		} else {
			cnode->vxlan.wire_mtu = (uint16_t)wm;
		}
	}

	/* outer_ipv6 -- select IPv6 outer header for VXLAN tunnel */
	cnode->vxlan.outer_ipv6 = 0;
	node = find_by_path(vxlan_node, "outer_ipv6");
	if (node && strlen(node->value) &&
	    (!strcmp("true", node->value) || !strcmp("TRUE", node->value))) {
		cnode->vxlan.outer_ipv6 = 1;
	}

	/* VXLAN traffic generation disabled: skip outer config parsing
	 * so the user isn't forced to provide outer IPs/ports/MACs.
	 * Mutation udp_vxlan still works with zeroed defaults here. */
	if (!cnode->vxlan.enable) {
		return 0;
	}

	if (config_parse_bless_vxlan_ether(vxlan_node, cnode) < 0) {
		return -1;
	}

	if (cnode->vxlan.outer_ipv6) {
		/* VXLAN over IPv6: parse ``vxlan.ether.type.ipv6`` */		Node *tnode;

		tnode = find_by_path(vxlan_node, "ether.type.ipv6.src");
		if (!tnode) {
			goto ERROR;
		}
		if (tnode->type == NODE_SCALAR) {
			int64_t range = 0;
			if (bless_parse_ipv6_range(tnode->value,
					cnode->vxlan.ether.type.ipv6.src[0], &range) == 0) {
				cnode->vxlan.ether.type.ipv6.src_range = range;
				cnode->vxlan.ether.type.ipv6.n_src = range ? 0 : 1;
			}
		} else if (tnode->type == NODE_SEQUENCE) {
			int k = 0;
			for (Node *i = tnode->child; i && k < IP_ADDR_MAX; i = i->next, k++)
				inet_pton(AF_INET6, i->value,
					cnode->vxlan.ether.type.ipv6.src[k]);
			cnode->vxlan.ether.type.ipv6.n_src = k;
			cnode->vxlan.ether.type.ipv6.src_range = 0;
		}
		if (!cnode->vxlan.ether.type.ipv6.n_src &&
		    !cnode->vxlan.ether.type.ipv6.src_range) {
			goto ERROR;
		}

		tnode = find_by_path(vxlan_node, "ether.type.ipv6.dst");
		if (!tnode) {
			goto ERROR;
		}
		if (tnode->type == NODE_SCALAR) {
			int64_t range = 0;
			if (bless_parse_ipv6_range(tnode->value,
					cnode->vxlan.ether.type.ipv6.dst[0], &range) == 0) {
				cnode->vxlan.ether.type.ipv6.dst_range = range;
				cnode->vxlan.ether.type.ipv6.n_dst = range ? 0 : 1;
			}
		} else if (tnode->type == NODE_SEQUENCE) {
			int k = 0;
			for (Node *i = tnode->child; i && k < IP_ADDR_MAX; i = i->next, k++)
				inet_pton(AF_INET6, i->value,
					cnode->vxlan.ether.type.ipv6.dst[k]);
			cnode->vxlan.ether.type.ipv6.n_dst = k;
			cnode->vxlan.ether.type.ipv6.dst_range = 0;
		}
		if (!cnode->vxlan.ether.type.ipv6.n_dst &&
		    !cnode->vxlan.ether.type.ipv6.dst_range) {
			goto ERROR;
		}

		tnode = find_by_path(vxlan_node, "ether.type.ipv6.udp.src");
		if (!tnode) {
			goto ERROR;
		}
		n = config_parse_port_maybe_range_to_array(tnode,
				cnode->vxlan.ether.type.ipv6.udp.src,
				&cnode->vxlan.ether.type.ipv6.udp.src_range,
				PORT_MAX);
		if (n <= 0) {
			goto ERROR;
		}
		cnode->vxlan.ether.type.ipv6.udp.n_src =
			cnode->vxlan.ether.type.ipv6.udp.src_range ? 0 : (uint16_t)n;

		tnode = find_by_path(vxlan_node, "ether.type.ipv6.udp.dst");
		if (!tnode) {
			goto ERROR;
		}
		n = config_parse_port_maybe_range_to_array(tnode,
				cnode->vxlan.ether.type.ipv6.udp.dst,
				&cnode->vxlan.ether.type.ipv6.udp.dst_range,
				PORT_MAX);
		if (n <= 0) {
			goto ERROR;
		}
		cnode->vxlan.ether.type.ipv6.udp.n_dst =
			cnode->vxlan.ether.type.ipv6.udp.dst_range ? 0 : (uint16_t)n;

		return n;
	}

	/* VXLAN over IPv4 (default): parse ``vxlan.ether.type.ipv4`` */
	node = find_by_path(vxlan_node, "ether.type.ipv4.src");
	if (!node) {
		goto ERROR;
	}

	n = config_parse_sequence_ipv4_vni_to_array(node,
			cnode->vxlan.ether.type.ipv4.src, cnode->vxlan.ether.type.ipv4.vni,
			sizeof(uint32_t), PORT_MAX);
	if (n > 0) {
		cnode->vxlan.ether.type.ipv4.n_src = n;
	} else {
		goto ERROR;
	}

	node = find_by_path(vxlan_node, "ether.type.ipv4.dst");
	if (!node) {
		LOG_ERR("Invalid vxlan ipv4 dst address");
		goto ERROR;
	}

	n = config_parse_sequence_ipv4_to_array(node, cnode->vxlan.ether.type.ipv4.dst,
			sizeof(uint32_t), IP_ADDR_MAX);
	if (n > 0) {
		cnode->vxlan.ether.type.ipv4.n_dst = n;
	} else {
		goto ERROR;
	}

	node = find_by_path(vxlan_node, "ether.type.ipv4.udp.src");
	if (!node) {
		goto ERROR;
	}

	n = config_parse_sequence_to_array(node, cnode->vxlan.ether.type.ipv4.udp.src,
			sizeof(uint16_t), PORT_MAX);
	if (n > 0) {
		cnode->vxlan.ether.type.ipv4.udp.n_src = n;
	} else {
		goto ERROR;
	}

	node = find_by_path(vxlan_node, "ether.type.ipv4.udp.dst");
	if (!node) {
		goto ERROR;
	}
	n = config_parse_sequence_to_array(node, cnode->vxlan.ether.type.ipv4.udp.dst,
			sizeof(uint16_t), PORT_MAX);
	if (n > 0) {
		cnode->vxlan.ether.type.ipv4.udp.n_dst = n;
	} else {
		goto ERROR;
	}

	return n;

ERROR:
	LOG_ERR("yaml config parse error: vxlan");
	return -1;
}
