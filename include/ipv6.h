#ifndef __IPV6_H__
#define __IPV6_H__

#include <stdint.h>
#include "bless.h"           /* mutation_func */

/* IPv6 erroneous mutations (injectable via YAML ``erroneous.class.ipv6``) */
uint64_t mutation_ipv6_version(void **mbufs, unsigned int n, void *data);
uint64_t mutation_ipv6_traffic_class(void **mbufs, unsigned int n, void *data);
uint64_t mutation_ipv6_flow_label(void **mbufs, unsigned int n, void *data);
uint64_t mutation_ipv6_hop_limit(void **mbufs, unsigned int n, void *data);

#define IPV6_MUTATORS_COUNT 4

#endif
