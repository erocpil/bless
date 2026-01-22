#ifndef __LOG_H__
#define __LOG_H__

#include "color.h"
#include <time.h>
#include <stdlib.h>

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

/* =========================
 * 编译期功能开关
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
 * 时间戳
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
 * 线程 / CPU
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
 * 核心日志宏
 * ========================= */

#define LOG_BASE(fp, level, color, fmt, ...)                            \
	do {                                                                \
		fprintf(fp, "%s", COLOR(C_META));                               \
		fprintf(fp, "[");                                               \
		if (LOG_ENABLE_TIMESTAMP) {                                     \
			log_print_timestamp(fp);                                    \
			fprintf(fp, " ");                                           \
		}                                                               \
		if (LOG_ENABLE_THREAD) {                                        \
			fprintf(fp, "T%ld ", log_thread_id());                      \
		}                                                               \
		if (LOG_ENABLE_CPU) {                                           \
			fprintf(fp, "C%d ", log_cpu_id());                          \
		}                                                               \
		fprintf(fp, "]");                                               \
		\
		fprintf(fp, "%s[%s]%s ",                                        \
				COLOR(ANSI_BOLD),                                       \
				level,                                                  \
				COLOR(ANSI_RESET));                                     \
		\
		fprintf(fp, "%s", COLOR(color));                                \
		fprintf(fp, fmt, ##__VA_ARGS__);                                \
		fprintf(fp, "%s\n", COLOR(ANSI_RESET));                         \
	} while (0)

/* =========================
 * 对外接口
 * ========================= */

#define _L(fmt, ...)                                                    \
	do {                                                                \
		fprintf(stdout, "%s", COLOR(C_META));                               \
		fprintf(stdout, fmt, ##__VA_ARGS__);                                \
		fprintf(stdout, "%s\n", COLOR(ANSI_RESET));                         \
	} while (0)

#define LOG_HINT(fmt, ...)                                              \
	LOG_BASE(stdout, "HINT", C_HINT, fmt, ##__VA_ARGS__)

#define LOG_INFO(fmt, ...)                                              \
	LOG_BASE(stdout, "INFO", C_INFO, fmt, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...)                                              \
	LOG_BASE(stderr, "WARN", C_WARN, fmt, ##__VA_ARGS__)

#define LOG_ERR(fmt, ...)                                               \
	LOG_BASE(stderr, "ERRR", C_ERR, fmt, ##__VA_ARGS__)

#if LOG_ENABLE_DEBUG
#define LOG_DEBUG(fmt, ...)                                             \
	LOG_BASE(stdout, "DBUG", C_DEBUG, fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif

#if LOG_ENABLE_TRACE
#define LOG_TRACE(fmt, ...)                                             \
	LOG_BASE(stdout, "TRAC", C_TRACE, fmt, ##__VA_ARGS__)
#else
#define LOG_TRACE(fmt, ...) ((void)0)
#endif

#endif /* LOG_H */
