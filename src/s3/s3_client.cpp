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

#include "notification_helpers.hpp"

#define MAX_PATH_LENGTH 255
#define PORT 8081

using namespace std;

struct S3Object {
	uint64_t size;
	char path[MAX_PATH_LENGTH];
	char payload[1024 * 1024];
};

class S3Bucket {
public:
	std::string bucket_name;
	std::unique_ptr<S3NotificationConfiguration> notifConfig = nullptr;

	// must be acquired for any operation on the bucket
	std::mutex bucketLock;
private:

	static std::unordered_map<std::string, std::unique_ptr<S3Bucket>> buckets;
	
	S3Bucket(const std::string &bucket_name) {
		this->bucket_name = bucket_name;
	}

public:
	static S3Bucket &getOrCreateS3Bucket(const std::string& bucket_name) {
		if (buckets.find(bucket_name) != buckets.end()) {
			return *(buckets[bucket_name]);
		}
		
		std::unique_ptr<S3Bucket> bucket = std::unique_ptr<S3Bucket>(new S3Bucket(bucket_name));
		bucket->loadNotifConfigFromDisk();
		buckets[bucket_name] = std::move(bucket);
		return *(buckets[bucket_name]);
	}

	void loadNotifConfigFromDisk() {
		char directory[PATH_MAX];
		snprintf(directory, sizeof(directory) - 1, 
			"./%s.xml", Base64encode(this->bucket_name).c_str());

		std::ifstream notifConfigFile(directory);
		if (!notifConfigFile.fail()) {
			fprintf(stdout, "found notification-config on disk: %s\n", directory);
			try {
				this->notifConfig = make_unique<S3NotificationConfiguration>(notifConfigFile);
			} catch (const parse_error &e) {
				fprintf(stderr, "Fatal error: notification configuration found on disk was corrupted. Failed to parse it\n");
				throw AWSError(500, "ServiceException").setDetails("notification configuration found on disk was corrupted. Failed to parse it.");
			}
		} else {
			fprintf(stdout, "File %s did not exist, there are no notifications configured for this bucket\n", directory);
		}
	}

	// std::string is copied intentionally here, I believe that doc.parse 
	// takes full ownership of the string that it is passed.
	void setNotifConfig(const std::string& notifConfig) {
		fprintf(stdout, "set notification config: %s\n", notifConfig.c_str());

		xml_document<> doc;
		std::unique_ptr<char[]> data(new char[notifConfig.length() + 1]);
		memcpy((void *)data.get(), (void *)notifConfig.c_str(), notifConfig.length());
		data[notifConfig.length()] = 0;

		try {
			doc.parse<0>((char *)data.get());
		} catch (const parse_error &e) {
			fprintf(stderr, "Fatal error: bad XML in notification configuration\n");
			throw AWSError(500, "ServiceException")
				.setDetails("bad XML in notification configuration.");
		}

		char directory[PATH_MAX];
		snprintf(directory, sizeof(directory) - 1, 
			"./%s.xml", Base64encode(this->bucket_name).c_str());

		FILE *fp = fopen(directory, "w");
		if (fp == NULL) 
			throw AWSError(500, "ServiceException")
				.setDetails("failed to write the new notification configuration to the disk");

		if (fwrite(notifConfig.c_str(), sizeof(char), notifConfig.length(), fp) == 0) {
			fclose(fp);
			throw AWSError(500, "ServiceException").setDetails("error writing file");
		}
		fclose(fp);
		
		this->notifConfig = make_unique<S3NotificationConfiguration>(doc);
	}
};

std::unordered_map<std::string, std::unique_ptr<S3Bucket>> S3Bucket::buckets;

std::mutex io_lock;

class S3Key {
private:
	S3Bucket *s3bucket;
	std::string key;
	std::string path;

public:
	S3Key(const std::string& path) {
		if (path[0] == '/')
			this->path = path.c_str() + 1;
		else 
			this->path = path.c_str();
		
		std::size_t slashPos = this->path.find('/', 0);
		
		std::string bucket_name;
		if (slashPos != std::string::npos) {
			bucket_name = this->path.substr(0, slashPos);
			this->key = this->path.substr(slashPos + 1);
		} else
			bucket_name = this->path;
		this->s3bucket = &(S3Bucket::getOrCreateS3Bucket(bucket_name));
	}

	inline bool haveKey() {
		return key.length() != 0;
	}

	inline const std::string& getKey() {
		return this->key;
	}

	inline const std::string& getBucket() {
		// s3bucket is guarantied not null
		return this->s3bucket->bucket_name;
	}

	inline const std::string& getRawPath() {
		return this->path;
	}

	// get path returns a full path to the object that should be created 
	inline const std::string getStoragePath() {
		return Base64encode(this->s3bucket->bucket_name + "/" + this->key);
	}

	S3Bucket &getS3Bucket() {
		return *(this->s3bucket);
	}
};


int callback_s3_put(const struct _u_request * httprequest, struct _u_response * httpresponse, void * user_data) {
	fprintf(stdout, "\n\nPUT REQUEST: callback_s3_put\n");

	// figure out the bucketname
	S3Key key(httprequest->http_url);
	S3Bucket &bucket = key.getS3Bucket();

	std::lock_guard<std::mutex> g1(bucket.bucketLock);

	if (!key.haveKey()) {
		throw AWSError(500, "Object path not specified, only found bucket name");
	}
	
	std::string b64key = key.getStoragePath(); // base64bucket/base64key is the structure

	fprintf(stdout, "putting as key %s (originally %s) in bucket %s\n", 
		b64key.c_str(), key.getKey().c_str(), key.getBucket().c_str());

	// store the payload in a new WooF at that location
	size_t payload_size = httprequest->binary_body_length;
	const char *payload = (const char *)httprequest->binary_body;
	
	if (payload_size > sizeof(S3Object().payload)) {
		throw AWSError(500, "Payload too large for the S3 object");
	}
	
	unique_ptr<S3Object> obj = make_unique<S3Object>();
	memset((void *)(obj.get()), 0, sizeof(S3Object));
	obj->size = payload_size;
	memcpy((void *)obj->payload, (void *)payload, payload_size);
	fprintf(stdout, "payload: (%lu)\n%s\n", (unsigned long)obj->size, obj->payload);
	{
		struct stat st = {0};
		if (stat(b64key.c_str(), &st) == -1) {
			if (WooFCreate((char *)b64key.c_str(), sizeof(S3Object), 1) != 1) {
				throw AWSError(500, "failed to create the WooF for the object");
			}
		}

		if (WooFInvalid(WooFPut((char *)b64key.c_str(), NULL, (void *)(obj.get())))) {
			throw AWSError(500, "Failed to write the object into WooF");
		}
	}

	ulfius_set_string_body_response(httpresponse, 200, "");

	if (bucket.notifConfig != nullptr) {
		// https://docs.aws.amazon.com/AmazonS3/latest/dev/notification-content-structure.html
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
			char buf[sizeof "2000-00-00T00:00:00Z"];
			strftime(buf, sizeof buf, "%FT%TZ", gmtime(&now));
			json_object_set_new(event, "eventTime", json_string(buf));
		}

		json_t *event_s3 = json_object();
		json_object_set(event, "s3", event_s3);

		json_object_set_new(event_s3, "s3SchemaVersion", json_string("1.0"));
		json_object_set_new(event_s3, "bucket", json_object());
		
		json_object_set_new(json_object_get(event_s3, "bucket"), "name", json_string(key.getBucket().c_str()));
		json_object_set_new(json_object_get(event_s3, "bucket"), "arn", 
			json_string(getArnForBucketName(key.getBucket().c_str()).c_str())
		);

		json_object_set_new(event_s3, "object", json_object());
		json_object_set_new(json_object_get(event_s3, "object"), "key", json_string(key.getKey().c_str()));
		json_object_set_new(json_object_get(event_s3, "object"), "size", json_integer(payload_size));

		json_decref(event_s3);
		json_decref(event);

		fprintf(stdout, "JSON EVENT NOTIFICATION: \n");
		json_dumpf(event_full, stdout, JSON_INDENT(2));
		fprintf(stdout, "\n");

		// dispatch the notification
		bucket.notifConfig->notify("s3:ObjectCreated:Put", event_full);
		json_decref(event_full);
	}

	return U_CALLBACK_CONTINUE;
}

int callback_s3_get(const struct _u_request * httprequest, struct _u_response * httpresponse, void * user_data) {
	fprintf(stdout, "\n\nPUT REQUEST: callback_s3_get\n");

	// figure out the raw path the request specified 
	S3Key key(httprequest->http_url);
	std::string b64key = key.getStoragePath(); 
	
	fprintf(stdout, "getting with key %s (originally %s)\n", b64key.c_str(), key.getRawPath().c_str());
	std::lock_guard<std::mutex> g(key.getS3Bucket().bucketLock);

	// read the file from the disk
	// struct stat st = {0};
	// if (stat(b64key.c_str(), &st) == -1) {
	// 	fprintf(stderr, "Fatal error: no such key %s (originally %s)\n", b64key.c_str(), key.getRawPath().c_str());
	// 	throw AWSError(404, "no such key");
	// }

	unsigned long seqno = WooFGetLatestSeqno((char *)b64key.c_str());

	if (WooFInvalid(seqno)) {
		throw AWSError(404, "Not Found");
	}

	fprintf(stdout, "Got latest seqno %lu\n", seqno);

	std::unique_ptr<S3Object> obj(new S3Object);
	if (WooFGet((char *)b64key.c_str(), (void *)obj.get(), seqno) != 1) {
		throw AWSError(500, "ServiceException");
	}
	
	fprintf(stdout, "The result from the WooF was: %s\n", std::string(obj->payload, obj->size).c_str());

	ulfius_set_binary_body_response(httpresponse, 200, obj->payload, obj->size);

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

	fprintf(stdout, "\n\nREQUEST PUT BUCKET/NOTIFICATION: %s\n", httprequest->http_url);
	if (strstr(httprequest->http_url, "?notification") != NULL) {
		fprintf(stdout, "determined that it is infact a put notification request\n");
		// if we find ?notification then it was a request to put a notification
		// otherwise we assume it is an attempt to create a bucket
		try {
			const char *bucket_name = u_map_get(httprequest->map_url, "bucket");
			fprintf(stdout, "\tNOTIFICATION CONFIG FOR BUCKET %s\n", bucket_name);

			if (bucket_name == NULL) {
				fprintf(stderr, "Fatal error: failed to get the bucket name\n");
				throw AWSError(404, "NotFound");
			}

			S3Key key(bucket_name); // should just be a bucket
			if (key.haveKey()) {
				fprintf(stderr, "Fatal error: found a key after the bucket name was provided: '%s'\n", key.getKey().c_str());
				throw AWSError(404, "NotFound");
			}

			// lock the bucket while we are working on it
			std::lock_guard<std::mutex> g(key.getS3Bucket().bucketLock);

			fprintf(stdout, "The notification config before we attempt to set it: %s\n", (char *)httprequest->binary_body);
			fprintf(stdout, "Setting the new notification config for bucket: %s\n", key.getBucket().c_str());
			key.getS3Bucket().setNotifConfig(
				std::string((const char *)httprequest->binary_body, httprequest->binary_body_length)
			);
		} catch (const parse_error &e) {
			fprintf(stderr, "Fatal error: failed to parse XML\n");
			ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
			return U_CALLBACK_CONTINUE;
		} catch (const AWSError &e) { 
			fprintf(stderr, "Caught error: %s\n", e.msg.c_str());
			ulfius_set_string_body_response(httpresponse, e.error_code, e.msg.c_str());
			return U_CALLBACK_CONTINUE;
		}
		ulfius_set_string_body_response(httpresponse, 200, "");
	} else {
		// since our implementation does not require buckets to be declared in advance,
		// we can just ignore this request
		fprintf(stdout, "determined that it is infact a create bucket request\n");
		ulfius_set_string_body_response(httpresponse, 200, "");
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

	fprintf(stdout, "forking woofcnamespace platform\n");
	
	int woofcnamespaceplatform_pid = fork();
	if (woofcnamespaceplatform_pid == 0) {
		fprintf(stdout, "Child process: successfully forked\n");
		execl("./woofc-namespace-platform", "./woofc-namespace-platform", "-m", "1", "-M", "1", NULL);
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
	ulfius_add_endpoint_by_val(&instance, "PUT", "/", "/:bucket", 0, &callback_s3_put_notification, NULL);
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
