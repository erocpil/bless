#ifndef INJECTOR_PARSE_H
#define INJECTOR_PARSE_H

#include <stdint.h>

/**
 * @file injector_parse.h
 * @brief Pure-C parsing functions shared between core engine and unit tests.
 *
 * These functions have no DPDK or hardware dependencies.
 * They can be unit-tested independently and fuzzed without DPDK linkage.
 */

/**
 * Parse a port specification in "base+N" format.
 *
 * Accepts "8080" (range=0) or "8080+10" (range=10).
 * On success *port is the base and *range is the count; 0 means a single port.
 * Returns 0 on success, -1 on parse failure.
 */
int injector_parse_port_range(const char *data, uint16_t *port, int32_t *range);

/**
 * Parse an IPv4 address in "base+N" format.
 *
 * Accepts "172.16.0.1" (range=0) or "172.16.0.1+10" (range=10).
 * On success *ip is the base address in network byte order
 * and *range is the count (0 means a single address).
 * Returns 0 on success, -1 on parse failure.
 */
int injector_parse_ip_range(const char *data, uint32_t *ip, int64_t *range);




#endif /* INJECTOR_PARSE_H */
