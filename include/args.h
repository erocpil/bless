#ifndef __ARGS_H__
#define __ARGS_H__

/**
 * args.h -- CLI override management for Bless.
 *
 * Design:
 *   Bless loads its primary configuration from YAML.  The YAML injector
 *   section is converted into CLI arguments by config_parse_dpdk(), then
 *   parsed by parse_args().  CLI overrides (extra arguments typed on the
 *   command line after the config file path) must be appended to that
 *   YAML-generated argv so they override YAML values.
 *
 *   These two functions separate that concern from both config.c and
 *   main.c.
 *
 * Flow:
 *   1. main() -> init_base() -> args_save_cli_overrides()
 *      Scans the original argv for config.yaml [--] [--key=val …] and
 *      stores the overrides internally.
 *
 *   2. main() -> init_eal()  -> config_parse_dpdk()
 *      Builds argv from YAML only -- no awareness of CLI overrides.
 *
 *   3. main() -> args_inject_cli_overrides()
 *      Appends the saved overrides to base.argc/base.argv so that
 *      parse_args() sees them after the YAML-generated injector args.
 *
 * Command-line forms supported:
 *   ./bless conf.yaml -- --traffic-model=1    (DPDK convention)
 *   ./bless conf.yaml --traffic-model=1       (compatibility shortcut)
 */

/**
 * Save CLI override arguments from the original command line.
 *
 * @param argc  Original argc (from main())
 * @param argv  Original argv (from main())
 *
 * Scans argv[2..] for a leading "--" separator (optional) then stores
 * the remaining tokens.  Special commands "version" and "config" are
 * recognised and skipped (they cause early exit in init_config()).
 *
 * Safe to call multiple times -- second call replaces previous save.
 */
void args_save_cli_overrides(int argc, char **argv);

/**
 * Append saved CLI overrides to an argv array.
 *
 * @param argcp  Pointer to current argc (updated in place)
 * @param argvp  Pointer to current argv (may be realloc'd)
 *
 * The original argv MUST have been allocated with malloc/realloc.
 * Each saved override token is strdup'd into the new buffer.
 * The new argv is NULL-terminated.
 *
 * If no overrides were saved, this is a no-op.
 */
void args_inject_cli_overrides(int *argcp, char ***argvp);

#endif /* __ARGS_H__ */
