#include <stdlib.h>
#include <stdio.h>
#include <semaphore.h>
#include <pthread.h>
#include <string.h>

#include "lib/utility.h"
#include "lib/wp.h"

static void *work_thread(void *args) {
	Queue *queue = (Queue *)args;
	
	int i;
	for (i = 0; i < 100; ++i) {
		int value;
		queue_get(queue, &value);
		fprintf(stdout, "%d, ", value);
		fflush(stdout);
	}
	
	fprintf(stdout, "\n");
}

// WORKER PROCESS COMMAND DEFINITION
struct arg_echo {
	char message[256];
};

struct result_echo {
	char message[512];
};

int handler_echo(WP *wp, WPJob *job) {
	struct arg_echo *arg = (struct arg_echo *)job->arg;
	struct result_echo *result = (struct result_echo *)job->result;
	int i;
	for (i = 0; i < 1000; ++i) {
		sprintf(result->message, "%s %s %s", arg->message, arg->message, arg->message);
	}

	return 0;
}


int main() {
	{
		fprintf(stdout, "\ttesting queue shared between threads\n");
		Queue queue;
		queue.head = 0;
		queue.tail = 0;
		queue_init(&queue, sizeof(int), 16);

		pthread_t mythread;
		pthread_create(&mythread, NULL, work_thread, &queue);

		int i;
		for (i = 0; i < 100; ++i) {
			queue_put(&queue, &i);
		}

		void *result;
		pthread_join(mythread, &result);
		queue_free(&queue);
		fprintf(stdout, "done.\n");
	}

	{
		fprintf(stdout, "\ttesting queue shared between multiple processes\n");
		Queue *queue = sharedqueue_create(sizeof(int), 2);

		int pid = fork();
		if (pid == 0) {
			int i;
			for (i = 0; i < 10; ++i){
				int value;
				queue_get(queue, &value);
				fprintf(stdout, "p1: %d\n", value);
			}
			exit(0);
		}

		int pid2 = fork();
		if (pid2 == 0) {
			int i;
			for (i = 0; i < 10; ++i){
				int value;
				queue_get(queue, &value);
				fprintf(stdout, "p2: %d\n", value);
			}
			exit(0);
		}

		int i;
		for (i = 0; i < 20; ++i) {
			queue_put(queue, &i);
		}
		int status;
		waitpid(pid, &status);
		waitpid(pid2, &status);
		sleep(1);
		sharedqueue_free(queue);
		fprintf(stdout, "Done.\n");
	}

	{
		fprintf(stdout, "\ttesting queue shared between processes that sends along strings\n");
		SharedBufferPool *bp = sharedbuffpool_create(1024, 2); // count, size
		Queue *queue = sharedqueue_create(sizeof(char *), 1); // size, count its a bit weird i know

		if (bp == NULL || queue == NULL) {
			fprintf(stderr, "Fatal error: failed to allocate one of the structures\n");
		}

		int pid = fork();
		if (pid == 0) {
			int i;
			for (i = 0; i < 10; ++i){
				char *value;
				queue_get(queue, &value);
				fprintf(stdout, "string: %s", value);
				bp_freechunk(bp, value);
			}
			exit(0);
		}
		
		int i;
		for (i = 0; i < 10; ++i) {
			char *buf = (char *)bp_getchunk(bp);
			sprintf(buf, "hey there, index is %d\n", i);
			queue_put(queue, &buf);
		}

		int status;
		waitpid(pid, &status);
		sleep(1);
		fprintf(stdout, "Done.\n");
	}

	{
		fprintf(stdout, "Testing that worker processes work as expected\n");
		WP wp;
		WPHandler handlers[] = {
			handler_echo,
			NULL
		};

		// NOTE: all shared memory objects must be created BEFORE the fork
		SharedBufferPool *largeObjects = sharedbuffpool_create(1024, 64);


		init_wp(&wp, 32, handlers, 4); // the last number is the number of work threads
		
		int i;
		for (i = 0; i < 1000; ++i) {
			WPJob* theJob = create_job_easy(&wp, handler_echo);
			struct arg_echo *arg = theJob->arg = bp_getchunk(largeObjects);
			struct result_echo *result = theJob->result = bp_getchunk(largeObjects);

			strcpy(arg->message, "hello world!!");
			fprintf(stdout, "job returned with value: %d\n", wp_job_invoke(&wp, theJob));
			fprintf(stdout, "echo'd message: %s\n", result->message);
			bp_freechunk(largeObjects, (void *)arg);
			bp_freechunk(largeObjects, (void *)result);
		}

		free_wp(&wp);
	}

	return 0;
}