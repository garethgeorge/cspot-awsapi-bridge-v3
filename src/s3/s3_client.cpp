#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <linux/limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <mutex>
#include <memory>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <csignal>
#include <ctime>
#include <fstream>

#include <src/constants.h>
#include <3rdparty/base64.h>
#include <3rdparty/rapidxml/rapidxml.hpp>
#include <lib/utility.h>
#include <lib/wp.h>
#include <lib/aws.hpp>
#include <lib/fsutil.hpp>
#include <lib/helpers.hpp>

#ifdef __cplusplus
extern "C" {
	#include <ulfius.h> // rest API library
	#include <jansson.h> // json library
}
#else 
#include <ulfius.h> // rest API library
#include <jansson.h> // json library
#endif

extern "C" {
	#define LOG_H // prevent this header from loading, it causes problems
	#include "woofc.h"
	#include "woofc-host.h"
}

#define MAX_PATH_LENGTH 255
#define PORT 8081

using namespace rapidxml;
using namespace std;

struct S3Object {
	uint64_t size;
	char path[MAX_PATH_LENGTH];
	char payload[1024 * 1024];
};

const vector<const char *> eventTypes = {
	"s3:ObjectCreated:Put",
	"s3:ObjectCreated:Post",
	"s3:ObjectCreated:Copy",
	"s3:ObjectRemoved:Delete"
};

class S3NotificationConfiguration {
private:

	unique_ptr<xml_document<>> readAndParseFile(std::istream& stream) {
		std::string str((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

		fprintf(stdout, "read from disk: %s\n", str.c_str());

		unique_ptr<xml_document<>> doc = make_unique<xml_document<>>();
		doc->parse<0>((char *)str.c_str());
		return doc;
	}

public:
	struct EventHandler {
		std::string lambdaArn;

		virtual void handleEvent(const std::string &eventName, json_t *event) {
			// TODO: change this to use RabbitMQ to guarantee the delivery and execution
			// of event notifications

			fprintf(stdout, "handling event %s by invoking lambda %s\n", eventName.c_str(), lambdaArn.c_str());
			
			const char *dump = json_dumps(event, 0);
			const std::string lambdaName = getNameFromLambdaArn(this->lambdaArn.c_str());

			struct _u_map req_headers;
			u_map_init(&req_headers);
			u_map_put(&req_headers, "Content-Type", "application/json");
			u_map_put(&req_headers, "X-Amz-Invocation-Type", "Event");
		
			struct _u_request request;
			ulfius_init_request(&request);
			request.http_verb = strdup("POST");
			std::string http_url = (
				std::string(LAMBDA_API_ENDPOINT "/2015-03-31/functions/") + 
				lambdaName.c_str() + std::string("/invocations")).c_str();
			request.http_url = strdup(http_url.c_str());
			request.timeout = 30;
			u_map_copy_into(request.map_header, &req_headers);

			request.binary_body = (char *)dump;
			request.binary_body_length = strlen(dump);

			struct _u_response response;
			fprintf(stdout, "Making HTTP request to URL %s\n", request.http_url);
			ulfius_init_response(&response);
			int retval = ulfius_send_http_request(&request, &response);
			if (retval == U_OK) {
				fprintf(stdout, "The HTTP response was U_OK, successful\n");

				char *response_body = (char *) malloc(response.binary_body_length + 10);
				strncpy(response_body, (char *)response.binary_body, response.binary_body_length);
				response_body[response.binary_body_length] = '\0';
				fprintf(stdout, "RESPONSE BODY: %s\n", response_body);
				free(response_body);
			} else {
				fprintf(stderr, "Failed to invoke the handler lambda subscribed to this event\n");
			}
			ulfius_clean_response(&response);

			
			free((void *)dump);
			u_map_clean(&req_headers);
		}

		virtual ~EventHandler() { };
	};

	unordered_map<
		string, 
		vector<unique_ptr<EventHandler>>> handlerMap;

	S3NotificationConfiguration(std::istream& stream) : S3NotificationConfiguration(*readAndParseFile(stream)) {

	}

	template<typename T>
	S3NotificationConfiguration(xml_document<T>& xmldocument) {
		xml_node<> *nodeNotifConfig = xmldocument.first_node("NotificationConfiguration");
		if (nodeNotifConfig == nullptr) 
			throw AWSError(500, "Did not find the NotificationConfiguration node in the xml document");
		
		for (xml_node<> *cloudFuncConfig = nodeNotifConfig->first_node("CloudFunctionConfiguration");
			cloudFuncConfig != nullptr; 
			cloudFuncConfig = cloudFuncConfig->next_sibling("CloudFunctionConfiguration")) {
			this->loadCloudFunctionConfiguration(*cloudFuncConfig);
		}
	}

	void notify(const std::string& eventName, json_t *event) {
		if (handlerMap.find(eventName) != handlerMap.end()) {
			for (const auto& handler : handlerMap[eventName]) {
				handler->handleEvent(eventName, event);
			}
		}
	}

private:
	// list of allowable events found here: https://docs.aws.amazon.com/AmazonS3/latest/dev/NotificationHowTo.html

	template<typename T>
	void loadCloudFunctionConfiguration(xml_node<T>& cloudFuncConfig) {
		fprintf(stdout, "loading cloud function configuration from XML\n");

		xml_node<T>* funcArnNode = cloudFuncConfig.first_node("CloudFunction");
		if (funcArnNode == nullptr) 
			throw AWSError(500, "CloudFunctionConfiguration did not contain a 'CloudFunction' node specifying the function arn to invoke");

		const char *funcArn = funcArnNode->value();

		for (xml_node<T>* node = cloudFuncConfig.first_node("Event"); node != nullptr; node = node->next_sibling("Event")) {
			const char *event = node->value();
			int event_len = strlen(event);

			// we just ignore the last character if its a wild card
			if (event[event_len - 1] == '*')
				event_len--;
			
			for (const char *eventType : eventTypes) {
				if (strncmp(event, eventType, event_len) == 0) {
					fprintf(stdout, "\tadded handler '%s' -> invoke '%s'\n", eventType, funcArn);
					unique_ptr<EventHandler> handler = make_unique<EventHandler>();
					handler->lambdaArn = funcArn;
					
					if (this->handlerMap.find(eventType) == this->handlerMap.end()) {
						this->handlerMap[eventType] = vector<unique_ptr<EventHandler>>();
					}
					this->handlerMap[eventType].push_back(std::move(handler));
				}
			}
		}
	}
};


/*
<NotificationConfiguration xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
	<CloudFunctionConfiguration>
		<CloudFunction>mylambdafunc</CloudFunction>
		<Event>s3:ObjectCreated:*</Event>
		<Filter>
			<S3Key>
				<FilterRule>
					<Name>prefix</Name>
					<Value>test</Value>
				</FilterRule>
			</S3Key>
		</Filter>
	</CloudFunctionConfiguration>
</NotificationConfiguration>
*/

std::mutex io_lock;
std::unique_ptr<S3NotificationConfiguration> notifConfig = nullptr;

int callback_s3_put(const struct _u_request * httprequest, struct _u_response * httpresponse, void * user_data) {
	fprintf(stdout, "\n\nPUT REQUEST: callback_s3_put\n");

	// figure out the raw path the request specified 
	std::string raw_path = *(httprequest->http_url) == '/' ? httprequest->http_url + 1 : httprequest->http_url;
	char key[255 * 2];

	// figure out the bucketname
	std::size_t slashPos = raw_path.find('/', 0);
	if (slashPos == std::string::npos) {
		throw AWSError(500, "Object path not specified, only found bucket name");
	}
	std::string bucket_name = raw_path.substr(0, slashPos);
	std::string object_key = raw_path.substr(slashPos + 1);
	
	// make the key the base64 encoded path so that we escape symbols and all that
	if (Base64encode_len(raw_path.length()) >= MAX_PATH_LENGTH) {
		throw AWSError(500, "path length too long");
	}
	Base64encode(key, raw_path.c_str(), raw_path.length());
	
	fprintf(stdout, "putting as key %s (originally %s) in bucket %s\n", key, raw_path.c_str(), bucket_name.c_str());

	// store the payload in a new WooF at that location
	size_t payload_size = httprequest->binary_body_length;
	const char *payload = (const char *)httprequest->binary_body;

	fprintf(stdout, "payload: (%lu)\n%s\n", (unsigned long)payload_size, payload);

	if (payload_size > sizeof(S3Object().payload)) {
		throw AWSError(500, "Payload too large for the S3 object");
	}

	{
		std::lock_guard<std::mutex> g(io_lock);

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
	}
	ulfius_set_string_body_response(httpresponse, 200, "");

	// https://docs.aws.amazon.com/AmazonS3/latest/dev/notification-content-structure.html
	if (notifConfig != nullptr) {
		json_t *event_full = json_object();

		json_t *event_records = json_array();
		json_object_set(event_full, "Records", event_records);

		json_t *event = json_object();
		json_array_append(event_records, event);
		json_decref(event_records);

		// have put the event object in the event array
		json_object_set_new(event, "eventVersion", json_string("2.0"));
		json_object_set_new(event, "eventSource", json_string("aws:s3"));
		json_object_set_new(event, "awsRegion", json_string(FAKE_REGION));
		json_object_set_new(event, "eventName", json_string("s3:ObjectCreated:Put"));
		{
			// https://stackoverflow.com/questions/9527960/how-do-i-construct-an-iso-8601-datetime-in-c
			time_t now;
			time(&now);
			char buf[sizeof "2011-10-08T07:07:09Z"];
			strftime(buf, sizeof buf, "%FT%TZ", gmtime(&now));
			json_object_set_new(event, "eventTime", json_string(buf));
		}

		json_t *event_s3 = json_object();
		json_object_set(event, "s3", event_s3);

		json_object_set_new(event_s3, "s3SchemaVersion", json_string("1.0"));
		json_object_set_new(event_s3, "bucket", json_object());
		
		json_object_set_new(json_object_get(event_s3, "bucket"), "name", json_string(bucket_name.c_str()));
		json_object_set_new(json_object_get(event_s3, "bucket"), "arn", 
			json_string(getArnForBucketName(bucket_name.c_str()).c_str())
		);

		json_object_set_new(event_s3, "object", json_object());
		json_object_set_new(json_object_get(event_s3, "object"), "key", json_string(object_key.c_str()));
		json_object_set_new(json_object_get(event_s3, "object"), "size", json_integer(payload_size));

		json_decref(event_s3);
		json_decref(event);

		fprintf(stdout, "JSON EVENT NOTIFICATION: \n");
		json_dumpf(event_full, stdout, JSON_INDENT(2));
		fprintf(stdout, "\n");

		// dispatch the notification
		notifConfig->notify("s3:ObjectCreated:Put", event);
		json_decref(event_full);
	}


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
	std::lock_guard<std::mutex> g(io_lock);

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
	fprintf(stdout, "%s\n", payload);

	std::lock_guard<std::mutex> g(io_lock);
	try {

		xml_document<> doc;
		std::string payload_copy = payload;
		doc.parse<0>((char *)payload_copy.c_str());

		newConfig = make_unique<S3NotificationConfiguration>(doc);

		fprintf(stdout, "successfully created new config from parsed XML\n");

		notifConfig = std::move(newConfig);

		FILE *notifConfigFile = fopen("notification-config.xml", "wb");
		if (notifConfigFile == NULL) {
			fprintf(stderr, "Fatal error: failed to write notification-config.xml\n");
			throw AWSError(500, "ServiceException");
		}

		if (fwrite(payload, httprequest->binary_body_length, 1, notifConfigFile) < 0) {
			fclose(notifConfigFile);
			fprintf(stderr, "Fatal error: failed to write notification-config.xml\n");
			throw AWSError(500, "ServiceException");
		}
		fclose(notifConfigFile);
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

	// Load the xml notification config
	std::ifstream notifConfigFile("notification-config.xml");
	if (notifConfigFile.is_open()) {
		fprintf(stdout, "found notification-config on disk\n");
		try {
			notifConfig = make_unique<S3NotificationConfiguration>(notifConfigFile);
		} catch (const AWSError &e) {
			fprintf(stderr, "FAILED TO LOAD notification-config.xml FROM DISK, ENCOUNTERED FORMAT ERROR\n");
		} catch (const parse_error &e) {
			fprintf(stderr, "FAILED TO LOAD notification-config.xml FROM DISK, ENCOUNTERED PARSE ERROR\n");
		}
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
	WooFInit();

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
