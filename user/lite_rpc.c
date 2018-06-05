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
#include "lite-lib.h"



const int run_times = 10;

int testsize[7]={8,8,64,512,1024,2048,4096};

int test_MB_size;
int write_mode = 0;
int thread_node;
int thread_send_num=1;
int thread_recv_num=1;
pthread_mutex_t count_mutex;
int count = 0;
int go = 0;
pthread_mutex_t end_count_mutex;
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
	double total_lat;
	double *record=calloc(run_times, sizeof(double));

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
                        ret = userspace_liteapi_receive_message_fast(port, read, 4096, &descriptor, &ret_length, BLOCK_CALL);
			printf("i=%d ret=%d ret_buf=%s\n", i, ret, read);
                        userspace_liteapi_reply_message(write, testsize[j], descriptor);
                }
	}
}
int init_log(int remote_node)
{
        uint64_t xact_ID;
	int j, k;
	int *random_idx;
	struct timespec start, end;
        int temp[32];

	if(remote_node == 0)//receiver mode
	{
		pthread_t threads[64];
		
		char *name = malloc(16);
		int ret;
		sprintf(name, "test.1");
        	ret = userspace_liteapi_register_application(1, 4096, 16, name, strlen(name));
		printf("finish registeration ret-%d\n", ret);

                userspace_liteapi_dist_barrier(2);
                temp[0]=1; 
		pthread_create(&threads[0], NULL, thread_recv, &temp[0]);
		pthread_join(threads[0], NULL);
	}
	else//send to remote node
	{
		struct timespec start, end;
		double total_lat[7];

		pthread_t threads[64];

		thread_node = remote_node;

                userspace_liteapi_dist_barrier(2);
		userspace_liteapi_query_port(remote_node,1);

                temp[0] = 1;
                pthread_create(&threads[0], NULL, thread_send_lat, &temp[0]);
		for(j=0;j<7;j++)
		{
			pthread_mutex_lock(&count_mutex);
			count++;
			pthread_mutex_unlock(&count_mutex);
		}
		pthread_join(threads[0], NULL);
	}

	return 0;
}

int internal_value=0;
int main(int argc, char *argv[])
{
	if(argc!=2)
	{
		printf("./example_userspace_sr.o REMOTE_NODE\n");
		return 0;
	}

	pthread_mutex_init(&count_mutex, NULL);
	pthread_mutex_init(&end_count_mutex, NULL);
	init_log(atoi(argv[1]));
	return 0;
}
