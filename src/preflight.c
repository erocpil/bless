#include "preflight.h"
#include "base.h"
#include "device.h"
#include "log.h"
#include "pacing.h"

#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>

static int read_line(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	if (!fgets(buf, (int)len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[strcspn(buf, "\r\n")] = '\0';
	return 0;
}

static int cpu_list_contains(const char *list, unsigned wanted)
{
	const char *p = list;
	while (*p) {
		char *end = NULL;
		unsigned long lo = strtoul(p, &end, 10);
		if (end == p) {
			break;
		}
		unsigned long hi = lo;
		p = end;
		if (*p == '-') {
			hi = strtoul(p + 1, &end, 10);
			p = end;
		}
		if (wanted >= lo && wanted <= hi) {
			return 1;
		}
		if (*p == ',') {
			p++;
		} else if (*p) {
			break;
		}
	}
	return 0;
}

static int file_contains(const char *path, const char *needle)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		return 0;
	}
	char line[1024];
	int found = 0;
	while (fgets(line, sizeof(line), f))
		if (strstr(line, needle)) { found = 1; break; }
	fclose(f);
	return found;
}

static unsigned irq_worker_conflicts(const struct base *b)
{
	DIR *dir = opendir("/proc/irq");
	if (!dir) {
		return 0;
	}
	unsigned conflicts = 0;
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
			continue;
		}
		char path[256], affinity[256];
		snprintf(path, sizeof(path), "/proc/irq/%s/smp_affinity_list",
			 entry->d_name);
		if (read_line(path, affinity, sizeof(affinity)) != 0) {
			continue;
		}
		for (unsigned lc = 0; lc < RTE_MAX_LCORE; lc++) {
			unsigned cpu = rte_lcore_to_cpu_id(lc);
			if (b->topo.cv && b->topo.cv[lc].enabled &&
			    cpu_list_contains(affinity, cpu)) {
				conflicts++;
				break;
			}
		}
	}
	closedir(dir);
	return conflicts;
}

int preflight_run(const struct base *b, enum preflight_mode mode,
		  const char *snapshot_path)
{
	if (!b || mode == PREFLIGHT_OFF) {
		return 0;
	}
	unsigned warnings = 0, severe = 0;
	unsigned hugepages_total = 0, hugepages_free = 0;
	unsigned numa_hugepages_free = 0;
	char governor[64] = "unavailable";
	char path[192];
	unsigned main_lcore = rte_get_main_lcore();
	int constant_tsc = file_contains("/proc/cpuinfo", "constant_tsc");
	int nonstop_tsc = file_contains("/proc/cpuinfo", "nonstop_tsc");
	int isolcpus = file_contains("/proc/cmdline", "isolcpus=");
	int nohz_full = file_contains("/proc/cmdline", "nohz_full=");
	int rcu_nocbs = file_contains("/proc/cmdline", "rcu_nocbs=");
	FILE *meminfo = fopen("/proc/meminfo", "r");
	if (meminfo) {
		char line[160];
		while (fgets(line, sizeof(line), meminfo)) {
			(void)sscanf(line, "HugePages_Total: %u", &hugepages_total);
			(void)sscanf(line, "HugePages_Free: %u", &hugepages_free);
		}
		fclose(meminfo);
		if (!hugepages_total) {
			warnings++;
		}
	}
	for (unsigned node = 0; node < b->topo.n_numa; node++) {
		snprintf(path, sizeof(path), "/sys/devices/system/node/node%u/hugepages/"
			 "hugepages-2048kB/free_hugepages", node);
		char value[64];
		if (read_line(path, value, sizeof(value)) == 0) {
			numa_hugepages_free += (unsigned)strtoul(value, NULL, 10);
		}
		snprintf(path, sizeof(path), "/sys/devices/system/node/node%u/hugepages/"
			 "hugepages-1048576kB/free_hugepages", node);
		if (read_line(path, value, sizeof(value)) == 0) {
			numa_hugepages_free += (unsigned)strtoul(value, NULL, 10);
		}
	}
	unsigned irq_conflicts = irq_worker_conflicts(b);
	if (irq_conflicts) {
		warnings++;
	}
	if (!constant_tsc || !nonstop_tsc) {
		warnings++;
	}
	if (!isolcpus && !nohz_full) {
		warnings++;
	}

	for (unsigned lc = 0; lc < RTE_MAX_LCORE; lc++) {
		if (!rte_lcore_is_enabled(lc)) {
			continue;
		}
		unsigned cpu = rte_lcore_to_cpu_id(lc);
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_governor", cpu);
		char value[64];
		if (read_line(path, value, sizeof(value)) == 0) {
			if (!strcmp(governor, "unavailable")) {
				snprintf(governor, sizeof(governor), "%s", value);
			}
			if (strcmp(value, "performance")) {
				warnings++;
			}
		}
		if (lc == main_lcore || !b->topo.cv || !b->topo.cv[lc].enabled) {
			continue;
		}
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list", cpu);
		char siblings[128];
		if (read_line(path, siblings, sizeof(siblings)) == 0) {
			for (unsigned other = lc + 1; other < RTE_MAX_LCORE; other++) {
				if (!b->topo.cv[other].enabled) {
					continue;
				}
				if (cpu_list_contains(siblings,
					    rte_lcore_to_cpu_id(other))) { severe++; break; }
			}
		}
	}
	uint16_t checked_port;
	RTE_ETH_FOREACH_DEV(checked_port) {
		if (!(b->bconf->enabled_port_mask & (1u << checked_port))) {
			continue;
		}
		struct rte_eth_link link;
		memset(&link, 0, sizeof(link));
		if (rte_eth_link_get_nowait(checked_port, &link) == 0 &&
		    !link.link_status) {
			warnings++;
		}
	}

	for (unsigned lc = 0; lc < RTE_MAX_LCORE; lc++) {
		if (!b->topo.cv || !b->topo.cv[lc].enabled) {
			continue;
		}
		int socket = rte_eth_dev_socket_id(b->topo.cv[lc].port);
		if (socket >= 0 && socket != (int)b->topo.cv[lc].numa) {
			severe++;
		}
	}

	FILE *out = snapshot_path ? fopen(snapshot_path, "w") : NULL;
	if (out) {
		fprintf(out,
			"{\n  \"schema\": 1,\n  \"policy\": \"%s\",\n"
			"  \"cpu_governor\": \"%s\",\n"
			"  \"enabled_lcores\": %u,\n  \"affinity_cpus\": %d,\n"
			"  \"numa_nodes\": %u,\n  \"hugepages_total\": %u,\n"
			"  \"hugepages_free\": %u,\n  \"numa_hugepages_free\": %u,\n"
			"  \"checks\": {\"constant_tsc\":%s,\"nonstop_tsc\":%s,"
			"\"isolcpus\":%s,\"nohz_full\":%s,\"rcu_nocbs\":%s,"
			"\"worker_irq_conflicts\":%u},\n"
			"  \"warnings\": %u,\n  \"severe\": %u,\n"
			"  \"result\": \"%s\",\n  \"ports\": [",
			mode == PREFLIGHT_STRICT ? "strict" : "warn", governor,
			rte_lcore_count(), CPU_COUNT(&b->system->status.cpuset),
			b->topo.n_numa, hugepages_total, hugepages_free,
			numa_hugepages_free,
			constant_tsc ? "true" : "false", nonstop_tsc ? "true" : "false",
			isolcpus ? "true" : "false", nohz_full ? "true" : "false",
			rcu_nocbs ? "true" : "false", irq_conflicts, warnings, severe,
			severe ? "unreliable" : warnings ? "warning" : "ok");
		int first = 1;
		uint16_t port;
		RTE_ETH_FOREACH_DEV(port) {
			if (!(b->bconf->enabled_port_mask & (1u << port))) {
				continue;
			}
			struct rte_eth_dev_info info;
			struct rte_eth_link link;
			struct pacing_hw_caps pacing_caps;
			memset(&info, 0, sizeof(info));
			memset(&link, 0, sizeof(link));
			rte_eth_dev_info_get(port, &info);
			rte_eth_link_get_nowait(port, &link);
			pacing_probe_mlx5(port, &pacing_caps);
			fprintf(out, "%s{\"id\":%u,\"driver\":\"%s\","
				"\"numa\":%d,\"link_up\":%s,\"speed_mbps\":%u,"
				"\"mlx5_scheduled_metadata\":%s,"
				"\"scheduled_clock_calibrated\":%s}",
				first ? "" : ",", port,
				info.driver_name ? info.driver_name : "unknown",
				rte_eth_dev_socket_id(port),
				link.link_status ? "true" : "false", link.link_speed,
				pacing_caps.tx_timestamp_field && pacing_caps.tx_timestamp_flag
					? "true" : "false",
				pacing_caps.clock_calibrated ? "true" : "false");
			first = 0;
		}
		fprintf(out, "]\n}\n");
		fclose(out);
	} else if (snapshot_path) {
		LOG_WARN("pre-flight: cannot write %s: %s", snapshot_path,
			 strerror(errno));
		warnings++;
	}
	if (warnings) {
		LOG_WARN("pre-flight: %u environment warning(s); see %s", warnings,
			 snapshot_path ? snapshot_path : "log");
	}
	if (severe) {
		LOG_WARN("pre-flight: %u severe placement issue(s)", severe);
	}
	return mode == PREFLIGHT_STRICT && severe ? -1 : 0;
}
