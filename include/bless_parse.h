#ifndef BLESS_PARSE_H
#define BLESS_PARSE_H

#include <stdint.h>

/**
 * @file bless_parse.h
 * @brief Pure-C parsing functions shared between core engine and unit tests.
 *
 * These functions have no DPDK, civetweb, or hardware dependencies.
 * They can be unit-tested independently and fuzzed without DPDK linkage.
 */

/**
 * Parse a port specification in "base+N" format.
 *
 * Accepts "8080" (range=0) or "8080+10" (range=10).
 * On success *port is the base and *range is the count; 0 means a single port.
 * Returns 0 on success, -1 on parse failure.
 */
int bless_parse_port_range(const char *data, uint16_t *port, int32_t *range);

/**
 * Parse an IPv4 address in "base+N" format.
 *
 * Accepts "172.16.0.1" (range=0) or "172.16.0.1+10" (range=10).
 * On success *ip is the base address in network byte order
 * and *range is the count (0 means a single address).
 * Returns 0 on success, -1 on parse failure.
 */
int bless_parse_ip_range(const char *data, uint32_t *ip, int64_t *range);

/**
 * Parse an IPv6 address in "base+N" format.
 *
 * Accepts "2001:db8::1" (range=0) or "2001:db8::1+100" (range=100).
 * The parsed address is stored in the 16-byte buffer.  Range specifies
 * how many consecutive addresses follow the base (used for IP entropy).
 * Returns 0 on success, -1 on parse failure.
 */
int bless_parse_ipv6_range(const char *data, uint8_t addr[16], int64_t *range);

#endif /* BLESS_PARSE_H */
