#include "stats_guard.h"
#include "stats_payload.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static int passed;
static int failed;

#define CHECK(cond, name) do { \
	if (cond) passed++; \
	else { failed++; fprintf(stderr, "FAIL: %s\n", name); } \
} while (0)

static void test_reader_lifetime(void)
{
	struct stats_guard guard = {
		.active = ATOMIC_VAR_INIT(0),
		.readers = {ATOMIC_VAR_INIT(0), ATOMIC_VAR_INIT(0)}
	};
	int held = stats_guard_acquire(&guard);
	CHECK(held == 0, "reader pins active slot");

	stats_guard_publish(&guard, 1);
	CHECK(!stats_guard_writable(&guard, held),
		"slow reader keeps old slot non-writable");
	CHECK(stats_guard_writable(&guard, 1),
		"new inactive slot remains writable");

	stats_guard_publish(&guard, 0);
	CHECK(!stats_guard_writable(&guard, held),
		"three-publication window does not invalidate reader pin");
	stats_guard_release(&guard, held);
	CHECK(stats_guard_writable(&guard, held),
		"slot becomes writable after reader release");
}

static void test_payload_boundaries(void)
{
	char dst[8];
	const char payload[] = "1234567";
	CHECK(stats_payload_copy(dst, sizeof(dst), payload, 7) == 7,
		"below-limit copy length");
	CHECK(!strcmp(dst, "1234567"), "below-limit payload");
	CHECK(stats_payload_copy(dst, sizeof(dst), payload, 8) == 7,
		"exact-limit copy clamps");
	CHECK(dst[7] == '\0', "exact-limit terminator");
	CHECK(stats_payload_copy(dst, sizeof(dst), "123456789", 9) == 7,
		"above-limit copy clamps");
	CHECK(dst[7] == '\0', "above-limit terminator");
	CHECK(stats_payload_copy(NULL, sizeof(dst), payload, 7) == 0,
		"null destination rejected");
}

static void test_max_xstats_payload(void)
{
	char payload[8192];
	char copy[8192];
	size_t off = 0;
	int n = snprintf(payload, sizeof(payload), "{\"xstats\":{");
	CHECK(n > 0, "max xstats fixture starts");
	off = (size_t)n;
	for (int i = 0; i < 400 && off + 32 < sizeof(payload); i++) {
		n = snprintf(payload + off, sizeof(payload) - off,
		              "%s\"stat_%03d\":%d", i ? "," : "", i, i);
		if (n < 0 || (size_t)n >= sizeof(payload) - off) {
			break;
		}
		off += (size_t)n;
	}
	if (off + 3 < sizeof(payload)) {
		memcpy(payload + off, "}}\0", 3);
		off += 2;
	}
	size_t copied = stats_payload_copy(copy, sizeof(copy), payload, off);
	CHECK(copied == off, "max xstats payload preserved");
	CHECK(copy[copied - 1] == '}', "max xstats payload terminates");
	CHECK(strstr(copy, "stat_399") != NULL, "max xstats tail retained");
}

int main(void)
{
	test_reader_lifetime();
	test_payload_boundaries();
	test_max_xstats_payload();
	printf("stats publication tests: %d passed, %d failed\n",
		passed, failed);
	return failed ? 1 : 0;
}
