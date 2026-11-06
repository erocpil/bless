#include "system.h"
#include "log.h"
#include "server.h"
#include <string.h>

/**
 * @file system.c
 * @brief Daemonisation, terminal themes, and misc system helpers.
 *
 * Supports --daemon (fork + setsid), ANSI colour theme loading
 * from YAML config, and PID file management.
 */

/* Set system config to defaults (zero-initialised). */
void system_set_defaults(struct system_cfg *cfg)
{
	if (!cfg) {
		printf("Null system config.\n");
		return;
	}
	memset(cfg, 0, sizeof(struct system_cfg));
}

/* Pretty-print a CPU set as comma-dash ranges (e.g. 0-3,7,10-12). */
void system_show_cpuset(const cpu_set_t *set, int max_cpus)
{
	int first = 1;
	int start = -1;

	LOG_META_NNL("  cpuset  ");
	for (int cpu = 0; cpu <= max_cpus; cpu++) {
		int is_set = (cpu < max_cpus) && CPU_ISSET(cpu, set);

		if (is_set) {
			if (start < 0) {
				start = cpu;
			}
		} else {
			if (start >= 0) {
				if (!first) {
					printf(",");
				}
				if (start == cpu - 1) {
					printf("%d", start);
				} else {
					printf("%d-%d", start, cpu - 1);
				}

				first = 0;
				start = -1;
			}
		}
	}

	if (first) {
		printf("(empty)");
	}
	printf("\n");
}

/* Log the system status block (PID, PPID, CPU affinity). */
void system_show_status(struct system_status *sysstat)
{
	LOG_INFO("\nSystem status %p   ", sysstat);
	if (!sysstat) {
		return;
	}
	LOG_SHOW("  ppid    %u", sysstat->ppid);
	LOG_SHOW("  pid     %u", sysstat->pid);
	system_show_cpuset(&sysstat->cpuset, CPU_SETSIZE);
}

/* Log the full system configuration (daemonise, theme, server). */
void system_show(struct system *sys)
{
	if (!sys) {
		return;
	}
	LOG_HINT("system %p", sys);
	LOG_SHOW("  daemonize %d", sys->cfg.daemonize);
	LOG_SHOW("  theme     %s", sys->cfg.theme);
	server_show_format(&sys->cfg.server, "   ");
}
