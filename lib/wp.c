#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "wp.h"

struct wpthread_args {
	WP *wp;
	int threadid;
};

static void *wp_command_loop(void *arg) {
	struct wpthread_args *unpacked = arg;

	WP *wp = unpacked->wp;
	int threadid = unpacked->threadid;
	while (1) {
		WPJob *job;
		queue_get(wp->job_queue, &job);
		fprintf(stdout, "worker process threadid: %d awake\n", threadid);
		if (job->commandid == -1) {
			fprintf(stdout, "Worker process received shutdown command. Exiting\n");
			job->error = 0;
			sem_post(&(job->resultready));
			exit(0);
		} else if (wp->handlers[job->commandid] != NULL) {
			job->error = (*(wp->handlers[job->commandid]))(wp, job);
			sem_post(&(job->resultready));
		} else {
			fprintf(stderr, "Worker process: could not find commandid %d\n", job->commandid);
			job->error = -1;
			sem_post(&(job->resultready));
		}
	}
	return NULL;
}

int init_wp(WP *wp, int queue_depth, WPHandler *handlers, int work_threads) {
	wp->job_queue = sharedqueue_create(sizeof(WPJob *), queue_depth);
	wp->jobs = sharedbuffpool_create(sizeof(WPJob), queue_depth);
	wp->pid = fork();
	wp->handlers = handlers;
	if (wp->pid == -1) {
		return -1;
	} else if (wp->pid == 0) {
		int i;
		struct wpthread_args *args;

		for (i = 1; i < work_threads; ++i) {
			pthread_t thread;
			args = malloc(sizeof(struct wpthread_args));
			args->wp = wp;
			args->threadid = i;
			pthread_create(&thread, NULL, wp_command_loop, (void *)args);
		}

		args = malloc(sizeof(struct wpthread_args));
		args->wp = wp;
		args->threadid = work_threads;
		wp_command_loop(args);
		exit(0);
	}
	return 0;
}

void free_wp(WP *wp) {
	sharedqueue_free(wp->job_queue);
	sharedbuffpool_free(wp->jobs);
	kill(wp->pid, 9);
}

WPJob* create_job(WP* wp, int commandid) {
	fprintf(stdout, "create job, getting chunk\n");
	WPJob *job = (WPJob *)bp_getchunk(wp->jobs);
	fprintf(stdout, "create job, got chunk\n");
	sem_init(&(job->resultready), 1, 0);
	job->commandid = commandid;
	job->error = 0;
	job->arg = NULL;
	job->result = NULL;
	return job;
}

WPJob* create_job_easy(WP* wp, WPHandler func) {
	fprintf(stderr, "trying to create the job easily...\n");
	int commandid = 0;
	WPHandler *cur = wp->handlers;
	while (*cur != NULL && *cur != func) {
		cur++;
		commandid++;
	}

	if (*cur == NULL) {
		fprintf(stderr, "Fatal error: could not find the commandid\n");
		exit(0);
	}

	return create_job(wp, commandid);
}

int wp_job_invoke(WP *wp, WPJob *job) {
	queue_put(wp->job_queue, &job);
	sem_wait(&(job->resultready));
	int error = job->error;
	wp_job_free(wp, job);
	return error;
}

int wp_job_free(WP *wp, WPJob *job) {
	sem_destroy(&(job->resultready));
	bp_freechunk(wp->jobs, (void *)job);
}
