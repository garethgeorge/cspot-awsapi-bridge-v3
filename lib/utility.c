#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <pthread.h>
#include <assert.h>
#include "utility.h"

// TODO: possibly switch to https://stackoverflow.com/questions/22207546/shared-memory-ipc-synchronization-lock-free

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

	InitSem(&(queue->space), size);
	InitSem(&(queue->items), 0);
	InitSem(&(queue->mutex), 1);

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

	InitSem(&(queue->space), size);
	InitSem(&(queue->items), 0);
	InitSem(&(queue->mutex), 1);
	
	queue->elements = memory + sizeof(Queue);
	return queue;
}

void queue_free(Queue *queue) {
	FreeSem(&(queue->space));
	FreeSem(&(queue->items));
	FreeSem(&(queue->mutex));
	free(queue->elements);
}

void sharedqueue_free(Queue *queue) {
	FreeSem(&(queue->space));
	FreeSem(&(queue->items));
	FreeSem(&(queue->mutex));
	munmap((void *)queue, sizeof(Queue) + queue->size * queue->elem_size);
}

void queue_put(Queue *queue, void *value) {
	P(&(queue->space));
	P(&(queue->mutex));
	memcpy(queue->elements + queue->tail, value, queue->elem_size);
	queue->tail += queue->elem_size;
	if (queue->tail >= queue->size_bytes)
		queue->tail = 0;
	V(&(queue->mutex));
	V(&(queue->items));
}

void queue_get(Queue *queue, void *result) {
	P(&(queue->items));
	P(&(queue->mutex));
	memcpy(result, queue->elements + queue->head, queue->elem_size);
	queue->head += queue->elem_size;
	if (queue->head >= queue->size_bytes)
		queue->head = 0;
	V(&(queue->mutex));
	V(&(queue->space));
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

	memset(memory, 0, bufferpool_get_size(buffercount, buffersize));

	SharedBufferPool *bp = (SharedBufferPool *)memory;
	assert(InitSem(&(bp->free_chunks), buffercount));
	assert(InitSem(&(bp->mutex), 1));
	bp->free_chunks.value = buffercount;
	
	bp->count = buffercount;
	bp->size = buffersize;
	bp->inuse = (char *)(memory + sizeof(SharedBufferPool));
	bp->chunks = (void *)(memory + sizeof(SharedBufferPool) + sizeof(char) * buffercount);

	return bp;
}

void sharedbuffpool_free(SharedBufferPool *bp) {
	munmap((void *)bp, bufferpool_get_size(bp->count, bp->size));
}

void *bp_getchunk(SharedBufferPool *bp) {
	P(&(bp->free_chunks));
	P(&(bp->mutex));
	int i;
	for (i = 0; i < bp->count; ++i) {
		if (!bp->inuse[i]) break;
	}
	void *chunk = bp->chunks + (i * bp->size);
	bp->inuse[i] = 1;
	V(&(bp->mutex));
	return chunk;
};

int bp_freechunk(SharedBufferPool *bp, void *chunk) {
	P(&(bp->mutex));
	long chunk_offset = chunk - bp->chunks;
	bp->inuse[chunk_offset / bp->size] = 0;
	V(&(bp->mutex));
	V(&(bp->free_chunks));
}

