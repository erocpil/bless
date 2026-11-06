#ifndef __ERRONEOUS_H__
#define __ERRONEOUS_H__

#include "mutation.h"
#include "quic.h"
#include "ipv6.h"
#include "dns.h"
#include "ntp.h"
#include "http.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* quic_mutators and ipv6_mutators are defined in proto_quic.c / proto_ipv6.c */
extern struct Mutator quic_mutators[];
extern struct Mutator ipv6_mutators[];
extern struct Mutator dns_mutators[];
extern struct Mutator ntp_mutators[];
extern struct Mutator http_mutators[];

/* the Mutation Table */
static struct Mutation erroneous[] = {
	{ "mac", mac_mutators, ARRAY_SIZE(mac_mutators) },
	{ "arp", arp_mutators, ARRAY_SIZE(arp_mutators) },
	{ "ipv4", ip_mutators, ARRAY_SIZE(ip_mutators) },
	{ "icmp", icmp_mutators, ARRAY_SIZE(icmp_mutators) },
	{ "tcp", tcp_mutators, ARRAY_SIZE(tcp_mutators) },
	{ "udp", udp_mutators, ARRAY_SIZE(udp_mutators) },
	{ "other", other_mutators, ARRAY_SIZE(other_mutators) },
	{ "sctp", sctp_mutators, ARRAY_SIZE(sctp_mutators) },
	{ "quic", quic_mutators, QUIC_MUTATORS_COUNT },
	{ "ipv6", ipv6_mutators, IPV6_MUTATORS_COUNT },
	{ "dns",  dns_mutators,  DNS_MUTATORS_COUNT  },
	{ "ntp",  ntp_mutators,  NTP_MUTATORS_COUNT  },
	{ "http", http_mutators, HTTP_MUTATORS_COUNT },
};

static mutation_func find_mutation_func(const char *clas, const char *type)
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
