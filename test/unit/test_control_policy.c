#include "control_policy.h"

#include <stdio.h>
#include <string.h>

static int passed;
static int failed;

#define CHECK(cond, name) do { \
	if (cond) passed++; \
	else { failed++; fprintf(stderr, "FAIL: %s\n", name); } \
} while (0)

int main(void)
{
	CHECK(control_policy_is_loopback_only("127.0.0.1:8000"), "IPv4 loopback");
	CHECK(control_policy_is_loopback_only("127.0.0.2:8000,[::1]:8443s"),
		"all loopback entries");
	CHECK(control_policy_is_loopback_only("localhost:8000"),
		"localhost loopback");
	CHECK(!control_policy_is_loopback_only("0.0.0.0:8000"),
		"wildcard rejected");
	CHECK(!control_policy_is_loopback_only("8000"), "bare port rejected");
	CHECK(!control_policy_is_loopback_only("192.0.2.1:8000"),
		"remote address rejected");
	char long_listener[300];
	memset(long_listener, '1', sizeof(long_listener) - 1);
	long_listener[sizeof(long_listener) - 1] = '\0';
	CHECK(!control_policy_is_loopback_only(long_listener),
		"overlong listener rejected");
	CHECK(control_policy_is_loopback_only(NULL), "missing listener default");

	CHECK(!control_policy_key_length_valid(0), "empty key rejected");
	CHECK(!control_policy_key_length_valid(15), "short key rejected");
	CHECK(control_policy_key_length_valid(16), "minimum key accepted");
	CHECK(control_policy_key_length_valid(255), "maximum key accepted");
	CHECK(!control_policy_key_length_valid(256), "long key rejected");
	CHECK(control_policy_key_equal("0123456789abcdef", 16,
		"0123456789abcdef", 16), "matching key");
	CHECK(!control_policy_key_equal("0123456789abcdef", 16,
		"0123456789abcdeg", 16), "mismatching key");
	CHECK(!control_policy_key_equal("0123456789abcdef", 16,
		"0123456789abcdef", 15), "wrong-length key");
	char key[32];
	CHECK(control_policy_query_key("mode=read&api_key=0123456789abcdef&x=1",
		key, sizeof(key)) == 16, "query key extracted");
	CHECK(!strcmp(key, "0123456789abcdef"), "query key value");
	CHECK(control_policy_query_key("mode=read", key, sizeof(key)) == 0,
		"missing query key rejected");
	CHECK(control_policy_query_key("api_key=", key, sizeof(key)) == 0,
		"empty query key rejected");
	CHECK(control_policy_query_key("notapi_key=0123456789abcdef", key,
		sizeof(key)) == 0, "embedded query key rejected");

	CHECK(control_policy_allows_listener("127.0.0.1:8000", 0, 0),
		"loopback without opt-in");
	CHECK(!control_policy_allows_listener("203.0.113.1:8000", 0, 1),
		"remote without opt-in");
	CHECK(!control_policy_allows_listener("203.0.113.1:8000", 1, 0),
		"remote without key");
	CHECK(control_policy_allows_listener("203.0.113.1:8000", 1, 1),
		"remote with opt-in and key");

	printf("control_policy tests: %d passed, %d failed\n", passed, failed);
	return failed ? 1 : 0;
}
