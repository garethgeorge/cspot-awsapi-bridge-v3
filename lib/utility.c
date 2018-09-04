#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <pthread.h>
#include "utility.h"

/*
	Large object pool
*/

/*
	Create shared memory segment in a mmaped file
*/
void* create_shared_memory(size_t size) {
	return mmap(NULL, 
		size, 
		PROT_READ | PROT_WRITE, 
		MAP_ANONYMOUS | MAP_SHARED, 0, 0);
}

/*
	Shared Circular Queue Methods
*/
int queue_init(Queue *queue, int elem_size, int size) {
	queue->head = queue->tail = 0;
	queue->size = size;
	queue->elem_size = elem_size;
	queue->size_bytes = size * elem_size;

	sem_init(&(queue->space), 0, size);
	sem_init(&(queue->items), 0, 0);
	sem_init(&(queue->mutex), 0, 1);

	queue->elements = malloc(elem_size * size);
	if (queue->elements == NULL)
		return -1;
	return 0;
}

Queue *sharedqueue_create(int elem_size, int size) {
	void *memory = create_shared_memory(sizeof(Queue) + size * elem_size);
	if (memory == NULL)
		return NULL;

	Queue *queue = (Queue *)memory;
	queue->head = queue->tail = 0;
	queue->size = size;
	queue->elem_size = elem_size;
	queue->size_bytes = size * elem_size;

	sem_init(&(queue->space), 1, size);
	sem_init(&(queue->items), 1, 0);
	sem_init(&(queue->mutex), 1, 1);
	
	queue->elements = memory + sizeof(Queue);
	return queue;
}

void queue_free(Queue *queue) {
	sem_destroy(&(queue->space));
	sem_destroy(&(queue->items));
	sem_destroy(&(queue->mutex));
	free(queue->elements);
}

void sharedqueue_free(Queue *queue) {
	sem_destroy(&(queue->space));
	sem_destroy(&(queue->items));
	sem_destroy(&(queue->mutex));
	munmap((void *)queue, sizeof(Queue) + queue->size * queue->elem_size);
}

void queue_put(Queue *queue, void *value) {
	sem_wait(&(queue->space));
	sem_wait(&(queue->mutex));
	memcpy(queue->elements + queue->tail, value, queue->elem_size);
	queue->tail += queue->elem_size;
	if (queue->tail >= queue->size_bytes)
		queue->tail = 0;
	sem_post(&(queue->mutex));
	sem_post(&(queue->items));
}

void queue_get(Queue *queue, void *result) {
	sem_wait(&(queue->items));
	sem_wait(&(queue->mutex));
	memcpy(result, queue->elements + queue->head, queue->elem_size);
	queue->head += queue->elem_size;
	if (queue->head >= queue->size_bytes)
		queue->head = 0;
	sem_post(&(queue->mutex));
	sem_post(&(queue->space));
}

/*
	A Pool of Memory for Large Shared Buffers
*/

int bufferpool_get_size(int buffersize, int buffercount) {
	return sizeof(SharedBufferPool) + 
		sizeof(char) * buffercount + 
		buffersize * buffercount;
}

SharedBufferPool *sharedbuffpool_create(int buffersize, int buffercount) {
	void *memory = create_shared_memory(bufferpool_get_size(buffercount, buffersize));
	if (memory == NULL)
		return NULL;

	SharedBufferPool *bp = (SharedBufferPool *)memory;
	sem_init(&(bp->free_chunks), 1, buffercount);
	sem_init(&(bp->mutex), 1, 1);
	
	bp->count = buffercount;
	bp->size = buffersize;
	bp->inuse = (char *)(memory + sizeof(SharedBufferPool));
	bp->chunks = (void *)(memory + sizeof(SharedBufferPool) + sizeof(char) * buffercount);
}

void sharedbuffpool_free(SharedBufferPool *bp) {
	munmap((void *)bp, bufferpool_get_size(bp->count, bp->size));
}

void *bp_getchunk(SharedBufferPool *bp) {
	sem_wait(&(bp->free_chunks));
	sem_wait(&(bp->mutex));
	int i;
	for (i = 0; i < bp->count; ++i) {
		if (!bp->inuse[i]) break;
	}
	void *chunk = bp->chunks + (i * bp->size);
	bp->inuse[i] = 1;
	sem_post(&(bp->mutex));
	return chunk;
};

int bp_freechunk(SharedBufferPool *bp, void *chunk) {
	sem_wait(&(bp->mutex));
	long chunk_offset = chunk - bp->chunks;
	bp->inuse[chunk_offset / bp->size] = 0;
	sem_post(&(bp->mutex));
	sem_post(&(bp->free_chunks));
}

