#ifndef BLESS_CONTROL_POLICY_H
#define BLESS_CONTROL_POLICY_H

#include <stddef.h>

#define CONTROL_API_KEY_MIN_LEN 16
#define CONTROL_API_KEY_MAX_LEN 255

int control_policy_is_loopback_only(const char *ports);
int control_policy_key_length_valid(size_t len);
int control_policy_key_equal(const char *configured, size_t configured_len,
	const char *candidate, size_t candidate_len);
size_t control_policy_query_key(const char *query, char *out, size_t out_max);
int control_policy_allows_listener(const char *ports, int remote_enabled,
	int key_configured);

#endif
