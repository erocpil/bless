#ifndef __ERRONEOUS_H__
#define __ERRONEOUS_H__

#include "mutation.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* the Mutation Table */
static struct Mutation erroneous[] = {
	{ "mac", mac_mutators, ARRAY_SIZE(mac_mutators) },
	{ "arp", arp_mutators, ARRAY_SIZE(arp_mutators) },
	{ "ipv4", ip_mutators, ARRAY_SIZE(ip_mutators) },
	{ "icmp", icmp_mutators, ARRAY_SIZE(icmp_mutators) },
	{ "tcp", tcp_mutators, ARRAY_SIZE(tcp_mutators) },
	{ "udp", udp_mutators, ARRAY_SIZE(udp_mutators) },
};

mutation_func find_mutation_func(const char *clas, const char *type)
{
	for (int i = 0; i < (int)ARRAY_SIZE(erroneous); i++) {
		if (strcmp(clas, erroneous[i].name)) {
			continue;
		}
		for (int j = 0; j < (int)erroneous[i].count; j++) {
			if (strcmp(type, erroneous[i].mutator[j].name) == 0) {
				return erroneous[i].mutator[j].func;
			}
		}
	}
	return NULL;
}

#endif
