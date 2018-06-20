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
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <stdbool.h>
#include <malloc.h>
#include <getopt.h>
#include <sched.h>
#include "lite-lib.h"

#define MAX_BUF_SIZE	(1024 * 1024 * 4)

/*
 * lite rpc max port: 64
 * each thread pair need 2 ports
 */
struct thread_info {
	/*
	 * inbound_port is local's property, which is used by local to receive.
	 * outbound_port is remote's property, which is used by remote to receive
	 */
	int inbound_port;
	int outbound_port;

	int remote_nid;
};

#define NSEC_PER_SEC	(1000*1000*1000)
static inline long timespec_diff_ns(struct timespec end, struct timespec start)
{
	long e, s;

	e = end.tv_sec * NSEC_PER_SEC + end.tv_nsec;
	s = start.tv_sec * NSEC_PER_SEC + start.tv_nsec;
	return e - s;
}

static int bind_thread(int cpu_id)
{
	cpu_set_t cpu_set;

	CPU_ZERO(&cpu_set);
	CPU_SET(cpu_id, &cpu_set);

	return sched_setaffinity(0, sizeof (cpu_set), &cpu_set);
}

/*
 * ret_buf is _not_ guranteed to be ready upon return.
 * Caller is responsible for polling *ret_length for completion.
 *
 * When the underlying syscall returned, it only guranteed that
 * the send WQE has been posted to senq queue. It does not gurantee
 * the buffer has been set out by NIC.
 *
 * That means, caller can not free, nor reuse @buf. Otherwise, the
 * data that will be sent out by NIC, might be corrupted.
 *
 * You are safe to reuse @buf after async_rpc_completed() returns true.
 */
int async_rpc(int dst_nid, int dst_port, void *buf, int buf_size,
	      void *ret_buf, int *ret_size_ptr, int max_ret_size)
{
	int ret;

	if (buf_size >= MAX_BUF_SIZE || max_ret_size >= MAX_BUF_SIZE) {
		fprintf(stderr, "%s: buf_size %d max_ret_size %d too big\n",
			__func__, buf_size, max_ret_size);
		return -EINVAL;
	}

	ret = syscall(__NR_lite_send_reply_imm,
			dst_nid,
			(buf_size << IMM_MAX_PORT_BIT) + dst_port,
			buf, ret_buf, ret_size_ptr,
			(max_ret_size << IMM_MAX_PRIORITY_BIT) + NULL_PRIORITY);
	if (ret < 0)
		perror("lite_send_reply syscall failed");
	return 0;
}

/*
 * Return true if the @poll point indicate the
 * async RPC has completed.
 */
static inline bool async_rpc_completed(int *poll)
{
	if (*poll == SEND_REPLY_WAIT)
		return false;
	return true;
}

#define WAIT_COMPLETION_TIMEOUT_S	(5)

static inline void wait_for_completion(int *poll)
{
	time_t start;
	bool saved_ts = false;

	while (!async_rpc_completed(poll)) {
		time_t now;

		if (!saved_ts) {
			start = time(NULL);
			saved_ts = true;
		}

		now = time(NULL);
		if (now - start > WAIT_COMPLETION_TIMEOUT_S) {
			printf("%s: timeout (%d s) at poll %p\n",
				__func__, WAIT_COMPLETION_TIMEOUT_S, poll);
		}
	}
}

#define NR_ASYNC_RPC		(1000 * 1000 * 10)
#define NR_SYNC_RPC		(1000 * 1000 * 10)

void test_sync_rpc_send(struct thread_info *info)
{
	int *poll_array;
	int i, ret;
	char *read, *write;
	struct timespec start, end;
	long diff_ns;

	read = memalign(sysconf(_SC_PAGESIZE), 4096 * 2);
	write = memalign(sysconf(_SC_PAGESIZE), 4096 * 2);
	poll_array = memalign(sysconf(_SC_PAGESIZE),
		sizeof(int) * NR_SYNC_RPC);
	memset(poll_array, 0, sizeof(int) * NR_SYNC_RPC);
	mlock(read, 4096);
	mlock(write, 4096);
	mlock(poll_array, sizeof(int) * NR_SYNC_RPC);

	printf("Start sync rpc\n");

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (i = 0; i < NR_SYNC_RPC; i++) {
		/*
		 * send 4 bytes 
		 */
		userspace_liteapi_send_reply_imm_fast(info->remote_nid,
			info->outbound_port, write, 4, read, &poll_array[i], 4096);
	}
	clock_gettime(CLOCK_MONOTONIC, &end);

	diff_ns = timespec_diff_ns(end, start);

	printf("Performed #%d sync_rpc. Total %ld ns, per sync_rpc: %ld ns\n",
		NR_SYNC_RPC, diff_ns, diff_ns/NR_SYNC_RPC);

	printf("Done sync rpc\n");
}

void test_sync_rpc_recv(struct thread_info *info)
{
	int i, ret, ret_length;
	uintptr_t descriptor;
	char *read, *write;

	read = memalign(sysconf(_SC_PAGESIZE), 4096 * 2);
	write = memalign(sysconf(_SC_PAGESIZE), 4096 * 2);
	mlock(read, 4096);
	mlock(write, 4096);

	printf("Start sync test\n");
	for (i = 0; i < NR_SYNC_RPC; i++) {
		ret = userspace_liteapi_receive_message_fast(info->inbound_port,
			read, 4096, &descriptor, &ret_length, BLOCK_CALL);
        	userspace_liteapi_reply_message(write, 4, descriptor);
	}
	printf("Done async test\n");
}

/*
 * Can we reuse the send buffer? Depends on when the syscall returned.
 * If it is returned after send has completed, then we can.
 * If it is returned before send has completed, then we can not.
 */
void test_async_rpc_send(struct thread_info *info)
{
	int *poll_array;
	int i, ret;
	char *read, *write;
	struct timespec start, end;
	long diff_ns;

	read = memalign(sysconf(_SC_PAGESIZE), 4096 * 2);
	write = memalign(sysconf(_SC_PAGESIZE), 4096 * 2);
	poll_array = memalign(sysconf(_SC_PAGESIZE),
		sizeof(int) * NR_ASYNC_RPC);
	memset(poll_array, 0, sizeof(int) * NR_ASYNC_RPC);
	mlock(read, 4096);
	mlock(write, 4096);
	mlock(poll_array, sizeof(int) * NR_ASYNC_RPC);

	printf("Start async rpc\n");

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (i = 0; i < NR_ASYNC_RPC; i++) {
		/*
		 * send 4 bytes 
		 */
		async_rpc(info->remote_nid, info->outbound_port,
			write, 4, read, &poll_array[i], 4096);
	}
	clock_gettime(CLOCK_MONOTONIC, &end);

	diff_ns = timespec_diff_ns(end, start);
	for (i = 0; i < NR_ASYNC_RPC; i++)
		wait_for_completion(&poll_array[i]);

	printf("Performed #%d async_rpc. Total %ld ns, per async_rpc: %ld ns\n",
		NR_ASYNC_RPC, diff_ns, diff_ns/NR_ASYNC_RPC);

	printf("Done async rpc\n");
}

void test_async_rpc_recv(struct thread_info *info)
{
	int i, ret, ret_length;
	uintptr_t descriptor;
	char *read, *write;

	read = memalign(sysconf(_SC_PAGESIZE), 4096 * 2);
	write = memalign(sysconf(_SC_PAGESIZE), 4096 * 2);
	mlock(read, 4096);
	mlock(write, 4096);

	printf("Start async test\n");
	for (i = 0; i < NR_ASYNC_RPC; i++) {
		ret = userspace_liteapi_receive_message_fast(info->inbound_port,
			read, 4096, &descriptor, &ret_length, BLOCK_CALL);
        	userspace_liteapi_reply_message(write, 4, descriptor);
	}
	printf("Done async test\n");
}

int testsize[7]={8,8,64,512,1024,2048,4096};
int run_times = 10;
int base_port = 1;

void *thread_send_lat(void *_info)
{
	struct thread_info *info = _info;
	int ret;
	char *read = memalign(sysconf(_SC_PAGESIZE), 4096 * 2);
	char *write = memalign(sysconf(_SC_PAGESIZE), 4096 * 2);
        int ret_length;
	int i,j,cnt;
	uintptr_t descriptor;
	int *async_poll_buffer;

	printf("%s(): receive_message use port %d, send_reply use port %d\n",
		__func__, info->inbound_port, info->outbound_port);
	bind_thread(55);
	printf("Start testing on CPU%2d\n", sched_getcpu());

	memset(write, 'A', 4096);
	memset(read, 0, 4096);
        mlock(read, 4096);
        mlock(write, 4096);
        mlock(&ret_length, sizeof(int));

	/*
	 * Part I
	 * Test Synchronous Symmetric RPC
	 * - send_reply
	 * - receive + reply
	 */

	/* send_reply */
	for (cnt = 0, j = 0; j < 7; j++) {
		for (i=0;i<run_times;i++) {
			*(int *)write = cnt + 200;
			ret = userspace_liteapi_send_reply_imm_fast(info->remote_nid,
				info->outbound_port, write, 8, read, &ret_length, 4096);

			printf("send_reply cnt=%d send=%d receive=%d\n",\
				cnt, *(int *)write, *(int *)read);
			cnt++;
		}
		memset(read, 0, 4096);
	}

	/* Receive + Reply */
	for(cnt = 0, j=0;j<7;j++) {
                for (i=0;i<run_times;i++) {
                        ret = userspace_liteapi_receive_message_fast(info->inbound_port,
				read, 4096, &descriptor, &ret_length, BLOCK_CALL);

			*(int *)write = cnt + 100;
                        userspace_liteapi_reply_message(write, testsize[j], descriptor);

			printf("receive+reply cnt=%d receive=%d send=%d\n",\
				cnt, *(int *)read, *(int *)write);
			cnt++;
                }
		memset(read, 0, 4096);
	}

	/*
	 * Part II
	 * Test A-synchronous RPC
	 * - send_reply
	 */
	test_async_rpc_send(info);
	test_sync_rpc_send(info);
}

void *thread_recv(void *_info)
{
	struct thread_info *info = _info;
	uintptr_t descriptor, ret_descriptor;
	int i,j,k, cnt;
	char *read = memalign(sysconf(_SC_PAGESIZE),4096);
	char *write = memalign(sysconf(_SC_PAGESIZE),4096);
        int ret_length;
        int ret;

	printf("%s(): receive_message use port %d, send_reply use port %d\n",
		__func__, info->inbound_port, info->outbound_port);
	bind_thread(55);
	printf("Start testing on CPU%2d\n", sched_getcpu());

        mlock(write, 4096);
        mlock(read, 4096);
        mlock(&descriptor, sizeof(uintptr_t));
        mlock(&ret_length, sizeof(int));
	memset(write, 'B', 4096);
	memset(read, 0, 4096);

	/*
	 * Part I
	 * Symmetric RPC
	 * - receive + reply
	 * - send_reply
	 */

	/* Receive + Reply */
	for(cnt = 0, j=0;j<7;j++) {
                for (i=0;i<run_times;i++) {
                        ret = userspace_liteapi_receive_message_fast(info->inbound_port,
				read, 4096, &descriptor, &ret_length, BLOCK_CALL);

			*(int *)write = cnt + 100;
                        userspace_liteapi_reply_message(write, testsize[j], descriptor);

			printf("receive+reply cnt=%d receive=%d send=%d\n",\
				cnt, *(int *)read, *(int *)write);
			cnt++;
                }
		memset(read, 0, 4096);
	}

	/* send_reply */
	for (cnt = 0, j = 0; j < 7; j++) {
		for (i=0;i<run_times;i++) {
			*(int *)write = cnt + 200;
			ret = userspace_liteapi_send_reply_imm_fast(info->remote_nid,
				info->outbound_port, write, 8, read, &ret_length, 4096);

			printf("send_reply cnt=%d send=%d receive=%d\n",\
				cnt, *(int *)write, *(int *)read);
			cnt++;
		}
		memset(read, 0, 4096);
	}

	test_async_rpc_recv(info);
	test_sync_rpc_recv(info);
}

void run(bool server_mode, int remote_node)
{
	char name[32] = {'\0'};
	pthread_t threads[64];
	struct thread_info *info = malloc(sizeof(*info));

	sprintf(name, "test.1");

	/*
	 * Okay, symmetric RPC.
	 * Server use (base_port) to receive client's RPC request
	 * Client use (base_port + 1) to receive server's RPC request
	 *
	 * By doing this, server/client can both send RPC to each other.
	 */
	if (server_mode) {
		info->inbound_port = base_port;
		info->outbound_port = base_port + 1;

		/* XXX: hardcoded */
		info->remote_nid = 1;

       		userspace_liteapi_register_application(info->inbound_port,
			4096, 16, name, strlen(name));

                userspace_liteapi_dist_barrier(2);
		printf("Pass dist barrier..\n");

		/*
		 * Server should query client's inboud port info,
		 * which is base_port + 1
		 */
		userspace_liteapi_query_port(info->remote_nid,
					     info->outbound_port);

		pthread_create(&threads[0], NULL, thread_recv, info);
		pthread_join(threads[0], NULL);
	} else {
		info->inbound_port = base_port + 1;
		info->outbound_port = base_port;
		info->remote_nid = remote_node;

       		userspace_liteapi_register_application(info->inbound_port,	
			4096, 16, name, strlen(name));

                userspace_liteapi_dist_barrier(2);
		printf("Pass dist barrier..\n");

		/*
		 * Client should query server's inboud port info,
		 * which is base_port
		 */
		userspace_liteapi_query_port(info->remote_nid,
					     info->outbound_port);

                pthread_create(&threads[0], NULL, thread_send_lat, info);
		pthread_join(threads[0], NULL);
	}
}

static void usage(const char *argv0)
{
	printf("Usage:\n");
	printf("  %s -s                    start a server and wait for connection\n", argv0);
	printf("  %s -c -n <nid>           start a client and connect to server at <nid>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -c, --client              start a client\n");
	printf("  -s, --server              start a server\n");
	printf("  -n, --remote_nid=<nid>    remote server_id\n");
}

static struct option long_options[] = {
	{ .name = "server",	.has_arg = 0, .val = 's' },
	{ .name = "client",	.has_arg = 0, .val = 'c' },
	{ .name = "remote_nid",	.has_arg = 1, .val = 'n' },
	{}
};

int main(int argc, char *argv[])
{
	bool server_mode = false, client_mode = false;
	unsigned int remote_nid = -1;

	while (1) {
		int c;

		c = getopt_long(argc, argv, "n:sc",
				long_options, NULL);

		if (c == -1)
			break;

		switch (c) {
		case 's':
			server_mode = true;
			break;
		case 'c':
			client_mode = true;
			break;
		case 'n':
			remote_nid = strtoul(optarg, NULL, 0);
			if (remote_nid > 16) {
				usage(argv[0]);
				return -1;
			}
			break;
		default:
			usage(argv[0]);
			return -1;
		};
	}

	if (!server_mode && !client_mode) {
		usage(argv[0]);
		return -1;
	} else if (client_mode && (remote_nid == -1)) {
		usage(argv[0]);
		return -1;
	} else if (server_mode && (remote_nid != -1)) {
		usage(argv[0]);
		return -1;
	}

	if (server_mode)
		printf("RPC server, waiting for connection..\n");
	else
		printf("RPC client, connect to server at %d\n", remote_nid);

	run(server_mode, remote_nid);

	return 0;
}
