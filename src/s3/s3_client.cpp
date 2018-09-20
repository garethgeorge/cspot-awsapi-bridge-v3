#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <linux/limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <mutex>
#include <cassert>
#include <memory>
#include <unordered_map>

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
#include <3rdparty/rapidxml/rapidxml.hpp>
#include <3rdparty/rapidxml/rapidxml_utils.hpp>
#include <3rdparty/rapidxml/rapidxml_print.hpp>
#include <lib/utility.h>
#include <lib/wp.h>
#include <lib/awserror.hpp>
#include <lib/fsutil.hpp>
#include <lib/helpers.hpp>

extern "C" {
	#define LOG_H // prevent this header from loading, it causes problems
	#include "woofc.h"
	#include "woofc-host.h"
}

#define MAX_PATH_LENGTH 255
#define PORT 8081

using namespace rapidxml;

struct S3Object {
	uint64_t size;
	char path[MAX_PATH_LENGTH];
	char payload[1024 * 1024];
};

struct S3NotificationConfiguration {

	struct EventHandler {
		std::string filter;
	};

	std::vector<EventHandler> event_handlers;

	template<typename T>
	S3NotificationConfiguration(xml_document<T>& xmldocument) {
		
	}
};

std::mutex lock;
std::unique_ptr<S3NotificationConfiguration> notifConfig = nullptr;

int callback_s3_put(const struct _u_request * httprequest, struct _u_response * httpresponse, void * user_data) {
	fprintf(stdout, "\n\nPUT REQUEST: callback_s3_put\n");

	// figure out the raw path the request specified 
	char *raw_path = httprequest->http_url;
	int raw_path_len = strlen(raw_path);
	char key[255 * 2];

	// make the key the base64 encoded path so that we escape symbols and all that
	if (Base64encode_len(raw_path_len) >= MAX_PATH_LENGTH) {
		throw AWSError(500, "path length too long");
	}
	Base64encode(key, raw_path, raw_path_len);
	
	fprintf(stdout, "putting as key %s (originally %s)\n", key, raw_path);

	// store the payload in a new WooF at that location
	size_t payload_size = httprequest->binary_body_length;
	const char *payload = (const char *)httprequest->binary_body;

	fprintf(stdout, "payload: (%lu)\n%s\n", (unsigned long)payload_size, payload);

	if (payload_size > sizeof(S3Object().payload)) {
		throw AWSError(500, "Payload too large for the S3 object");
	}

	std::lock_guard<std::mutex> g(lock);
	struct stat st = {0};
	if (stat(key, &st) == -1) {
		if (WooFCreate(key, sizeof(S3Object), 1) != 1) {
			throw AWSError(500, "failed to create the WooF for the object");
		}
	}

	S3Object *obj = new S3Object;
	memset((void *)obj, 0, sizeof(obj));
	obj->size = payload_size;
	memset(obj->payload, 0, sizeof(obj->payload));
	memcpy(obj->payload, payload, payload_size);
	if (WooFInvalid(WooFPut(key, NULL, (void *)obj))) {
		delete obj;
		throw AWSError(500, "Failed to write the object into WooF");
	}
	delete obj;

	ulfius_set_string_body_response(httpresponse, 200, "");

	return U_CALLBACK_CONTINUE;
}

int callback_s3_get(const struct _u_request * httprequest, struct _u_response * httpresponse, void * user_data) {
	fprintf(stdout, "\n\nPUT REQUEST: callback_s3_get\n");

	// figure out the raw path the request specified 
	char *raw_path = httprequest->http_url;
	int raw_path_len = strlen(raw_path);
	char key[255 * 2];

	// make the key the base64 encoded path so that we escape symbols and all that
	if (Base64encode_len(raw_path_len) >= MAX_PATH_LENGTH) {
		throw AWSError(500, "path length too long");
	}
	Base64encode(key, raw_path, raw_path_len);
	
	fprintf(stdout, "getting with key %s (originally %s)\n", key, raw_path);

	// read the file from the disk
	std::lock_guard<std::mutex> g(lock);

	struct stat st = {0};
	if (stat(key, &st) == -1) {
		fprintf(stderr, "Fatal error: no such key %s (originally %s)\n", key, raw_path);
		throw AWSError(404, "no such key");
	}

	unsigned long seqno = WooFGetLatestSeqno(key);

	fprintf(stdout, "Got latest seqno %lu\n", seqno);

	S3Object *obj = new S3Object;
	if (WooFGet(key, (void *)obj, seqno) != 1) {
		delete obj;
		throw AWSError(500, "ServiceException");
	}
	
	ulfius_set_binary_body_response(httpresponse, 200, obj->payload, obj->size);
	delete obj;

	return U_CALLBACK_CONTINUE;
}

int callback_s3_request(const struct _u_request * httprequest, struct _u_response * httpresponse, void * user_data) {
	fprintf(stdout, "\n\nREQUEST TO S3 API URL: %s\n", httprequest->http_url);
	ulfius_set_string_body_response(httpresponse, 200, "success\n");

	try {
		if (strcmp(httprequest->http_verb, "PUT") == 0) {
		return callback_s3_put(httprequest, httpresponse, user_data);
		} else if (strcmp(httprequest->http_verb, "GET") == 0) {
			return callback_s3_get(httprequest, httpresponse, user_data);
		}
	} catch (const AWSError &e) {
		fprintf(stderr, "Caught error: %s\n", e.msg.c_str());
		ulfius_set_string_body_response(httpresponse, e.error_code, e.msg.c_str());
		return U_CALLBACK_CONTINUE;
	}
	
	fprintf(stdout, "UNRECOGNIZED HTTPVERB %s\n", httprequest->http_verb);

	return U_CALLBACK_CONTINUE;
}

int callback_s3_put_notification(const struct _u_request * httprequest, struct _u_response * httpresponse, void * user_data) {
	fprintf(stdout, "\n\nREQUEST PUT NOTIFICATION: %s\n", httprequest->http_url);

	const char *payload = (const char *)httprequest->binary_body;

	std::unique_ptr<S3NotificationConfiguration> newConfig;
	try {

		xml_document<> doc;
		doc.parse<0>((char *)payload);

		newConfig = make_unique<S3NotificationConfiguration>(doc);

		fprintf(stdout, "successfully created new config from parsed XML\n");
	} catch (const parse_error &e) {
		fprintf(stderr, "Fatal error: failed to parse XML\n");
		ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
		return U_CALLBACK_CONTINUE;
	} catch (const AWSError &e) { 
		fprintf(stderr, "Caught error: %s\n", e.msg.c_str());
		ulfius_set_string_body_response(httpresponse, e.error_code, e.msg.c_str());
		return U_CALLBACK_CONTINUE;
	}

	return U_CALLBACK_CONTINUE;
}

/*
/myBucket?notification
Host: localhost:8081
Accept-Encoding: identity
Content-Length: 236
User-Agent: aws-cli/1.16.7 Python/2.7.5 Linux/3.10.0-862.11.6.el7.x86_64 botocore/1.11.7

<NotificationConfiguration xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
	<CloudFunctionConfiguration>
		<CloudFunction>mylambdafunc</CloudFunction>
		<Event>s3:ObjectCreated:*</Event>
	</CloudFunctionConfiguration>
</NotificationConfiguration>
*/

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

int main(int argc, char **argv) {
	/*
		start the web server
	*/
	struct _u_instance instance;

	// Setup the functions directory
	struct stat st = {0};
	if (stat("./s3objects", &st) == -1) {
		fprintf(stdout, "Created s3objects directory.\n");
		mkdir("./s3objects", 0777);
		
		fprintf(stdout, "copy binaries into new ./s3objects directory\n");
		if (copy_file("./s3objects/woofc-namespace-platform", "./woofc-namespace-platform", 777) < 0 || 
			copy_file("./s3objects/woofc-container", "./woofc-container", 777) < 0) {
			fprintf(stderr, "Fatal error: failed to copy one of the binaries into the ./s3objects directory\n");
			exit(1);
		}
	}

	// Fork the WooFCNamespacePlatform
	if (chdir("./s3objects") != 0) {
		fprintf(stdout, "Fatal error: failed to change directory into the s3objects dir\n");
		return 1;
	}

	fprintf(stdout, "forking woofcnamespace platform\n");
	
	int woofcnamespaceplatform_pid = fork();
	if (woofcnamespaceplatform_pid == 0) {
		fprintf(stdout, "Child process: successfully forked\n");
		execl("./woofc-namespace-platform", "./woofc-namespace-platform", "-m", "4", "-M", "4", NULL);
		fprintf(stdout, "Child process: FAILURE TO EXECL\n");
		exit(1);
	}

	fprintf(stdout, "sleep 1 second then WooFInit\n");

	sleep(1);
	assert(WooFInit() == 1);

	// Initialize instance with the port number
	if (ulfius_init_instance(&instance, PORT, NULL, NULL) != U_OK) {
		fprintf(stderr, "Error ulfius_init_instance, abort\n");
		return(1);
	}


	// NOTE: we do not require that buckets be explicitly created, you can just start using them 
	// we will however implement a stubbed API or something eventually to allow compatibility
	// b/c of some limitation we can't set the default endpoint without also adding a regular endpoint
	ulfius_add_endpoint_by_val(&instance, "PUT", "/", "/:bucket?notification", 0, &callback_s3_put_notification, NULL);
	ulfius_set_default_endpoint(&instance, callback_s3_request, NULL);

	// TODO: implement https://github.com/awslabs/lambda-refarch-mapreduce/blob/master/src/python/lambdautils.py#L88

	// Start the framework
	signal(SIGINT, sig_handler);

	if (ulfius_start_framework(&instance) == U_OK) {
		printf("Start framework on port %d\n", instance.port);
		fgetc(stdin); // block until input from user
	} else {
		fprintf(stderr, "Error starting framework on port %d\n", instance.port);
	}
	printf("End framework\n");

	ulfius_stop_framework(&instance);
	ulfius_clean_instance(&instance);

	// PyMem_RawFree(program);
	return 0;
}
