#include "config_array.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static Node
scalar(const char *value)
{
	return (Node){ .value = (char *)value, .type = NODE_SCALAR };
}

static void
test_integer_arrays(void)
{
	Node third = scalar("30");
	Node second = scalar("20");
	Node first = scalar("10");
	first.next = &second;
	second.next = &third;
	Node sequence = { .type = NODE_SEQUENCE, .child = &first };
	struct { uint16_t values[2]; uint16_t canary; } output = {
		.canary = 0xa55a,
	};
	assert(config_parse_sequence_to_array(&sequence, output.values,
		sizeof(output.values[0]), 2) == 2);
	assert(output.values[0] == 10 && output.values[1] == 20);
	assert(output.canary == 0xa55a);

	Node bad = scalar("65536");
	assert(config_parse_sequence_to_array(&bad, output.values,
		sizeof(output.values[0]), 2) < 0);
	assert(config_parse_sequence_to_array(&bad, output.values, 3, 2) < 0);
}

static void
test_ports(void)
{
	uint16_t ports[3] = { 0 };
	int32_t range = -1;
	Node value = scalar("1000+25");
	assert(config_parse_port_maybe_range_to_array(&value, ports,
		&range, 3) == 1);
	assert(ports[0] == 1000 && range == 25);

	Node second = scalar("443");
	Node first = scalar("80");
	first.next = &second;
	Node sequence = { .type = NODE_SEQUENCE, .child = &first };
	assert(config_parse_port_maybe_range_to_array(&sequence, ports,
		&range, 3) == 2);
	assert(ports[0] == 80 && ports[1] == 443 && range == 0);

	Node bad = scalar("70000");
	assert(config_parse_port_maybe_range_to_array(&bad, ports,
		&range, 3) < 0);
}

static void
test_ipv4(void)
{
	uint32_t addresses[3] = { 0 };
	int64_t range = -1;
	Node value = scalar("192.0.2.10+16");
	assert(config_parse_ipv4_maybe_range_to_array(&value, addresses,
		&range, 3) == 1);
	assert(range == 16);
	char text[INET_ADDRSTRLEN];
	assert(inet_ntop(AF_INET, &addresses[0], text, sizeof(text)));
	assert(strcmp(text, "192.0.2.10") == 0);

	Node second = scalar("198.51.100.2");
	Node first = scalar("198.51.100.1");
	first.next = &second;
	Node sequence = { .type = NODE_SEQUENCE, .child = &first };
	assert(config_parse_ipv4_maybe_range_to_array(&sequence, addresses,
		&range, 3) == 2 && range == 0);
	assert(config_parse_sequence_ipv4_to_array(&sequence, addresses,
		sizeof(addresses[0]), 3) == 2);

	Node bad = scalar("300.1.1.1");
	assert(config_parse_ipv4_maybe_range_to_array(&bad, addresses,
		&range, 3) < 0);
}

static void
test_vtep_vni(void)
{
	uint32_t addresses[4] = { 0 };
	uint32_t vnis[4] = { 0 };
	Node value = scalar("203.0.113.8:100+3");
	assert(config_parse_sequence_ipv4_vni_to_array(&value, addresses,
		vnis, sizeof(addresses[0]), 4) == 3);
	assert(addresses[0] == addresses[1] && addresses[1] == addresses[2]);
	assert(vnis[0] == 100 && vnis[1] == 101 && vnis[2] == 102);

	Node second = scalar("203.0.113.2:20+3");
	Node first = scalar("203.0.113.1:10");
	first.next = &second;
	Node sequence = { .type = NODE_SEQUENCE, .child = &first };
	assert(config_parse_sequence_ipv4_vni_to_array(&sequence, addresses,
		vnis, sizeof(addresses[0]), 3) == 3);
	assert(vnis[0] == 10 && vnis[1] == 20 && vnis[2] == 21);

	const char *invalid[] = {
		"203.0.113.1", "bad:10", "203.0.113.1:",
		"203.0.113.1:10+0", "203.0.113.1:10+junk",
		"203.0.113.1:4294967295+2",
	};
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		Node bad = scalar(invalid[i]);
		assert(config_parse_sequence_ipv4_vni_to_array(&bad, addresses,
			vnis, sizeof(addresses[0]), 4) < 0);
	}
}

int
main(void)
{
	test_integer_arrays();
	test_ports();
	test_ipv4();
	test_vtep_vni();
	puts("config array parsing: PASS");
	return 0;
}
