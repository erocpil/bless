#include "bless.h"
#include "worker.h"
#include "metric.h"
#include "server.h"
#include "system.h"
#include "cJSON.h"
#ifdef VERSION
#include "version.h"
void print_version(void) {
	printf("Version Info:\n");
	printf("  Git Commit: %s\n", GIT_COMMIT);
	printf("  Git Branch: %s\n", GIT_BRANCH);
	printf("  Build Time: %s\n", BUILD_TIME);
}
#else
void print_version(void) {
	printf("No Version Info:\n");
}
#endif

#define DEFAULT_CONFIG_FILE "conf/config.yaml"
struct bless_conf *bconf = NULL;
struct config_file_map *cfm = NULL;
struct system_status sysstat;

/* mask of enabled ports */
static uint32_t enabled_port_mask = 0;
static uint32_t enabled_lcores = 0;

#define MAX_TIMER_PERIOD 86400 /* 1 day max */
/* A tsc-based timer responsible for triggering statistics printout */
static uint64_t timer_period = 1; /* default period is 10 seconds */

atomic_int g_state = STATE_INIT;

static struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];

void *metric_cbfn()
{
	return (void*)&sysstat;
}

void register_my_metrics(void)
{
	metric_set_cbfn(metric_cbfn);
	rte_telemetry_register_cmd("/bless/system", bless_handle_system,
			"Returns `system' metrics for BLESS");
}

/* MAC updating enabled by default */
static int mac_updating = 1;

/* Ports set in promiscuous mode off by default. */
static int promiscuous_on;

static uint16_t nb_rxd = RX_DESC_DEFAULT;
static uint16_t nb_txd = TX_DESC_DEFAULT;

/* ethernet addresses of ports */
static struct rte_ether_addr ports_eth_addr[RTE_MAX_ETHPORTS];

/* list of enabled ports */
static uint32_t dst_ports[RTE_MAX_ETHPORTS];

struct port_pair_params {
#define NUM_PORTS	2
	uint16_t port[NUM_PORTS];
} __rte_cache_aligned;

static struct port_pair_params port_pair_params_array[RTE_MAX_ETHPORTS / 2];
static struct port_pair_params *port_pair_params;
static uint16_t nb_port_pair_params;

static unsigned int rxtxq_per_port = 1;

// static struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_ETHPORTS];

static struct rte_eth_conf port_conf = {
	.txmode = {
		.mq_mode = RTE_ETH_MQ_TX_NONE,
		.offloads =
			RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |   /* IPv4校验和 */
			RTE_ETH_TX_OFFLOAD_UDP_CKSUM |    /* UDP校验和 */
			RTE_ETH_TX_OFFLOAD_TCP_CKSUM |    /* TCP校验和 */
			RTE_ETH_TX_OFFLOAD_OUTER_IPV4_CKSUM |
			RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM |
			// RTE_ETH_TX_OFFLOAD_UDP_TNL_TSO |
			// RTE_ETH_TX_OFFLOAD_VXLAN_TNL_TSO |
			// RTE_ETH_TX_OFFLOAD_VLAN_INSERT |  // VLAN插入
			// RTE_ETH_TX_OFFLOAD_MULTI_SEGS |   // 多段发送
			RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE, /* 快速释放mbuf */
	},
	.rx_adv_conf = {
		.rss_conf = {
			/* RSS（Receive Side Scaling）配置 */
			.rss_key = NULL, /* 使用默认 RSS hash key */
			.rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP,
		},
	},
	.rxmode = {
		.mq_mode = RTE_ETH_MQ_RX_RSS,
		.offloads =
			RTE_ETH_RX_OFFLOAD_IPV4_CKSUM |
			RTE_ETH_RX_OFFLOAD_UDP_CKSUM |
			RTE_ETH_RX_OFFLOAD_TCP_CKSUM |
			RTE_ETH_RX_OFFLOAD_TCP_LRO |
			// RTE_ETH_RX_OFFLOAD_OUTER_IPV4_CKSUM |
			RTE_ETH_RX_OFFLOAD_SCATTER,
	},
};

struct rte_mempool * rx_pktmbuf_pool = NULL;

void dpdk_generate_cmdReply(const char *str)
{
	if (!str || !strlen(str)) {
		return;
	}
	char *reply = encode_cmdReply_to_json(str);
	printf("cmdReply %s\n", reply);
	ws_broadcast_log(reply, strlen(reply));
	free(reply);
}

void dpdk_generate_log(const char *str)
{
	if (!str || !strlen(str)) {
		return;
	}
	char *msg = encode_log_to_json(str);
	printf("msg %s\n", msg);
	ws_broadcast_log(msg, strlen(msg));
	free(msg);
}

void dpdk_generate_stats(void)
{
	int active = stats_get_active_index();
	int inactive = active ^ 1;

	struct stats_snapshot *s = stats_get(inactive);

	char tmp[128];
	sprintf(tmp, "log: %lu", rte_rdtsc());
	char *msg = encode_stats_to_json(enabled_port_mask, tmp);
	/* JSON */
	s->json_len = snprintf(s->json, STATS_JSON_MAX, "%s\n", msg);
	free(msg);

	/* Prometheus */
	s->metric_len = encode_stats_to_text(enabled_port_mask, s->metric, STATS_METRIC_MAX);
	s->ts_ns = rte_get_tsc_cycles();

	stats_set(inactive);
	ws_broadcast_stats();
}

void main_loop(void *data)
{
	uint64_t prev_tsc = 0, diff_tsc = 0, cur_tsc = 0, timer_tsc = 0;

	unsigned int lcore_id = rte_lcore_id();
	struct bless_conf *conf = (struct bless_conf*)data;

	uint64_t timer_period = conf->timer_period;

	printf("lcore=%u cpu=%d\n", rte_lcore_id(), sched_getcpu());

	printf("[%s %d] pthread_barrier_wait(&conf->barrier);\n", __func__, __LINE__);
	pthread_barrier_wait(conf->barrier);
	printf("Bless pid %d is running ...\n", getpid());

	static uint64_t i = 0;
	uint64_t val = 0;
	while ((val = atomic_load_explicit(&g_state, memory_order_acquire)) != STATE_EXIT) {
		rte_delay_ms(100);
		cur_tsc = rte_rdtsc();
		diff_tsc = cur_tsc - prev_tsc;
		timer_tsc += diff_tsc;
		prev_tsc = cur_tsc;
		/* if timer has reached its timeout */
		if (unlikely(timer_tsc >= timer_period * 1)) {
			/* do this only on main core */
			if (lcore_id == rte_get_main_lcore()) {
				timer_tsc = 0;
				if (val == STATE_STOPPED) {
					if (i) {
						dpdk_generate_stats();
					}
					i = 0;
				} else {
					dpdk_generate_stats();
					i++;
				}
			}
		}
	}
}

static int bless_launch_one_lcore(void *conf)
{
	unsigned int lcore_id = rte_lcore_id();

	if (lcore_id == rte_get_main_lcore()) {
		main_loop((void*)conf);
	} else {
		if (!lcore_queue_conf[lcore_id].enabled) {
			printf("skip unused lcore %d\n", lcore_id);
			return 0;
		}
		worker_loop_txonly(conf);
	}

	return 0;
}

/* display usage */
static void usage(const char *prgname)
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

static int parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0')) {
		return 0;
	}

	return pm;
}

static int parse_port_pair_config(const char *q_arg)
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
		if (p0 == NULL) {
			return -1;
		}

		size = p0 - p;
		if (size >= sizeof(s)) {
			return -1;
		}

		memcpy(s, p, size);
		s[size] = '\0';
		if (rte_strsplit(s, sizeof(s), str_fld, _NUM_FLD, ',') != _NUM_FLD) {
			return -1;
		}
		for (i = 0; i < _NUM_FLD; i++) {
			errno = 0;
			int_fld[i] = strtoul(str_fld[i], &end, 0);
			if (errno != 0 || end == str_fld[i] || int_fld[i] >= RTE_MAX_ETHPORTS) {
				return -1;
			}
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

static unsigned int parse_nqueue(const char *q_arg)
{
	char *end = NULL;
	unsigned long n;

	/* parse hexadecimal string */
	n = strtoul(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0')) {
		return 0;
	}
	if (n == 0) {
		return 0;
	}
	if (n >= MAX_RX_QUEUE_PER_LCORE) {
		return 0;
	}

	return n;
}

static int parse_timer_period(const char *q_arg)
{
	char *end = NULL;
	int n;

	/* parse number string */
	n = strtol(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0')) {
		return -1;
	}
	if (n >= MAX_TIMER_PERIOD) {
		return -1;
	}

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
#define CMD_LINE_OPT_AUTO_START "auto-start"
#define CMD_LINE_OPT_MODE "mode"
#define CMD_LINE_OPT_NUM "num"
#define CMD_LINE_OPT_BATCH "batch"
#define CMD_LINE_OPT_BATCH_DELAY_US "batch-delay-us"
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
	CMD_LINE_OPT_AUTO_START_NUM,
	CMD_LINE_OPT_MODE_NUM,
	CMD_LINE_OPT_NUM_NUM,
	CMD_LINE_OPT_BATCH_NUM,
	CMD_LINE_OPT_BATCH_DELAY_US_NUM,
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
	{ CMD_LINE_OPT_AUTO_START, 1, 0, CMD_LINE_OPT_AUTO_START_NUM },
	{ CMD_LINE_OPT_MODE, 1, 0, CMD_LINE_OPT_MODE_NUM },
	{ CMD_LINE_OPT_NUM, 1, 0, CMD_LINE_OPT_NUM_NUM },
	{ CMD_LINE_OPT_BATCH, 1, 0, CMD_LINE_OPT_BATCH_NUM },
	{ CMD_LINE_OPT_BATCH_DELAY_US, 1, 0, CMD_LINE_OPT_BATCH_DELAY_US_NUM },
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
static int parse_args(int argc, char **argv)
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
				enabled_port_mask = parse_portmask(optarg);
				printf("enabled_port_mask %s => %d\n", optarg, enabled_port_mask);
				if (enabled_port_mask == 0) {
					printf("invalid portmask\n");
					usage(prgname);
					return -1;
				}
				break;
			case 'P':
				promiscuous_on = 1;
				break;

				/* nqueue */
			case 'q':
				rxtxq_per_port = parse_nqueue(optarg);
				if (rxtxq_per_port == 0) {
					printf("invalid queue number\n");
					usage(prgname);
					return -1;
				}
				break;
			case 'e':
				ratio.weight[TYPE_ERRONEOUS] = bless_parse_type(TYPE_ERRONEOUS, optarg);
				printf("TYPE_ERRONEOUS %d\n", ratio.weight[TYPE_ERRONEOUS]);
				break;

			case 'T': /* timer period */
				timer_secs = parse_timer_period(optarg);
				if (timer_secs < 0) {
					printf("invalid timer period\n");
					usage(prgname);
					return -1;
				}
				timer_period = timer_secs;
				break;
				/* long options */
			case CMD_LINE_OPT_NUM_NUM:
				bconf->num = optarg ? atoi(optarg) : 0;
				ratio.num = optarg ? atoi(optarg) : 0;
				printf("bconf->size ratio.num %lu\n", bconf->num);
				break;
			case CMD_LINE_OPT_BATCH_NUM:
				bconf->batch = optarg ? atoi(optarg) : 256;
				printf("batch %s %d %d\n", optarg, atoi(optarg), bconf->batch);
				break;
			case CMD_LINE_OPT_BATCH_DELAY_US_NUM:
				bconf->batch_delay_us = optarg ? atoi(optarg) : 0;
				printf("batch delay us %s %d %d\n", optarg, atoi(optarg), bconf->batch);
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
				ret = parse_port_pair_config(optarg);
				if (ret) {
					fprintf(stderr, "Invalid config\n");
					usage(prgname);
					return -1;
				}
				break;
			case CMD_LINE_OPT_AUTO_START_NUM:
				if (optarg && 0 == strcmp(optarg, "true")) {
					bconf->auto_start = 1;
				}
				printf("auto start %d\n", bconf->auto_start);
				break;
			case CMD_LINE_OPT_MODE_NUM:
				if (!optarg || !strcmp(optarg, "tx-only")) {
					bconf->mode = BLESS_MODE_TX_ONLY;
				} else if (!strcmp(optarg, "rx-only")) {
					bconf->mode = BLESS_MODE_RX_ONLY;
				} else if (!strcmp(optarg, "fwd")) {
					bconf->mode = BLESS_MODE_FWD;
				} else {
					rte_exit(EXIT_FAILURE, "Invalid mode: %s.\n", optarg);
				}
				printf("mode %d\n", bconf->mode);
				break;
			case CMD_LINE_OPT_NO_MAC_UPDATING_NUM:
				mac_updating = 0;
				break;
			case CMD_LINE_OPT_DST_MAC_NUM:
				printf("%s\n", CMD_LINE_OPT_DST_MAC);
				if (rte_ether_unformat_addr(optarg, &mc_inner->dst_addr) < 0) {
					rte_exit(EXIT_FAILURE, "Invalid mac address.\n");
				}
				strncpy((char*)mc_inner->str.dst_addr, optarg, 17);
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
				strncpy((char*)mc_inner->str.src_ip, optarg, 15);
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
				strncpy((char*)mc_inner->str.dst_ip, optarg, 15);
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
				usage(prgname);
				return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind - 1;
	optind = 1; /* reset getopt lib */

	if (-EPERM == bless_set_dist(bconf, &ratio, &bep)) {
		rte_exit(EXIT_FAILURE, "Cannot bless_set_dist()\n");
	}

	return ret;
}

/*
 * Check port pair config with enabled port mask,
 * and for valid port pair combinations.
 */
static int check_port_pair_config(void)
{
	uint32_t port_pair_config_mask = 0;
	uint32_t port_pair_mask = 0;
	uint16_t index, i, portid;

	for (index = 0; index < nb_port_pair_params; index++) {
		port_pair_mask = 0;

		for (i = 0; i < NUM_PORTS; i++)  {
			portid = port_pair_params[index].port[i];
			if ((enabled_port_mask & (1 << portid)) == 0) {
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

	enabled_port_mask &= port_pair_config_mask;

	return 0;
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void check_all_ports_link_status(uint32_t port_mask)
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
		if (atomic_load_explicit(&g_state, memory_order_acquire) == STATE_EXIT) {
			return;
		}
		all_ports_up = 1;
		RTE_ETH_FOREACH_DEV(portid) {
			if (atomic_load_explicit(&g_state, memory_order_acquire) != STATE_EXIT) {
				return;
			}
			if ((port_mask & (1u << portid)) == 0) {
				continue;
			}
			memset(&link, 0, sizeof(link));
			ret = rte_eth_link_get_nowait(portid, &link);
			if (ret < 0) {
				all_ports_up = 0;
				if (print_flag == 1) {
					printf("Port %u link get failed: %s\n",
							portid, rte_strerror(-ret));
				}
				continue;
			}
			/* print link status if flag set */
			if (print_flag == 1) {
				rte_eth_link_to_str(link_status_text,
						sizeof(link_status_text), &link);
				printf("Port %d %s\n", portid, link_status_text);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == RTE_ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1) {
			break;
		}

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

static void signal_handler(int signum)
{
	if (signum == SIGINT) {
		system_dump_status(&sysstat);
	} else if (signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n", signum);
		atomic_store(&g_state, STATE_EXIT);
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

	for (int i = 0; i < (int)NELEMS(mbuf_bless_fields); i++) {
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

const char *ws_json_get_string(cJSON *obj, const char *key)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
	return cJSON_IsString(item) ? item->valuestring : NULL;
}

void ws_user_func(void *data, size_t size)
{
	if (!data || !size) {
		return;
	}

	cJSON *root = cJSON_Parse(data);
	if (!root) {
		printf("[%s %d] JSON parse error\n", __func__, __LINE__);
		return;
	}

	const char *cmd = ws_json_get_string(root, "cmd");
	if (cmd) {
		printf("cmd = %s\n", cmd);
		if (strcmp(cmd, "start") == 0) {
			printf("Received START command\n");
			atomic_store(&g_state, STATE_RUNNING);
			cmd = "started";
		} else if (strcmp(cmd, "stop") == 0) {
			printf("Received STOP command\n");
			atomic_store(&g_state, STATE_STOPPED);
			cmd = "stopped";
		} else if (strcmp(cmd, "init") == 0) {
			printf("Received INIT command\n");
			// TODO reinit
			// atomic_store(&g_state, STATE_INIT);
			cmd = "already inited";
		} else if (strcmp(cmd, "exit") == 0) {
			printf("Received EXIT command\n");
			atomic_store(&g_state, STATE_EXIT);
			cmd = "exited";
		} else if (strcmp(cmd, "conf") == 0) {
			printf("Received conf command\n");
			cmd = (char*)cfm->addr;
		} else {
			cmd = "Not supported";
		}
	} else {
		printf("cmd missing or not a string\n");
		cmd = "cmd missing or not a string";
	}
	cJSON_Delete(root);
	dpdk_generate_cmdReply(cmd);
}

int main(int argc, char **argv)
{
	int targc = argc;
	char **targv = argv;
	Node *conf_root = NULL;


	char *f = DEFAULT_CONFIG_FILE;
	if (2 == argc) {
#ifdef VERSION
		if (0 == strcmp(argv[1], "version")) {
			print_version();
			exit(0);
		}
#endif
		f = argv[1];
	}
	cfm = config_check_file(f);
	if (!cfm) {
		rte_exit(EXIT_FAILURE, "Cannot check %s\n", f);
	}

	conf_root = config_init(f);

	config_parse_dpdk(conf_root, &targc, &targv);
	if (!conf_root) {
		rte_exit(EXIT_FAILURE, "Cannot parse %s\n", f);
	}

	for (int i = 0; i < targc; i++) {
		printf("'%s' ", targv[i]);
	}
	printf("\n");
	bconf = bless_init();
	if (!bconf) {
		rte_exit(EXIT_FAILURE, "Cannot rte_malloc(bless_conf)\n");
	}
	// bless_set_config_file(bconf, cfm);

	struct system_cfg syscfg;
	if (config_parse_system(conf_root, &syscfg) < 0) {
		rte_exit(EXIT_FAILURE, "Invalid server arguments\n");
	}
	if (syscfg.daemonize) {
		daemon(1, 1);
	}

	struct ws_user_data wsud = {
		.data = (void*)&syscfg.srvcfg,
		.func = ws_user_func,
	};
	struct mg_context *ctx = ws_server_start(&wsud);
	if (!ctx) {
		rte_exit(EXIT_FAILURE, "ws_server_start failed\n");
	}

	int ret = rte_eal_init(targc, targv);
	if (ret < 0) {
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	}
	targc -= ret;
	targv += ret;
	argv += ret;

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

	/* parse application arguments (after the EAL ones) */
	ret = parse_args(targc, targv);
	if (ret < 0) {
		rte_exit(EXIT_FAILURE, "Invalid BLESS arguments\n");
	}
	/* >8 End of init EAL. */

	atomic_store(&g_state, bconf->auto_start ? STATE_RUNNING : STATE_INIT);
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	if (conf_root && !bconf->cnode) {
		bconf->cnode = config_parse_bless(conf_root);
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
	// printf("nb_ports %u enabed 0x%x\n", nb_ports, enabled_port_mask);
	if (enabled_port_mask & ~((1 << nb_ports) - 1)) {
		rte_exit(EXIT_FAILURE, "Invalid portmask; possible (0x%x)\n",
				(1 << nb_ports) - 1);
	}

	/* Initialization of the driver. 8< */

	/* reset dst_ports */
	uint16_t portid, last_port;
	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
		dst_ports[portid] = 0;
	}
	last_port = 0;

	{
		rte_lcore_dump(stdout);
		uint32_t mid = rte_get_main_lcore();
		printf("main %d\n", mid);
		uint32_t lid = 0;
		unsigned int nlcore = 0;
		RTE_LCORE_FOREACH_WORKER(lid) {
			printf("worker %d\n", lid);
			nlcore += ROLE_RTE == rte_eal_lcore_role(lid);
		}
		int nport = rte_eth_dev_count_avail();
		printf("rte lcore %d\n", nlcore);
		printf("rxtxq_per_port %d\n", rxtxq_per_port);
		printf("nport %d\n", nport);
		printf("enabled_port_mask 0x%x\n", enabled_port_mask);
		int n_enabled_port = rte_popcount32(enabled_port_mask);
		if (nlcore < rxtxq_per_port * n_enabled_port) {
			rte_exit(EXIT_FAILURE, "Not enough cores %d < %d * %d\n",
					nlcore, rxtxq_per_port, n_enabled_port);
		} else if (nlcore > rxtxq_per_port * n_enabled_port) {
			printf("%d lcore will not be used\n",
					nlcore - rxtxq_per_port * n_enabled_port);
		}
		lid = 0;
		uint32_t pid = 0;
		memset(lcore_queue_conf, 0, sizeof(struct lcore_queue_conf) * RTE_MAX_LCORE);
		RTE_ETH_FOREACH_DEV(pid) {
			if ((enabled_port_mask & (1u << pid)) == 0) {
				printf("skip port %d\n", pid);
				continue;
			}

			printf("Port %d\n", pid);

			for (uint32_t qid = 0; qid < rxtxq_per_port; qid++) {
				/* get the lcore_id for this port */
				while (!rte_lcore_is_enabled(lid) ||
						lid == rte_get_main_lcore()) {
					lid++;
					if (lid >= RTE_MAX_LCORE) {
						rte_exit(EXIT_FAILURE, "[%s %d] Not enough cores\n", __func__, __LINE__);
					}
				}
				/* queue view */
				printf("  queue %d lcore %d\n", qid, lid);
				lcore_queue_conf[lid].enabled = 1;
				lcore_queue_conf[lid].txl_id = lid;
				lcore_queue_conf[lid].txp_id = pid;
				lcore_queue_conf[lid].txq_id = qid;
				lid++;
			}
			enabled_lcores += rxtxq_per_port;
		}
	}
	/* lcore view */
	for (int i = 0; i < RTE_MAX_LCORE; i++) {
		if (!lcore_queue_conf[i].enabled) {
			continue;
		}
		printf("lcore %d port %d txq %d\n", i, lcore_queue_conf[i].txp_id, lcore_queue_conf[i].txq_id);
	}

	/* populate destination port details */
	if (port_pair_params != NULL) {
		uint16_t idx, p;

		for (idx = 0; idx < (nb_port_pair_params << 1); idx++) {
			p = idx & 1;
			portid = port_pair_params[idx >> 1].port[p];
			dst_ports[portid] =
				port_pair_params[idx >> 1].port[p ^ 1];
		}
	} else {
		uint32_t nb_ports_in_mask = 0;
		RTE_ETH_FOREACH_DEV(portid) {
			/* skip ports that are not enabled */
			if ((enabled_port_mask & (1u << portid)) == 0) {
				continue;
			}

			if (nb_ports_in_mask % 2) {
				dst_ports[portid] = last_port;
				dst_ports[last_port] = portid;
			} else {
				last_port = portid;
			}

			nb_ports_in_mask++;
		}
		if (nb_ports_in_mask % 2) {
			printf("Notice: odd number of ports in portmask.\n");
			dst_ports[last_port] = last_port;
		}
	}
	/* >8 End of initialization of the driver. */

	struct lcore_queue_conf *qconf = NULL;

	/* Initialize the port/queue configuration of each logical core */
	uint32_t rx_lcore_id = 0;
	uint32_t nb_lcores = 0;
	RTE_ETH_FOREACH_DEV(portid) {
		/* skip ports that are not enabled */
		if ((enabled_port_mask & (1u << portid)) == 0) {
			continue;
		}

		/* get the lcore_id for this port */
		while (rx_lcore_id == rte_get_main_lcore() ||
				(rte_lcore_is_enabled(rx_lcore_id) == 0 ||
				 lcore_queue_conf[rx_lcore_id].n_rx_port ==
				 rxtxq_per_port)) {
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
				portid, dst_ports[portid]);
	}

	uint16_t ap = __builtin_popcount(enabled_port_mask);
	unsigned int nb_mbufs =
		ap * rxtxq_per_port * nb_rxd      /* RX ring 硬需求 */
		+ ap * MAX_PKT_BURST * rxtxq_per_port
		+ nb_lcores * MEMPOOL_CACHE_SIZE * 2
		+ 8192; /* 安全冗余 */
	printf("ap %d rxtxq_per_port %d nb_rxd %d MAX_PKT_BURST %d nb_lcores %d MEMPOOL_CACHE_SIZE %d nb_mbufs %u\n",
			ap, rxtxq_per_port, nb_rxd, MAX_PKT_BURST, nb_lcores, MEMPOOL_CACHE_SIZE, nb_mbufs);

	/* Create the mbuf pool. 8< */
	rx_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
			MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
			rte_socket_id());
	if (rx_pktmbuf_pool == NULL) {
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");
	}
	/* >8 End of create the mbuf pool. */

	uint16_t nb_ports_available = 0;

	/* Initialise each port */
	RTE_ETH_FOREACH_DEV(portid) {
		struct rte_eth_conf local_port_conf = port_conf;
		struct rte_eth_dev_info dev_info;

		/* skip ports that are not enabled */
		if ((enabled_port_mask & (1u << portid)) == 0) {
			printf("Skipping disabled port %u\n", portid);
			continue;
		}
		nb_ports_available++;

		/* init port */
		printf("Initializing port %u... ", portid);
		fflush(stdout);
		// getchar();

		ret = rte_eth_dev_info_get(portid, &dev_info);
		if (ret != 0) {
			rte_exit(EXIT_FAILURE,
					"Error during getting device (port %u) info: %s\n",
					portid, strerror(-ret));
		}

		local_port_conf.txmode.offloads &= dev_info.tx_offload_capa;
#if 0
		if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) {
			// local_port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
		} else {
			local_port_conf.txmode.offloads ^= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
		}
#endif
		/* Configure the number of queues for a port. */
		ret = rte_eth_dev_configure(portid, rxtxq_per_port, rxtxq_per_port, &local_port_conf);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
					ret, portid);
		}
		/* >8 End of configuration of the number of queues for a port. */

		ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd, &nb_txd);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE,
					"Cannot adjust number of descriptors: err=%d, port=%u\n",
					ret, portid);
		}
		printf("TODO nb_txd %d\n", nb_txd);

		ret = rte_eth_macaddr_get(portid, &ports_eth_addr[portid]);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE,
					"Cannot get MAC address: err=%d, port=%u\n",
					ret, portid);
		}
		fflush(stdout);

		/* Init queue on each port. 8< */
		fflush(stdout);
		struct rte_eth_rxconf rxq_conf;
		rxq_conf = dev_info.default_rxconf;
		rxq_conf.offloads = local_port_conf.rxmode.offloads;
		struct rte_eth_txconf txq_conf;
		txq_conf = dev_info.default_txconf;
		txq_conf.offloads = local_port_conf.txmode.offloads;
		for (uint16_t i = 0; i < rxtxq_per_port; i++) {
#if 1
			/* RX queue setup. 8< */
			ret = rte_eth_rx_queue_setup(portid, i, nb_rxd,
					rte_eth_dev_socket_id(portid),
					&rxq_conf, rx_pktmbuf_pool);
			if (ret < 0) {
				rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
						ret, portid);
			}
			/* >8 End of RX queue setup. */
#endif
			/* init one TX queue */
			ret = rte_eth_tx_queue_setup(portid, i, nb_txd,
					rte_eth_dev_socket_id(portid), &txq_conf);
			if (ret < 0) {
				rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n", ret, portid);
			}
		}
		/* >8 End of RX queue setup. */

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

		ret = rte_eth_dev_set_ptypes(portid, RTE_PTYPE_UNKNOWN, NULL, 0);
		if (ret < 0) {
			printf("Port %u, Failed to disable Ptype parsing\n", portid);
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
			if (ret != 0) {
				rte_exit(EXIT_FAILURE,
						"rte_eth_promiscuous_enable:err=%s, port=%u\n",
						rte_strerror(-ret), portid);
			}
		}

		{
			struct rte_eth_dev_info *dev_info = rte_zmalloc(NULL, sizeof(struct rte_eth_dev_info), 0);
			if (rte_eth_dev_info_get(portid, dev_info)) {;
				rte_exit(EXIT_FAILURE, "[%s %d] rte_eth_dev_info_get(%d)\n",
						__func__, __LINE__, portid);
			}
			printf("Port %u, MAC address: " RTE_ETHER_ADDR_PRT_FMT "\n",
					portid, RTE_ETHER_ADDR_BYTES(&ports_eth_addr[portid]));
			printf("driver name %s\n", dev_info->driver_name);
			printf("if index %u\n", dev_info->if_index);
			printf("mtu [%d, %d]\n", dev_info->min_mtu, dev_info->max_mtu);
			printf("nb rx queues %u\n", dev_info->nb_rx_queues);
			printf("nb tx queues %u\n", dev_info->nb_tx_queues);
		}

		/* initialize port stats */
		// memset(port_statistics, 0, sizeof(port_statistics));
	}

	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE,
				"All available ports are disabled. Please set portmask.\n");
	}
	printf("nb_ports_available %u %u\n",
			nb_ports_available, __builtin_popcount(enabled_port_mask));

	check_all_ports_link_status(enabled_port_mask);

	char name[16];
	pthread_getname_np(pthread_self(), name, sizeof(name));
	RTE_LOG(INFO, BLESS, "entering main loop %s: pid %d tid %d self %lu\n", name,
			getpid(), rte_gettid(), (unsigned long)rte_thread_self().opaque_id);

	sysstat.pid = getpid();
	sysstat.ppid = getppid();
	CPU_ZERO(&sysstat.cpuset);
	if (sched_getaffinity(0, sizeof(cpu_set_t), &sysstat.cpuset)) {
		rte_exit(EXIT_FAILURE, "sched_getaffinity()");
	}

	bconf->qconf = lcore_queue_conf;
	bconf->stats = rte_malloc(NULL, sizeof(bconf->stats) * RTE_MAX_ETHPORTS, 0);
	bconf->dst_ports = dst_ports;
	bconf->state = &g_state;
	bconf->timer_period = timer_period;
	bconf->enabled_port_mask = enabled_port_mask;
	pthread_barrier_t barrier;
	pthread_barrier_init(&barrier, NULL, enabled_lcores + 1); /* + main lcore */
	bconf->barrier = &barrier;

	printf("\n\n >> %d nb_lcores %d\n\n", rte_lcore_id(), nb_lcores + 1);

	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(bless_launch_one_lcore, (void*)bconf, CALL_MAIN);

	ret = 0;
	uint32_t lcore_id = 0;
	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}

	RTE_ETH_FOREACH_DEV(portid) {
		if ((enabled_port_mask & (1u << portid)) == 0) {
			continue;
		}
		printf("Closing port %d...", portid);
		ret = rte_eth_dev_stop(portid);
		if (ret != 0)
			printf("rte_eth_dev_stop: err=%d, port=%d\n",
					ret, portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}

	pthread_barrier_destroy(&barrier);

#ifdef RTE_LIB_PDUMP
	rte_pdump_uninit();
#endif

	/* clean up the EAL */
	rte_eal_cleanup();

	ws_server_stop(ctx);

	config_exit(conf_root);

	printf("Bye...\n");

	return ret;
}
