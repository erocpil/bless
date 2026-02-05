#include "base.h"
#include "color.h"
#include "worker.h"
#include "metric.h"
#include "server.h"
#include "device.h"
#include "civetweb.h"
#include "log.h"
#include <string.h>

void base_show_version(void)
{
	_(ANSI_BOLD FG_BRIGHT_BLUE "Version Info:" ANSI_RESET);
	_(C_ERR "  Version    : " FG_RED "%s", BL_VERSION);
	_(C_INFO "  Git Branch : " FG_GREEN "%s", GIT_BRANCH);
	_(C_WARN "  Git Commit : " FG_YELLOW "%s %s", GIT_COMMIT, GIT_STATE);
	_(C_DEBUG "  Build Type : " FG_CYAN "%s %s", BUILD_TYPE, STATIC ? "static" : "shared");
	_(C_TRACE "  Build Host : " FG_MAGENTA "%s", BUILD_HOST);
	_(C_HINT "  Build Time : " FG_BRIGHT_BLACK "%s\n" ANSI_RESET, BUILD_TIME);
}

struct base base;
uint32_t dev_mode_mask = 0; /* base.dev_mode_mask */

/* mask of enabled ports */
static uint32_t enabled_port_mask = 0;

atomic_int g_state = STATE_INIT;

void *metric_cbfn()
{
	return (void*)&base.system->status;
}

void init_metrics(void)
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

struct rte_mempool *rx_pktmbuf_pool = &base.rx_pktmbuf_pool;

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

static struct rte_eth_conf port_conf_default = {
};

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

static int launch_one_core(void *conf)
{
	unsigned int lcore_id = rte_lcore_id();

	if (lcore_id == rte_get_main_lcore()) {
		LOG_INFO("main core");
		// base_show_core_view(base.topo.cv);
		worker_main_loop((void*)conf);
	} else {
		struct base_core_view *view = base.topo.cv + lcore_id;
		if (!view->enabled) {
			LOG_TRACE("skip unused core %d", lcore_id);
			return 1;
		}
		if (base.dev_mode_mask & ETHDEV_PCAP_MASK) {
			static int n_pcap_dev = 0;
			if (view->type != ETHDEV_PCAP) {
				LOG_HINT("skip core %u with none pcap device", lcore_id);
				pthread_barrier_wait(&base.barrier);
				return 1;
			}
			if (n_pcap_dev) {
				LOG_HINT("skip other core %u with pcap device", lcore_id);
				pthread_barrier_wait(&base.barrier);
				return 1;
			}
			n_pcap_dev++;
			LOG_WARN("launch one core %u with pcap device", lcore_id);
		}
		LOG_TRACE("Running core %u port %u queue %u %u",
				lcore_id, view->port, view->rxq, view->txq);
		worker_loop(conf);
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
			LOG_ERR("exceeded max number of port pair params: %hu\n",
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
	{NULL, 0, 0, 0}
};

/* Parse the argument given in the command line of the application */
static int parse_args(int argc, char **argv)
{
	struct bless_conf *bconf = base.bconf;
	int opt, ret, timer_secs;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];

	struct dist_ratio ratio;

	struct bless_encap_params bep = { NULL, NULL };
	bep.inner = rte_malloc(NULL, sizeof(struct mbuf_conf), 0);

	argvopt = argv;
	port_pair_params = NULL;

	dist_ratio_init(&ratio);

	while ((opt = getopt_long(argc, argvopt, short_options,
					lgopts, &option_index)) != EOF) {
		switch (opt) {
			/* portmask */
			case 'p':
				enabled_port_mask = parse_portmask(optarg);
				if (enabled_port_mask == 0) {
					LOG_ERR("invalid portmask\n");
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
					LOG_ERR("invalid queue number\n");
					usage(prgname);
					return -1;
				}
				break;

			case 'T': /* timer period */
				timer_secs = parse_timer_period(optarg);
				if (timer_secs < 0) {
					LOG_ERR("invalid timer period\n");
					usage(prgname);
					return -1;
				}
				bconf->timer_period = timer_secs;
				break;
				/* long options */
			case CMD_LINE_OPT_NUM_NUM:
				bconf->num = optarg ? atoi(optarg) : 0;
				ratio.num = optarg ? atoi(optarg) : 0;
				break;
			case CMD_LINE_OPT_BATCH_NUM:
				bconf->batch = optarg ? atoi(optarg) : 256;
				break;
			case CMD_LINE_OPT_BATCH_DELAY_US_NUM:
				bconf->batch_delay_us = optarg ? atoi(optarg) : 0;
				break;
			case CMD_LINE_OPT_ARP_NUM:
				ratio.weight[TYPE_ARP] = bless_parse_type(TYPE_ARP, optarg);
				break;
			case CMD_LINE_OPT_ICMP_NUM:
				ratio.weight[TYPE_ICMP] = bless_parse_type(TYPE_ICMP, optarg);
				break;
			case CMD_LINE_OPT_TCP_NUM:
				ratio.weight[TYPE_TCP] = bless_parse_type(TYPE_TCP, optarg);
				break;
			case CMD_LINE_OPT_UDP_NUM:
				ratio.weight[TYPE_UDP] = bless_parse_type(TYPE_UDP, optarg);
				break;
			case CMD_LINE_OPT_PORTMAP_NUM:
				ret = parse_port_pair_config(optarg);
				if (ret) {
					usage(prgname);
					return -1;
				}
				break;
			case CMD_LINE_OPT_AUTO_START_NUM:
				if (optarg && 0 == strcmp(optarg, "true")) {
					bconf->auto_start = 1;
				}
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
				break;
			case CMD_LINE_OPT_NO_MAC_UPDATING_NUM:
				mac_updating = 0;
				break;
			default:
				usage(prgname);
				return -1;
		}
	}

	if (optind >= 0) {
		argv[optind - 1] = prgname;
	}

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
#if 0
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
#endif

/* Check the link status of all ports in up to 9s, and print them finally */
static void check_all_ports_link_status(uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint16_t portid;
	uint8_t count, all_ports_up, print_flag = 1;
	struct rte_eth_link link;
	int ret;
	char link_status_text[RTE_ETH_LINK_MAX_STR_LEN];

	LOG_INFO("Checking link status ...");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (atomic_load_explicit(&g_state, memory_order_acquire) == STATE_EXIT) {
			LOG_INFO("exit");
			return;
		}
		all_ports_up = 1;
		RTE_ETH_FOREACH_DEV(portid) {
			if (atomic_load_explicit(&g_state, memory_order_acquire) == STATE_EXIT) {
				LOG_INFO("exit");
				return;
			}
			enum ethdev_type etype = device_get_ethdev_type(portid);
			if (etype == ETHDEV_PHYSICAL) {
				if ((port_mask & (1u << portid)) == 0) {
					LOG_TRACE("skip port %u", portid);
					continue;
				}
			}
			memset(&link, 0, sizeof(link));
			ret = rte_eth_link_get_nowait(portid, &link);
			if (ret < 0) {
				all_ports_up = 0;
				if (print_flag == 1) {
					LOG_WARN("Port %u link get failed: %s\n",
							portid, rte_strerror(-ret));
				}
				continue;
			}
			/* print link status if flag set */
			if (print_flag == 1) {
				rte_eth_link_to_str(link_status_text,
						sizeof(link_status_text), &link);
				LOG_WARN("Port %d %s", portid, link_status_text);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == RTE_ETH_LINK_DOWN) {
				all_ports_up = 0;
				LOG_WARN("port %u link down", portid);
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
			LOG_INFO("done\n");
		}
	}
}

static int mbuf_dynfields_offset[MBUF_DYNFIELDS_MAX];

enum {
	MBUF_DYN_TYPE = 0,
};

static int init_mbuf_dynfield()
{
	const struct rte_mbuf_dynfield mbuf_bless_fields[] = {
		[ MBUF_DYN_TYPE ] = {
			.name = "field_name",
			.size = sizeof(char),
			.align = __alignof__(char),
		},
	};

	LOG_INFO("mbuf dynfield:");
	for (int i = 0; i < (int)NELEMS(mbuf_bless_fields); i++) {
		if (mbuf_bless_fields[i].size == 0) {
			continue;
		}
		const struct rte_mbuf_dynfield *md = &mbuf_bless_fields[i];
		int offset = rte_mbuf_dynfield_register(md);
		if (offset < 0) {
			RTE_LOG(ERR, MBUF, "fail to register dynfield[%d %d] in mbuf!\n",
					i, rte_errno);
			rte_mbuf_dyn_dump(stdout);
			return rte_errno;
		}
		mbuf_dynfields_offset[i] = offset;
		LOG_HINT("name: %s", md->name);
		LOG_HINT("size: %lu", md->size);
		LOG_HINT("align: %lu", md->align);
	}

	rte_mbuf_dyn_dump(stdout);

	return 0;
}

static void base_show_core_view_format(struct base_core_view *view, char *pref)
{
	if (!view) {
		return;
	}

	LOG_INFO("%score view    %p", pref, view);
	for (int i = 0; i < RTE_MAX_LCORE; i++) {
		struct base_core_view *v = view + i;
		if (!v->enabled) {
			continue;
		}
		LOG_HINT("%s  [%d]        %p", pref, i, v);
		LOG_PATH("%s  enabled    %u", pref,v->enabled);
		LOG_PATH("%s  socket     %u", pref,v->socket);
		LOG_PATH("%s  numa       %u", pref,v->numa);
		LOG_PATH("%s  core       %u", pref,v->core);
		LOG_PATH("%s  role       %u", pref,v->role);
		LOG_PATH("%s  port       %u", pref,v->port);
		LOG_PATH("%s  type       %u (%s)", pref,v->type, device_get_string(v->type));
		LOG_PATH("%s  rxq        %u", pref,v->rxq);
		LOG_PATH("%s  txq        %u", pref,v->txq);
	}
}

void base_show_core_view(struct base_core_view *view)
{
	base_show_core_view_format(view, "");
}

void base_show_topo(struct base_topo *topo)
{
	if (!topo) {
		return;
	}
	// rte_lcore_dump(stdout);

	LOG_HINT("base_topo           %p", topo);
	LOG_PATH("  n_socket          %u", topo->n_socket);
	LOG_PATH("  n_numa            %u", topo->n_numa);
	LOG_PATH("  n_core            %u", topo->n_core);
	LOG_PATH("  n_enabled_core    %u", topo->n_enabled_core);
	LOG_PATH("  n_port            %u", topo->n_port);
	LOG_PATH("  n_enabled_port    %u", topo->n_enabled_port);
	LOG_PATH("  port_mask         %u", topo->port_mask);
	LOG_PATH("  main_core         %u", topo->main_core);

	base_show_core_view_format(topo->cv, "  ");
}

void base_show()
{
	LOG_INFO("base %p", &base);
}

void exit_base()
{
	usleep(300000);
	LOG_INFO("bless exit");
}

void init_base(int argc, char **argv)
{
	base.argc = argc;
	base.argv = argv;
	struct config *cfg = (struct config*)malloc(sizeof(struct config));
	if (!cfg) {
		rte_exit(EXIT_FAILURE, "malloc(config)\n");
	}
	base.config = cfg;
}

int base_init_topo()
{
	struct base_topo *topo = &base.topo;
	struct base_core_view *view = NULL;

	if (!topo) {
		return -1;
	}
	memset(topo, 0, sizeof(struct base_topo));

	topo->main_core = rte_get_main_lcore();
	topo->n_port = rte_eth_dev_count_avail();
	if (!topo->n_port) {
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");
	}

	// maybe base.bconf->enabled_port_mask?
	topo->port_mask = enabled_port_mask;
	topo->n_enabled_port = rte_popcount32(topo->port_mask);

	topo->cv = malloc(sizeof(struct base_core_view) * RTE_MAX_LCORE);
	if (!topo->cv) {
		rte_exit(EXIT_FAILURE, "[%s %d] malloc(base_core_view)\n", __func__, __LINE__);
	}
	struct base_core_view *cv = topo->cv;
	memset(cv, 0, sizeof(struct base_core_view) * RTE_MAX_LCORE);

	uint16_t core_id = 0;
	uint16_t port_id = 0;
	RTE_ETH_FOREACH_DEV(port_id) {
		enum ethdev_type etype = device_get_ethdev_type(port_id);
		if (etype == ETHDEV_PHYSICAL) {
			if ((topo->port_mask & (1u << port_id)) == 0) {
				LOG_HINT("skip physical port %d", port_id);
				continue;
			}
		}

		for (uint32_t qid = 0; qid < rxtxq_per_port; qid++) {
			/* get the lcore_id for this port */
			while (!rte_lcore_is_enabled(core_id) ||
					core_id == rte_get_main_lcore()) {
				core_id++;
				if (core_id >= RTE_MAX_LCORE) {
					rte_exit(EXIT_FAILURE, "[%s %d] Not enough cores\n", __func__, __LINE__);
				}
			}
			/* queue view */
			view = cv + core_id;
			/* TODO socket/numa, etc... */
			view->enabled = 1;
			view->core = core_id;
			view->port = port_id;
			view->type = etype;
			LOG_INFO("core %d uses port %u(%s) with queue %u",
					core_id, port_id, device_get_string(etype), qid);
			view->rxq = qid;
			view->txq = qid;
			core_id++;
			topo->n_enabled_core++;
			/* if vdev is pcap, just use one txq */
			if (ETHDEV_PCAP == etype) {
				LOG_WARN("core %u uses pcap port %u with 1 txq", view->core, view->port);
				/* go for next port */
				break;
			}
		}
	}

	/* init main core view */
	view = cv + base.topo.main_core;
	view->core = base.topo.main_core;
	view->port = -1;
	view->type = ETHDEV_MAX;
	view->rxq = -1;
	view->txq = -1;

	/* init worker core view */
	RTE_LCORE_FOREACH_WORKER(core_id) {
		topo->n_core += ROLE_RTE == rte_eal_lcore_role(core_id);
		view = cv + core_id;
		view->role = rte_eal_lcore_role(core_id);
	}
	if (topo->n_enabled_core > topo->n_core) {
		rte_exit(EXIT_FAILURE, "Not enough cores");
	} else {
		LOG_HINT("%d core will not be used", topo->n_core - topo->n_enabled_core);
	}

	LOG_INFO("%u cores will be used excluding main", topo->n_enabled_core);

	/* check port mask to possible port mask */
	if (enabled_port_mask & ~((1 << base.topo.n_port) - 1)) {
		rte_exit(EXIT_FAILURE, "Invalid portmask; possible (0x%x)\n",
				(1 << base.topo.n_port) - 1);
	}

	base_show_topo(topo);

	return 0;
}

void init_system()
{
	struct system *system = (struct system*)malloc(sizeof(struct system));
	if (!system) {
		rte_exit(EXIT_FAILURE, "malloc(system)\n");
	}
	struct system_cfg *cfg = &system->cfg;
	if (config_parse_system(base.config->root, cfg) < 0) {
		rte_exit(EXIT_FAILURE, "Invalid server arguments\n");
	}

	log_init(system->cfg.theme);
	log_show_all_theme();

	if (cfg->daemonize) {
		daemon(1, 1);
		LOG_INFO("Daemonized");
	}

	struct server *server = &system->cfg.server;
	server->wsud.conf = (void*)server;
	server->wsud.data = (void*)&base;
	server->wsud.func = ws_user_func;

	struct mg_context *ctx = ws_server_start(&server->wsud);
	if (!ctx) {
		rte_exit(EXIT_FAILURE, "ws_server_start failed\n");
	}
	system->cfg.server.ctx = ctx;
	base.system = system;
	LOG_INFO("Websocket Server Started");
	system_show(system);
	getchar();
}

void init_config()
{
#define DEFAULT_CONFIG_FILE "conf/config.yaml"
	struct config *cfg = base.config;
	cfg->cfm.name = DEFAULT_CONFIG_FILE;
	if (1 == base.argc) {
		LOG_INFO("use default config %s", cfg->cfm.name);
	} else if (2 == base.argc) {
		if (0 == strcmp(base.argv[1], "version")) {
			base_show_version();
			exit(0);
		}
		cfg->cfm.name = base.argv[1];
	} else {
		rte_exit(EXIT_FAILURE, "unacceptable params");
	}

	if (config_check_file_map(&cfg->cfm)) {
		rte_exit(EXIT_FAILURE, "Cannot check %s\n", cfg->cfm.name);
	}

	/* init config node from file, aka. parse yaml */
	cfg->root = config_init(cfg->cfm.name);
	if (!cfg->root) {
		rte_exit(EXIT_FAILURE, "Cannot init config %s\n", cfg->cfm.name);
	}

	config_show(cfg);
}

void init_eal()
{
	/* parse dpdk, injector, bless */
	if (-1 == config_parse_dpdk(base.config->root, &base.argc, &base.argv)) {
		rte_exit(EXIT_FAILURE, "Cannot parse %s\n", base.config->cfm.name);
	}
	/* now base.{argc, argv} have all dpdk and app params */

	int ret = rte_eal_init(base.argc, base.argv);
	if (ret < 0) {
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	}

	base.argc -= ret;
	base.argv += ret;

	/* TODO rte_eth_dev_callback_register */

#ifdef RTE_LIB_PDUMP
	if (rte_pdump_init()) {
		rte_exit(EXIT_FAILURE, "rte_pdump_init()\n");
	}
	RTE_LOG(INFO, BLESS, "rte_pdump_init()\n");
#else
	RTE_LOG(INFO, BLESS, "rte_pdump not supported.\n");
#endif
}

void init_args()
{
	struct bless_conf *bconf = bless_init();
	if (!bconf) {
		rte_exit(EXIT_FAILURE, "Cannot rte_malloc(bless_conf)\n");
	}

	base.bconf = bconf;

	/* parse application(bless) arguments from base.{argc, argv} */
	if (parse_args(base.argc, base.argv) < 0) {
		rte_exit(EXIT_FAILURE, "Invalid BLESS arguments\n");
	}
	// FIXME
	LOG_HINT("MAC updating %s", mac_updating ? "enabled" : "disabled");

	if (base.config->root && !bconf->cnode) {
		bconf->cnode = config_parse_bless(base.config->root);
		if (!bconf->cnode) {
			rte_exit(EXIT_FAILURE, "Cannot parse bless\n");
		}
		base.config->cnode = bconf->cnode;
	} else {
		LOG_WARN("No config.ymal used");
		rte_exit(EXIT_FAILURE, "Cannot init bless\n");
	}

	// FIXME
	atomic_store(&g_state, bconf->auto_start ? STATE_RUNNING : STATE_INIT);

	/* convert to number of cycles */
	bconf->timer_period *= rte_get_timer_hz();
	bconf->stats = rte_malloc(NULL, sizeof(bconf->stats) * RTE_MAX_ETHPORTS, 0);
	if (!bconf->stats) {
		rte_exit(EXIT_FAILURE, "rte_malloc(bconf->stats)");
	}

	/* bconf */
	bconf->stats = rte_malloc(NULL, sizeof(bconf->stats) * RTE_MAX_ETHPORTS, 0);
	if (!bconf->stats) {
		rte_exit(EXIT_FAILURE, "rte_malloc(bconf->stats)");
	}
	bconf->dst_ports = dst_ports;
	bconf->state = &g_state;
	bconf->enabled_port_mask = enabled_port_mask;

	bconf->barrier = &base.barrier;
	bconf->base = &base;

	// dump_cnode_summary(bconf->cnode);
	// exit(0);

	/* TODO */
#if 0
	/* reset dst_ports */
	uint16_t portid;
	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
		dst_ports[portid] = 0;
	}
#endif
#if 0
	/* populate destination port details */
	uint16_t last_port = 0;
	if (port_pair_params != NULL) {
		if (check_port_pair_config() < 0)
			rte_exit(EXIT_FAILURE, "Invalid port pair config\n");
	}
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
#endif
}

static void signal_handler(int signum)
{
	_L();
	switch (signum) {
		case SIGINT:
			config_show_root(base.config);
			system_show_status(&base.system->status);
			break;
		case SIGTERM:
			LOG_WARN("\nSignal %d received, preparing to exit...", signum);
			atomic_store(&g_state, STATE_EXIT);
			exit_base();
			break;
		default:
			base_show_version();
			break;
	}
}

void init_signal()
{
	/* TODO sighandler? */
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
}

void init_port()
{
	uint16_t ap = __builtin_popcount(enabled_port_mask);
	unsigned int nb_mbufs =
		ap * rxtxq_per_port * nb_rxd      /* RX ring 硬需求 */
		+ ap * MAX_PKT_BURST * rxtxq_per_port
		+ base.topo.n_enabled_core * MEMPOOL_CACHE_SIZE * 2
		+ 8192; /* 安全冗余 */
	LOG_HINT("ap %d rxtxq_per_port %d nb_rxd %d MAX_PKT_BURST %d base.topo.n_enabled_core %d MEMPOOL_CACHE_SIZE %d nb_mbufs %u",
			ap, rxtxq_per_port, nb_rxd, MAX_PKT_BURST, base.topo.n_enabled_core, MEMPOOL_CACHE_SIZE, nb_mbufs);

	/* Create the mbuf pool. 8< */
	rx_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
			MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
			rte_socket_id());
	if (rx_pktmbuf_pool == NULL) {
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");
	}
	/* >8 End of create the mbuf pool. */

	uint16_t nb_physical_ports_available = 0;
	uint16_t nb_ports_available = 0;

	/* Initialise each port */
	LOG_INFO("Initializing port...");
	int ret = 0;
	uint16_t portid;
	RTE_ETH_FOREACH_DEV(portid) {
		struct rte_eth_conf local_port_conf = port_conf;
		struct rte_eth_dev_info dev_info;
		enum ethdev_type etype = device_get_ethdev_type(portid);

		dev_mode_mask |= device_type_to_mask(etype);

		if (ETHDEV_NOT_SUPPORTED == etype) {
			rte_exit(EXIT_FAILURE,
					"Not supported ether device (port %u) info: %s\n",
					portid, strerror(-ret));
		}
		/* skip physical ports that are not enabled */
		if (etype == ETHDEV_PHYSICAL) {
			if ((enabled_port_mask & (1u << portid)) == 0) {
				LOG_TRACE("Skipping disabled physical port %u", portid);
				continue;
			}
			nb_physical_ports_available++;
		}
		nb_ports_available++;

		/* init port */
		fflush(stdout);

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
		if (ETHDEV_PHYSICAL == etype) {
			ret = rte_eth_dev_configure(portid, rxtxq_per_port, rxtxq_per_port, &local_port_conf);
		} else if (ETHDEV_PCAP == etype) {
			ret = rte_eth_dev_configure(portid, 0, 1, &port_conf_default);
		} else {
			/* TODO other types */
			ret = -1;
		}
		if (ret < 0) {
			rte_exit(EXIT_FAILURE, "Cannot configure device: type %d err=%d, port=%u\n",
					etype, ret, portid);
		}
		/* >8 End of configuration of the number of queues for a port. */

		if (ETHDEV_PHYSICAL == etype) {
			ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd, &nb_txd);
			if (ret < 0) {
				rte_exit(EXIT_FAILURE,
						"Cannot adjust number of descriptors: err=%d port=%u"
						"nb_rxd %u nb_txd %u", ret, portid, nb_rxd, nb_txd);
			}
		}

		ret = rte_eth_macaddr_get(portid, &ports_eth_addr[portid]);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE,
					"Cannot get MAC address: err=%d, port=%u\n",
					ret, portid);
		}
		fflush(stdout);

		/* Init queue on each port. 8< */
		struct rte_eth_rxconf rxq_conf;
		rxq_conf = dev_info.default_rxconf;
		rxq_conf.offloads = local_port_conf.rxmode.offloads;
		struct rte_eth_txconf txq_conf;
		txq_conf = dev_info.default_txconf;
		txq_conf.offloads = local_port_conf.txmode.offloads;
		for (uint16_t i = 0; i < rxtxq_per_port; i++) {
			/* RX queue setup. 8< */
			if (ETHDEV_PHYSICAL == etype) {
				ret = rte_eth_rx_queue_setup(portid, i, nb_rxd,
						rte_eth_dev_socket_id(portid),
						&rxq_conf, rx_pktmbuf_pool);
			} else {
				/* TODO others types except ETHDEV_PCAP */
				ret = 1;
				/*
				   ret = rte_eth_rx_queue_setup(portid, i, nb_rxd,
				   rte_eth_dev_socket_id(portid),
				   &dev_info.default_rxconf, rx_pktmbuf_pool);
				   */
			}
			if (ret < 0) {
				rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
						ret, portid);
			}
			/* >8 End of RX queue setup. */
			/* init one TX queue */
			if (ETHDEV_PHYSICAL == etype) {
				ret = rte_eth_tx_queue_setup(portid, i, nb_txd,
						rte_eth_dev_socket_id(portid), &txq_conf);
			} else if (ETHDEV_PCAP == etype) {
				LOG_INFO("setup one txq for pcap device");
				ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
						rte_eth_dev_socket_id(portid), &dev_info.default_txconf);
				/* only 1 tx queue */
				break;
			} else {
				/* TODO other type */
				ret = -1;
			}
			if (ret < 0) {
				rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n", ret, portid);
			}
		}
		/* >8 End of RX/TX queue setup. */

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
			rte_exit(EXIT_FAILURE, "Port %u, Failed to disable Ptype parsing\n", portid);
		}
		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
					ret, portid);
		}


		if (promiscuous_on) {
			ret = rte_eth_promiscuous_enable(portid);
			if (ret != 0) {
				rte_exit(EXIT_FAILURE,
						"rte_eth_promiscuous_enable:err=%s, port=%u\n",
						rte_strerror(-ret), portid);
			}
		}

		LOG_HINT("Port %u, MAC address: " RTE_ETHER_ADDR_PRT_FMT,
				portid, RTE_ETHER_ADDR_BYTES(&ports_eth_addr[portid]));

		device_show_info(portid);
		LOG_HINT("done.");

		/* TODO initialize port stats */
	}

	if (nb_ports_available != nb_physical_ports_available &&
			nb_physical_ports_available) {
		LOG_WARN("Mix physical and other ports are detected.");
	}

	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE,
				"All available ports are disabled. Please set portmask.\n");
	}

	check_all_ports_link_status(enabled_port_mask);

	/* base */
	base.dev_mode_mask = dev_mode_mask;
	base.g_state = &g_state;
	// base.nb_rxd = nb_rxd;
	// base.nb_txd = nb_txd;
	// base.promiscuous_on = promiscuous_on;
	// FIXME
	// base.enabled_port_mask = enabled_port_mask;
	// base.enabled_lcores = base.enabled_lcores;
	// base.timer_period = timer_period;
}

void init_rumtime()
{
	char name[32];
	pthread_getname_np(pthread_self(), name, sizeof(name));
	LOG_INFO("init runtime: %s pid %d tid %d self %lu", name,
			getpid(), rte_gettid(), (unsigned long)rte_thread_self().opaque_id);

	base.system->status.pid = getpid();
	base.system->status.ppid = getppid();
	CPU_ZERO(&base.system->status.cpuset);
	if (sched_getaffinity(0, sizeof(cpu_set_t), &base.system->status.cpuset)) {
		rte_exit(EXIT_FAILURE, "sched_getaffinity()");
	}
}

void run()
{
	LOG_INFO("main core %u init %d barriers",
			base.topo.main_core, base.topo.n_enabled_core + 1);
	/* including main lcore */
	pthread_barrier_init(&base.barrier, NULL, base.topo.n_enabled_core + 1);

	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(launch_one_core, (void*)base.bconf, CALL_MAIN);
	exit(0);
}

void stop()
{
	int ret = 0;
	uint32_t lcore_id = 0;
	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		int n = rte_eal_wait_lcore(lcore_id);
		if (n < 0) {
			LOG_WARN("lcore %d returnd %d\n", lcore_id, n);
			ret = -1;
			break;
		}
	}

	uint16_t portid;
	RTE_ETH_FOREACH_DEV(portid) {
		if ((enabled_port_mask & (1u << portid)) == 0) {
			continue;
		}
		LOG_INFO("Closing port %d ...", portid);
		ret = rte_eth_dev_stop(portid);
		if (ret != 0) {
			LOG_INFO("rte_eth_dev_stop: err=%d, port=%d\n", ret, portid);
		}
		rte_eth_dev_close(portid);
		LOG_INFO(" Done\n");
	}

	pthread_barrier_destroy(&base.barrier);

	base.return_value = ret;
}

void quit()
{
#ifdef RTE_LIB_PDUMP
	rte_pdump_uninit();
#endif

	/* clean up the EAL */
	rte_eal_cleanup();

	ws_server_stop(base.system->cfg.server.ctx);

	config_exit(base.config->root);

	LOG_WARN("Bye ...\n");
}

int main(int argc, char **argv)
{
	LOG_INFO("%p", ws_server_start(NULL));

	sleep(-1);

	init_base(argc, argv);

	init_config();

	init_system();
	while (1);

	init_eal();

	init_mbuf_dynfield();

	init_metrics();

	init_args();

	init_signal();

	base_init_topo();

	init_port();

	init_rumtime();

	run();

	stop();

	quit();

	return 0;
}
