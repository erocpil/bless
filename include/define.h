#ifndef __DEFINE_H__
#define __DEFINE_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   // access()
#include <ctype.h>    // isprint()
#include <stdatomic.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <bits/socket.h>
#include <x86intrin.h>

#define RTE_LOGTYPE_BLESS RTE_LOGTYPE_USER1

#define MAX_PKT_BURST 32
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
#define MEMPOOL_CACHE_SIZE 256

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RX_DESC_DEFAULT 1024
#define TX_DESC_DEFAULT 1024

#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT 16

#ifndef NELEMS
#define NELEMS(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define MBUF_DYNFIELDS_MAX  8

enum {
	STATE_INIT = 0,
	STATE_RUNNING = 1,
	STATE_STOPPED = 2,
	STATE_EXIT = 3,
};

typedef uint64_t (*mutation_func)(void **mbufs, unsigned int n, void *data);

/* 计算 16-bit one's complement checksum */
static uint16_t icmp_calc_cksum(const void *buf, size_t len)
{
	const uint16_t *data = buf;
	uint32_t sum = 0;

	while (len > 1) {
		sum += *data++;
		len -= 2;
	}
	if (len == 1) {
		sum += *((const uint8_t *)data) << 8;
	}
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return (uint16_t)(~sum);
}

static inline uint64_t rdtsc_serialized()
{
	// _mm_lfence();
	return __rdtsc();
}

static inline uint8_t rdtsc8()
{
	return rdtsc_serialized();
}

static inline uint16_t rdtsc16()
{
	return rdtsc_serialized();
}

static inline uint32_t rdtsc32()
{
	return rdtsc_serialized();
}

static inline uint64_t rdtsc64()
{
	return rdtsc_serialized();
}

#endif
