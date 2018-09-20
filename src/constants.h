#ifndef __CONSTANTS_H
#define __CONSTANTS_H

// OPTIONS FOR LAMBDA INVOCATIONS

#define LAMBDA_API_ENDPOINT "http://cspot.lastpengu.in:8080"
#define S3_API_ENDPOINT "http://cspot.lastpengu.in:8081"

#define PARALLELISM_SUPPORT 4 

#define FAKE_REGION "us-west-1"

#define WPTHREAD_COUNT PARALLELISM_SUPPORT
#define WORKER_QUEUE_DEPTH (PARALLELISM_SUPPORT * 2)
#define OBJECT_POOL_SIZE (PARALLELISM_SUPPORT * 2)
#define RESULT_WOOF_COUNT PARALLELISM_SUPPORT

#define CALL_WOOF_NAME "lambda.woof"
#define CALL_WOOF_EL_SIZE (16 * 1024)
#define CALL_WOOF_QUEUE_DEPTH (PARALLELISM_SUPPORT * 2)
#define RESULT_WOOF_EL_SIZE CALL_WOOF_EL_SIZE

#define MAX_WOOF_EL_SIZE CALL_WOOF_EL_SIZE

#endif
