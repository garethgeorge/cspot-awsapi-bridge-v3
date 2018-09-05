#ifndef __CONSTANTS_H
#define __CONSTANTS_H

#define PORT 80 // port for the API client
#define WORKER_QUEUE_DEPTH 32
#define RESULT_WOOF_COUNT 8

#define CALL_WOOF_NAME "lambda.woof"
#define CALL_WOOF_EL_SIZE 16 * 1024
#define CALL_WOOF_QUEUE_DEPTH 16
#define RESULT_WOOF_EL_SIZE CALL_WOOF_EL_SIZE

#define MAX_WOOF_EL_SIZE CALL_WOOF_EL_SIZE

#endif
