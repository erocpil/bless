#include "bless.h"
#include "worker.h"
#include "metrics.h"
#include "config.h"

struct bless_conf *bconf = NULL;

/* mask of enabled ports */
static uint32_t l2fwd_enabled_port_mask = 0;

// volatile static struct port_statistics *port_statistics[RTE_MAX_ETHPORTS];

#define MAX_TIMER_PERIOD 86400 /* 1 day max */
/* A tsc-based timer responsible for triggering statistics printout */
uint64_t timer_period = 1; /* default period is 10 seconds */

static volatile bool force_quit;

struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];

void register_my_metrics(void)
{
	rte_telemetry_register_cmd("/bless/arp", bless_handle_arp,
			"Returns `arp' metrics for BLESS");
	rte_telemetry_register_cmd("/bless/icmp", bless_handle_icmp,
			"Returns `icmp' metrics for BLESS");
	rte_telemetry_register_cmd("/bless/udp", bless_handle_udp,
			"Returns `udp' metrics for BLESS");
	rte_telemetry_register_cmd("/bless/tcp", bless_handle_tcp,
			"Returns `tcp' metrics for BLESS");
	rte_telemetry_register_cmd("/bless/erroneous", bless_handle_erroneous,
			"Returns `erroneous' metrics for BLESS");
	rte_telemetry_register_cmd("/bless/vxlan", bless_handle_vxlan,
			"Returns `vxlan' metrics for BLESS");

	rte_telemetry_register_cmd("/myapp/metrics", my_custom_metrics_cb,
			"Returns custom metrics for myapp");
}

/* MAC updating enabled by default */
static int mac_updating = 1;

/* Ports set in promiscuous mode off by default. */
static int promiscuous_on;

static uint16_t nb_rxd = RX_DESC_DEFAULT;
static uint16_t nb_txd = TX_DESC_DEFAULT;

/* ethernet addresses of ports */
static struct rte_ether_addr l2fwd_ports_eth_addr[RTE_MAX_ETHPORTS];

/* list of enabled ports */
static uint32_t l2fwd_dst_ports[RTE_MAX_ETHPORTS];

struct port_pair_params {
#define NUM_PORTS	2
	uint16_t port[NUM_PORTS];
} __rte_cache_aligned;

static struct port_pair_params port_pair_params_array[RTE_MAX_ETHPORTS / 2];
static struct port_pair_params *port_pair_params;
static uint16_t nb_port_pair_params;

static unsigned int l2fwd_rx_queue_per_lcore = 1;

// static struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_ETHPORTS];

static struct rte_eth_conf port_conf = {
	.txmode = {
		.mq_mode = RTE_ETH_MQ_TX_NONE,
	},
};

struct rte_mempool * l2fwd_pktmbuf_pool = NULL;

#if 0
static void l2fwd_mac_updating(struct rte_mbuf *m, unsigned dest_portid)
{
	struct rte_ether_hdr *eth;
	void *tmp;

	eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

	/* 02:00:00:00:00:xx */
	tmp = &eth->dst_addr.addr_bytes[0];
	*((uint64_t *)tmp) = 0x000000000002 + ((uint64_t)dest_portid << 40);

	/* src addr */
	rte_ether_addr_copy(&l2fwd_ports_eth_addr[dest_portid], &eth->src_addr);
}
#endif

/* Simple forward. 8< */
#if 0
static void l2fwd_simple_forward(struct rte_mbuf *m, unsigned portid)
{
	unsigned dst_port;
	int sent;
	struct rte_eth_dev_tx_buffer *buffer;

	dst_port = l2fwd_dst_ports[portid];

	if (mac_updating)
		l2fwd_mac_updating(m, dst_port);

	buffer = tx_buffer[dst_port];
	sent = rte_eth_tx_buffer(dst_port, 0, buffer, m);
	if (sent)
		port_statistics[dst_port].tx += sent;
}
/* >8 End of simple forward. */
#endif

/* main processing loop */
#if 0
static void l2fwd_main_loop(void)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	struct rte_mbuf *m;
	int sent;
	unsigned lcore_id;
	uint64_t prev_tsc, diff_tsc, cur_tsc, timer_tsc;
	unsigned i, j, portid, nb_rx;
	struct lcore_queue_conf *qconf;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S *
		BURST_TX_DRAIN_US;
	struct rte_eth_dev_tx_buffer *buffer;

	prev_tsc = 0;
	timer_tsc = 0;

	lcore_id = rte_lcore_id();
	qconf = &lcore_queue_conf[lcore_id];

	if (qconf->n_rx_port == 0) {
		RTE_LOG(INFO, BLESS, "lcore %u has nothing to do\n", lcore_id);
		return;
	}

	RTE_LOG(INFO, BLESS, "entering main loop on lcore %u\n", lcore_id);

	for (i = 0; i < qconf->n_rx_port; i++) {

		portid = qconf->rx_port_list[i];
		RTE_LOG(INFO, BLESS, " -- lcoreid=%u portid=%u\n", lcore_id,
				portid);

	}

	while (!force_quit) {

		/* Drains TX queue in its main loop. 8< */
		cur_tsc = rte_rdtsc();

		/*
		 * TX burst queue drain
		 */
		diff_tsc = cur_tsc - prev_tsc;
		if (unlikely(diff_tsc > drain_tsc)) {

			for (i = 0; i < qconf->n_rx_port; i++) {

				portid = l2fwd_dst_ports[qconf->rx_port_list[i]];
				buffer = tx_buffer[portid];

				sent = rte_eth_tx_buffer_flush(portid, 0, buffer);
				if (sent)
					port_statistics[portid].tx += sent;

			}

			/* if timer is enabled */
			if (timer_period > 0) {

				/* advance the timer */
				timer_tsc += diff_tsc;

				/* if timer has reached its timeout */
				if (unlikely(timer_tsc >= timer_period)) {

					/* do this only on main core */
					if (lcore_id == rte_get_main_lcore()) {
						print_stats();
						/* reset the timer */
						timer_tsc = 0;
					}
				}
			}

			prev_tsc = cur_tsc;
		}
		/* >8 End of draining TX queue. */

		/* Read packet from RX queues. 8< */
		for (i = 0; i < qconf->n_rx_port; i++) {

			portid = qconf->rx_port_list[i];
			nb_rx = rte_eth_rx_burst(portid, 0,
					pkts_burst, MAX_PKT_BURST);

			if (unlikely(nb_rx == 0))
				continue;

			port_statistics[portid].rx += nb_rx;

			for (j = 0; j < nb_rx; j++) {
				m = pkts_burst[j];
				rte_prefetch0(rte_pktmbuf_mtod(m, void *));
				l2fwd_simple_forward(m, portid);
			}
		}
		/* >8 End of read packet from RX queues. */
	}
}
#endif

uint64_t pps = 0;
uint64_t bps = 0;
/* Print out statistics on packets dropped */
static void print_stats(struct port_statistics **port_statistics)
{
	uint64_t total_packets_tx_pkts = 0;
	uint64_t total_packets_tx_bytes = 0;
	uint64_t total_packets_rx = 0;
	uint64_t total_packets_dropped_pkts = 0;
	uint64_t total_packets_dropped_bytes = 0;
	unsigned portid;

	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };

	/* Clear screen and move to top left */
	printf("%s%s", clr, topLeft);

	printf("\nPort statistics ====================================");

	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
		/* skip disabled ports */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
			// printf("\nskipped %d\n", portid);
			continue;
		}
		if (!port_statistics[portid]) {
			continue;
		}
		for (int i = 0; i < TYPE_MAX; i++) {
			/*
			   printf("\nStatistics for port %u ------------------------------"
			   "\nPackets sent: %24"PRIu64
			   "\nBytes sent: %24"PRIu64
			   "\nPackets received: %20"PRIu64
			   "\nPackets dropped: %21"PRIu64
			   "\nBytes dropped: %21"PRIu64,
			   portid,
			   (port_statistics[portid] + i)->tx_pkts,
			   (port_statistics[portid] + i)->tx_bytes,
			   (port_statistics[portid] + i)->rx,
			   (port_statistics[portid] + i)->dropped_pkts,
			   (port_statistics[portid] + i)->dropped_bytes);
			   */
			total_packets_tx_pkts += (port_statistics[portid] + i)->tx_pkts;
			total_packets_tx_bytes += (port_statistics[portid] + i)->tx_bytes;
			total_packets_dropped_pkts += (port_statistics[portid] + i)->dropped_pkts;
			total_packets_dropped_bytes += (port_statistics[portid] + i)->dropped_bytes;
			total_packets_rx += (port_statistics[portid] + i)->rx;
		}
	}
	printf("\nAggregate statistics ==============================="
			"\nTotal packets sent: %18"PRIu64
			"\nTotal bytes sent: %18"PRIu64
			"\nTotal packets received: %14"PRIu64
			"\nTotal packets dropped: %15"PRIu64
			"\nTotal bytes dropped: %15"PRIu64,
			total_packets_tx_pkts, total_packets_tx_bytes,
			total_packets_rx,
			total_packets_dropped_pkts, total_packets_dropped_bytes);
	printf("\n====================================================\n");
	printf("PPS: %lu = %lu - %lu\n", total_packets_tx_pkts - pps, total_packets_tx_pkts, pps);
	printf("bPS: %lu = (%lu - %lu) * 8\n", (total_packets_tx_bytes - bps) << 3 , total_packets_tx_bytes, bps);
	pps = total_packets_tx_pkts;
	bps = total_packets_tx_bytes;

	fflush(stdout);
}

void main_loop(void *data)
{
	uint64_t prev_tsc = 0, diff_tsc = 0, cur_tsc = 0;
	uint64_t timer_tsc;
	// return;

	unsigned int lcore_id = rte_lcore_id();
	struct bless_conf *conf = (struct bless_conf*)data;

	uint64_t timer_period = conf->timer_period;

	printf("pthread_barrier_wait(&conf->barrier);");
	pthread_barrier_wait(conf->barrier);

	while (!force_quit) {

		// rte_delay_ms(100);
		// continue;

		cur_tsc = rte_rdtsc();
		diff_tsc = cur_tsc - prev_tsc;
		timer_tsc += diff_tsc;
		/* if timer has reached its timeout */
		if (unlikely(timer_tsc >= timer_period)) {
			/* do this only on main core */
			if (lcore_id == rte_get_main_lcore()) {
				print_stats(conf->stats);
				/* reset the timer */
				timer_tsc = 0;
			}
		}
		prev_tsc = cur_tsc;
		rte_delay_us(1000);
	}
}

static int bless_launch_one_lcore(void *conf)
{
	unsigned int lcore_id = rte_lcore_id();
	printf("lcore %d\n", lcore_id);

	if (lcore_id == rte_get_main_lcore()) {
		main_loop((void*)conf);
	} else {
		worker_loop_txonly(conf);
	}

	return 0;
}

/* display usage */
static void l2fwd_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK [-P] [-q NQ]\n"
			"  -p PORTMASK: hexadecimal bitmask of ports to configure\n"
			"  -P : Enable promiscuous mode\n"
			"  -q NQ: number of queue (=ports) per lcore (default is 1)\n"
			"  -T PERIOD: statistics will be refreshed each PERIOD seconds (0 to disable, 10 default, 86400 maximum)\n"
			"  --no-mac-updating: Disable MAC addresses updating (enabled by default)\n"
			"      When enabled:\n"
			"       - The source MAC address is replaced by the TX port MAC address\n"
			"       - The destination MAC address is replaced by 02:00:00:00:00:TX_PORT_ID\n"
			"  --portmap: Configure forwarding port pair mapping\n"
			"	      Default: alternate port pairs\n\n",
			prgname);
}

static int l2fwd_parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;

	return pm;
}

static int l2fwd_parse_port_pair_config(const char *q_arg)
{
	enum fieldnames {
		FLD_PORT1 = 0,
		FLD_PORT2,
		_NUM_FLD
	};
	unsigned long int_fld[_NUM_FLD];
	const char *p, *p0 = q_arg;
	char *str_fld[_NUM_FLD];
	unsigned int size;
	char s[256];
	char *end;
	int i;

	nb_port_pair_params = 0;

	while ((p = strchr(p0, '(')) != NULL) {
		++p;
		p0 = strchr(p, ')');
		if (p0 == NULL)
			return -1;

		size = p0 - p;
		if (size >= sizeof(s))
			return -1;

		memcpy(s, p, size);
		s[size] = '\0';
		if (rte_strsplit(s, sizeof(s), str_fld,
					_NUM_FLD, ',') != _NUM_FLD)
			return -1;
		for (i = 0; i < _NUM_FLD; i++) {
			errno = 0;
			int_fld[i] = strtoul(str_fld[i], &end, 0);
			if (errno != 0 || end == str_fld[i] ||
					int_fld[i] >= RTE_MAX_ETHPORTS)
				return -1;
		}
		if (nb_port_pair_params >= RTE_MAX_ETHPORTS/2) {
			printf("exceeded max number of port pair params: %hu\n",
					nb_port_pair_params);
			return -1;
		}
		port_pair_params_array[nb_port_pair_params].port[0] =
			(uint16_t)int_fld[FLD_PORT1];
		port_pair_params_array[nb_port_pair_params].port[1] =
			(uint16_t)int_fld[FLD_PORT2];
		++nb_port_pair_params;
	}
	port_pair_params = port_pair_params_array;
	return 0;
}

static unsigned int l2fwd_parse_nqueue(const char *q_arg)
{
	char *end = NULL;
	unsigned long n;

	/* parse hexadecimal string */
	n = strtoul(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;
	if (n == 0)
		return 0;
	if (n >= MAX_RX_QUEUE_PER_LCORE)
		return 0;

	return n;
}

static int l2fwd_parse_timer_period(const char *q_arg)
{
	char *end = NULL;
	int n;

	/* parse number string */
	n = strtol(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;
	if (n >= MAX_TIMER_PERIOD)
		return -1;

	return n;
}

static const char short_options[] =
"e::" /* erroneous */
"p:"  /* portmask */
"P"   /* promiscuous */
"q:"  /* number of queues */
"T:"  /* timer period */
;

#define CMD_LINE_OPT_NO_MAC_UPDATING "no-mac-updating"
#define CMD_LINE_OPT_PORTMAP_CONFIG "portmap"
#define CMD_LINE_OPT_NUM "num"
#define CMD_LINE_OPT_BATCH "batch"
#define CMD_LINE_OPT_ARP "arp"
#define CMD_LINE_OPT_ICMP "icmp"
#define CMD_LINE_OPT_TCP "tcp"
#define CMD_LINE_OPT_UDP "udp"
#define CMD_LINE_OPT_DST_MAC "dst-mac"
#define CMD_LINE_OPT_SRC_MAC "src-mac"
#define CMD_LINE_OPT_SRC_IP "src-ip"
#define CMD_LINE_OPT_DST_IP "dst-ip"
#define CMD_LINE_OPT_SRC_PORT "src-port"
#define CMD_LINE_OPT_DST_PORT "dst-port"
#define CMD_LINE_OPT_VXLAN "vxlan"

enum {
	/* long options mapped to a short option */

	/* first long only option value must be >= 256, so that we won't
	 * conflict with short options */
	CMD_LINE_OPT_NO_MAC_UPDATING_NUM = 256,
	CMD_LINE_OPT_PORTMAP_NUM,
	CMD_LINE_OPT_NUM_NUM,
	CMD_LINE_OPT_BATCH_NUM,
	CMD_LINE_OPT_ARP_NUM,
	CMD_LINE_OPT_ICMP_NUM,
	CMD_LINE_OPT_TCP_NUM,
	CMD_LINE_OPT_UDP_NUM,
	CMD_LINE_OPT_DST_MAC_NUM,
	CMD_LINE_OPT_SRC_MAC_NUM,
	CMD_LINE_OPT_SRC_IP_NUM,
	CMD_LINE_OPT_DST_IP_NUM,
	CMD_LINE_OPT_SRC_PORT_NUM,
	CMD_LINE_OPT_DST_PORT_NUM,
	CMD_LINE_OPT_VXLAN_NUM,
};

static const struct option lgopts[] = {
	{ CMD_LINE_OPT_NO_MAC_UPDATING, no_argument, 0, CMD_LINE_OPT_NO_MAC_UPDATING_NUM },
	{ CMD_LINE_OPT_PORTMAP_CONFIG, 1, 0, CMD_LINE_OPT_PORTMAP_NUM },
	{ CMD_LINE_OPT_NUM, 1, 0, CMD_LINE_OPT_NUM_NUM },
	{ CMD_LINE_OPT_BATCH, 1, 0, CMD_LINE_OPT_BATCH_NUM },
	{ CMD_LINE_OPT_ARP, 1, 0, CMD_LINE_OPT_ARP_NUM },
	{ CMD_LINE_OPT_ICMP, 1, 0, CMD_LINE_OPT_ICMP_NUM },
	{ CMD_LINE_OPT_TCP, 1, 0, CMD_LINE_OPT_TCP_NUM },
	{ CMD_LINE_OPT_UDP, 1, 0, CMD_LINE_OPT_UDP_NUM },
	{ CMD_LINE_OPT_DST_MAC, 1, 0, CMD_LINE_OPT_DST_MAC_NUM },
	{ CMD_LINE_OPT_SRC_MAC, 1, 0, CMD_LINE_OPT_SRC_MAC_NUM },
	{ CMD_LINE_OPT_SRC_IP, 1, 0, CMD_LINE_OPT_SRC_IP_NUM },
	{ CMD_LINE_OPT_DST_IP, 1, 0, CMD_LINE_OPT_DST_IP_NUM },
	{ CMD_LINE_OPT_SRC_PORT, 1, 0, CMD_LINE_OPT_SRC_PORT_NUM },
	{ CMD_LINE_OPT_DST_PORT, 1, 0, CMD_LINE_OPT_DST_PORT_NUM },
	{ CMD_LINE_OPT_VXLAN, 1, 0, CMD_LINE_OPT_VXLAN_NUM },
	{NULL, 0, 0, 0}
};

/* Parse the argument given in the command line of the application */
static int bless_parse_args(int argc, char **argv)
{
	int opt, ret, timer_secs;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];

	struct dist_ratio ratio;

	struct bless_encap_params bep = { NULL, NULL };
	bep.inner = rte_malloc(NULL, sizeof(struct mbuf_conf), 0);
	struct mbuf_conf *mc_inner = bep.inner;

	argvopt = argv;
	port_pair_params = NULL;

	dist_ratio_init(&ratio);

	while ((opt = getopt_long(argc, argvopt, short_options,
					lgopts, &option_index)) != EOF) {

		switch (opt) {
			/* portmask */
			case 'p':
				l2fwd_enabled_port_mask = l2fwd_parse_portmask(optarg);
				printf("l2fwd_enabled_port_mask %d\n", l2fwd_enabled_port_mask);
				if (l2fwd_enabled_port_mask == 0) {
					printf("invalid portmask\n");
					l2fwd_usage(prgname);
					return -1;
				}
				break;
			case 'P':
				promiscuous_on = 1;
				break;

				/* nqueue */
			case 'q':
				l2fwd_rx_queue_per_lcore = l2fwd_parse_nqueue(optarg);
				if (l2fwd_rx_queue_per_lcore == 0) {
					printf("invalid queue number\n");
					l2fwd_usage(prgname);
					return -1;
				}
				break;
			case 'e':
				ratio.weight[TYPE_ERRONEOUS] = bless_parse_type(TYPE_ERRONEOUS, optarg);
				printf("TYPE_ERRONEOUS %d\n", ratio.weight[TYPE_ERRONEOUS]);
				break;

			case 'T': /* timer period */
				timer_secs = l2fwd_parse_timer_period(optarg);
				if (timer_secs < 0) {
					printf("invalid timer period\n");
					l2fwd_usage(prgname);
					return -1;
				}
				timer_period = timer_secs;
				break;

				/* long options */
			case CMD_LINE_OPT_NUM_NUM:
				bconf->size = optarg ? atoi(optarg) : 0;
				ratio.num = optarg ? atoi(optarg) : 0;
				printf("bconf->size ratio.num %lu\n", bconf->size);
				// getchar();
				break;
			case CMD_LINE_OPT_BATCH_NUM:
				bconf->batch = optarg ? atoi(optarg) : 256;
				break;
			case CMD_LINE_OPT_ARP_NUM:
				ratio.weight[TYPE_ARP] = bless_parse_type(TYPE_ARP, optarg);
				printf("TYPE_ARP %d\n", ratio.weight[TYPE_ARP]);
				break;
			case CMD_LINE_OPT_ICMP_NUM:
				ratio.weight[TYPE_ICMP] = bless_parse_type(TYPE_ICMP, optarg);
				printf("TYPE_ICMP %d\n", ratio.weight[TYPE_ICMP]);
				break;
			case CMD_LINE_OPT_TCP_NUM:
				ratio.weight[TYPE_TCP] = bless_parse_type(TYPE_TCP, optarg);
				printf("TYPE_TCP %d\n", ratio.weight[TYPE_TCP]);
				break;
			case CMD_LINE_OPT_UDP_NUM:
				ratio.weight[TYPE_UDP] = bless_parse_type(TYPE_UDP, optarg);
				printf("TYPE_UDP %d\n", ratio.weight[TYPE_UDP]);
				break;
			case CMD_LINE_OPT_PORTMAP_NUM:
				ret = l2fwd_parse_port_pair_config(optarg);
				if (ret) {
					fprintf(stderr, "Invalid config\n");
					l2fwd_usage(prgname);
					return -1;
				}
				break;

			case CMD_LINE_OPT_NO_MAC_UPDATING_NUM:
				mac_updating = 0;
				break;
			case CMD_LINE_OPT_DST_MAC_NUM:
				printf("%s\n", CMD_LINE_OPT_DST_MAC);
				if (rte_ether_unformat_addr(optarg, &mc_inner->dst_addr) < 0) {
					rte_exit(EXIT_FAILURE, "Invalid mac address.\n");
				}
				strncpy((char*)mc_inner->str.dst_addr, optarg, 18);
				bless_print_mac(&mc_inner->dst_addr);
				printf("%s\n", mc_inner->str.dst_addr);
				break;
			case CMD_LINE_OPT_SRC_MAC_NUM:
				printf("%s\n", CMD_LINE_OPT_SRC_MAC);
				break;
			case CMD_LINE_OPT_SRC_IP_NUM:
				mc_inner->src_ip_range = bless_seperate_ip_range(optarg);
				printf("%s\n", CMD_LINE_OPT_SRC_IP);
				if (inet_pton(AF_INET, optarg, &mc_inner->src_ip) != 1) {
					rte_exit(EXIT_FAILURE, "Invalid ip address %s.\n", optarg);
				}
				strncpy((char*)mc_inner->str.src_ip, optarg, 16);
				bless_print_ipv4(mc_inner->src_ip);
				mc_inner->src_ip = rte_cpu_to_be_32(mc_inner->src_ip);
				printf("%s + %ld\n", mc_inner->str.src_ip, mc_inner->src_ip_range);
				break;
			case CMD_LINE_OPT_DST_IP_NUM:
				mc_inner->dst_ip_range = bless_seperate_ip_range(optarg);
				printf("%s\n", CMD_LINE_OPT_DST_IP);
				if (inet_pton(AF_INET, optarg, &mc_inner->dst_ip) != 1) {
					rte_exit(EXIT_FAILURE, "Invalid ip address %s.\n", optarg);
				}
				bless_print_ipv4(mc_inner->dst_ip);
				mc_inner->dst_ip = rte_cpu_to_be_32(mc_inner->dst_ip);
				strncpy((char*)mc_inner->str.dst_ip, optarg, 16);
				bless_print_ipv4(mc_inner->dst_ip);
				printf("%s + %ld\n", mc_inner->str.dst_ip, mc_inner->dst_ip_range);
				break;
			case CMD_LINE_OPT_SRC_PORT_NUM:
				mc_inner->src_port_range = bless_seperate_port_range(optarg);
				mc_inner->src_port = atoi(optarg);
				printf("src port %u + %d\n", mc_inner->src_port, mc_inner->src_port_range);
				break;
			case CMD_LINE_OPT_DST_PORT_NUM:
				mc_inner->dst_port_range = bless_seperate_port_range(optarg);
				mc_inner->dst_port = atoi(optarg);
				printf("dst port %u + %d\n", mc_inner->dst_port, mc_inner->dst_port_range);
				break;
			case CMD_LINE_OPT_VXLAN_NUM:
				{
					struct mbuf_conf *mc_outer = rte_zmalloc(NULL, sizeof(struct mbuf_conf), 0);
					char *arg = strdup(optarg);
					if (!arg) {
						/* FIXME */
						perror("strdup");
						return -1;
					}

					char *saveptr = NULL;
					char *token = strtok_r(arg, ",", &saveptr);
					if (token) {
						if (inet_pton(AF_INET, token, &mc_outer->src_ip) != 1) {
							fprintf(stderr, "Invalid src_ip: %s\n", token);
							free(arg);
							return -1;
						}
						/* TODO check */
					} else {
					}

					token = strtok_r(NULL, ",", &saveptr);
					if (token) {
						if (inet_pton(AF_INET, token, &mc_outer->dst_ip) != 1) {
							fprintf(stderr, "Invalid dst_ip: %s\n", token);
							free(arg);
							return -1;
						}
						/* TODO check */
					} else {
					}

					token = strtok_r(NULL, ",", &saveptr);
					if (token) {
						printf("vni+range %s\n", token);
						uint32_t vni = strtoul(token, NULL, 0);
						mc_outer->ratio_vni |= vni;
						printf("vni %d\n", vni);
						/* TODO check */
						while (*token != '+' && *token != '\0') {
							token++;
						}
						if ('+' == *token) {
							int32_t range = atoi(++token);
							mc_outer->range = range;
							printf("range %d\n", range);
							/* TODO check */
						}
					} else {
					}

					token = strtok_r(NULL, ",", &saveptr);
					if (token) {
						int32_t ratio = atoi(token);
						if (ratio < 0 || ratio > 100) {
							rte_exit(EXIT_FAILURE, "Invalid ratio %d\n", ratio);
						}
						mc_outer->ratio_vni |= ratio << 24;
						/* TODO check */
					} else {
						;
					}

					free(arg);

					printf("VXLAN config:\n");
					printf("  src_ip = %s\n", inet_ntoa(*(struct in_addr*)&mc_outer->src_ip));
					printf("  dst_ip = %s\n", inet_ntoa(*(struct in_addr*)&mc_outer->dst_ip));
					printf("  vni    = %u\n", mc_outer->ratio_vni & ((1 << 24) - 1));
					printf("  ratio    = %u\n", mc_outer->ratio_vni >> 24);
					printf("  vni    = %u\n", mc_outer->fields.vni);
					printf("  range    = %u\n", mc_outer->range);
					printf("  ratio    = %u\n", mc_outer->fields.ratio);

					bep.outer = mc_outer;
				}
				break;
			default:
				l2fwd_usage(prgname);
				return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind - 1;
	optind = 1; /* reset getopt lib */

	printf("bep %p %p\n\n", bep.inner, bep.outer);

	if (-EPERM == bless_set_dist(bconf, &ratio, &bep)) {
		rte_exit(EXIT_FAILURE, "Cannot bless_set_dist()\n");
	}

	return ret;
}

/*
 * Check port pair config with enabled port mask,
 * and for valid port pair combinations.
 */
	static int
check_port_pair_config(void)
{
	uint32_t port_pair_config_mask = 0;
	uint32_t port_pair_mask = 0;
	uint16_t index, i, portid;

	for (index = 0; index < nb_port_pair_params; index++) {
		port_pair_mask = 0;

		for (i = 0; i < NUM_PORTS; i++)  {
			portid = port_pair_params[index].port[i];
			if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
				printf("port %u is not enabled in port mask\n",
						portid);
				return -1;
			}
			if (!rte_eth_dev_is_valid_port(portid)) {
				printf("port %u is not present on the board\n",
						portid);
				return -1;
			}

			port_pair_mask |= 1 << portid;
		}

		if (port_pair_config_mask & port_pair_mask) {
			printf("port %u is used in other port pairs\n", portid);
			return -1;
		}
		port_pair_config_mask |= port_pair_mask;
	}

	l2fwd_enabled_port_mask &= port_pair_config_mask;

	return 0;
}

/* Check the link status of all ports in up to 9s, and print them finally */
	static void
check_all_ports_link_status(uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint16_t portid;
	uint8_t count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;
	int ret;
	char link_status_text[RTE_ETH_LINK_MAX_STR_LEN];

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (force_quit)
			return;
		all_ports_up = 1;
		RTE_ETH_FOREACH_DEV(portid) {
			if (force_quit)
				return;
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			ret = rte_eth_link_get_nowait(portid, &link);
			if (ret < 0) {
				all_ports_up = 0;
				if (print_flag == 1)
					printf("Port %u link get failed: %s\n",
							portid, rte_strerror(-ret));
				continue;
			}
			/* print link status if flag set */
			if (print_flag == 1) {
				rte_eth_link_to_str(link_status_text,
						sizeof(link_status_text), &link);
				printf("Port %d %s\n", portid,
						link_status_text);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == RTE_ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

	static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n",
				signum);
		force_quit = true;
	}
}

static int mbuf_dynfields_offset[MBUF_DYNFIELDS_MAX];

enum {
	MBUF_DYN_TYPE = 0,
};

typedef union {
	void *hdr;
	struct {
		uint64_t l2_len:RTE_MBUF_L2_LEN_BITS;           /* L2 Header Length */
		uint64_t l3_len:RTE_MBUF_L3_LEN_BITS;           /* L3 Header Length */
		uint64_t l4_len:RTE_MBUF_L4_LEN_BITS;           /* L4 Header Length */
		uint64_t outer_l2_len:RTE_MBUF_OUTL2_LEN_BITS;  /* Outer L2 Header Length */
		uint64_t outer_l3_len:RTE_MBUF_OUTL3_LEN_BITS;  /* Outer L3 Header Length */
	};
} mbuf_userdata_field_proto_t;

int mbuf_dynfield_init()
{
	const struct rte_mbuf_dynfield mbuf_bless_fields[] = {
		[ MBUF_DYN_TYPE ] = {
			.name = "type",
			.size = sizeof(char),
			.align = __alignof__(char),
		},
	};

	for (int i = 0; i < NELEMS(mbuf_bless_fields); i++) {
		if (mbuf_bless_fields[i].size == 0) { continue; }
		const struct rte_mbuf_dynfield *md = &mbuf_bless_fields[i];
		int offset = rte_mbuf_dynfield_register(md);
		if (offset < 0) {
			RTE_LOG(ERR, MBUF, "fail to register dynfield[%d %d] in mbuf!\n",
					i, rte_errno);
			rte_mbuf_dyn_dump(stdout);
			return rte_errno;
		}
		mbuf_dynfields_offset[i] = offset;
		printf("name %s size %lu align %lu\n", md->name, md->size, md->align);
	}

	rte_mbuf_dyn_dump(stdout);

	return 0;
}

int main(int argc, char **argv)
{
	int targc = argc;
	char **targv = argv;
	Node *conf_root = NULL;

	if (2 == argc && config_check_file(argv[1])) {
	}
	conf_root = config_init(argc, argv);
	config_parse_dpdk(conf_root, &targc, &targv);
	if (!conf_root) {
		rte_exit(EXIT_FAILURE, "Cannot parse config.yaml\n");
	}

	bconf = bless_init(targc, targv);
	if (!bconf) {
		rte_exit(EXIT_FAILURE, "Cannot rte_malloc(bless_conf)\n");
	}

	int ret = rte_eal_init(targc, targv);
	if (ret < 0) {
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	}
	targc -= ret;
	targv += ret;
	argv += ret;
	printf("%p %s %p %s\n", targv, *targv, argv, *argv);

#ifdef RTE_LIB_PDUMP
	ret = rte_pdump_init();
	if (ret) {
		rte_exit(EXIT_FAILURE, "rte_pdump_init()\n");
	}
	RTE_LOG(INFO, BLESS, "rte_pdump_init()\n");
#else
	RTE_LOG(INFO, BLESS, "rte_pdump not supported.\n");
#endif

	/* TODO rte_eth_dev_callback_register */

	mbuf_dynfield_init();

	register_my_metrics();

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* parse application arguments (after the EAL ones) */
	ret = bless_parse_args(targc, targv);
	if (ret < 0) {
		rte_exit(EXIT_FAILURE, "Invalid BLESS arguments\n");
	}
	/* >8 End of init EAL. */

	if (conf_root && !bconf->cnode) {
		bconf->cnode = config_parse_bless(conf_root);
		/*
		   if (bconf->cnode) {
		   printf("\n\n\ncnode %p\n\n\n\n", bconf->cnode);
		   for (int i = 0; i < bconf->cnode->erroneous.n_mutation; i++) {
		   printf("[%s %d] %d %p\n", __func__, __LINE__, i, bconf->cnode->erroneous.func[i]);
		   }
		   getchar();
		   }
		   */
	}

	printf("MAC updating %s\n", mac_updating ? "enabled" : "disabled");

	/* convert to number of cycles */
	timer_period *= rte_get_timer_hz();

	uint16_t nb_ports = rte_eth_dev_count_avail();
	if (nb_ports == 0) {
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");
	}

	if (port_pair_params != NULL) {
		if (check_port_pair_config() < 0)
			rte_exit(EXIT_FAILURE, "Invalid port pair config\n");
	}

	/* check port mask to possible port mask */
	if (l2fwd_enabled_port_mask & ~((1 << nb_ports) - 1)) {
		rte_exit(EXIT_FAILURE, "Invalid portmask; possible (0x%x)\n",
				(1 << nb_ports) - 1);
	}

	/* Initialization of the driver. 8< */

	/* reset l2fwd_dst_ports */
	uint16_t portid, last_port;
	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
		l2fwd_dst_ports[portid] = 0;
	}
	last_port = 0;

	/* populate destination port details */
	if (port_pair_params != NULL) {
		uint16_t idx, p;

		for (idx = 0; idx < (nb_port_pair_params << 1); idx++) {
			p = idx & 1;
			portid = port_pair_params[idx >> 1].port[p];
			l2fwd_dst_ports[portid] =
				port_pair_params[idx >> 1].port[p ^ 1];
		}
	} else {
		uint32_t nb_ports_in_mask = 0;
		RTE_ETH_FOREACH_DEV(portid) {
			/* skip ports that are not enabled */
			if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
				continue;
			}

			if (nb_ports_in_mask % 2) {
				l2fwd_dst_ports[portid] = last_port;
				l2fwd_dst_ports[last_port] = portid;
			} else {
				last_port = portid;
			}

			nb_ports_in_mask++;
		}
		if (nb_ports_in_mask % 2) {
			printf("Notice: odd number of ports in portmask.\n");
			l2fwd_dst_ports[last_port] = last_port;
		}
	}
	/* >8 End of initialization of the driver. */

	struct lcore_queue_conf *qconf = NULL;

	/* Initialize the port/queue configuration of each logical core */
	uint32_t rx_lcore_id = 0;
	uint32_t nb_lcores = 0;
	RTE_ETH_FOREACH_DEV(portid) {
		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
			continue;
		}

		/* get the lcore_id for this port */
		while (rx_lcore_id == rte_get_main_lcore() ||
				(rte_lcore_is_enabled(rx_lcore_id) == 0 ||
				 lcore_queue_conf[rx_lcore_id].n_rx_port ==
				 l2fwd_rx_queue_per_lcore)) {
			rx_lcore_id++;
			if (rx_lcore_id >= RTE_MAX_LCORE) {
				rte_exit(EXIT_FAILURE, "Not enough cores\n");
			}
		}

		if (qconf != &lcore_queue_conf[rx_lcore_id]) {
			/* Assigned a new logical core in the loop above. */
			qconf = &lcore_queue_conf[rx_lcore_id];
			nb_lcores++;
		}

		qconf->rx_port_list[qconf->n_rx_port] = portid;
		qconf->n_rx_port++;
		printf("Lcore %u: RX port %u TX port %u\n", rx_lcore_id,
				portid, l2fwd_dst_ports[portid]);
	}

	unsigned int nb_mbufs = RTE_MAX(nb_ports * (nb_rxd + nb_txd + MAX_PKT_BURST +
				nb_lcores * MEMPOOL_CACHE_SIZE), 8192U);

	/* Create the mbuf pool. 8< */
	l2fwd_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
			MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
			rte_socket_id());
	if (l2fwd_pktmbuf_pool == NULL) {
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");
	}
	/* >8 End of create the mbuf pool. */

	/* Initialise each port */
	uint16_t nb_ports_available = 0;
	RTE_ETH_FOREACH_DEV(portid) {
		struct rte_eth_rxconf rxq_conf;
		struct rte_eth_txconf txq_conf;
		struct rte_eth_conf local_port_conf = port_conf;
		struct rte_eth_dev_info dev_info;

		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
			printf("Skipping disabled port %u\n", portid);
			continue;
		}
		nb_ports_available++;

		/* init port */
		printf("Initializing port %u... ", portid);
		fflush(stdout);

		ret = rte_eth_dev_info_get(portid, &dev_info);
		if (ret != 0) {
			rte_exit(EXIT_FAILURE,
					"Error during getting device (port %u) info: %s\n",
					portid, strerror(-ret));
		}

		if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) {
			local_port_conf.txmode.offloads |=
				RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
		}
		/* Configure the number of queues for a port. */
		ret = rte_eth_dev_configure(portid, 1, 1, &local_port_conf);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
					ret, portid);
		}
		/* >8 End of configuration of the number of queues for a port. */

		ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd,
				&nb_txd);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE,
					"Cannot adjust number of descriptors: err=%d, port=%u\n",
					ret, portid);
		}

		ret = rte_eth_macaddr_get(portid,
				&l2fwd_ports_eth_addr[portid]);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE,
					"Cannot get MAC address: err=%d, port=%u\n",
					ret, portid);
		}

		/* init one RX queue */
		fflush(stdout);
		rxq_conf = dev_info.default_rxconf;
		rxq_conf.offloads = local_port_conf.rxmode.offloads;
		/* RX queue setup. 8< */
		ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
				rte_eth_dev_socket_id(portid),
				&rxq_conf,
				l2fwd_pktmbuf_pool);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
					ret, portid);
		}
		/* >8 End of RX queue setup. */

		/* Init one TX queue on each port. 8< */
		fflush(stdout);
		txq_conf = dev_info.default_txconf;
		txq_conf.offloads = local_port_conf.txmode.offloads;
		ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
				rte_eth_dev_socket_id(portid),
				&txq_conf);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
					ret, portid);
		}
		/* >8 End of init one TX queue on each port. */

		/* Initialize TX buffers */
		/*
		   tx_buffer[portid] = rte_zmalloc_socket("tx_buffer",
		   RTE_ETH_TX_BUFFER_SIZE(MAX_PKT_BURST), 0,
		   rte_eth_dev_socket_id(portid));
		   if (tx_buffer[portid] == NULL)
		   rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u\n",
		   portid);

		   rte_eth_tx_buffer_init(tx_buffer[portid], MAX_PKT_BURST);

		   ret = rte_eth_tx_buffer_set_err_callback(tx_buffer[portid],
		   rte_eth_tx_buffer_count_callback,
		   port_statistics[portid]->dropped);
		   if (ret < 0)
		   rte_exit(EXIT_FAILURE,
		   "Cannot set error callback for tx buffer on port %u\n",
		   portid);
		   */

		ret = rte_eth_dev_set_ptypes(portid, RTE_PTYPE_UNKNOWN, NULL,
				0);
		if (ret < 0) {
			printf("Port %u, Failed to disable Ptype parsing\n",
					portid);
		}
		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
					ret, portid);
		}

		printf("done: \n");
		if (promiscuous_on) {
			ret = rte_eth_promiscuous_enable(portid);
			if (ret != 0)
				rte_exit(EXIT_FAILURE,
						"rte_eth_promiscuous_enable:err=%s, port=%u\n",
						rte_strerror(-ret), portid);
		}

		printf("Port %u, MAC address: " RTE_ETHER_ADDR_PRT_FMT "\n\n",
				portid,
				RTE_ETHER_ADDR_BYTES(&l2fwd_ports_eth_addr[portid]));

		/* initialize port stats */
		// memset(port_statistics, 0, sizeof(port_statistics));
	}

	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE,
				"All available ports are disabled. Please set portmask.\n");
	}

	check_all_ports_link_status(l2fwd_enabled_port_mask);


	char name[16];
	pthread_getname_np(pthread_self(), name, sizeof(name));
	RTE_LOG(INFO, BLESS, "entering main loop %s: pid %d tid %d self %lu\n", name,
			getpid(), rte_gettid(), (unsigned long)rte_thread_self().opaque_id);

	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);

	// 获取当前线程 CPU 亲和性
	int s = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
	if (s != 0) {
		perror("pthread_getaffinity_np");
		pthread_exit(NULL);
	}

	printf("Thread running on CPUs: ");
	for (int i = 0; i < CPU_SETSIZE; i++) {
		if (CPU_ISSET(i, &cpuset)) {
			printf("%d ", i);
		}
	}
	printf("\n");

	bconf->qconf = lcore_queue_conf;
	bconf->stats = rte_malloc(NULL, sizeof(bconf->stats) * RTE_MAX_ETHPORTS, 0);
	// printf("port_statistics %p\n", port_statistics);
	bconf->dst_ports = l2fwd_dst_ports;
	bconf->force_quit = &force_quit;
	bconf->timer_period = timer_period;
	bconf->enabled_port_mask = l2fwd_enabled_port_mask;
	pthread_barrier_t barrier;
	pthread_barrier_init(&barrier, NULL, nb_lcores + 1); /* main lcore */
	bconf->barrier = &barrier;

	printf("\n\n >> %d nb_lcores %d\n\n", rte_lcore_id(), nb_lcores + 1);

	// DISTRIBUTION_DUMP(bconf->dist);

	ret = 0;
	/* launch per-lcore init on every lcore */
	uint32_t lcore_id = 0;
	rte_eal_mp_remote_launch(bless_launch_one_lcore, (void*)bconf, CALL_MAIN);
	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}

	RTE_ETH_FOREACH_DEV(portid) {
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;
		printf("Closing port %d...", portid);
		ret = rte_eth_dev_stop(portid);
		if (ret != 0)
			printf("rte_eth_dev_stop: err=%d, port=%d\n",
					ret, portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}

#ifdef RTE_LIB_PDUMP
	rte_pdump_uninit();
#endif

	pthread_barrier_destroy(&barrier);

	/* clean up the EAL */
	rte_eal_cleanup();

	config_exit(conf_root);

	printf("Bye...\n");

	return ret;
}
