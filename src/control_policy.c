#include "control_policy.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

int control_policy_is_loopback_only(const char *ports)
{
	char buf[256];
	if (!ports || !ports[0]) {
		return 1;
	}
	if (snprintf(buf, sizeof(buf), "%s", ports) >= (int)sizeof(buf)) {
		return 0;
	}

	for (const char *p = buf; *p;) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p) {
			break;
		}
		const char *end = strchr(p, ',');
		if (!end) {
			end = p + strlen(p);
		}
		size_t len = (size_t)(end - p);
		char token[128];
		if (len == 0 || len >= sizeof(token)) {
			return 0;
		}
		memcpy(token, p, len);
		token[len] = '\0';
		size_t tlen = len;
		if (tlen && token[tlen - 1] == 's') {
			token[--tlen] = '\0';
		}
		char *colon = strrchr(token, ':');
		if (!colon) {
			return 0;
		}
		*colon = '\0';
		const char *ip = token;
		if (*ip == '[') {
			ip++;
			char *rb = strchr(ip, ']');
			if (rb) {
				*rb = '\0';
			}
		}

		int loopback = 0;
		struct in_addr ipv4;
		struct in6_addr ipv6;
		if (inet_pton(AF_INET, ip, &ipv4) == 1) {
			uint32_t addr = ntohl(ipv4.s_addr);
			loopback = (addr & 0xff000000U) == 0x7f000000U;
		} else if (inet_pton(AF_INET6, ip, &ipv6) == 1) {
			loopback = IN6_IS_ADDR_LOOPBACK(&ipv6);
		} else if (strcasecmp(ip, "localhost") == 0) {
			loopback = 1;
		}
		if (!loopback) {
			return 0;
		}
		p = *end ? end + 1 : end;
	}
	return 1;
}

int control_policy_key_length_valid(size_t len)
{
	return len >= CONTROL_API_KEY_MIN_LEN && len <= CONTROL_API_KEY_MAX_LEN;
}

int control_policy_key_equal(const char *configured, size_t configured_len,
	const char *candidate, size_t candidate_len)
{
	if (!configured || !candidate || configured_len != candidate_len) {
		return 0;
	}
	unsigned char diff = 0;
	for (size_t i = 0; i < configured_len; i++)
		diff |= (unsigned char)configured[i] ^ (unsigned char)candidate[i];
	return diff == 0;
}

size_t control_policy_query_key(const char *query, char *out, size_t out_max)
{
	if (!query || !out || out_max == 0) {
		return 0;
	}
	const char *start = query;
	for (;;) {
		start = strstr(start, "api_key=");
		if (!start) {
			return 0;
		}
		if (start == query || start[-1] == '&' || start[-1] == '?') {
			break;
		}
		start += 8;
	}
	start += 8;
	const char *end = strchr(start, '&');
	if (!end) {
		end = start + strlen(start);
	}
	size_t len = (size_t)(end - start);
	if (len == 0 || len >= out_max) {
		return 0;
	}
	memcpy(out, start, len);
	out[len] = '\0';
	return len;
}

int control_policy_allows_listener(const char *ports, int remote_enabled,
	int key_configured)
{
	if (control_policy_is_loopback_only(ports)) {
		return 1;
	}
	return remote_enabled && key_configured;
}
