#include "base.h"
#include "color.h"
#include "worker.h"
#include "metric.h"
#include "server.h"

/**
 * @file main.c
 * @brief Application entry point -- DPDK EAL init, device setup,
 *        worker launch, and WebSocket control-plane lifecycle.
 *
 * Startup flow:
 *   1. Parse config file (YAML) + CLI overrides
 *   2. rte_eal_init()
 *   3. Detect / configure NIC ports (RX/TX queues, offloads)
 *   4. Launch per-lcore workers
 *   5. Start embedded HTTP/WS server
 *   6. Wait for signal -> graceful shutdown
 */
#include "device.h"
#include "civetweb.h"
#include "log.h"
#include "args.h"
#include "bless_plugin.h"
#include "define.h"
#include "preflight.h"

#include <math.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

/* safe CLI integer parsing -- exits on invalid input */
static inline long parse_cli_long(const char *s, const char *name, long def)
{
	if (!s) {
		return def;
	}
	char *endptr = NULL;
	errno = 0;
	long v = strtol(s, &endptr, 10);
	if (errno || endptr == s || *endptr != '\0') {
		rte_exit(EXIT_FAILURE, "Invalid --%s value: %s\n", name, s);
	}
	return v;
}

/* Read and display the embedded .note.buildinfo ELF section.
 *
 * Opens /proc/self/exe, finds the .note.buildinfo section by name,
 * and dumps lines from the descriptor. Verbosity controls depth:
 *   0 -- no output (called when box was already shown alone)
 *   1 -- key fields only (git, build host, time, compiler)
 *   2 -- full dump including CFLAGS, LDFLAGS, and all linker flags
 * Key=value lines are aligned at a 24-char key column; continuation
 * lines (no '=', or starting with '-') receive an extra indent. */
static void show_buildinfo(int verbose)
{
	if (verbose < 1) {
		return;
	}

	// Whitelist for verbose=1 -- the frequently-referenced fields
	static const char *key_fields[] = {
		"git_branch",
		"git_commit",
		"build_host",
		"build_time",
		"cc_version",
		NULL
	};

	int fd = open("/proc/self/exe", O_RDONLY);
	if (fd < 0) {
		return;
	}

	Elf64_Ehdr ehdr;
	if (pread(fd, &ehdr, sizeof(ehdr), 0) != sizeof(ehdr)) {
		goto out;
	}
	if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
		goto out;
	}

	// Locate section header string table
	Elf64_Shdr shstr;
	off_t shstr_off = ehdr.e_shoff +
		ehdr.e_shstrndx * ehdr.e_shentsize;
	if (pread(fd, &shstr, sizeof(shstr), shstr_off) != sizeof(shstr)) {
		goto out;
	}

	char *strtab = (char*)malloc(shstr.sh_size);
	if (strtab == NULL) {
		goto out;
	}
	if (pread(fd, strtab, shstr.sh_size, shstr.sh_offset) !=
			(ssize_t)shstr.sh_size) {
		free(strtab);
		goto out;
	}

	// Scan sections for .note.buildinfo
	Elf64_Shdr sh;
	int found = 0;
	for (int i = 0; i < ehdr.e_shnum; i++) {
		off_t off = ehdr.e_shoff + i * ehdr.e_shentsize;
		if (pread(fd, &sh, sizeof(sh), off) != sizeof(sh)) {
			continue;
		}
		if (strcmp(strtab + sh.sh_name, ".note.buildinfo") != 0) {
			continue;
		}

		found = 1;
		char *data = (char*)malloc(sh.sh_size);
		if (data == NULL) {
			break;
		}
		if (pread(fd, data, sh.sh_size, sh.sh_offset) !=
				(ssize_t)sh.sh_size) {
			free(data);
			break;
		}

		// Walk ELF notes, looking for the BLESS descriptor
		size_t pos = 0;
		while (pos + sizeof(Elf64_Nhdr) <= sh.sh_size) {
			Elf64_Nhdr *nh = (Elf64_Nhdr*)(data + pos);
			uint32_t namesz = (nh->n_namesz + 3) & ~3u;
			uint32_t descsz = (nh->n_descsz + 3) & ~3u;

			char *name	= (char*)(data + pos + sizeof(*nh));
			char *desc	= (char*)(name + namesz);

			if (nh->n_type != 1 ||
					strcmp(name, "BLESS") != 0) {
				pos += sizeof(*nh) + namesz + descsz;
				continue;
			}

			// Found the BLESS note descriptor
			char *p = desc;
			char *end = desc + nh->n_descsz;
			int in_multi = 0;  // 1 = inside CFLAGS/LDFLAGS cont.
			while (p < end) {
				char *nl = (char*)memchr(p, '\n', end - p);
				if (nl == NULL) {
					nl = end;
				}

				// Strip leading whitespace
				const char *start = p;
				while (start < nl &&
						(*start == ' ' ||
						 *start == '	')) {
					start++;
				}

				char *eq = (char*)memchr(start, '=',
						nl - start);
				if (eq != NULL && eq > start &&
						start[0] != '-') {
					size_t klen = (size_t)(eq - start);

					// New key=value signals end of
					// any active multi-line row
					if (in_multi) {
						fputc('\n', stdout);
						in_multi = 0;
					}

					// Verbose=1: skip non-whitelisted
					if (verbose == 1) {
						int matched = 0;
						for (int ki = 0;
								key_fields[ki];
								ki++) {
							size_t kflen =
								strlen(key_fields[ki]);
							if (klen == kflen &&
									memcmp(start,
										key_fields[ki],
										klen) == 0) {
								matched = 1;
								break;
							}
						}
						if (!matched) {
							p = nl + 1;
							continue;
						}
					}

					// CFLAGS / LDFLAGS fold their
					// continuations onto one line
					int is_multi_key =
						verbose > 1 &&
						((klen == 6 &&
						  memcmp(start, "CFLAGS",
							  6) == 0) ||
						 (klen == 7 &&
						  memcmp(start, "LDFLAGS",
							  7) == 0));

					fputs("  ", stdout);
					fwrite(start, 1, klen, stdout);

					// Pad key column to 24 chars
					int pad = 24 - (int)klen;
					if (pad < 1) {
						pad = 1;
					}
					for (int k = 0; k < pad; k++) {
						fputc(' ', stdout);
					}
					fwrite(eq + 1, 1,
							(size_t)(nl - eq - 1),
							stdout);

					if (is_multi_key) {
						// Suppress trailing \n --
						// continuation lines will
						// append after a space
						in_multi = 1;
					} else {
						fputc('\n', stdout);
					}
				} else if (start < nl && verbose > 1) {
					// Continuation / non-key line
					if (in_multi) {
						// Append to CFLAGS/LDFLAGS
						// line with a space
						fputc(' ', stdout);
						fwrite(start, 1,
								(size_t)(nl - start),
								stdout);
					} else {
						fputs("    ", stdout);
						fwrite(start, 1,
								(size_t)(nl - start),
								stdout);
						fputc('\n', stdout);
					}
				}
				p = nl + 1;
			}
			// Flush any open multi-line row
			if (in_multi) {
				fputc('\n', stdout);
			}

			// Verbose=3: raw hex dump for byte-level inspection
			if (verbose >= 3) {
				fputs("\n  --- raw hex ---\n", stdout);
				size_t remain = nh->n_descsz;
				const unsigned char *raw =
					(const unsigned char*)desc;
				for (size_t off = 0; off < remain;
						off += 16) {
					fprintf(stdout, "  %04zx  ", off);
					size_t flen = remain - off;
					if (flen > 16) {
						flen = 16;
					}
					for (size_t b = 0; b < 16; b++) {
						if (b < flen) {
							fprintf(stdout,
									"%02x ",
									raw[off + b]);
						} else {
							fputs("   ", stdout);
						}
						if (b == 7) {
							fputc(' ', stdout);
						}
					}
					fputs(" |", stdout);
					for (size_t b = 0; b < flen; b++) {
						unsigned char c =
							raw[off + b];
						fputc(c >= 32 && c < 127
								? c : '.', stdout);
					}
					fputs("|\n", stdout);
				}
			}
			free(data);
			goto done;
		}
		free(data);
	}
	if (!found) {
		fputs("  (no embedded buildinfo)\n", stdout);
	}
done:
	free(strtab);
out:
	close(fd);
}

void base_show_version(void)
{
	/* UTF-8 box frame with per-field color accents */
	char build_str[64];
	snprintf(build_str, sizeof(build_str), "%s %s", BUILD_TYPE,
			STATIC ? "static" : "shared");

	fprintf(stdout, "%s", COLOR(C_RATE));
	fprintf(stdout, "+--------------------------------------------+\n");
	fprintf(stdout, "|%s  %-42s%s|\n",
			COLOR(ANSI_BOLD FG_BRIGHT_WHITE), "BLESS",
			COLOR(C_RATE));
	fprintf(stdout, "|%s  %-42s%s|\n",
			COLOR(C_TRACE), "Behavioral Leverage via Entropy-",
			COLOR(C_RATE));
	fprintf(stdout, "|%s  %-42s%s|\n",
			COLOR(C_TRACE), "controlled Systematic Synthesizer",
			COLOR(C_RATE));
	fprintf(stdout, "+--------------------------------------------+\n");
	fprintf(stdout, "|  %s%-10s%s  %s%-30s%s|\n",
			COLOR(C_ERR),    "Version", COLOR(C_RATE),
			COLOR(FG_RED),   BL_VERSION, COLOR(C_RATE));
	fprintf(stdout, "|  %s%-10s%s  %s%-30s%s|\n",
			COLOR(C_INFO),   "Branch",  COLOR(C_RATE),
			COLOR(FG_GREEN), GIT_BRANCH, COLOR(C_RATE));
	fprintf(stdout, "|  %s%-10s%s  %s%-30s%s|\n",
			COLOR(C_WARN),   "Commit",  COLOR(C_RATE),
			COLOR(FG_YELLOW), GIT_COMMIT, COLOR(C_RATE));
	if (GIT_STATE[0]) {
		fprintf(stdout, "|  %s%-10s%s  %s%-30s%s|\n",
			COLOR(C_TRACE), "State", COLOR(C_RATE),
			COLOR(FG_YELLOW), GIT_STATE, COLOR(C_RATE));
	}
	fprintf(stdout, "|  %s%-10s%s  %s%-30s%s|\n",
			COLOR(C_DEBUG),  "Build",   COLOR(C_RATE),
			COLOR(FG_CYAN),  build_str, COLOR(C_RATE));
	fprintf(stdout, "|  %s%-10s%s  %s%-30s%s|\n",
			COLOR(FG_MAGENTA), "Host", COLOR(C_RATE),
			COLOR(FG_BRIGHT_MAGENTA), BUILD_HOST, COLOR(C_RATE));
	fprintf(stdout, "|  %s%-10s%s  %s%-30s%s|\n",
			COLOR(C_HINT),   "Time",    COLOR(C_RATE),
			COLOR(FG_BRIGHT_BLACK), BUILD_TIME, COLOR(C_RATE));
	fprintf(stdout, "+--------------------------------------------+\n");
	fprintf(stdout, "%s", COLOR(ANSI_RESET));
}

struct base base;

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

static struct dist_ratio ratio;
static struct bless_encap_params bep;

// static struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_ETHPORTS];

static struct rte_eth_conf port_conf_default = {
};

static struct rte_eth_conf port_conf = {
	.txmode = {
		.mq_mode = RTE_ETH_MQ_TX_NONE,
		.offloads =
			RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |   /* IPv4 checksum */
			RTE_ETH_TX_OFFLOAD_UDP_CKSUM |    /* UDP checksum */
			RTE_ETH_TX_OFFLOAD_TCP_CKSUM |    /* TCP checksum */
			RTE_ETH_TX_OFFLOAD_OUTER_IPV4_CKSUM |
			RTE_ETH_TX_OFFLOAD_OUTER_UDP_CKSUM |
			// RTE_ETH_TX_OFFLOAD_UDP_TNL_TSO |
			// RTE_ETH_TX_OFFLOAD_VXLAN_TNL_TSO |
			// RTE_ETH_TX_OFFLOAD_VLAN_INSERT |  // VLAN insert
			// RTE_ETH_TX_OFFLOAD_MULTI_SEGS |   // Multi-segment send
			RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE, /* Quick mbuf free */
	},
	.rx_adv_conf = {
		.rss_conf = {
			/* RSS (Receive Side Scaling) config */
			.rss_key = NULL, /* Use default RSS hash key */
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
		base_show_core_view(base.topo.cv);
		worker_main_loop((void*)conf);
	} else {
		struct base_core_view *view = base.topo.cv + lcore_id;
		assert(view->numa == rte_socket_id());
		if (!view->enabled) {
			LOG_TRACE("skip unused core %u", lcore_id);
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
			LOG_WARN("exceeded max number of port pair params: %hu",
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
#define CMD_LINE_OPT_BATCH_JITTER_US "batch-jitter-us"
#define CMD_LINE_OPT_TRAFFIC_MODEL "traffic-model"
#define CMD_LINE_OPT_ENTROPY_TARGET "entropy-target"
#define CMD_LINE_OPT_ENTROPY_DIM "entropy-dim"
#define CMD_LINE_OPT_ENTROPY_GAIN "entropy-adapt-gain"
#define CMD_LINE_OPT_SAMPLE_INTERVAL "sample-interval"
#define CMD_LINE_OPT_BENCH_MODE    "bench-mode"
#define CMD_LINE_OPT_WEIGHT "weight"
/* backward compat aliases -- just set weight via name lookup */
#define CMD_LINE_OPT_ARP   "arp"
#define CMD_LINE_OPT_ICMP  "icmp"
#define CMD_LINE_OPT_TCP   "tcp"
#define CMD_LINE_OPT_UDP   "udp"
#define CMD_LINE_OPT_SCTP  "sctp"
#define CMD_LINE_OPT_QUIC  "quic"
#define CMD_LINE_OPT_HS_RATE "hs-rate"
#define CMD_LINE_OPT_HS_TIMEOUT "hs-timeout-us"
#define CMD_LINE_OPT_HS_MIX_RATIO "hs-mix-ratio"
#define CMD_LINE_OPT_PPS_RATE "pps-rate"
#define CMD_LINE_OPT_PPS_BURST "pps-burst"
#define CMD_LINE_OPT_BPS_RATE "bps-rate"
#define CMD_LINE_OPT_BPS_BURST "bps-burst"
#define CMD_LINE_OPT_INTERLEAVE "interleave"
#define CMD_LINE_OPT_INTERLEAVE_DEPTH "interleave-depth"
#define CMD_LINE_OPT_LATENCY_HIST_ENABLE "latency-hist-enable"
#define CMD_LINE_OPT_MI_SMOOTHING_WINDOW "mi-smoothing-window"
#define CMD_LINE_OPT_SEED "seed"

#define CMD_LINE_OPT_LOG_FORMAT "log-format"
#define CMD_LINE_OPT_STATS_DUMP "stats-dump"
#define CMD_LINE_OPT_PREFLIGHT "preflight"
#define CMD_LINE_OPT_ENVIRONMENT_DUMP "environment-dump"
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
	CMD_LINE_OPT_BATCH_JITTER_US_NUM,
	CMD_LINE_OPT_TRAFFIC_MODEL_NUM,
	CMD_LINE_OPT_ENTROPY_TARGET_NUM,
	CMD_LINE_OPT_ENTROPY_DIM_NUM,
	CMD_LINE_OPT_ENTROPY_GAIN_NUM,
	CMD_LINE_OPT_SAMPLE_INTERVAL_NUM,
	CMD_LINE_OPT_BENCH_MODE_NUM,
	CMD_LINE_OPT_WEIGHT_NUM,
	CMD_LINE_OPT_ARP_NUM,
	CMD_LINE_OPT_ICMP_NUM,
	CMD_LINE_OPT_TCP_NUM,
	CMD_LINE_OPT_UDP_NUM,
	CMD_LINE_OPT_SCTP_NUM,
	CMD_LINE_OPT_QUIC_NUM,
	CMD_LINE_OPT_HS_RATE_NUM,
	CMD_LINE_OPT_HS_TIMEOUT_NUM,
	CMD_LINE_OPT_HS_MIX_RATIO_NUM,
	CMD_LINE_OPT_PPS_RATE_NUM,
	CMD_LINE_OPT_PPS_BURST_NUM,
	CMD_LINE_OPT_BPS_RATE_NUM,
	CMD_LINE_OPT_BPS_BURST_NUM,
	CMD_LINE_OPT_INTERLEAVE_NUM,
	CMD_LINE_OPT_INTERLEAVE_DEPTH_NUM,
	CMD_LINE_OPT_LATENCY_HIST_ENABLE_NUM,
	CMD_LINE_OPT_MI_SMOOTHING_WINDOW_NUM,
	CMD_LINE_OPT_SEED_NUM,
	CMD_LINE_OPT_LOG_FORMAT_NUM,
	CMD_LINE_OPT_STATS_DUMP_NUM,
	CMD_LINE_OPT_PREFLIGHT_NUM,
	CMD_LINE_OPT_ENVIRONMENT_DUMP_NUM,
};

static const struct option lgopts[] = {
	{ CMD_LINE_OPT_NO_MAC_UPDATING, no_argument, 0, CMD_LINE_OPT_NO_MAC_UPDATING_NUM },
	{ CMD_LINE_OPT_PORTMAP_CONFIG, 1, 0, CMD_LINE_OPT_PORTMAP_NUM },
	{ CMD_LINE_OPT_AUTO_START, 1, 0, CMD_LINE_OPT_AUTO_START_NUM },
	{ CMD_LINE_OPT_MODE, 1, 0, CMD_LINE_OPT_MODE_NUM },
	{ CMD_LINE_OPT_NUM, 1, 0, CMD_LINE_OPT_NUM_NUM },
	{ CMD_LINE_OPT_BATCH, 1, 0, CMD_LINE_OPT_BATCH_NUM },
	{ CMD_LINE_OPT_BATCH_DELAY_US, 1, 0, CMD_LINE_OPT_BATCH_DELAY_US_NUM },
	{ CMD_LINE_OPT_BATCH_JITTER_US, 1, 0, CMD_LINE_OPT_BATCH_JITTER_US_NUM },
	{ CMD_LINE_OPT_TRAFFIC_MODEL, 1, 0, CMD_LINE_OPT_TRAFFIC_MODEL_NUM },
	{ CMD_LINE_OPT_ENTROPY_TARGET, 1, 0, CMD_LINE_OPT_ENTROPY_TARGET_NUM },
	{ CMD_LINE_OPT_ENTROPY_DIM, 1, 0, CMD_LINE_OPT_ENTROPY_DIM_NUM },
	{ CMD_LINE_OPT_ENTROPY_GAIN, 1, 0, CMD_LINE_OPT_ENTROPY_GAIN_NUM },
	{ CMD_LINE_OPT_SAMPLE_INTERVAL, 1, 0, CMD_LINE_OPT_SAMPLE_INTERVAL_NUM },
	{ CMD_LINE_OPT_BENCH_MODE,    1, 0, CMD_LINE_OPT_BENCH_MODE_NUM },
	{ CMD_LINE_OPT_WEIGHT, 1, 0, CMD_LINE_OPT_WEIGHT_NUM },
	{ CMD_LINE_OPT_ARP, 1, 0, CMD_LINE_OPT_ARP_NUM },
	{ CMD_LINE_OPT_ICMP, 1, 0, CMD_LINE_OPT_ICMP_NUM },
	{ CMD_LINE_OPT_TCP, 1, 0, CMD_LINE_OPT_TCP_NUM },
	{ CMD_LINE_OPT_UDP, 1, 0, CMD_LINE_OPT_UDP_NUM },
	{ CMD_LINE_OPT_SCTP, 1, 0, CMD_LINE_OPT_SCTP_NUM },
	{ CMD_LINE_OPT_QUIC, 1, 0, CMD_LINE_OPT_QUIC_NUM },
	{ CMD_LINE_OPT_HS_RATE, 1, 0, CMD_LINE_OPT_HS_RATE_NUM },
	{ CMD_LINE_OPT_HS_TIMEOUT, 1, 0, CMD_LINE_OPT_HS_TIMEOUT_NUM },
	{ CMD_LINE_OPT_HS_MIX_RATIO, 1, 0, CMD_LINE_OPT_HS_MIX_RATIO_NUM },
	{ CMD_LINE_OPT_PPS_RATE, 1, 0, CMD_LINE_OPT_PPS_RATE_NUM },
	{ CMD_LINE_OPT_PPS_BURST, 1, 0, CMD_LINE_OPT_PPS_BURST_NUM },
	{ CMD_LINE_OPT_BPS_RATE, 1, 0, CMD_LINE_OPT_BPS_RATE_NUM },
	{ CMD_LINE_OPT_BPS_BURST, 1, 0, CMD_LINE_OPT_BPS_BURST_NUM },
	{ CMD_LINE_OPT_INTERLEAVE, 1, 0, CMD_LINE_OPT_INTERLEAVE_NUM },
	{ CMD_LINE_OPT_INTERLEAVE_DEPTH, 1, 0, CMD_LINE_OPT_INTERLEAVE_DEPTH_NUM },
	{ CMD_LINE_OPT_LATENCY_HIST_ENABLE, 1, 0, CMD_LINE_OPT_LATENCY_HIST_ENABLE_NUM },
	{ CMD_LINE_OPT_MI_SMOOTHING_WINDOW, 1, 0, CMD_LINE_OPT_MI_SMOOTHING_WINDOW_NUM },
	{ CMD_LINE_OPT_LOG_FORMAT, 1, 0, CMD_LINE_OPT_LOG_FORMAT_NUM },
	{ CMD_LINE_OPT_SEED, 1, 0, CMD_LINE_OPT_SEED_NUM },
	{ CMD_LINE_OPT_STATS_DUMP, 1, 0, CMD_LINE_OPT_STATS_DUMP_NUM },
	{ CMD_LINE_OPT_PREFLIGHT, 1, 0, CMD_LINE_OPT_PREFLIGHT_NUM },
	{ CMD_LINE_OPT_ENVIRONMENT_DUMP, 1, 0, CMD_LINE_OPT_ENVIRONMENT_DUMP_NUM },
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

	bep.inner = (struct mbuf_conf*)malloc(sizeof(struct mbuf_conf));
	if (!bep.inner) {
		LOG_ERR("malloc(mbuf_conf) failed");
		return -1;
	}

	argvopt = argv;
	port_pair_params = NULL;

	dist_ratio_init(&ratio);

	while ((opt = getopt_long(argc, argvopt, short_options,
					lgopts, &option_index)) != EOF) {
		switch (opt) {
			/* portmask */
			case 'p':
				bconf->enabled_port_mask = parse_portmask(optarg);
				if (bconf->enabled_port_mask == 0) {
					LOG_ERR("invalid portmask");
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
					LOG_ERR("invalid queue number");
					usage(prgname);
					return -1;
				}
				break;

			case 'T': /* timer period */
				timer_secs = parse_timer_period(optarg);
				if (timer_secs < 0) {
					LOG_ERR("invalid timer period");
					usage(prgname);
					return -1;
				}
				bconf->timer_period = timer_secs;
				break;
				/* long options */
			case CMD_LINE_OPT_NUM_NUM:
				bconf->num = optarg ? parse_cli_long(optarg, "num", 0) : 0;
				ratio.num = optarg ? parse_cli_long(optarg, "num", 0) : 0;
				break;
			case CMD_LINE_OPT_BATCH_NUM:
				bconf->batch = optarg ? parse_cli_long(optarg, "num", 0) : 256;
				break;
			case CMD_LINE_OPT_BATCH_DELAY_US_NUM:
				bconf->batch_delay_us = optarg ? parse_cli_long(optarg, "num", 0) : 0;
				break;
			case CMD_LINE_OPT_BATCH_JITTER_US_NUM:
				bconf->batch_jitter_us = optarg ? parse_cli_long(optarg, "num", 0) : 0;
				break;
			case CMD_LINE_OPT_TRAFFIC_MODEL_NUM:
				bconf->traffic_model = optarg ? (uint8_t)parse_cli_long(optarg, "num", 0) : 0;
				break;
			case CMD_LINE_OPT_ENTROPY_TARGET_NUM:
				bconf->entropy_target = optarg ? atof(optarg) : 0.0;
				break;
			case CMD_LINE_OPT_ENTROPY_DIM_NUM:
				bconf->entropy_dim = optarg ? (uint8_t)parse_cli_long(optarg, "num", 0) : 0;
				break;
			case CMD_LINE_OPT_ENTROPY_GAIN_NUM:
				bconf->entropy_adapt_gain = optarg ? atof(optarg) : 0.1;
				break;
			case CMD_LINE_OPT_SAMPLE_INTERVAL_NUM:
				bconf->sample_interval = optarg ? (uint32_t)parse_cli_long(optarg, "num", 0) : 10;
				break;
			case CMD_LINE_OPT_BENCH_MODE_NUM:
				bconf->bench_mode = (optarg && !strcmp(optarg, "template")) ? 1 : 0;
				break;
			case CMD_LINE_OPT_WEIGHT_NUM:
				{
					/* Parse --weight name=value */
					if (!optarg) {
						break;
					}
					char *eq = strchr(optarg, '=');
					if (!eq || eq == optarg) {
						rte_exit(EXIT_FAILURE,
								"--weight: expected 'name=value', got '%s'\n", optarg);
					}
					size_t nlen = (size_t)(eq - optarg);
					char name[64];
					if (nlen >= sizeof(name)) {
						nlen = sizeof(name) - 1;
					}
					memcpy(name, optarg, nlen);
					name[nlen] = '\0';
					char *end = NULL;
					long v = strtol(eq + 1, &end, 10);
					if (*end || v < 0 || v > 100000) {
						rte_exit(EXIT_FAILURE,
								"--weight: invalid value for '%s'\n", name);
					}
					bless_set_type_weight(name, (int32_t)v);
					/* For backward compat with legacy --tcp=N / --sctp=N usage pattern,
					 * also set ratio.weight for built-in types. */
					for (int i = 0; i < TYPE_MAX; i++) {
						if (strcmp(name, bless_get_type_name(i)) == 0) {
							ratio.weight[i] = (int32_t)v;
							break;
						}
					}
					break;
				}
				/* backward compat CLI aliases -- all set weight by name */
			case CMD_LINE_OPT_ARP_NUM:
				bless_set_type_weight("arp", (int32_t)parse_cli_long(optarg, "num", 0));
				ratio.weight[TYPE_ARP] = (int32_t)parse_cli_long(optarg, "num", 0);
				break;
			case CMD_LINE_OPT_ICMP_NUM:
				bless_set_type_weight("icmp", (int32_t)parse_cli_long(optarg, "num", 0));
				ratio.weight[TYPE_ICMP] = (int32_t)parse_cli_long(optarg, "num", 0);
				break;
			case CMD_LINE_OPT_TCP_NUM:
				bless_set_type_weight("tcp", (int32_t)parse_cli_long(optarg, "num", 0));
				ratio.weight[TYPE_TCP] = (int32_t)parse_cli_long(optarg, "num", 0);
				break;
			case CMD_LINE_OPT_UDP_NUM:
				bless_set_type_weight("udp", (int32_t)parse_cli_long(optarg, "num", 0));
				ratio.weight[TYPE_UDP] = (int32_t)parse_cli_long(optarg, "num", 0);
				break;
			case CMD_LINE_OPT_SCTP_NUM:
				bless_set_type_weight("sctp", (int32_t)parse_cli_long(optarg, "num", 0));
				ratio.weight[TYPE_SCTP] = (int32_t)parse_cli_long(optarg, "num", 0);
				break;
			case CMD_LINE_OPT_QUIC_NUM:
				bless_set_type_weight("quic", (int32_t)parse_cli_long(optarg, "num", 0));
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
			case CMD_LINE_OPT_INTERLEAVE_NUM:
				if (optarg && 0 == strcmp(optarg, "true")) {
					bconf->interleave = 1;
				}
				break;
			case CMD_LINE_OPT_INTERLEAVE_DEPTH_NUM:
				if (optarg) {
					int d = (int)parse_cli_long(optarg, "interleave-depth", 100);
					bconf->interleave_depth = d < 1 ? 1 : (d > 100 ? 100 : (uint8_t)d);
				}
				break;
			case CMD_LINE_OPT_LATENCY_HIST_ENABLE_NUM:
				if (optarg && 0 == strcmp(optarg, "true")) {
					bconf->latency_hist_enable = 1;
				}
				break;
			case CMD_LINE_OPT_MI_SMOOTHING_WINDOW_NUM:
				if (optarg) {
					bconf->mi_smoothing_window = (uint32_t)parse_cli_long(optarg, "mi-smoothing-window", 1);
				}
				break;
			case CMD_LINE_OPT_MODE_NUM:
				if (!optarg || !strcmp(optarg, "tx-only")) {
					bconf->mode = BLESS_MODE_TX_ONLY;
				} else if (!strcmp(optarg, "rx-only")) {
					bconf->mode = BLESS_MODE_RX_ONLY;
				} else if (!strcmp(optarg, "fwd")) {
					bconf->mode = BLESS_MODE_FWD;
				} else if (!strcmp(optarg, "handshake")) {
					bconf->mode = BLESS_MODE_HANDSHAKE;
				} else {
					rte_exit(EXIT_FAILURE, "Invalid mode: %s.\n", optarg);
				}
				break;
			case CMD_LINE_OPT_NO_MAC_UPDATING_NUM:
				mac_updating = 0;
				break;
			case CMD_LINE_OPT_HS_RATE_NUM:
				bconf->hs_rate = parse_cli_long(optarg, "num", 0);
				break;
			case CMD_LINE_OPT_HS_TIMEOUT_NUM:
				bconf->hs_timeout_us = parse_cli_long(optarg, "hs-timeout-us", 0);
				break;
			case CMD_LINE_OPT_HS_MIX_RATIO_NUM:
				bconf->hs_mix_ratio = (uint16_t)parse_cli_long(optarg, "num", 0);
				if (bconf->hs_mix_ratio > 1000) {
					bconf->hs_mix_ratio = 1000;
				}
				break;
			case CMD_LINE_OPT_PPS_RATE_NUM:
				bconf->pps_rate = optarg ? (uint32_t)parse_cli_long(optarg, "num", 0) : 0;
				break;
			case CMD_LINE_OPT_PPS_BURST_NUM:
				bconf->pps_burst = optarg ? (uint32_t)parse_cli_long(optarg, "num", 0) : 0;
				break;
			case CMD_LINE_OPT_BPS_RATE_NUM:
				bconf->bps_rate = optarg ? (uint32_t)parse_cli_long(optarg, "num", 0) : 0;
				break;
			case CMD_LINE_OPT_BPS_BURST_NUM:
				bconf->bps_burst = optarg ? (uint32_t)parse_cli_long(optarg, "num", 0) : 0;
				break;
			case CMD_LINE_OPT_SEED_NUM:
				if (optarg) {
					fast_rand_set_seed((uint64_t)parse_cli_long(optarg, "seed", 0));
				}
				break;
			case CMD_LINE_OPT_LOG_FORMAT_NUM:
				if (optarg && !strcmp(optarg, "json")) {
					log_set_format_json(1);
				}
				break;
			case CMD_LINE_OPT_STATS_DUMP_NUM:
				if (optarg) {
					bconf->stats_dump_path = strdup(optarg);
				}
				break;
			case CMD_LINE_OPT_PREFLIGHT_NUM:
				if (!optarg || !strcmp(optarg, "warn")) {
					bconf->preflight_mode = PREFLIGHT_WARN;
				} else if (!strcmp(optarg, "strict")) {
					bconf->preflight_mode = PREFLIGHT_STRICT;
				} else if (!strcmp(optarg, "off")) {
					bconf->preflight_mode = PREFLIGHT_OFF;
				} else {
					rte_exit(EXIT_FAILURE, "Invalid --preflight value: %s\n", optarg);
				}
				break;
			case CMD_LINE_OPT_ENVIRONMENT_DUMP_NUM:
				if (optarg) {
					free(bconf->environment_dump_path);
					bconf->environment_dump_path = strdup(optarg);
				}
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

	return ret;
}

/*
 * Check port pair config with enabled port mask,
 * and for valid port pair combinations.
 */


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
			if (device_is_full_featured(etype)) {
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
					LOG_WARN("Port %u link get failed: %s",
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
			LOG_INFO("done");
		}
	}
}

static int mbuf_dynfields_offset[MBUF_DYNFIELDS_MAX];

enum {
	MBUF_DYN_TYPE = 0,
};

/* Register mbuf dynamic fields used by the bless pipeline.
 *
 * Dynamic fields let per-packet metadata (e.g. type tag, TX timestamp)
 * ride along without permanent mbuf struct modifications.
 * Returns 0 on success, negative on registration failure. */
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
		LOG_HINT("  name: %s", md->name);
		LOG_HINT("  size: %lu", md->size);
		LOG_HINT("  align: %lu", md->align);
	}

	rte_mbuf_dyn_dump(stdout);

	return 0;
}

/* Print a core-view table entry for debugging. */
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
		LOG_SHOW("%s  enabled    %u", pref,v->enabled);
		LOG_SHOW("%s  numa       %u", pref,v->numa);
		LOG_SHOW("%s  core       %u", pref,v->core);
		LOG_SHOW("%s  role       %u", pref,v->role);
		LOG_SHOW("%s  port       %u", pref,v->port);
		LOG_SHOW("%s  type       %u (%s)", pref,v->type, device_get_string(v->type));
		LOG_SHOW("%s  rxq        %u", pref,v->rxq);
		LOG_SHOW("%s  txq        %u", pref,v->txq);
	}
}

/* Display the core-view table to stdout. */
void base_show_core_view(struct base_core_view *view)
{
	base_show_core_view_format(view, "");
}

/* Log the topology summary: NUMA count, cores, ports, and core-view table. */
void base_show_topo(struct base_topo *topo)
{
	if (!topo) {
		return;
	}
	// rte_lcore_dump(stdout);

	LOG_HINT("base_topo           %p", topo);
	LOG_SHOW("  n_numa            %u", topo->n_numa);
	LOG_SHOW("  n_core            %u", topo->n_core);
	LOG_SHOW("  n_enabled_core    %u", topo->n_enabled_core);
	LOG_SHOW("  n_port            %u", topo->n_port);
	LOG_SHOW("  n_enabled_port    %u", topo->n_enabled_port);
	LOG_SHOW("  port_mask         %u", topo->port_mask);
	LOG_SHOW("  main_core         %u", topo->main_core);

	base_show_core_view_format(topo->cv, "  ");
}

/* Log the base struct summary. */
void base_show()
{
	LOG_INFO("base %p", &base);
}

/* Gracefully tear down the base struct -- sleep briefly then log exit. */
void exit_base()
{
	usleep(300000);
	LOG_INFO("bless exit");
}

/* Phase 0: Allocate the config struct and stash CLI argc/argv. */
void init_base(int argc, char **argv)
{
	base.argc = argc;
	base.argv = argv;
	base.nb_rxd = RX_DESC_DEFAULT;
	base.nb_txd = TX_DESC_DEFAULT;
	base.mempool_cache_size = MEMPOOL_CACHE_SIZE;
	base.max_pkt_burst = MAX_PKT_BURST;
	struct config *cfg = (struct config*)malloc(sizeof(struct config));
	if (!cfg) {
		rte_exit(EXIT_FAILURE, "malloc(config)\n");
	}
	base.config = cfg;
}

/* Phase 5: Build the topology from available DPDK ports and enabled lcores.
 *
 * Maps port <-> queue <-> lcore assignments into base.topo.core_view[].
 * Skips disabled physical ports, handles PCAP single-queue constraint.
 * Ensures enough worker cores exist for the requested port+queue count. */
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

	topo->port_mask = base.bconf->enabled_port_mask;
	topo->n_enabled_port = rte_popcount32(topo->port_mask);
	topo->n_numa = rte_socket_count();

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
		if (device_is_full_featured(etype)) {
			if ((topo->port_mask & (1u << port_id)) == 0) {
				LOG_HINT("skip physical port %d", port_id);
				continue;
			}
		}

		/* Physical NICs report a real socket; virtual devices return -1.
		 * Prefer cores on the same NUMA node to avoid cross-socket
		 * memory latency (~2x penalty on typical dual-socket systems). */
		int port_socket = rte_eth_dev_socket_id(port_id);

		for (uint32_t qid = 0; qid < rxtxq_per_port; qid++) {
			uint16_t best = RTE_MAX_LCORE;

			if (port_socket >= 0) {
				/* First pass: same-socket core. */
				for (uint16_t c = core_id; c < RTE_MAX_LCORE; c++) {
					if (!rte_lcore_is_enabled(c) ||
					    c == rte_get_main_lcore()) {
						continue;
					}
					if (cv[c].enabled) {
						continue;
					}
					if (rte_lcore_to_socket_id(c) ==
					    (unsigned int)port_socket) {
						best = c;
						break;
					}
				}
			}

			if (best >= RTE_MAX_LCORE) {
				/* Fallback: any available core (cross-socket). */
				for (uint16_t c = core_id;
				     c < RTE_MAX_LCORE; c++) {
					if (!rte_lcore_is_enabled(c) ||
					    c == rte_get_main_lcore()) {
						continue;
					}
					if (cv[c].enabled) {
						continue;
					}
					best = c;
					break;
				}
			}

			if (best >= RTE_MAX_LCORE) {
				rte_exit(EXIT_FAILURE,
				         "[%s %d] Not enough cores\n",
				         __func__, __LINE__);
			}

			view = cv + best;
			view->enabled = best + 1;
			view->core = best;
			view->port = port_id;
			view->type = etype;
			view->numa = rte_lcore_to_socket_id(best);

			if (port_socket >= 0 &&
			    (int)view->numa != port_socket) {
				LOG_WARN("core %u (socket %u) -> port %u"
				         " (socket %d) cross-NUMA",
				         best, view->numa,
				         port_id, port_socket);
			} else {
				LOG_INFO("core %u uses port %u(%s)"
				         " with queue %u",
				         best, port_id,
				         device_get_string(etype), qid);
			}

			view->rxq = qid;
			view->txq = qid;
			core_id = best + 1;
			topo->n_enabled_core++;

			/* Simple virtual devices use a single TX queue. */
			if (device_is_simple_vdev(etype)) {
				LOG_WARN("core %u uses %s port %u with 1 txq",
				         view->core,
				         device_get_string(etype),
				         view->port);
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

	/* init core view */
	RTE_LCORE_FOREACH_WORKER(core_id) {
		topo->n_core += ROLE_RTE == rte_eal_lcore_role(core_id);
		view = cv + core_id;
		view->role = rte_eal_lcore_role(core_id);
		view->numa = rte_lcore_to_socket_id(core_id);
	}
	if (topo->n_enabled_core > topo->n_core) {
		rte_exit(EXIT_FAILURE, "Not enough cores");
	} else {
		LOG_HINT("%d core will not be used", topo->n_core - topo->n_enabled_core);
	}

	LOG_INFO("%u cores will be used excluding main", topo->n_enabled_core);

	/* Diagnostic: worker topology summary. */
	LOG_INFO("topo: n_core=%u n_enabled_core=%u main_core=%u",
		 topo->n_core, topo->n_enabled_core, topo->main_core);
	for (unsigned lc = 0; lc < RTE_MAX_LCORE; lc++) {
		if (!cv[lc].enabled && lc != topo->main_core) {
			continue;
		}
		LOG_INFO("  core view lcore=%u enabled=%u core=%u port=%u type=%u "
			 "rxq=%u txq=%u numa=%u role=%u",
			 lc, cv[lc].enabled, cv[lc].core, cv[lc].port,
			 cv[lc].type, cv[lc].rxq, cv[lc].txq,
			 cv[lc].numa, cv[lc].role);
	}

	/* check port mask to possible port mask */
	if (base.bconf->enabled_port_mask & ~((1 << base.topo.n_port) - 1)) {
		rte_exit(EXIT_FAILURE, "Invalid portmask; possible (0x%x)\n",
				(1 << base.topo.n_port) - 1);
	}

	base_show_topo(topo);

	return 0;
}

/* Phase 1b: Parse YAML config, start WebSocket server, daemonise.
 *
 * Order: config_parse_system -> log_init -> [daemon] -> ws_server_start.
 * The HTTP/WS control plane is operational after this call. */
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

	if (!server->enable) {
		LOG_INFO("server.enable is false — "
			 "running without HTTP/WS server");
	} else {
		struct mg_context *ctx = ws_server_start(&server->wsud);
		if (!ctx) {
			rte_exit(EXIT_FAILURE, "ws_server_start failed\n");
		}
		system->cfg.server.ctx = ctx;
	}
	base.system = system;
	system_show(system);
}

/* Phase 1a: Load and parse the YAML config file.
 *
 * Uses the first CLI argument as the config path (default: ``conf/config.yaml``).
 * Special subcommands: ``version`` prints build info, ``config`` prints a template. */
void init_config()
{
#define DEFAULT_CONFIG_FILE "conf/config.yaml"
	struct config *cfg = base.config;
	cfg->cfm.name = DEFAULT_CONFIG_FILE;
	if (1 == base.argc) {
		LOG_INFO("use default config %s", cfg->cfm.name);
	} else if (0 == strcmp(base.argv[1], "version") ||
		   0 == strcmp(base.argv[1], "--version")) {
		int v = 0;
		if (base.argc > 2) {
			if (0 == strcmp(base.argv[2], "-v")) {
				v = 1;
			} else if (0 == strcmp(base.argv[2], "-vv")) {
				v = 2;
			} else if (0 == strcmp(base.argv[2], "-vvv")) {
				v = 3;
			}
		}
		printf("BLESS %s", BL_VERSION);
		if (v > 0) {
			printf(" (%s @ %s", GIT_BRANCH, GIT_COMMIT);
			if (GIT_STATE[0]) {
				printf("-%s", GIT_STATE);
			}
			printf(", %s %s)", BUILD_TYPE,
			       STATIC ? "static" : "shared");
		}
		printf("\n");
		if (v > 0) {
			show_buildinfo(v);
		}
		printf("\n");
		exit(0);
	} else if (0 == strcmp(base.argv[1], "config")) {
		server_config_template();
		exit(0);
	} else if (0 == strcmp(base.argv[1], "help") ||
			0 == strcmp(base.argv[1], "--help") ||
			0 == strcmp(base.argv[1], "-h")) {
		base_show_version();
		printf("\n");
		printf("Usage:  %s [EAL options] -- [options] [<config.yaml>]\n",
				base.argv[0]);
		printf("\n");
		printf("Subcommands:\n");
		printf("  version, --version  Print version and build info\n");
		printf("                    -v     + key build fields\n");
		printf("                    -vv    + full dump\n");
		printf("                    -vvv   + raw hex dump\n");
		printf("  config            Print default YAML config template\n");
		printf("  help              Show this help message\n");
		printf("\n");
		printf("Config file (first non-flag argument):\n");
		printf("  bless <path>      Path to YAML config (default: conf/config.yaml)\n");
		printf("\n");
		printf("For full CLI flag reference run without arguments and see the\n");
		printf("EAL + bless option output (--help within EAL also works).\n");
		printf("\n");
		printf("Web dashboard:\n");
		printf("  Point a browser at http://<host>:<port>/ (default :8080)\n");
		printf("  Endpoints: /metrics, /entropy, /observe, /stats\n");
		printf("\n");
		exit(0);
	} else {
		cfg->cfm.name = base.argv[1];
	}

	if (config_check_file_map(&cfg->cfm)) {
		rte_exit(EXIT_FAILURE, "Cannot check %s\n", cfg->cfm.name);
	}

	/* init config node from file, aka. parse yaml */
	cfg->root = config_init(&cfg->cfm);
	if (!cfg->root) {
		rte_exit(EXIT_FAILURE, "Cannot init config %s\n", cfg->cfm.name);
	}

	config_show(cfg);
}

/* Phase 2: Parse DPDK EAL args from config and call rte_eal_init().
 *
 * Merges ``dpdk:`` YAML section with command-line EAL parameters.
 * Initialises the DPDK Environment Abstraction Layer. */
void init_eal()
{
	/* parse dpdk, injector, bless */
	if (-1 == config_parse_dpdk(base.config->root, &base.argc, &base.argv)) {
		rte_exit(EXIT_FAILURE, "Cannot parse %s\n", base.config->cfm.name);
	}
	/* now base.{argc, argv} have all dpdk and app params */

	/* Diagnostic: effective EAL argv before rte_eal_init(). */
	LOG_INFO("EAL argc=%d argv:", base.argc);
	for (int ai = 0; ai < base.argc; ai++)
		LOG_INFO("  argv[%d]=\"%s\"", ai, base.argv[ai]);

	int ret = rte_eal_init(base.argc, base.argv);
	if (ret < 0) {
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	}

	/* Diagnostic: EAL lcore layout after rte_eal_init(). */
	LOG_INFO("rte_lcore_count()=%u  rte_get_main_lcore()=%u",
		 rte_lcore_count(), rte_get_main_lcore());
	for (unsigned lc = 0; lc < RTE_MAX_LCORE; lc++) {
		if (!rte_lcore_is_enabled(lc)) {
			continue;
		}
		LOG_INFO("  lcore %u: role=%u cpu=%u socket=%u",
			 lc, (unsigned)rte_eal_lcore_role(lc),
			 rte_lcore_to_cpu_id(lc),
			 rte_lcore_to_socket_id(lc));
	}

	base.args_jump = ret;
}

/* Phase 4a: Parse bless-specific args and merge CLI + YAML configs.
 *
 * - Allocates bless_conf if needed
 * - Parses CLI flags via parse_args()
 * - Parses YAML bless section via config_parse_bless()
 * - Validates and logs the max possible entropy per dimension
 *
 * Does NOT touch EAL runtime state (timer, rte_malloc, atomics).
 * Does NOT build the distribution table (deferred to init_runtime_state). */
static void parse_and_merge_config(void)
{
	struct bless_conf *bconf = base.bconf;
	if (!bconf) {
		bconf = bless_init();
		if (!bconf) {
			rte_exit(EXIT_FAILURE, "Cannot malloc(bless_conf)\n");
		}
		base.bconf = bconf;
	}

	/* parse application(bless) arguments from base.{argc, argv} */
	if (parse_args(base.argc - base.args_jump, base.argv + base.args_jump) < 0) {
		rte_exit(EXIT_FAILURE, "Invalid BLESS arguments\n");
	}
	if (fast_rand_get_seed()) {
		LOG_HINT("PRNG seed: %lu (deterministic replay)", (unsigned long)fast_rand_get_seed());
	} else {
		LOG_HINT("PRNG seed: auto (rdtsc-based)");
	}
	LOG_HINT("MAC updating %s", mac_updating ? "enabled" : "disabled");

	if (base.config->root && !bconf->cnode) {
		bconf->cnode = config_parse_bless(base.config->root);
		if (!bconf->cnode) {
			rte_exit(EXIT_FAILURE, "Cannot parse bless\n");
		}
		base.config->cnode = bconf->cnode;

		/* entropy target validation: log max possible from configured ranges */
		Cnode *c = bconf->cnode;
		uint32_t n_sip = 0, n_dip = 0, n_spt = 0, n_dpt = 0;

		if (c->ether.type.ipv4.src_range > 0) {
			n_sip = (uint32_t)c->ether.type.ipv4.src_range;
		} else if (c->ether.type.ipv4.n_dst > 0) { /* NOTE: config.c stores n_dst for src */
			n_sip = c->ether.type.ipv4.n_dst;
		}

		if (c->ether.type.ipv4.dst_range > 0) {
			n_dip = (uint32_t)c->ether.type.ipv4.dst_range;
		} else if (c->ether.type.ipv4.n_dst > 0) {
			n_dip = c->ether.type.ipv4.n_dst;
		}

		if (c->ether.type.ipv4.tcp.src_range > 0 ||
				c->ether.type.ipv4.udp.src_range > 0) {
			if (c->ether.type.ipv4.tcp.src_range > 0) {
				n_spt = (uint32_t)c->ether.type.ipv4.tcp.src_range;
			} else if (c->ether.type.ipv4.tcp.n_src > 0) {
				n_spt = (uint32_t)c->ether.type.ipv4.tcp.n_src;
			}
			if (c->ether.type.ipv4.udp.src_range > 0) {
				n_spt = MAX(n_spt, (uint32_t)c->ether.type.ipv4.udp.src_range);
			} else if ((uint32_t)c->ether.type.ipv4.udp.n_src > n_spt) {
				n_spt = c->ether.type.ipv4.udp.n_src;
			}
		}
		/* SCTP ext configs -- aggregate port ranges generically */
		bless_ext_aggregate_port_ranges(c, &n_spt, &n_dpt);
		if (c->ether.type.ipv4.tcp.dst_range > 0 ||
				c->ether.type.ipv4.udp.dst_range > 0) {
			if (c->ether.type.ipv4.tcp.dst_range > 0) {
				n_dpt = (uint32_t)c->ether.type.ipv4.tcp.dst_range;
			} else if (c->ether.type.ipv4.tcp.n_dst > 0) {
				n_dpt = (uint32_t)c->ether.type.ipv4.tcp.n_dst;
			}
			if (c->ether.type.ipv4.udp.dst_range > 0) {
				n_dpt = MAX(n_dpt, (uint32_t)c->ether.type.ipv4.udp.dst_range);
			} else if ((uint32_t)c->ether.type.ipv4.udp.n_dst > n_dpt) {
				n_dpt = c->ether.type.ipv4.udp.n_dst;
			}
		}

		LOG_INFO("Entropy max possible: src_ip=%.2f (%u), dst_ip=%.2f (%u), "
				"src_port=%.2f (%u), dst_port=%.2f (%u) bits",
				n_sip > 0 ? log2((double)n_sip) : 32.0, n_sip,
				n_dip > 0 ? log2((double)n_dip) : 32.0, n_dip,
				n_spt > 0 ? log2((double)n_spt) : 16.0, n_spt,
				n_dpt > 0 ? log2((double)n_dpt) : 16.0, n_dpt);

	/* Build distribution AFTER both CLI flags and YAML weights are parsed */
	if (-EPERM == bless_set_dist(bconf, &ratio, &bep)) {
		rte_exit(EXIT_FAILURE, "Cannot bless_set_dist()\n");
	}
	} else {
		LOG_WARN("No config.ymal used");
		rte_exit(EXIT_FAILURE, "Cannot init bless\n");
	}
}

/* Phase 4b: Build the distribution table and initialise runtime state
 * (state machine, timer, stats arrays, pointer wiring).
 *
 * Must be called AFTER parse_and_merge_config(). */
static void init_runtime_state(void)
{
	struct bless_conf *bconf = base.bconf;

	atomic_store(&g_state, bconf->auto_start ? STATE_RUNNING : STATE_INIT);

	/* convert to number of cycles */
	bconf->timer_period *= rte_get_timer_hz();

	bconf->dst_ports = dst_ports;
	bconf->state = &g_state;

	bconf->barrier = &base.barrier;
	bconf->base = &base;
}

/* Signal handler: set state flag for graceful shutdown on SIGINT/SIGTERM.
 *
 * Async-signal-safe: only calls atomic_store. The main loop polls g_state
 * and triggers exit_base() from a safe context. */
static void signal_handler(int signum)
{
	(void)signum;
	atomic_store(&g_state, STATE_EXIT);
}

/* Register SIGINT and SIGTERM with sigaction for portable signal handling. */
void init_signal(void)
{
	struct sigaction sa = {
		.sa_handler = signal_handler,
		.sa_flags   = SA_RESTART,
	};
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

/* Phase 6: Configure and start all DPDK ports.
 *
 * Creates the mbuf pool (sized for RX rings + burst + cache + margin),
 * configures each port (RX/TX queues, offloads, promiscuous mode),
 * starts the device, and waits for link-up.
 * Skips physical ports not in enabled_port_mask. */
void init_port()
{
	LOG_HINT("Calculating number of mbufs to be used");
	uint16_t available_ports = __builtin_popcount(base.bconf->enabled_port_mask);
	uint32_t nb_mbufs =
		available_ports * rxtxq_per_port * base.nb_rxd      /* RX ring hard requirement */
		+ available_ports * base.max_pkt_burst * rxtxq_per_port
		+ base.topo.n_enabled_core * base.mempool_cache_size * 2
		+ 8192; /* Safety margin */
	LOG_HINT("number of mbufs = ");
	LOG_HINT("  available_ports * rxtxq_per_port * base.nb_rxd +");
	LOG_HINT("  available_ports * base.max_pkt_burst * rxtxq_per_port +");
	LOG_HINT("  base.topo.n_enabled_core * base.mempool_cache_size * 2 +");
	LOG_HINT("  redundancy");
	LOG_HINT("%u = ", nb_mbufs);
	LOG_HINT("  %u * %u * %u +", available_ports, rxtxq_per_port, base.nb_rxd);
	LOG_HINT("  %u * %u * %u +", available_ports, base.max_pkt_burst, rxtxq_per_port);
	LOG_HINT("  %u * %u * 2 +", base.topo.n_enabled_core, base.mempool_cache_size);
	LOG_HINT("  8192");

	/* Create the mbuf pool. 8< */
	rx_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
			base.mempool_cache_size, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
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

		base.dev_mode_mask |= device_type_to_mask(etype);

		if (ETHDEV_NOT_SUPPORTED == etype) {
			rte_exit(EXIT_FAILURE,
					"Not supported ether device (port %u) info: %s\n",
					portid, strerror(-ret));
		}
		/* skip physical ports that are not enabled */
		if (device_is_full_featured(etype)) {
			if ((base.bconf->enabled_port_mask & (1u << portid)) == 0) {
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
		/* Configure the number of queues for a port. */
		if (device_is_full_featured(etype)) {
			ret = rte_eth_dev_configure(portid, rxtxq_per_port,
			                            rxtxq_per_port, &local_port_conf);
		} else if (device_is_simple_vdev(etype)) {
			ret = rte_eth_dev_configure(portid, 0, 1, &port_conf_default);
		} else {
			ret = -1;
		}
		if (ret < 0) {
			rte_exit(EXIT_FAILURE, "Cannot configure device: type %d err=%d, port=%u\n",
					etype, ret, portid);
		}
		/* >8 End of configuration of the number of queues for a port. */

		if (device_is_full_featured(etype)) {
			ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &base.nb_rxd, &base.nb_txd);
			if (ret < 0) {
				rte_exit(EXIT_FAILURE,
						"Cannot adjust number of descriptors: err=%d port=%u"
						"nb_rxd %u nb_txd %u", ret, portid, base.nb_rxd, base.nb_txd);
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
			if (device_is_full_featured(etype)) {
				ret = rte_eth_rx_queue_setup(portid, i, base.nb_rxd,
						rte_eth_dev_socket_id(portid),
						&rxq_conf, rx_pktmbuf_pool);
			} else {
				/* Simple virtual devices skip RX queue setup. */
				ret = 0;
			}
			if (ret < 0) {
				rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
						ret, portid);
			}
			/* >8 End of RX queue setup. */
			/* init one TX queue */
			if (device_is_full_featured(etype)) {
				ret = rte_eth_tx_queue_setup(portid, i, base.nb_txd,
						rte_eth_dev_socket_id(portid), &txq_conf);
			} else if (device_is_simple_vdev(etype)) {
				LOG_INFO("setup one txq for %s device",
				         etype == ETHDEV_PCAP ? "pcap" : "ring/null");
				ret = rte_eth_tx_queue_setup(portid, 0, base.nb_txd,
						rte_eth_dev_socket_id(portid), &dev_info.default_txconf);
				/* only 1 tx queue */
				break;
			} else {
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

	}

	if (nb_ports_available != nb_physical_ports_available &&
			nb_physical_ports_available) {
		LOG_WARN("Mix physical and other ports are detected.");
	}

	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE,
				"All available ports are disabled. Please set portmask.\n");
	}

	check_all_ports_link_status(base.bconf->enabled_port_mask);

}

/* Phase 7: Log runtime info (PID, PPID, CPU affinity). */
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

/* Phase 8: Launch all worker threads and wait.
 *
 * Initialises the pthread barrier (main core + n_enabled_core workers),
 * then calls rte_eal_mp_remote_launch() to dispatch launch_one_core()
 * on every lcore (including the main core). */
void run()
{
	LOG_WARN("Use only on systems and networks you own or are authorized to test.");
	LOG_INFO("main core %u init %d barriers",
			base.topo.main_core, base.topo.n_enabled_core + 1);
	/* including main lcore */
	pthread_barrier_init(&base.barrier, NULL, base.topo.n_enabled_core + 1);

	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(launch_one_core, (void*)base.bconf, CALL_MAIN);
}

/* Phase 9: Wait for workers to finish, then stop all ports.
 *
 * Iterates worker lcores with rte_eal_wait_lcore(),
 * stops and closes each DPDK port, destroys the barrier. */
void stop()
{
	int ret = 0;
	uint32_t lcore_id = 0;
	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		int n = rte_eal_wait_lcore(lcore_id);
		if (n < 0) {
			LOG_WARN("lcore %u returnd %d", lcore_id, n);
			ret = -1;
			break;
		}
	}

	/* Drain TX completions before touching port state.
	 * With mlx5 PMD in legacy mode and NO-CARRIER, the NIC may
	 * still be processing descriptor rings internally when
	 * workers exit.  rte_eth_dev_stop races with in-flight DMA
	 * completions if called immediately.  A short delay lets the
	 * PMD finish its cleanup internally.
	 * Empire: observed segfault at 0x17ff78330 (rte_table_action.c)
	 * during rte_eth_dev_stop on ConnectX-6 PF with ≥4 cores. */
	rte_delay_ms(200);

	uint16_t portid;
	RTE_ETH_FOREACH_DEV(portid) {
		if ((base.bconf->enabled_port_mask & (1u << portid)) == 0) {
			continue;
		}
		LOG_INFO("Closing port %d ...", portid);
		ret = rte_eth_dev_stop(portid);
		if (ret != 0) {
			LOG_INFO("rte_eth_dev_stop: err=%d, port=%d", ret, portid);
		}
		rte_eth_dev_close(portid);
		LOG_INFO("Done");
	}

	pthread_barrier_destroy(&base.barrier);

	base.return_value = ret;
}

/* Phase 10: Clean up EAL, WebSocket server, and config tree.
 *
 * rte_eal_cleanup(), stop WebSocket server, free config. */
void quit()
{
	if (base.system && base.system->cfg.server.ctx) {
		ws_server_stop(base.system->cfg.server.ctx);
	}

	bless_free(base.bconf);

	/* clean up the EAL */
	rte_eal_cleanup();

	config_exit(base.config->root);
	config_file_unmap_close(&base.config->cfm);

	LOG_WARN("Bye ...");
}

int main(int argc, char **argv)
{
	init_base(argc, argv);
	base.g_state = &g_state;
	args_save_cli_overrides(argc, argv);

	init_config();

	/* Pre-allocate bless_conf early so WS can safely handle cmd=get.
	 * Full runtime init (rte_malloc etc) runs after init_eal(). */
	{
		struct bless_conf *b = bless_init();
		if (!b) {
			rte_exit(EXIT_FAILURE, "bless_init failed\n");
		}
		base.bconf = b;
	}

	init_system();

	init_eal();
	args_inject_cli_overrides(&base.argc, &base.argv);

	init_mbuf_dynfield();

	init_metrics();

	parse_and_merge_config();

	init_runtime_state();

	init_signal();

	base_init_topo();

	init_port();

	init_rumtime();
	if (preflight_run(&base, (enum preflight_mode)base.bconf->preflight_mode,
			  base.bconf->environment_dump_path) < 0) {
		rte_exit(EXIT_FAILURE, "Pre-flight strict policy rejected the environment\n");
	}

	run();

	stop();

	quit();

	return 0;
}
