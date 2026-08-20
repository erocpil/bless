#ifndef __LOG_H__
#define __LOG_H__

/**
 * @file log.h
 * @brief Logging macros -- levels, colours, syslog bridge.
 *
 * Provides LOG_ERR / LOG_WARN / LOG_INFO / LOG_HINT / LOG_DEBUG /
 * LOG_TRACE and helpers for structured output (LOG_META, LOG_SHOW).
 * All translate to rte_log() calls with a single BLESS logtype.
 */

#include "color.h"
#include <time.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <sched.h>
#endif

#define LOG_ENABLE_DEBUG     1
#define LOG_ENABLE_TRACE     1
#define LOG_ENABLE_TIMESTAMP 1
#define LOG_ENABLE_THREAD    1
#define LOG_ENABLE_CPU       1
#define LOG_ENABLE_LOCATION  1

/* Runtime JSON log format toggle.  Set via --log-format=json CLI.
 * When enabled, all LOG_* macros produce one JSON object per line:
 *   {"ts":"HH:MM:SS.mmm","lvl":"INFO","tid":123,"cpu":0,"msg":"..."}
 * When 0 (default), output is ANSI-coloured plain text. */
extern int g_log_format_json;
void log_set_format_json(int enabled);

/* =========================
 * Compile-time feature switches
 * ========================= */

#ifndef LOG_ENABLE_DEBUG
#define LOG_ENABLE_DEBUG 1
#endif

#ifndef LOG_ENABLE_TIMESTAMP
#define LOG_ENABLE_TIMESTAMP 1
#endif

#ifndef LOG_ENABLE_LOCATION
#define LOG_ENABLE_LOCATION 1
#endif

#ifndef LOG_ENABLE_THREAD
#define LOG_ENABLE_THREAD 1
#endif

#ifndef LOG_ENABLE_CPU
#define LOG_ENABLE_CPU 1
#endif

/* =========================
 * Timestamp
 * ========================= */

#if LOG_ENABLE_TIMESTAMP
static inline void log_print_timestamp(FILE *fp)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);

	struct tm tm;
	localtime_r(&ts.tv_sec, &tm);

	fprintf(fp, "%02d:%02d:%02d.%03ld",
			tm.tm_hour,
			tm.tm_min,
			tm.tm_sec,
			ts.tv_nsec / 1000000);
}
#endif

/* =========================
 * Thread / CPU
 * ========================= */

#if LOG_ENABLE_THREAD
static inline long log_thread_id(void)
{
#if defined(__linux__)
	return syscall(SYS_gettid);
#else
	return 0;
#endif
}
#endif

#if LOG_ENABLE_CPU
static inline int log_cpu_id(void)
{
#if defined(__linux__)
	return sched_getcpu();
#else
	return -1;
#endif
}
#endif

/* =========================
 * Core logging macros
 * ========================= */

#define LOG_BASE(fp, level, color, fmt, ...)                            \
	do {                                                                \
		if (g_log_format_json) {                                        \
			fprintf(fp, "{\"ts\":\"");                                  \
			log_print_timestamp(fp);                                    \
			fprintf(fp, "\",\"lvl\":\"%s\",\"tid\":%ld,\"cpu\":%d,"     \
				"\"msg\":\"", level,                                  \
				log_thread_id(), log_cpu_id());                       \
			fprintf(fp, fmt, ##__VA_ARGS__);                            \
			fprintf(fp, "\"}\\n");                                       \
		} else {                                                        \
			fprintf(fp, "%s", COLOR(C_META));                           \
			fprintf(fp, "[ ");                                          \
			if (LOG_ENABLE_TIMESTAMP) {                                 \
				log_print_timestamp(fp);                                \
				fprintf(fp, " ");                                       \
			}                                                           \
			if (LOG_ENABLE_THREAD) {                                    \
				fprintf(fp, "T%ld ", log_thread_id());                  \
			}                                                           \
			if (LOG_ENABLE_CPU) {                                       \
				fprintf(fp, "C%02d ", log_cpu_id());                      \
			}                                                           \
			fprintf(fp, "]");                                           \
			\
			fprintf(fp, "%s[ %s ]%s ",                                  \
					COLOR(ANSI_BOLD),                                   \
					level,                                              \
					COLOR(ANSI_RESET));                                 \
			\
			fprintf(fp, "%s", COLOR(color));                            \
			fprintf(fp, fmt, ##__VA_ARGS__);                            \
			fprintf(fp, "%s\n", COLOR(ANSI_RESET));                     \
		}                                                               \
	} while (0)

#define LOG_BASE_NNL(fp, level, color, fmt, ...)                            \
	do {                                                                \
		if (g_log_format_json) {                                        \
			fprintf(fp, "{\"ts\":\"");                                  \
			log_print_timestamp(fp);                                    \
			fprintf(fp, "\",\"lvl\":\"%s\",\"tid\":%ld,\"cpu\":%d,"     \
				"\"msg\":\"", level,                                  \
				log_thread_id(), log_cpu_id());                       \
			fprintf(fp, fmt, ##__VA_ARGS__);                            \
			fprintf(fp, "\"}\n");                                       \
		} else {                                                        \
			fprintf(fp, "%s", COLOR(C_META));                           \
			fprintf(fp, "[ ");                                          \
			if (LOG_ENABLE_TIMESTAMP) {                                 \
				log_print_timestamp(fp);                                \
				fprintf(fp, " ");                                       \
			}                                                           \
			if (LOG_ENABLE_THREAD) {                                    \
				fprintf(fp, "T%ld ", log_thread_id());                  \
			}                                                           \
			if (LOG_ENABLE_CPU) {                                       \
				fprintf(fp, "C%02d ", log_cpu_id());                      \
			}                                                           \
			fprintf(fp, "]");                                           \
			\
			fprintf(fp, "%s[ %s ]%s ",                                  \
					COLOR(ANSI_BOLD),                                   \
					level,                                              \
					COLOR(ANSI_RESET));                                 \
			\
			fprintf(fp, "%s", COLOR(color));                            \
			fprintf(fp, fmt, ##__VA_ARGS__);                            \
			fprintf(fp, "%s", COLOR(ANSI_RESET));                       \
		}                                                               \
	} while (0)

/* =========================
 * Public interface
 * ========================= */
#define _L(...)                                                 \
	do {                                                        \
		struct timespec ts;                                     \
		clock_gettime(CLOCK_REALTIME, &ts);                     \
		fprintf(stdout, "%s[ %ld.%06ld %s:%d ]%s ",             \
				COLOR(C_UI_VALUE    C_LATENCY),                    \
				ts.tv_sec, ts.tv_nsec / 1000,                   \
				__func__, __LINE__,                             \
				COLOR(ANSI_RESET));                             \
		fprintf(stdout, "" __VA_ARGS__);                        \
		fprintf(stdout, "\n");                                  \
	} while (0)

#define _E(...)                                                 \
	do {                                                        \
		fprintf(stdout, "%s", COLOR(FG_BRIGHT_MAGENTA));        \
		fprintf(stdout, "" __VA_ARGS__);                        \
		fprintf(stdout, "%s\n", COLOR(ANSI_RESET));             \
	} while (0)

#define _(...)                                                  \
	do {                                                        \
		fprintf(stdout, "%s", COLOR(C_RATE));                   \
		fprintf(stdout, "" __VA_ARGS__);                        \
		fprintf(stdout, "%s\n", COLOR(ANSI_RESET));             \
	} while (0)

typedef struct {
    const char *name;
    const char *green;
    const char *yellow;
    const char *red;
    const char *blue;
    const char *purple;
    const char *grey;
} theme_config;

extern const theme_config *g_current_theme;

#define LOG_META(...) \
	LOG_BASE(stdout, "META", C_SILVER, __VA_ARGS__)

#define LOG_META_NNL(...) \
	LOG_BASE_NNL(stdout, "META", C_SILVER, __VA_ARGS__)

#define LOG_SHOW( fmt, ...)                                     \
	LOG_BASE(stdout, "SHOW", g_current_theme->grey, fmt, ##__VA_ARGS__)

#define LOG_TRACE(fmt, ...)                                     \
	LOG_BASE(stdout, "TRAC", g_current_theme->grey, fmt, ##__VA_ARGS__)

#define LOG_HINT(fmt, ...)                                      \
	LOG_BASE(stdout, "HINT", g_current_theme->green, fmt, ##__VA_ARGS__)

#define LOG_PATH(fmt, ...)                                      \
	LOG_BASE(stdout, "PATH", g_current_theme->purple, fmt, ##__VA_ARGS__)

#define LOG_INFO(fmt, ...)                                      \
	LOG_BASE(stdout, "INFO", g_current_theme->blue, fmt, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...)                                      \
	LOG_BASE(stderr, "WARN", g_current_theme->yellow, fmt, ##__VA_ARGS__)

#define LOG_ERR(fmt, ...)                                       \
	LOG_BASE(stderr, "ERRR", g_current_theme->red, fmt, ##__VA_ARGS__)

#if LOG_ENABLE_DEBUG
#define LOG_DEBUG(fmt, ...)                                     \
	LOG_BASE(stdout, "DBUG", C_DEBUG, fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif

void log_init(const char *theme);
void log_show_theme();
void log_show_all_theme(void);

#endif /* LOG_H */
