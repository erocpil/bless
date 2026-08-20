#include "bless_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>

/**
 * @file parse.c
 * @brief Pure-C parsing functions (no DPDK, no civetweb, no log deps).
 *
 * These functions parse the "base+N" range syntax used throughout
 * the bless YAML/CLI configuration system.  They live here so unit
 * tests and fuzz targets can link the real implementation instead
 * of maintaining copies.
 *
 * Error messages go to stderr for diagnostics; callers in the main
 * bless engine handle their own log formatting via LOG_ERR.
 */

int bless_parse_port_range(const char *data, uint16_t *port, int32_t *range)
{
	if (!data || strlen(data) < 1) {
		fprintf(stderr, "bless_parse_port_range: empty input\n");
		return -1;
	}

	int i = 0;
	char *port_range = strdup(data);

	/* search for '+' with \0 guard -- avoids OOB read on bare port */
	while (port_range[i] != '\0' && port_range[i] != '+') {
		i++;
	}

	if ('+' == port_range[i]) {
		port_range[i] = '\0';
		char *endptr = NULL;
		errno = 0;
		long r = strtol(&port_range[++i], &endptr, 10);
		if (errno || endptr == &port_range[i] || *endptr != '\0') {
			fprintf(stderr, "bless_parse_port_range: bad range in \"%s\": %s\n",
			        data, strerror(errno));
			free(port_range);
			return -1;
		}
		*range = (int32_t)r;
	} else {
		*range = 0;
	}
	{
		char *endptr = NULL;
		errno = 0;
		long p = strtol(port_range, &endptr, 10);
		if (errno || endptr == port_range || *endptr != '\0'
		    || p < 0 || p > 65535) {
			fprintf(stderr, "bless_parse_port_range: bad port in \"%s\": %s\n",
			        data, errno ? strerror(errno) : "out of range");
			free(port_range);
			return -1;
		}
		*port = (uint16_t)p;
	}

	free(port_range);

	return 0;
}

/* Parse an IPv4 address in "base+N" format.
 *
 * Accepts "172.16.0.1" (range=0) or "172.16.0.1+10" (range=10).
 * On success *ip is the base address in network byte order
 * and *range is the count (0 means a single address).
 * Returns 0 on success, -1 on parse failure. */
int bless_parse_ip_range(const char *data, uint32_t *ip, int64_t *range)
{
	if (!data || strlen(data) < 8) {
		fprintf(stderr, "bless_parse_ip_range: input too short: \"%s\"\n",
		        data ? data : "(null)");
		return -1;
	}

	int i = 0;
	char *ip_range = strdup(data);

	while (ip_range[i] != '\0' && ip_range[i] != '+' ) {
		i++;
	}

	if ('+' == ip_range[i]) {
		ip_range[i] = '\0';
		char *endptr = NULL;
		errno = 0;
		long r = strtol(&ip_range[++i], &endptr, 10);
		if (errno || endptr == &ip_range[i] || *endptr != '\0') {
			fprintf(stderr, "bless_parse_ip_range: bad range in \"%s\": %s\n",
			        data, strerror(errno));
			free(ip_range);
			return -1;
		}
		*range = (int64_t)r;
	} else {
		/* no range -- base IP only */
		*range = 0;
	}

	if (inet_pton(AF_INET, ip_range, ip) != 1) {
		fprintf(stderr, "bless_parse_ip_range: invalid IP \"%s\"\n", ip_range);
		free(ip_range);
		return -1;
	}

	free(ip_range);

	return 0;
}

/* Parse an IPv6 address in "base+N" format.
 *
 * Accepts "2001:db8::1" (range=0) or "2001:db8::1+100" (range=100).
 * The parsed address is stored in the 16-byte buffer.  Range specifies
 * how many consecutive addresses follow the base (used for IP entropy).
 * Returns 0 on success, -1 on parse failure. */
int bless_parse_ipv6_range(const char *data, uint8_t addr[16], int64_t *range)
{
	if (!data || strlen(data) < 3) {
		fprintf(stderr, "bless_parse_ipv6_range: input too short: \"%s\"\n",
		        data ? data : "(null)");
		return -1;
	}

	int i = 0;
	char *ip_range = strdup(data);

	while (ip_range[i] != '\0' && ip_range[i] != '+') {
		i++;
	}

	if ('+' == ip_range[i]) {
		ip_range[i] = '\0';
		char *endptr = NULL;
		errno = 0;
		long r = strtol(&ip_range[++i], &endptr, 10);
		if (errno || endptr == &ip_range[i] || *endptr != '\0') {
			fprintf(stderr, "bless_parse_ipv6_range: bad range in \"%s\": %s\n",
			        data, strerror(errno));
			free(ip_range);
			return -1;
		}
		*range = (int64_t)r;
	} else {
		*range = 0;
	}

	if (inet_pton(AF_INET6, ip_range, addr) != 1) {
		fprintf(stderr, "bless_parse_ipv6_range: invalid IPv6 \"%s\"\n",
		        ip_range);
		free(ip_range);
		return -1;
	}

	free(ip_range);

	return 0;
}
