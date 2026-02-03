#include "cnode.h"
#include "color.h"

/**
 * helper
 */

// 打印缩进
static void print_indent(int depth)
{
	for (int i = 0; i < depth; i++) {
		printf("  ");
	}
}

// 打印 MAC 地址
static void print_mac(const uint8_t *mac)
{
	printf("%02x:%02x:%02x:%02x:%02x:%02x",
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// 打印 IP 地址（网络字节序）
static void print_ip(uint32_t ip)
{
	struct in_addr addr;
	addr.s_addr = ip;
	printf("%s", inet_ntoa(addr));
}

// 打印数组（通用）
static void print_array_u16(const char *name, const uint16_t *arr,
		uint16_t count, int32_t range, int depth)
{
	print_indent(depth);

	if (!count) {
		range += range ? 0 : 1;
		/* range case */
		printf("%s[%d]: [ ", name, range);
		printf("%u", arr[0]);
		printf(",%s%d )\n", range > 0 ? " +" : " ", range);
		return;
	}

	/* array case */
	printf("%s[%u]: [ ", name, count);
	for (uint16_t i = 0; i < count && i < 10; i++) {
		printf("%u%s", arr[i], (i < count - 1) ? ", " : "");
	}
	if (count > 10) {
		printf(" ... (%u more)", count - 10);
	}
	printf(" ]\n");
}

static void print_array_u32(const char *name, const uint32_t *arr, uint16_t count, int depth)
{
	if (count == 0) return;

	print_indent(depth);
	printf("%s[%u]: [ ", name, count);
	for (uint16_t i = 0; i < count && i < 10; i++) {
		printf("0x%08x%s", arr[i], (i < count - 1) ? ", " : "");
	}
	if (count > 10) {
		printf(" ... (%u more)", count - 10);
	}
	printf(" ]\n");
}

static void print_ip_array(const char *name, const uint32_t *arr,
		uint16_t count, int64_t range, int depth)
{
	// if (count == 0) return;

	print_indent(depth);

	if (!count) {
		range += range ? 0 : 1;
		/* range case */
		printf("%s[%ld]: [ ", name, range);
		print_ip(arr[0]);
		printf(",%s%ld )\n", range > 0 ? " +" : " ", range);
		return;
	}

	/* array case */
	printf("%s[%u]: [ ", name, count);
	for (uint16_t i = 0; i < count && i < 10; i++) {
		print_ip(arr[i]);
		if (i < count - 1) {
			printf(", ");
		}
	}
	if (count > 10) {
		printf(" ... (%u more)", count - 10);
	}
	printf(" ]\n");
}

// ============================================
// 主 dump 函数
// ============================================

void cnode_show(const Cnode *node, int depth)
{
	if (!node) {
		printf("Cnode is NULL\n");
		return;
	}

	printf(ANSI_BOLD C_LIME "8< --------\n" ANSI_RESET);
	print_indent(depth);
	printf("Cnode {\n");
	depth++;

	// 顶层字段
	print_indent(depth);
	printf("offload: 0x%016" PRIx64 "\n", node->offload);

	// ========================================
	// ether
	// ========================================
	print_indent(depth);
	printf("ether: {\n");
	depth++;

	print_indent(depth);
	printf("mtu: %u\n", node->ether.mtu);

	print_indent(depth);
	printf("copy_payload: %u\n", node->ether.copy_payload);

	print_indent(depth);
	printf("dst: ");
	print_mac(node->ether.dst);
	printf(" (n_dst: %u)\n", node->ether.n_dst);

	print_indent(depth);
	printf("src: ");
	print_mac(node->ether.src);
	printf(" (n_src: %u)\n", node->ether.n_src);

	// ether.type
	print_indent(depth);
	printf("type: {\n");
	depth++;

	// ether.type.arp
	print_indent(depth);
	printf("arp: {\n");
	depth++;
	print_ip_array("src", node->ether.type.arp.src, node->ether.type.arp.n_src,
			-1, depth);
	print_ip_array("dst", node->ether.type.arp.dst, node->ether.type.arp.n_dst,
			-1, depth);
	depth--;
	print_indent(depth);
	printf("}\n");

	// ether.type.ipv4
	print_indent(depth);
	printf("ipv4: {\n");
	depth++;

	print_ip_array("src", node->ether.type.ipv4.src, node->ether.type.ipv4.n_src,
			node->ether.type.ipv4.src_range, depth);
	print_ip_array("dst", node->ether.type.ipv4.dst, node->ether.type.ipv4.n_dst,
			node->ether.type.ipv4.dst_range, depth);

	print_indent(depth);
	printf("src_range: %" PRId64 "\n", node->ether.type.ipv4.src_range);
	print_indent(depth);
	printf("dst_range: %" PRId64 "\n", node->ether.type.ipv4.dst_range);

	// proto 数组中只打印前几个
	if (node->ether.type.ipv4.proto[0] != 0) {
		print_indent(depth);
		printf("proto: [ ");
		for (int i = 0; i < 10 && node->ether.type.ipv4.proto[i] != 0; i++) {
			printf("0x%04x ", node->ether.type.ipv4.proto[i]);
		}
		printf("]\n");
	}

	// ether.type.ipv4.icmp
	print_indent(depth);
	printf("icmp: {\n");
	depth++;
	/* icmp, only array supported */
	print_array_u16("ident", node->ether.type.ipv4.icmp.ident,
			node->ether.type.ipv4.icmp.n_ident, 0, depth);
	if (node->ether.type.ipv4.icmp.payload) {
		print_indent(depth);
		printf("payload: \"%.*s\" (len: %u)\n",
				node->ether.type.ipv4.icmp.payload_len,
				node->ether.type.ipv4.icmp.payload,
				node->ether.type.ipv4.icmp.payload_len);
	}
	depth--;
	print_indent(depth);
	printf("}\n");

	// ether.type.ipv4.tcp
	print_indent(depth);
	printf("tcp: {\n");
	depth++;
	print_array_u16("src", node->ether.type.ipv4.tcp.src,
			node->ether.type.ipv4.tcp.n_src,
			node->ether.type.ipv4.tcp.src_range,
			depth);
	print_array_u16("dst", node->ether.type.ipv4.tcp.dst,
			node->ether.type.ipv4.tcp.n_dst,
			node->ether.type.ipv4.tcp.dst_range,
			depth);
	print_indent(depth);
	printf("src_range: %d\n", node->ether.type.ipv4.tcp.src_range);
	print_indent(depth);
	printf("dst_range: %d\n", node->ether.type.ipv4.tcp.dst_range);
	if (node->ether.type.ipv4.tcp.payload) {
		print_indent(depth);
		printf("payload: \"%.*s\" (len: %u)\n",
				node->ether.type.ipv4.tcp.payload_len,
				node->ether.type.ipv4.tcp.payload,
				node->ether.type.ipv4.tcp.payload_len);
	}
	depth--;
	print_indent(depth);
	printf("}\n");

	// ether.type.ipv4.udp
	print_indent(depth);
	printf("udp: {\n");
	depth++;
	print_array_u16("src", node->ether.type.ipv4.udp.src,
			node->ether.type.ipv4.udp.n_src,
			node->ether.type.ipv4.udp.src_range,
			depth);
	print_array_u16("dst", node->ether.type.ipv4.udp.dst,
			node->ether.type.ipv4.udp.n_dst,
			node->ether.type.ipv4.udp.dst_range,
			depth);
	print_indent(depth);
	printf("src_range: %d\n", node->ether.type.ipv4.udp.src_range);
	print_indent(depth);
	printf("dst_range: %d\n", node->ether.type.ipv4.udp.dst_range);
	if (node->ether.type.ipv4.udp.payload) {
		print_indent(depth);
		printf("payload: \"%.*s\" (len: %u)\n",
				node->ether.type.ipv4.udp.payload_len,
				node->ether.type.ipv4.udp.payload,
				node->ether.type.ipv4.udp.payload_len);
	}
	depth--;
	print_indent(depth);
	printf("}\n");

	depth--; // ipv4
	print_indent(depth);
	printf("}\n");

	depth--; // type
	print_indent(depth);
	printf("}\n");

	depth--; // ether
	print_indent(depth);
	printf("}\n");

	// ========================================
	// vxlan
	// ========================================
	print_indent(depth);
	printf("vxlan: {\n");
	depth++;

	print_indent(depth);
	printf("enable: %u\n", node->vxlan.enable);
	print_indent(depth);
	printf("ratio: %u\n", node->vxlan.ratio);

	print_indent(depth);
	printf("ether: {\n");
	depth++;

	print_indent(depth);
	printf("dst: ");
	print_mac(node->vxlan.ether.dst);
	printf(" (n_dst: %u)\n", node->vxlan.ether.n_dst);

	print_indent(depth);
	printf("src: ");
	print_mac(node->vxlan.ether.src);
	printf(" (n_src: %u)\n", node->vxlan.ether.n_src);

	// vxlan.ether.type.ipv4
	print_indent(depth);
	printf("type: {\n");
	depth++;
	print_indent(depth);
	printf("ipv4: {\n");
	depth++;

	print_ip_array("src", node->vxlan.ether.type.ipv4.src,
			node->vxlan.ether.type.ipv4.n_src,
			node->vxlan.ether.type.ipv4.src_range,
			depth);
	print_ip_array("dst", node->vxlan.ether.type.ipv4.dst,
			node->vxlan.ether.type.ipv4.n_dst,
			node->vxlan.ether.type.ipv4.dst_range,
			depth);
	/* currently, only 1 supported */
	print_array_u32("vni", node->vxlan.ether.type.ipv4.vni,
			node->vxlan.ether.type.ipv4.n_src, depth);

	print_indent(depth);
	printf("n_src: %" PRId16 "\n", node->vxlan.ether.type.ipv4.n_src);
	print_indent(depth);
	printf("n_dst: %" PRId16 "\n", node->vxlan.ether.type.ipv4.n_dst);

	print_indent(depth);
	printf("src_range: (not supported)\n");
	print_indent(depth);
	printf("dst_range: (not supported)\n");

	// vxlan.ether.type.ipv4.udp
	print_indent(depth);
	printf("udp: {\n");
	depth++;
	/* vxlan udp range not supported */
	print_array_u16("src", node->vxlan.ether.type.ipv4.udp.src,
			node->vxlan.ether.type.ipv4.udp.n_src,
			-1, depth);
	print_array_u16("dst", node->vxlan.ether.type.ipv4.udp.dst,
			node->vxlan.ether.type.ipv4.udp.n_dst,
			-1, depth);
	print_indent(depth);
	printf("src_range: %d\n", node->vxlan.ether.type.ipv4.udp.src_range);
	print_indent(depth);
	printf("dst_range: %d\n", node->vxlan.ether.type.ipv4.udp.dst_range);
	if (node->vxlan.ether.type.ipv4.udp.payload) {
		print_indent(depth);
		printf("payload: \"%.*s\" (len: %u)\n",
				node->vxlan.ether.type.ipv4.udp.payload_len,
				node->vxlan.ether.type.ipv4.udp.payload,
				node->vxlan.ether.type.ipv4.udp.payload_len);
	}
	depth--;
	print_indent(depth);
	printf("}\n");

	depth--; // ipv4
	print_indent(depth);
	printf("}\n");
	depth--; // type
	print_indent(depth);
	printf("}\n");

	depth--; // ether
	print_indent(depth);
	printf("}\n");

	depth--; // vxlan
	print_indent(depth);
	printf("}\n");

	// ========================================
	// erroneous
	// ========================================
	print_indent(depth);
	printf("erroneous: {\n");
	depth++;

	print_indent(depth);
	printf("ratio: %u\n", node->erroneous.ratio);
	print_indent(depth);
	printf("n_mutation: %u\n", node->erroneous.n_mutation);
	print_indent(depth);
	printf("func: %p\n", (void *)node->erroneous.func);

	print_indent(depth);
	printf("classes[%u]: [\n", node->erroneous.n_clas);
	depth++;
	for (uint16_t i = 0; i < node->erroneous.n_clas; i++) {
		print_indent(depth);
		printf("[%u] {\n", i);
		depth++;

		print_indent(depth);
		printf("name: \"%s\"\n",
				node->erroneous.clas[i].name ? node->erroneous.clas[i].name : "(null)");

		print_indent(depth);
		printf("types[%u]: [ ", node->erroneous.clas[i].n_type);
		for (uint16_t j = 0; j < node->erroneous.clas[i].n_type && j < 10; j++) {
			printf("\"%s\"%s",
					node->erroneous.clas[i].type[j] ? node->erroneous.clas[i].type[j] : "(null)",
					(j < node->erroneous.clas[i].n_type - 1) ? ", " : "");
		}
		if (node->erroneous.clas[i].n_type > 10) {
			printf(" ... (%u more)", node->erroneous.clas[i].n_type - 10);
		}
		printf(" ]\n");

		depth--;
		print_indent(depth);
		printf("}\n");
	}
	depth--;
	print_indent(depth);
	printf("]\n");

	depth--; // erroneous
	print_indent(depth);
	printf("}\n");

	depth--; // Cnode
	print_indent(depth);
	printf("}\n");
	printf(ANSI_BOLD C_LIME "-------- >8\n" ANSI_RESET);
}

// ============================================
// 简化版 dump（只显示关键字段）
// ============================================
void cnode_show_summary(const Cnode *node)
{
	if (!node) {
		printf("Cnode is NULL\n");
		return;
	}

	printf(ANSI_BOLD C_MINT "=== Cnode Summary ===\n" ANSI_RESET);
	printf("offload: 0x%016" PRIx64 "\n", node->offload);

	printf("\nEther:\n");
	printf("  mtu: %u, copy_payload: %u\n",
			node->ether.mtu, node->ether.copy_payload);
	printf("  dst MAC: ");
	print_mac(node->ether.dst);
	printf("\n  src MAC: ");
	print_mac(node->ether.src);
	printf("\n");

	printf("\n  ARP: %u src IPs, %u dst IPs\n",
			node->ether.type.arp.n_src, node->ether.type.arp.n_dst);

	printf("  IPv4: %ld src IPs, %ld dst IPs\n",
			(long)(node->ether.type.ipv4.n_src ?
			node->ether.type.ipv4.n_src :
			labs(node->ether.type.ipv4.src_range)),
			(long)(node->ether.type.ipv4.n_dst ?
			node->ether.type.ipv4.n_dst :
			labs(node->ether.type.ipv4.dst_range)));
	printf("    ICMP: %u identifiers, payload_len: %u\n",
			node->ether.type.ipv4.icmp.n_ident,
			node->ether.type.ipv4.icmp.payload_len);
	printf("    TCP: %u src ports, %u dst ports, payload_len: %u\n",
			(node->ether.type.ipv4.tcp.n_src ?
			node->ether.type.ipv4.tcp.n_src :
			abs(node->ether.type.ipv4.tcp.src_range)),
			(node->ether.type.ipv4.tcp.n_dst ?
			node->ether.type.ipv4.tcp.n_dst :
			abs(node->ether.type.ipv4.tcp.dst_range)),
			node->ether.type.ipv4.tcp.payload_len);
	printf("    UDP: %u src ports, %u dst ports, payload_len: %u\n",
			(node->ether.type.ipv4.udp.n_src ?
			node->ether.type.ipv4.udp.n_src :
			abs(node->ether.type.ipv4.udp.src_range)),
			(node->ether.type.ipv4.udp.n_dst ?
			node->ether.type.ipv4.udp.n_dst :
			abs(node->ether.type.ipv4.udp.dst_range)),
			node->ether.type.ipv4.udp.payload_len);

	printf("\nVXLAN:\n");
	printf("  enable: %u, ratio: %u\n",
			node->vxlan.enable, node->vxlan.ratio);
	printf("  IPv4: %u src IPs, %u dst IPs\n",
			node->vxlan.ether.type.ipv4.n_src,
			node->vxlan.ether.type.ipv4.n_dst);
	printf("  UDP: %u src ports, %u dst ports\n",
			node->vxlan.ether.type.ipv4.udp.n_src,
			node->vxlan.ether.type.ipv4.udp.n_dst);

	printf("\nErroneous:\n");
	printf("  ratio: %u, n_mutation: %u, n_clas: %u\n",
			node->erroneous.ratio,
			node->erroneous.n_mutation,
			node->erroneous.n_clas);

	printf(C_MINT "===================\n" ANSI_RESET);
}
