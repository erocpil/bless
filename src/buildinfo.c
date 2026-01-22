#include <stddef.h>

#ifndef BUILD_GIT_COMMIT
#define BUILD_GIT_COMMIT "unknown"
#endif
#ifndef BUILD_GIT_BRANCH
#define BUILD_GIT_BRANCH "unknown"
#endif
#ifndef BUILD_TIME
#define BUILD_TIME "unknown"
#endif
#ifndef BUILD_HOST
#define BUILD_HOST "unknown"
#endif
#ifndef BUILD_CC_VERSION
#define BUILD_CC_VERSION "unknown"
#endif
#ifndef BUILD_CFLAGS
#define BUILD_CFLAGS "unknown"
#endif
#ifndef BUILD_LDFLAGS
#define BUILD_LDFLAGS "unknown"
#endif

#define STR1(x) #x
#define STR(x) STR1(x)

// #pragma message("BUILD_GIT_COMMIT = " STR(BUILD_GIT_COMMIT))

/*
 * used      : 防止被编译器优化掉
 * section   : 放入自定义 ELF section
 * aligned   : 便于工具读取（可选）
 */
__attribute__((used, section(".buildinfo"), aligned(1)))
const char buildinfo[] =
"git commit: " BUILD_GIT_COMMIT "\n"
"git branch: " BUILD_GIT_BRANCH "\n"
"build time: " BUILD_TIME "\n"
"build host: " BUILD_HOST "\n"
"cc version: " BUILD_CC_VERSION "\n"
"cflags: " BUILD_CFLAGS "\n"
"ldflags: " BUILD_LDFLAGS "\n";
