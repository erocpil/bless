#include "base.h"
#include "color.h"
#include "worker.h"

/**
 * @file main.c
 * @brief Application entry point -- DPDK EAL init, device setup,
 *        worker launch, and signal-driven shutdown.
 *
 * Startup flow:
 *   1. Parse config file (YAML) + CLI overrides
 *   2. rte_eal_init()
 *   3. Detect / configure NIC ports (TX queues, offloads)
 *   4. Launch per-lcore workers
 *   5. Wait for signal -> graceful shutdown
 */
#include "device.h"
#include "log.h"
#include "args.h"
#include "injector_plugin.h"
#include "define.h"
#include "bless_dpdk_runtime.h"

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
	if (!s) return def;
	char *endptr = NULL;
	errno = 0;
	long v = strtol(s, &endptr, 10);
	if (errno || endptr == s || *endptr != '\0')
		rte_exit(EXIT_FAILURE, "Invalid --%s value: %s\n", name, s);
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

		// Walk ELF notes, looking for the INJECTOR descriptor
		size_t pos = 0;
		while (pos + sizeof(Elf64_Nhdr) <= sh.sh_size) {
			Elf64_Nhdr *nh = (Elf64_Nhdr*)(data + pos);
			uint32_t namesz = (nh->n_namesz + 3) & ~3u;
			uint32_t descsz = (nh->n_descsz + 3) & ~3u;

			char *name	= (char*)(data + pos + sizeof(*nh));
			char *desc	= (char*)(name + namesz);

			if (nh->n_type != 1 ||
					strcmp(name, "INJECTOR") != 0) {
				pos += sizeof(*nh) + namesz + descsz;
				continue;
			}

			// Found the INJECTOR note descriptor
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
			COLOR(ANSI_BOLD FG_BRIGHT_WHITE), "INJECTOR",
			COLOR(C_RATE));
	fprintf(stdout, "|%s  %-42s%s|\n",
			COLOR(C_TRACE), "Packet construction and sending",
			COLOR(C_RATE));
	fprintf(stdout, "+--------------------------------------------+\n");
	fprintf(stdout, "|  %s%-10s%s  %s%-30s%s|\n",
			COLOR(C_ERR),    "Version", COLOR(C_RATE),
			COLOR(FG_RED),   INJECTOR_VERSION, COLOR(C_RATE));
	fprintf(stdout, "|  %s%-10s%s  %s%-30s%s|\n",
			COLOR(C_INFO),   "Branch",  COLOR(C_RATE),
			COLOR(FG_GREEN), GIT_BRANCH, COLOR(C_RATE));
	fprintf(stdout, "|  %s%-10s%s  %s%-30s%s|\n",
			COLOR(C_WARN),   "Commit",  COLOR(C_RATE),
			COLOR(FG_YELLOW), GIT_COMMIT, COLOR(C_RATE));
	if (GIT_STATE[0])
		fprintf(stdout, "|  %s%-10s%s  %s%-30s%s|\n",
				COLOR(C_TRACE), "State", COLOR(C_RATE),
				COLOR(FG_YELLOW), GIT_STATE, COLOR(C_RATE));
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

atomic_int g_state = STATE_RUNNING;

static unsigned int txq_per_port = 1;

static struct dist_ratio ratio;
static struct injector_encap_params bep;

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
		LOG_TRACE("Running core %u port %u TX queue %u",
				lcore_id, view->port, view->txq);
		worker_loop(conf);
	}

	return 0;
}

/* Display transmission options. */
static void usage(const char *prgname)
{
    printf("%s <config.yaml> [--] [options]\n"
           "  -p PORTMASK: hexadecimal bitmask of ports to transmit on\n"
           "  -q NQ: TX queues per NIC (default 1; PCAP/ring/null use 1)\n"
           "  --num N: packets per worker (-1 continuous, 0 no packets)\n"
           "  --batch N: packets per burst\n"
           "  --weight PROTO=N: protocol weight (e.g. udp=1)\n"
           "  --pps-rate N: packets/second per worker (0 unlimited)\n"
           "  --bps-rate N: bytes/second per worker (0 unlimited)\n"
           "  --seed N: deterministic random seed\n", prgname);
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
	if (n >= MAX_TX_QUEUE_PER_PORT) {
		return 0;
	}

	return n;
}

static const char short_options[] =
"p:"  /* portmask */
"q:"  /* number of queues */
;

#define CMD_LINE_OPT_NUM "num"
#define CMD_LINE_OPT_BATCH "batch"
#define CMD_LINE_OPT_BATCH_DELAY_US "batch-delay-us"
#define CMD_LINE_OPT_BATCH_JITTER_US "batch-jitter-us"
#define CMD_LINE_OPT_TRAFFIC_MODEL "traffic-model"
#define CMD_LINE_OPT_BENCH_MODE    "bench-mode"
#define CMD_LINE_OPT_WEIGHT "weight"
/* backward compat aliases -- just set weight via name lookup */
#define CMD_LINE_OPT_ARP   "arp"
#define CMD_LINE_OPT_ICMP  "icmp"
#define CMD_LINE_OPT_TCP   "tcp"
#define CMD_LINE_OPT_UDP   "udp"
#define CMD_LINE_OPT_PPS_RATE "pps-rate"
#define CMD_LINE_OPT_PPS_BURST "pps-burst"
#define CMD_LINE_OPT_BPS_RATE "bps-rate"
#define CMD_LINE_OPT_BPS_BURST "bps-burst"
#define CMD_LINE_OPT_INTERLEAVE "interleave"
#define CMD_LINE_OPT_INTERLEAVE_DEPTH "interleave-depth"
#define CMD_LINE_OPT_SEED "seed"

#define CMD_LINE_OPT_LOG_FORMAT "log-format"
enum {
	/* long options mapped to a short option */
	/* first long only option value must be >= 256, so that we won't
	 * conflict with short options */
	CMD_LINE_OPT_NUM_NUM = 256,
	CMD_LINE_OPT_BATCH_NUM,
	CMD_LINE_OPT_BATCH_DELAY_US_NUM,
	CMD_LINE_OPT_BATCH_JITTER_US_NUM,
	CMD_LINE_OPT_TRAFFIC_MODEL_NUM,
	CMD_LINE_OPT_BENCH_MODE_NUM,
	CMD_LINE_OPT_WEIGHT_NUM,
	CMD_LINE_OPT_ARP_NUM,
	CMD_LINE_OPT_ICMP_NUM,
	CMD_LINE_OPT_TCP_NUM,
	CMD_LINE_OPT_UDP_NUM,
	CMD_LINE_OPT_PPS_RATE_NUM,
	CMD_LINE_OPT_PPS_BURST_NUM,
	CMD_LINE_OPT_BPS_RATE_NUM,
	CMD_LINE_OPT_BPS_BURST_NUM,
	CMD_LINE_OPT_INTERLEAVE_NUM,
	CMD_LINE_OPT_INTERLEAVE_DEPTH_NUM,
	CMD_LINE_OPT_SEED_NUM,
	CMD_LINE_OPT_LOG_FORMAT_NUM,
};

static const struct option lgopts[] = {
	{ CMD_LINE_OPT_NUM, 1, 0, CMD_LINE_OPT_NUM_NUM },
	{ CMD_LINE_OPT_BATCH, 1, 0, CMD_LINE_OPT_BATCH_NUM },
	{ CMD_LINE_OPT_BATCH_DELAY_US, 1, 0, CMD_LINE_OPT_BATCH_DELAY_US_NUM },
	{ CMD_LINE_OPT_BATCH_JITTER_US, 1, 0, CMD_LINE_OPT_BATCH_JITTER_US_NUM },
	{ CMD_LINE_OPT_TRAFFIC_MODEL, 1, 0, CMD_LINE_OPT_TRAFFIC_MODEL_NUM },
	{ CMD_LINE_OPT_BENCH_MODE,    1, 0, CMD_LINE_OPT_BENCH_MODE_NUM },
	{ CMD_LINE_OPT_WEIGHT, 1, 0, CMD_LINE_OPT_WEIGHT_NUM },
	{ CMD_LINE_OPT_ARP, 1, 0, CMD_LINE_OPT_ARP_NUM },
	{ CMD_LINE_OPT_ICMP, 1, 0, CMD_LINE_OPT_ICMP_NUM },
	{ CMD_LINE_OPT_TCP, 1, 0, CMD_LINE_OPT_TCP_NUM },
	{ CMD_LINE_OPT_UDP, 1, 0, CMD_LINE_OPT_UDP_NUM },
	{ CMD_LINE_OPT_PPS_RATE, 1, 0, CMD_LINE_OPT_PPS_RATE_NUM },
	{ CMD_LINE_OPT_PPS_BURST, 1, 0, CMD_LINE_OPT_PPS_BURST_NUM },
	{ CMD_LINE_OPT_BPS_RATE, 1, 0, CMD_LINE_OPT_BPS_RATE_NUM },
	{ CMD_LINE_OPT_BPS_BURST, 1, 0, CMD_LINE_OPT_BPS_BURST_NUM },
	{ CMD_LINE_OPT_INTERLEAVE, 1, 0, CMD_LINE_OPT_INTERLEAVE_NUM },
	{ CMD_LINE_OPT_INTERLEAVE_DEPTH, 1, 0, CMD_LINE_OPT_INTERLEAVE_DEPTH_NUM },
	{ CMD_LINE_OPT_LOG_FORMAT, 1, 0, CMD_LINE_OPT_LOG_FORMAT_NUM },
	{ CMD_LINE_OPT_SEED, 1, 0, CMD_LINE_OPT_SEED_NUM },
	{NULL, 0, 0, 0}
};

/* Parse the argument given in the command line of the application */
static int parse_args(int argc, char **argv)
{
	struct injector_conf *bconf = base.bconf;
	int opt, ret;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];

	bep.inner = (struct mbuf_conf*)malloc(sizeof(struct mbuf_conf));
	if (!bep.inner) {
		LOG_ERR("malloc(mbuf_conf) failed");
		return -1;
	}

	argvopt = argv;

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
			case 'q':
				txq_per_port = parse_nqueue(optarg);
				if (txq_per_port == 0) {
					LOG_ERR("invalid queue number");
					usage(prgname);
					return -1;
				}
				break;

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
			case CMD_LINE_OPT_BENCH_MODE_NUM:
				bconf->bench_mode = (optarg && !strcmp(optarg, "template")) ? 1 : 0;
				break;
			case CMD_LINE_OPT_WEIGHT_NUM:
				{
					/* Parse --weight name=value */
					if (!optarg) break;
					char *eq = strchr(optarg, '=');
					if (!eq || eq == optarg) {
						rte_exit(EXIT_FAILURE,
								"--weight: expected 'name=value', got '%s'\n", optarg);
					}
					size_t nlen = (size_t)(eq - optarg);
					char name[64];
					if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
					memcpy(name, optarg, nlen);
					name[nlen] = '\0';
					char *end = NULL;
					long v = strtol(eq + 1, &end, 10);
					if (*end || v < 0 || v > 100000) {
						rte_exit(EXIT_FAILURE,
								"--weight: invalid value for '%s'\n", name);
					}
					if (!injector_find_pkt_type(name))
						rte_exit(EXIT_FAILURE, "--weight: unsupported protocol '%s'\n", name);
					injector_set_type_weight(name, (int32_t)v);
					/* For backward compat with legacy --tcp=N usage pattern,
					 * also set ratio.weight for built-in types. */
					for (int i = 0; i < TYPE_MAX; i++) {
						if (strcmp(name, injector_get_type_name(i)) == 0) {
							ratio.weight[i] = (int32_t)v;
							break;
						}
					}
					break;
				}
				/* backward compat CLI aliases -- all set weight by name */
			case CMD_LINE_OPT_ARP_NUM:
				injector_set_type_weight("arp", (int32_t)parse_cli_long(optarg, "num", 0));
				ratio.weight[TYPE_ARP] = (int32_t)parse_cli_long(optarg, "num", 0);
				break;
			case CMD_LINE_OPT_ICMP_NUM:
				injector_set_type_weight("icmp", (int32_t)parse_cli_long(optarg, "num", 0));
				ratio.weight[TYPE_ICMP] = (int32_t)parse_cli_long(optarg, "num", 0);
				break;
			case CMD_LINE_OPT_TCP_NUM:
				injector_set_type_weight("tcp", (int32_t)parse_cli_long(optarg, "num", 0));
				ratio.weight[TYPE_TCP] = (int32_t)parse_cli_long(optarg, "num", 0);
				break;
			case CMD_LINE_OPT_UDP_NUM:
				injector_set_type_weight("udp", (int32_t)parse_cli_long(optarg, "num", 0));
				ratio.weight[TYPE_UDP] = (int32_t)parse_cli_long(optarg, "num", 0);
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
				if (optarg)
					fast_rand_set_seed((uint64_t)parse_cli_long(optarg, "seed", 0));
				break;
			case CMD_LINE_OPT_LOG_FORMAT_NUM:
				if (optarg && !strcmp(optarg, "json"))
					log_set_format_json(1);
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
			if (!device_port_enabled(port_mask, portid))
				continue;
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

/* Register mbuf dynamic fields used by the injector pipeline.
 *
 * Dynamic fields let per-packet metadata (e.g. type tag, TX timestamp)
 * ride along without permanent mbuf struct modifications.
 * Returns 0 on success, negative on registration failure. */
static int init_mbuf_dynfield()
{
	const struct rte_mbuf_dynfield mbuf_injector_fields[] = {
		[ MBUF_DYN_TYPE ] = {
			.name = "field_name",
			.size = sizeof(char),
			.align = __alignof__(char),
		},
	};

	LOG_INFO("mbuf dynfield:");
	for (int i = 0; i < (int)NELEMS(mbuf_injector_fields); i++) {
		if (mbuf_injector_fields[i].size == 0) {
			continue;
		}
		const struct rte_mbuf_dynfield *md = &mbuf_injector_fields[i];
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
	LOG_INFO("injector exit");
}

/* Phase 0: Allocate the config struct and stash CLI argc/argv. */
void init_base(int argc, char **argv)
{
	base.argc = argc;
	base.argv = argv;
	base.nb_txd = TX_DESC_DEFAULT;
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
    uint32_t available_mask = 0;
    RTE_ETH_FOREACH_DEV(port_id) {
        if (port_id < 32) available_mask |= 1u << port_id;
    }
    if (!topo->port_mask || (topo->port_mask & ~available_mask))
        rte_exit(EXIT_FAILURE, "Invalid portmask; available 0x%x\n", available_mask);

    RTE_ETH_FOREACH_DEV(port_id) {
        if (!device_port_enabled(topo->port_mask, port_id))
            continue;
        enum ethdev_type etype = device_get_ethdev_type(port_id);
        if (!device_is_full_featured(etype) && !device_is_simple_vdev(etype))
            rte_exit(EXIT_FAILURE, "Unsupported TX device on port %u\n", port_id);

		/* Physical NICs report a real socket; virtual devices return -1.
		 * Prefer cores on the same NUMA node to avoid cross-socket
		 * memory latency (~2x penalty on typical dual-socket systems). */
		int port_socket = rte_eth_dev_socket_id(port_id);

		for (uint32_t qid = 0; qid < txq_per_port; qid++) {
			uint16_t best = RTE_MAX_LCORE;

			if (port_socket >= 0) {
				/* First pass: same-socket core. */
				for (uint16_t c = core_id; c < RTE_MAX_LCORE; c++) {
					if (!rte_lcore_is_enabled(c) ||
					    c == rte_get_main_lcore())
						continue;
					if (cv[c].enabled)
						continue;
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
					    c == rte_get_main_lcore())
						continue;
					if (cv[c].enabled)
						continue;
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

	base_show_topo(topo);

	return 0;
}

/* Parse system configuration and optionally run in the background. */
void init_system()
{
	struct system *system = (struct system*)malloc(sizeof(struct system));
	if (!system) {
		rte_exit(EXIT_FAILURE, "malloc(system)\n");
	}
	struct system_cfg *cfg = &system->cfg;
	if (config_parse_system(base.config->root, cfg) < 0) {
		rte_exit(EXIT_FAILURE, "Invalid system arguments\n");
	}

	if (cfg->daemonize) {
		daemon(1, 1);
		LOG_INFO("Daemonized");
	}

	base.system = system;
	system_show(system);
}

/* Phase 1a: Load and parse the YAML config file.
 *
 * Uses the first CLI argument as the config path (default: ``conf/config.yaml``).
 * Special subcommands: ``version`` prints build info, ``help`` prints usage. */
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
			if (0 == strcmp(base.argv[2], "-v"))
				v = 1;
			else if (0 == strcmp(base.argv[2], "-vv"))
				v = 2;
			else if (0 == strcmp(base.argv[2], "-vvv"))
				v = 3;
		}
		printf("INJECTOR %s", INJECTOR_VERSION);
		if (v > 0) {
			printf(" (%s @ %s", GIT_BRANCH, GIT_COMMIT);
			if (GIT_STATE[0])
				printf("-%s", GIT_STATE);
			printf(", %s %s)", BUILD_TYPE,
			       STATIC ? "static" : "shared");
		}
		printf("\n");
		if (v > 0)
			show_buildinfo(v);
		printf("\n");
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
		printf("  help              Show this help message\n");
		printf("\n");
		printf("Config file (first non-flag argument):\n");
		printf("  injector <path>      Path to YAML config (default: conf/config.yaml)\n");
		printf("\n");
		printf("For full CLI flag reference run without arguments and see the\n");
		printf("EAL + injector option output (--help within EAL also works).\n");
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

	int ret = br_runtime_eal_init(base.argc, base.argv);
	if (ret < 0) {
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	}

	base.args_jump = ret;
}

/* Phase 4a: Parse injector-specific args and merge CLI + YAML configs.
 *
 * - Allocates injector_conf if needed
 * - Parses CLI flags via parse_args()
 * - Parses YAML bless section via config_parse_injector()
 *
 * Does NOT touch EAL runtime state (timer, rte_malloc, atomics).
 * Does NOT build the distribution table (deferred to init_runtime_state). */
static void parse_and_merge_config(void)
{
	struct injector_conf *bconf = base.bconf;
	if (!bconf) {
		bconf = injector_init();
		if (!bconf) {
			rte_exit(EXIT_FAILURE, "Cannot malloc(injector_conf)\n");
		}
		base.bconf = bconf;
	}

	/* parse application(injector) arguments from base.{argc, argv} */
	if (parse_args(base.argc - base.args_jump, base.argv + base.args_jump) < 0) {
		rte_exit(EXIT_FAILURE, "Invalid INJECTOR arguments\n");
	}
	if (fast_rand_get_seed())
		LOG_HINT("PRNG seed: %lu (deterministic replay)", (unsigned long)fast_rand_get_seed());
	else
		LOG_HINT("PRNG seed: auto (rdtsc-based)");

	if (base.config->root && !bconf->cnode) {
		bconf->cnode = config_parse_injector(base.config->root);
		if (!bconf->cnode) {
			rte_exit(EXIT_FAILURE, "Cannot parse injector\n");
		}
		base.config->cnode = bconf->cnode;

	/* Build distribution AFTER both CLI flags and YAML weights are parsed */
	if (injector_set_dist(bconf, &ratio, &bep) < 0) {
		rte_exit(EXIT_FAILURE, "Cannot injector_set_dist()\n");
	}
	} else {
		LOG_WARN("No config.ymal used");
		rte_exit(EXIT_FAILURE, "Cannot init injector\n");
	}
}

/* Phase 4b: Build the distribution table and initialise runtime state
 * (state machine and pointer wiring).
 *
 * Must be called AFTER parse_and_merge_config(). */
static void init_runtime_state(void)
{
	struct injector_conf *bconf = base.bconf;

	atomic_store(&g_state, STATE_RUNNING);

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

/* Configure enabled ports with TX queues only. Workers own TX packet pools. */
void init_port(void)
{
    uint16_t portid;
    RTE_ETH_FOREACH_DEV(portid) {
        if (!device_port_enabled(base.bconf->enabled_port_mask, portid))
            continue;

        enum ethdev_type type = device_get_ethdev_type(portid);
        struct rte_eth_dev_info info;
        int ret = rte_eth_dev_info_get(portid, &info);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "Cannot query port %u: %s\n", portid, rte_strerror(-ret));
        uint16_t queues = device_is_simple_vdev(type) ? 1 : txq_per_port;
        struct rte_eth_conf config = port_conf;
        config.txmode.offloads &= info.tx_offload_capa;
        struct rte_eth_txconf txconf = info.default_txconf;
        txconf.offloads = config.txmode.offloads;
        struct br_runtime_port_config port_config = {
            .rx_queues = 0,
            .tx_queues = queues,
            .tx_descriptors = base.nb_txd,
            .adjust_tx_descriptors = device_is_full_featured(type),
            .port_conf = &config,
            .tx_queue_conf = &txconf,
        };
        ret = br_runtime_port_configure(portid, &port_config, NULL, NULL);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "Cannot configure TX port %u: %s\n", portid, rte_strerror(-ret));
        ret = br_runtime_port_start(portid);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "Cannot start port %u: %s\n", portid, rte_strerror(-ret));
        LOG_INFO("Port %u started with %u TX queues", portid, queues);
        device_show_info(portid);
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

	ret = br_runtime_ports_stop(base.bconf->enabled_port_mask);
	if (ret)
		LOG_INFO("port shutdown returned %d", ret);
	br_runtime_mempools_release();

	pthread_barrier_destroy(&base.barrier);

	if (ret) base.return_value = ret;
}

/* Clean up EAL and configuration. */
void quit()
{

	injector_free(base.bconf);

	/* clean up the EAL */
	br_runtime_eal_cleanup();

	config_exit(base.config->root);

	LOG_WARN("Bye ...");
}

int main(int argc, char **argv)
{
	init_base(argc, argv);
	args_save_cli_overrides(argc, argv);

	init_config();

	init_system();

	init_eal();
	args_inject_cli_overrides(&base.argc, &base.argv);

	init_mbuf_dynfield();


	parse_and_merge_config();

	init_runtime_state();

	init_signal();

	base_init_topo();

	init_port();

	init_rumtime();

	run();

	stop();

	quit();

	return base.return_value ? EXIT_FAILURE : EXIT_SUCCESS;
}
