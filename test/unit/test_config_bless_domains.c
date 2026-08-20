#include "config_bless_internal.h"
#include "config_node.h"
#include "config_yaml.h"
#include "log.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_log_format_json;
static const theme_config test_theme = {
	.name = "test",
	.green = "", .yellow = "", .red = "", .blue = "",
	.purple = "", .grey = "",
};
const theme_config *g_current_theme = &test_theme;

void
bless_set_type_weight(const char *name, int32_t weight)
{
	(void)name;
	(void)weight;
}

static Cnode
parse_domains(const char *yaml)
{
	Node *root = config_yaml_parse_memory((const unsigned char *)yaml,
		strlen(yaml));
	assert(root);
	Node *bless_node = find_by_path(root, "bless");
	Node *ether_node = find_by_path(bless_node, "ether");
	assert(bless_node && ether_node);
	Cnode cnode = { 0 };
	assert(config_parse_bless_ether_all(bless_node, ether_node, &cnode) == 0);
	assert(config_parse_bless_vxlan(bless_node, &cnode) >= 0);
	config_yaml_free(root);
	return cnode;
}

static void
free_payloads(Cnode *cnode)
{
	free(cnode->ether.type.ipv4.icmp.payload);
	free(cnode->ether.type.ipv4.tcp.payload);
	free(cnode->ether.type.ipv4.udp.payload);
}

static void
parse_config_file(const char *path)
{
	FILE *fp = fopen(path, "rb");
	assert(fp);
	assert(fseek(fp, 0, SEEK_END) == 0);
	long length = ftell(fp);
	assert(length > 0 && fseek(fp, 0, SEEK_SET) == 0);
	char *yaml = malloc((size_t)length + 1);
	assert(yaml);
	assert(fread(yaml, 1, (size_t)length, fp) == (size_t)length);
	yaml[length] = '\0';
	assert(fclose(fp) == 0);
	Cnode cnode = parse_domains(yaml);
	free_payloads(&cnode);
	free(yaml);
}

int
main(void)
{
	static const char inner[] =
		"bless:\n"
		"  ether:\n"
		"    dst: '02:00:00:00:00:02'\n"
		"    src: '02:00:00:00:00:01'\n"
		"    imix: [64, 512]\n"
		"    type:\n"
		"      ipv4:\n"
		"        src: '10.0.0.1+16'\n"
		"        dst: [192.0.2.1, 192.0.2.2]\n"
		"        tcp:\n"
		"          src: '10000+8'\n"
		"          dst: 443\n"
		"          payload: tcp-test\n"
		"        udp:\n"
		"          src: [20000, 20001]\n"
		"          dst: 53\n"
		"  vxlan:\n"
		"    enable: false\n"
		"    ratio: 0\n";
	Cnode cnode = parse_domains(inner);
	assert(cnode.ether.n_dst == 1 && cnode.ether.n_src == 1);
	assert(cnode.ether.n_imix == 2);
	assert(cnode.ether.type.ipv4.src_range == 16);
	assert(cnode.ether.type.ipv4.n_dst == 2);
	assert(cnode.ether.type.ipv4.tcp.src_range == 8);
	assert(cnode.ether.type.ipv4.udp.n_src == 2);
	assert(cnode.vxlan.enable == 0);
	free_payloads(&cnode);

	static const char vxlan4[] =
		"bless:\n"
		"  ether:\n"
		"    dst: '02:00:00:00:00:02'\n"
		"    type:\n"
		"      ipv4:\n"
		"        src: 10.0.0.1\n"
		"        dst: 10.0.0.2\n"
		"  vxlan:\n"
		"    enable: true\n"
		"    ratio: 100\n"
		"    wire-mtu: 1500\n"
		"    ether:\n"
		"      dst: '02:00:00:00:00:03'\n"
		"      type:\n"
		"        ipv4:\n"
		"          src: '172.16.0.1:100+3'\n"
		"          dst: [172.16.1.1]\n"
		"          udp:\n"
		"            src: 45000\n"
		"            dst: 4789\n";
	cnode = parse_domains(vxlan4);
	assert(cnode.vxlan.enable == 1 && cnode.vxlan.ratio == 100);
	assert(cnode.vxlan.ether.type.ipv4.n_src == 3);
	assert(cnode.vxlan.ether.type.ipv4.vni[0] == 100);
	assert(cnode.vxlan.ether.type.ipv4.vni[2] == 102);
	assert(cnode.vxlan.ether.type.ipv4.udp.n_dst == 1);
	free_payloads(&cnode);

	static const char vxlan6[] =
		"bless:\n"
		"  ether:\n"
		"    dst: '02:00:00:00:00:02'\n"
		"    type:\n"
		"      ipv4:\n"
		"        src: 10.0.0.1\n"
		"        dst: 10.0.0.2\n"
		"  vxlan:\n"
		"    enable: true\n"
		"    ratio: 50\n"
		"    outer_ipv6: true\n"
		"    ether:\n"
		"      type:\n"
		"        ipv6:\n"
		"          src: 'fd00:10::1+5'\n"
		"          dst: 'fd00:20::100+5'\n"
		"          udp:\n"
		"            src: 45000\n"
		"            dst: 4789\n";
	cnode = parse_domains(vxlan6);
	assert(cnode.vxlan.outer_ipv6 == 1);
	assert(cnode.vxlan.ether.type.ipv6.src_range == 5);
	assert(cnode.vxlan.ether.type.ipv6.dst_range == 5);
	assert(cnode.vxlan.ether.type.ipv6.udp.n_src == 1);
	free_payloads(&cnode);

	static const char *configs[] = {
		"conf/config-ci.yaml",
		"conf/config-test.yaml",
		"conf/config-t10-vxlan.yaml",
		"conf/config-t11-full.yaml",
		"conf/config-vxlan6.yaml",
	};
	for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++)
		parse_config_file(configs[i]);

	puts("config bless domains: PASS");
	return 0;
}
