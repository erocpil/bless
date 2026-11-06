#ifndef __BUILDINFO_H__
#define __BUILDINFO_H__

/**
 * @file buildinfo.h
 * @brief Build-time volatile fields — time and host.
 *
 * Declared here as extern const arrays, defined in the auto-generated
 * src/buildinfo.c.  The compat macros (BUILD_TIME, BUILD_HOST) keep
 * existing call sites unchanged.
 */

extern const char build_time[];
extern const char build_host[];

#define BUILD_TIME  build_time
#define BUILD_HOST  build_host

#endif /* __BUILDINFO_H__ */
