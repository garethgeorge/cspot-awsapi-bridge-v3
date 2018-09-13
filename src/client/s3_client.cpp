#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <linux/limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pthread.h>
#include <mutex>
#include <unordered_map>

#define PORT 8081

#ifdef __cplusplus
extern "C" {
	#include <ulfius.h> // rest API library
	#include <jansson.h> // json library
}
#else 
#include <ulfius.h> // rest API library
#include <jansson.h> // json library
#endif


#include <src/constants.h>
#include <3rdparty/base64.h>
#include <lib/utility.h>
#include <lib/wp.h>

extern "C" {
	#define LOG_H // prevent this header from loading
	#include <woofc.h>
}

int callback_s3_put(const struct _u_request * httprequest, struct _u_response * httpresponse, void * user_data) {
	ulfius_set_string_body_response(httpresponse, 200, "success\n");
	
	/*
		proper response values

		HTTP/1.1 100 Continue

		HTTP/1.1 200 OK
		x-amz-id-2: LriYPLdmOdAiIfgSm/F1YsViT1LW94/xUQxMsF7xiEb1a0wiIOIxl+zbwZ163pt7
		x-amz-request-id: 0A49CE4060975EAC
		Date: Wed, 12 Oct 2009 17:50:00 GMT
		ETag: "1b2cf535f27731c974343645a3985328"
		Content-Length: 0
		Connection: close
		Server: AmazonS3
	*/

	return U_CALLBACK_CONTINUE;
}

int callback_s3_get(const struct _u_request * httprequest, struct _u_response * httpresponse, void * user_data) {
	ulfius_set_string_body_response(httpresponse, 200, "success\n");
	
	/*
		TODO: figure out proper response object format
	*/

	return U_CALLBACK_CONTINUE;
}

void sig_handler(int sig) {
	switch (sig) {
	case SIGINT:
		fprintf(stderr, "\n\nCAUGHT SIGNAL: shutting down\n");
		// TODO: kill any child processes we spawn
		abort();
	default:
		fprintf(stderr, "Unhandled termination signal encountered\n");
		abort();
	}
}

int main(int argc, char **argv)
{
	/*
		start the web server
	*/
	struct _u_instance instance;

	// Setup the functions directory
	struct stat st = {0};
	if (stat("./s3objects", &st) == -1) {
		fprintf(stdout, "Created s3objects directory.\n");
		mkdir("./s3objects", 0700);
	}

	// Initialize instance with the port number
	if (ulfius_init_instance(&instance, PORT, NULL, NULL) != U_OK) {
		fprintf(stderr, "Error ulfius_init_instance, abort\n");
		return(1);
	}

	ulfius_add_endpoint_by_val(&instance, "POST", "/2015-03-31/", "/:bucket/:key", 0, &callback_s3_put, NULL);
	ulfius_add_endpoint_by_val(&instance, "GET", "/2015-03-31/", "/:bucket/:key", 0, &callback_s3_get, NULL);

	// Start the framework
	signal(SIGINT, sig_handler);

	if (ulfius_start_framework(&instance) == U_OK) {
		printf("Start framework on port %d\n", instance.port);
		fgetc(stdin); // block until input from user
	} else {
		fprintf(stderr, "Error starting framework\n");
	}
	printf("End framework\n");

	ulfius_stop_framework(&instance);
	ulfius_clean_instance(&instance);

	// PyMem_RawFree(program);
	return 0;
}
