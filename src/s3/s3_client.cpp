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
#include <random>

#include <src/constants.h>
#include <3rdparty/base64.h>
#include <3rdparty/rapidxml/rapidxml.hpp>
#include <lib/utility.h>
#include <lib/wp.h>
#include <lib/aws.hpp>
#include <lib/fsutil.hpp>
#include <lib/helpers.hpp>
#include <lib/sha256_util.hpp>

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

#define RUN_TESTS

using namespace std;

struct S3LogRef {
	int64_t logId = -1;
	int64_t recordIdx = -1; // index of the record in the log
	S3LogRef() : logId(-1), recordIdx(-1) {};
	S3LogRef(uint64_t logId, uint64_t recordIdx) : logId(logId), recordIdx(recordIdx) {};
};

template<size_t record_size>
class S3StorageLog {
	std::mutex lock;
	uint64_t capacity = 0;
	uint64_t used = 0;
	uint64_t logId = 0;
	std::string logName;

	bool initialized = false;

	void initialize() {
		if (!initialized) {
			WooFCreate((char *)this->logName.c_str(), record_size, this->capacity);
			initialized = true;
		}
	}
public:
	constexpr static uint64_t recordSize = record_size;

	struct OutOfSpaceException : public std::exception {
	};
	
	S3StorageLog(uint64_t recordCount) {
		fprintf(stdout, "Constructed S3StorageLog with capacity %lu records of size %lu (total storage %lu)\n", recordCount, recordSize, recordCount * recordSize);
		this->capacity = recordCount;

		// generate an ID for the next log with a very low probability of collision
		std::random_device rd;
		std::default_random_engine generator(rd());
		std::uniform_int_distribution<long long unsigned> distribution(0,0xFFFFFFFFFFFFFFFF);
		this->logId = distribution(generator);
		this->logName = getLogName(this->logId);

		struct stat st = {0};
		if (stat(this->logName.c_str(), &st) != -1) {
			throw AWSError(500, "S3StorageLog picked an identifier that was already in use. The probability of this is INCREDIBLY low. Oops.");
		}
	}

	uint64_t getLogID() {
		return logId;
	}

	uint64_t getRecordSize() {
		return recordSize;
	}

	S3LogRef append(void *data) {
		std::lock_guard<std::mutex> guard(this->lock);
		
		if (this->used >= this->capacity) {
			throw OutOfSpaceException();
		}

		this->initialize();
		this->used++;
		long idx = WooFPut((char *)this->logName.c_str(), NULL, (char *)data);
		if (WooFInvalid(idx)) {
			throw AWSError(500, "failed to WooFPut record into the log");
		}
		// fprintf(stdout, "appended record to log %s at idx %li\n", this->logName.c_str(), idx);

		return S3LogRef(this->logId, idx);
	}

	static std::string getLogName(uint64_t logid) {
		char buffer[128];
		snprintf(buffer, sizeof(buffer) - 1, "%lx.shard.log", logid);
		return buffer;
	}

	static void get(const S3LogRef logref, void *result) {
		std::string logName = getLogName(logref.logId);
		if (WooFGet((char *)logName.c_str(), (char *)result, logref.recordIdx) != 1) {
			throw AWSError(500, "Bad LogId when attempting to get element from log");
		}
	}
};

template<size_t record_size>
class S3LogWriter {
public:
	uint64_t objectsPerLog;
	std::unique_ptr<S3StorageLog<record_size>> storageLog = nullptr;

	S3LogWriter() : S3LogWriter(256) {
	}

	S3LogWriter(uint64_t objectsPerLog) {
		this->objectsPerLog = objectsPerLog;
		this->refreshLog();
	}

	void refreshLog() {
		this->storageLog = std::unique_ptr<S3StorageLog<record_size>>(new S3StorageLog<record_size>(objectsPerLog));
	}

	S3LogRef append(void *data) {
		try {
			return this->storageLog->append(data);
		} catch (typename S3StorageLog<record_size>::OutOfSpaceException& e) {
			fprintf(stdout, "current log (id: %lu) ran out of space, replacing with a new log\n", this->storageLog->getLogID());
			this->refreshLog();
		}
		return this->storageLog->append(data);
	}

	void get(const S3LogRef logref, void *result) {
		S3StorageLog<record_size>::get(logref, result);
	}
};

struct S3FileSystem {
	// each log holds 16 megabytes of data
	constexpr static size_t S3FILE_SHARD_BYTES = 16 * 1024;
	constexpr static size_t S3OBJECTS_PER_LOG = 1024;

	struct FileExistsException : public std::exception { };
	struct FileDoesNotExistException : public std::exception { };

	struct S3Shard {
		S3LogRef nextShard; // may be initialized as some sort of null value
		uint64_t data_remaining = 0;
		uint8_t data[S3FILE_SHARD_BYTES];
	};

	std::unordered_map<std::string, S3LogRef> files;
	S3LogWriter<sizeof(S3Shard)> theShardWriter = S3LogWriter<sizeof(S3Shard)>(S3OBJECTS_PER_LOG);

	S3LogRef writeBuffer(void *data, size_t data_len) {
		// fprintf(stdout, "Writing ... data remaining ... %lu\n", data_len);
		if (data_len > S3FILE_SHARD_BYTES) {
			// fprintf(stdout, "\twrite large chunk and set nextShard to the appropriate ref\n");
			auto nextShardRef = writeBuffer((void *)((uint8_t *)data + S3FILE_SHARD_BYTES), data_len - S3FILE_SHARD_BYTES);
			// fprintf(stdout, "\t\twrote the chunk as ref %lx:%lu\n", nextShardRef.logId, nextShardRef.recordIdx);

			std::unique_ptr<S3Shard> shard(new S3Shard);
			shard->data_remaining = data_len;
			memcpy(&(shard->data), data, S3FILE_SHARD_BYTES);
			shard->nextShard = nextShardRef;
			return this->theShardWriter.append((void *)shard.get());
		} else {
			// fprintf(stdout, "\twrite small and final chunk\n");
			std::unique_ptr<S3Shard> shard(new S3Shard);
			shard->data_remaining = data_len;
			memcpy(&(shard->data), data, data_len);
			return this->theShardWriter.append((void *)shard.get());
		}
	}

	std::string readBuffer(S3LogRef ref) {
		std::stringstream ss(std::stringstream::binary | std::stringstream::out);
		
		while (ref.logId != -1) {
			// fprintf(stdout, "reading ... read from ref %lx:%lu\n", ref.logId, ref.recordIdx);
			std::unique_ptr<S3Shard> shard(new S3Shard);
			S3StorageLog<sizeof(S3Shard)>::get(ref, (void *)shard.get());
			size_t data_in_shard = 
				shard->data_remaining > S3FileSystem::S3FILE_SHARD_BYTES ? 
				S3FileSystem::S3FILE_SHARD_BYTES : shard->data_remaining;
			// fprintf(stdout, "\tdata in shard: %d\n", data_in_shard);
			ss.write((char const*)shard->data, data_in_shard);
			ref = shard->nextShard;
		}

		return ss.str();
	}

	S3LogRef writeFile(const std::string &key, void *data, size_t data_len) {
		S3LogRef retval = this->writeBuffer(data, data_len);
		fprintf(stdout, "Done writing file\n");
		return retval;
	}
};

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
		char buff[1024];
		snprintf(buff, sizeof(buff), "Payload too large for the S3 object, max size: %lu", sizeof(S3Object().payload));
		throw AWSError(500, buff);
	}
	
	unique_ptr<S3Object> obj = make_unique<S3Object>();
	memset((void *)(obj.get()), 0, sizeof(S3Object));
	obj->size = payload_size;
	memcpy((void *)obj->payload, (void *)payload, payload_size);

	if (obj->size < 4096) {
		fprintf(stdout, "payload: (%lu)\n%s\n", (unsigned long)obj->size, obj->payload);
	} else {
		fprintf(stdout, "payload: (%lu) <too large to print>\n", (unsigned long)obj->size);
	}

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

void run_tests();

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

#ifdef RUN_TESTS
	// a small test suite to run when RUN_TESTS is defined
	run_tests();
#endif

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


void run_s3_tests() {
	fprintf(stdout, "Testing the new S3 filesystem\n");
	
	std::array<size_t, 9> sizes = {
		1000,
		10000,
		100000,
		1000000,
		1000000,
		10000000,
		1000000,
		1000000,
		10000000
	};

	S3FileSystem fs;

	for (auto size : sizes) {
		std::stringstream ss;
		for (size_t i = 0; i < size; ++i) {
			ss << i << ", ";
		}

		S3LogRef ref = fs.writeFile("helloworld.txt", (void *)ss.str().c_str(), ss.str().length());
		std::string output = fs.readBuffer(ref);
		fprintf(stdout, "length out %d == length written %d\n", output.length(), ss.str().length());
		assert(output.length() == ss.str().length());
	}

	exit(0);
}


void run_tests() {
	run_s3_tests();
}