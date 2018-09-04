#ifndef DATASTRUCTURES_H
#define DATASTRUCTURES_H

#include <semaphore.h>

#ifdef __cplusplus
extern "C" {
#endif

struct Queue {
	int head;
	int tail;
	int size;
	int size_bytes;
	int elem_size;
	sem_t space;
	sem_t items;
	sem_t mutex;
	void *elements;
};

typedef struct Queue Queue;

// create a simple queue for use in one process
extern int queue_init(Queue *queue, int elem_size, int size); // a queue that can be shared between threads
extern Queue *sharedqueue_create(int elem_size, int size); // a queue that can be shared between a parent and child process
extern void queue_free(Queue *queue);
extern void sharedqueue_free(Queue *queue);
extern void queue_put(Queue *queue, void *value);
extern void queue_get(Queue *queue, void *result);

struct SharedBufferPool {
	int count;
	int size;
	char *inuse;
	void *chunks;
	sem_t free_chunks;
	sem_t mutex;
};

typedef struct SharedBufferPool SharedBufferPool;

extern int bufferpool_get_size(int buffersize, int buffercount);

extern SharedBufferPool *sharedbuffpool_create(int buffersize, int buffercount);

extern void sharedbuffpool_free(SharedBufferPool *bp);

extern void *bp_getchunk(SharedBufferPool *bp);

extern int bp_freechunk(SharedBufferPool *bp, void *chunk);

#ifdef __cplusplus
}
#endif

#endif