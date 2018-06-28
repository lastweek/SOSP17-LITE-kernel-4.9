/*
 * Copyright (c) 2018 Yizhou Shan <ys@purdue.edu>. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <malloc.h>
#include "lite-lib.h"

static int remote_node;
static unsigned int pg_size;

static inline void die(const char * str, ...)
{
        va_list args;
        va_start(args, str);
        vfprintf(stderr, str, args);
        fputc('\n', stderr);
        exit(1);
}

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define MAX_BUF_SIZE	(1024 * 1024 * 32)

/*
 * How many runs each size will run.
 * Amortize random variation.
 */
#define NR_TESTS_PER_SIZE	(1000*1000)
#define NSEC_PER_SEC	(1000*1000*1000)

static inline long timespec_diff_ns(struct timespec end, struct timespec start)
{
	long e, s;

	e = end.tv_sec * NSEC_PER_SEC + end.tv_nsec;
	s = start.tv_sec * NSEC_PER_SEC + start.tv_nsec;
	return e - s;
}

static void rdma_write_read()
{
	int i, j;
	uint64_t test_key;
	int testsize[10]={8,64,512,1024,1024*2,1024*4,1024*8, 1024*16, 1024*32, 1024*64};
	int password=100;
	char *buf;
	int *poll;

	buf = aligned_alloc(pg_size, MAX_BUF_SIZE);
	if (!buf)
		die("oom");

	poll = malloc(4 * NR_TESTS_PER_SIZE);
	memset(poll, 0, 4 * NR_TESTS_PER_SIZE);

	memset(buf, 'A', 1024 * 64);

	test_key = userspace_liteapi_alloc_remote_mem(remote_node,
						      MAX_BUF_SIZE, 0, password);
        printf("Finish remote mem alloc. Key: %#lx %ld\n", test_key, test_key);

	printf(" Test RDMA Write (avg of # %d run)\n", NR_TESTS_PER_SIZE);
	for (i = 0; i < ARRAY_SIZE(testsize); i++) {
		struct timespec start, end;
		long diff_ns;

		clock_gettime(CLOCK_MONOTONIC, &start);
		for (j = 0; j < NR_TESTS_PER_SIZE; j++) {
			userspace_liteapi_rdma_write(test_key, buf, testsize[i], 0, password);
		}
		clock_gettime(CLOCK_MONOTONIC, &end);
		diff_ns = timespec_diff_ns(end, start);

		printf("  CPU%2d  size = %#10x (%10d) avg_time = %15ld ns\n",
			sched_getcpu(),
			testsize[i], testsize[i], diff_ns/NR_TESTS_PER_SIZE);
	}

	printf(" Test RDMA Sync Read (avg of # %d run)\n", NR_TESTS_PER_SIZE);
	for (i = 0; i < ARRAY_SIZE(testsize); i++) {
		struct timespec start, end;
		long diff_ns;

		clock_gettime(CLOCK_MONOTONIC, &start);
		for (j = 0; j < NR_TESTS_PER_SIZE; j++) {
			userspace_liteapi_rdma_read(test_key, buf, testsize[i], 0, password);
		}
		clock_gettime(CLOCK_MONOTONIC, &end);
		diff_ns = timespec_diff_ns(end, start);

		printf("  CPU%2d  size = %#10x (%10d) avg_time = %15ld ns\n",
			sched_getcpu(),
			testsize[i], testsize[i], diff_ns/NR_TESTS_PER_SIZE);
	}

	printf(" Test RDMA Async Read (avg of # %d run)\n", NR_TESTS_PER_SIZE);
	for (i = 0; i < ARRAY_SIZE(testsize); i++) {
		struct timespec start, end;
		long diff_ns;

		clock_gettime(CLOCK_MONOTONIC, &start);
		for (j = 0; j < NR_TESTS_PER_SIZE; j++) {
			async_rdma_read(test_key, buf, testsize[i], 0, &poll[i]);
		}
		clock_gettime(CLOCK_MONOTONIC, &end);
		diff_ns = timespec_diff_ns(end, start);

		printf("  CPU%2d  size = %#10x (%10d) avg_time = %15ld ns\n",
			sched_getcpu(),
			testsize[i], testsize[i], diff_ns/NR_TESTS_PER_SIZE);
	}
}

static int bind_thread(int cpu_id)
{
	cpu_set_t cpu_set;

	CPU_ZERO(&cpu_set);
	CPU_SET(cpu_id, &cpu_set);

	return sched_setaffinity(0, sizeof (cpu_set), &cpu_set);
}

int main(int argc, char *argv[])
{
	int cpu;

        if (argc != 2) {
		printf("Usage: ./latency remote_node_id\n");
		return -EINVAL;
	}

	pg_size = sysconf(_SC_PAGESIZE);
	remote_node = atoi(argv[1]);

	//bind_thread(42);
	bind_thread(55);
	printf("Start testing on CPU%2d\n", sched_getcpu());

	rdma_write_read();

	printf("Finish testing on CPU%2d\n", sched_getcpu());
	return 0;
}
