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
#include "lite-lib.h"

/*
 * lite rpc max port: 64
 * each thread pair need 2 ports
 */

const int run_times = 10;

int testsize[7]={8,8,64,512,1024,2048,4096};

int test_MB_size;
int write_mode = 0;
int thread_send_num=1;
int thread_recv_num=1;
int count = 0;
int go = 0;
int end_count = 0;

int base_port = 1;

struct thread_info {
	/*
	 * inbound_port is local's property, which is used by local to receive.
	 * outbound_port is remote's property, which is used by remote to receive
	 */
	int inbound_port;
	int outbound_port;

	int remote_nid;
};

void *thread_send_lat(void *_info)
{
	struct thread_info *info = _info;
	int ret;
	char *read = memalign(sysconf(_SC_PAGESIZE),4096*2);
	char *write = memalign(sysconf(_SC_PAGESIZE),4096*2);
        int ret_length;
	int i,j;
	struct timespec start, end;
	double *record=calloc(run_times, sizeof(double));
	uintptr_t descriptor;

	printf("%s(): receive_message use port %d, send_reply use port %d\n",
		__func__, info->inbound_port, info->outbound_port);

	memset(write, 'A', 4096);
	memset(read, 0, 4096);
        mlock(read, 4096);
        mlock(write, 4096);
        mlock(&ret_length, sizeof(int));

	for(j=0;j<7;j++) {
		memset(read, 0, 4096);

		for(i=0;i<run_times;i++) {
			ret = userspace_liteapi_send_reply_imm_fast(info->remote_nid,
				info->outbound_port, write, 8, read, &ret_length, 4096);
			printf("i=%d ret=%d ret_buf=%s\n", i, ret, read);
		}
	}

	printf("Before do receive\n");

	userspace_liteapi_receive_message_fast(info->inbound_port,
		read, 4096, &descriptor, &ret_length, BLOCK_CALL);

	printf("ret_buf: %s\n", read);
	printf("after do receive\n");
	userspace_liteapi_reply_message(write, 8, descriptor);
	printf("after do reply\n");

	return 0;
}

void *thread_recv(void *_info)
{
	struct thread_info *info = _info;
	uintptr_t descriptor, ret_descriptor;
	int i,j,k;
	char *read = memalign(sysconf(_SC_PAGESIZE),4096);
	char *write = memalign(sysconf(_SC_PAGESIZE),4096);
        int ret_length;
	
        int ret;
	int recv_num = thread_send_num/thread_recv_num;

	printf("%s(): receive_message use port %d, send_reply use port %d\n",
		__func__, info->inbound_port, info->outbound_port);

        mlock(write, 4096);
        mlock(read, 4096);
        mlock(&descriptor, sizeof(uintptr_t));
        mlock(&ret_length, sizeof(int));
	memset(write, 'B', 4096);
	memset(read, 0, 4096);

	for(j=0;j<7;j++) {
		memset(read, 0, 4096);
                for (i=0;i<run_times;i++) {
                        ret = userspace_liteapi_receive_message_fast(info->inbound_port,
				read, 4096, &descriptor, &ret_length, BLOCK_CALL);

			printf("i=%d ret=%d ret_buf=%s\n", i, ret, read);
                        userspace_liteapi_reply_message(write, testsize[j], descriptor);
                }
	}

	printf("beore send_reply\n");
	userspace_liteapi_send_reply_imm_fast(info->remote_nid, info->outbound_port,
		write, 8, read, &ret_length, 4096);
	printf("ret_buf=%s\n", read);
	printf("after send_reply\n");
}

void run(bool server_mode, int remote_node)
{
	char name[32] = {'\0'};
	pthread_t threads[64];
	struct thread_info *info = malloc(sizeof(*info));

	sprintf(name, "test.1");

	/*
	 * Okay
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
	printf("  %s -n <nid>              connect to server at <nid>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -s, --server              start a server\n");
	printf("  -n, --remote_nid=<nid>    remote server_id\n");
}

static struct option long_options[] = {
	{ .name = "server",	.has_arg = 0, .val = 's' },
	{ .name = "remote_nid",	.has_arg = 1, .val = 'n' },
	{}
};

int main(int argc, char *argv[])
{
	bool server_mode = false;
	unsigned int remote_nid = -1;

	while (1) {
		int c;

		c = getopt_long(argc, argv, "n:s",
				long_options, NULL);

		if (c == -1)
			break;

		switch (c) {
		case 's':
			server_mode = true;
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

	if (!server_mode && (remote_nid == -1)) {
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
