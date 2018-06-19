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

const int run_times = 10;

int testsize[7]={8,8,64,512,1024,2048,4096};

int test_MB_size;
int write_mode = 0;
int thread_node;
int thread_send_num=1;
int thread_recv_num=1;
int count = 0;
int go = 0;
int end_count = 0;

void *thread_send_lat(void *tmp)
{
	int ret;
	int remote_node = thread_node;
	int port = *(int *)tmp;
	char *read = memalign(sysconf(_SC_PAGESIZE),4096*2);
	char *write = memalign(sysconf(_SC_PAGESIZE),4096*2);
        int ret_length;
	int i,j;
	struct timespec start, end;
	double *record=calloc(run_times, sizeof(double));
	uintptr_t descriptor;

	memset(write, 'A', 4096);
	memset(read, 0, 4096);
        mlock(read, 4096);
        mlock(write, 4096);
        mlock(&ret_length, sizeof(int));

	for(j=0;j<7;j++) {
		memset(read, 0, 4096);

		for(i=0;i<run_times;i++)
		{
			ret = userspace_liteapi_send_reply_imm_fast(remote_node, port, write, 8, read, &ret_length, 4096);
			printf("i=%d ret=%d ret_buf=%s\n", i, ret, read);
		}
	}

	printf("Before do receive\n");
	userspace_liteapi_receive_message_fast(port, read, 4096,
		&descriptor, &ret_length, BLOCK_CALL);
	printf("ret_buf: %s\n", read);
	printf("after do receive\n");
	userspace_liteapi_reply_message(write, 8, descriptor);
	printf("after do reply\n");

	return 0;
}

void *thread_recv(void *tmp)
{
	int port = *(int *)tmp;
	uintptr_t descriptor, ret_descriptor;
	int i,j,k;
	char *read = memalign(sysconf(_SC_PAGESIZE),4096);
	char *write = memalign(sysconf(_SC_PAGESIZE),4096);
        int ret_length;
	
        int ret;
	int recv_num = thread_send_num/thread_recv_num;

        mlock(write, 4096);
        mlock(read, 4096);
        mlock(&descriptor, sizeof(uintptr_t));
        mlock(&ret_length, sizeof(int));
	memset(write, 'B', 4096);
	memset(read, 0, 4096);

	for(j=0;j<7;j++) {
		memset(read, 0, 4096);
                for (i=0;i<run_times;i++) {
                        ret = userspace_liteapi_receive_message_fast(port, read, 4096,
				&descriptor, &ret_length, BLOCK_CALL);

			printf("i=%d ret=%d ret_buf=%s\n", i, ret, read);
                        userspace_liteapi_reply_message(write, testsize[j], descriptor);
                }
	}

	printf("beore send_reply\n");
	userspace_liteapi_send_reply_imm_fast(1, port, write, 8, read, &ret_length, 4096);
	printf("ret_buf=%s\n", read);
	printf("after send_reply\n");
}

void run(bool server_mode, int remote_node)
{
        int temp[32];

	if (server_mode) {
		char name[32] = {'\0'};
		pthread_t threads[64];

		sprintf(name, "test.1");
       		userspace_liteapi_register_application(1, 4096, 16, name, strlen(name));
		printf("Finish app registeration..\n");

                userspace_liteapi_dist_barrier(2);
		printf("Pass dist barrier..\n");

                temp[0]=1; 
		pthread_create(&threads[0], NULL, thread_recv, &temp[0]);
		pthread_join(threads[0], NULL);
	} else {
		pthread_t threads[64];

		thread_node = remote_node;

                userspace_liteapi_dist_barrier(2);
		printf("Pass dist barrier..\n");

		userspace_liteapi_query_port(remote_node, 1);

                temp[0] = 1;
                pthread_create(&threads[0], NULL, thread_send_lat, &temp[0]);
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
