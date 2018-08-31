#ifndef WP_H
#define WP_H

#include "utility.h"

struct WP;
struct WPJob;
typedef struct WPJob WPJob;
typedef struct WP WP;
typedef int(*WPHandler)(WP *, WPJob *);

struct WPJob {
	int commandid;
	sem_t resultready;
	int error;
	void *arg;
	void *result;
};


struct WP {
	int pid;
	SharedBufferPool* jobs;
	Queue *job_queue;
	WPHandler *handlers;
};

// takes the wp, the queue depth to setup, and the array of handler functions
extern int init_wp(WP *wp, int queue_depth, WPHandler *handlers, int work_threads);
// free's the worker process
extern void free_wp(WP *wp); 
// creates a job to send to the worker process (recommended to use create_job_easy)
extern WPJob* create_job(WP* wp, int commandid);
// creates a job to send to the worker process taking only the ptr to the handler
extern WPJob* create_job_easy(WP* wp, WPHandler func);
// sends the job to the worker process's processing queue
// 	   this function free's the job when done, it returns the error
//     you are responsible for cleaning / handling the arg struct and the result struct
extern int wp_job_invoke(WP *wp, WPJob *job);
// should never need to be called directly
extern int wp_job_free(WP *wp, WPJob *job);

#endif