#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <linux/limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pthread.h>

#include <ulfius.h> // rest API library
#include <jansson.h> // json library

#include "woofc.h"
#include "woofc-host.h"

#include "constants.h"

#include "3rdparty/base64.h"
#include "3rdparty/hashtable.h"

#include "lib/utility.h"
#include "lib/wp.h"

// https://jansson.readthedocs.io/en/latest/

#define PORT 80

#define PIPE_READ_END 0
#define PIPE_WRITE_END 1

/*
	Worker process commands etc
*/
struct wpcmd_initdir_arg {
	char dir[PATH_MAX];
};

static int wpcmd_initdir(WP *wp, WPJob* job) {
	struct wpcmd_initdir_arg *arg = job->arg;
	fprintf(stdout, "Child process: chdir('%s')\n", arg->dir);
	if (chdir(arg->dir) != 0)
		return -1;
	return WooFInit();
}

struct wpcmd_woofcreate_arg {
	int el_size;
	int queue_depth;
	char woofname[PATH_MAX];
};

static int wpcmd_woofcreate(WP *wp, WPJob* job) {
	struct wpcmd_woofcreate_arg *arg = job->arg;
	fprintf(stdout, "Child process: WooFCreate('%s', %d, %d)\n", arg->woofname, arg->el_size, arg->queue_depth);
	int retval = WooFCreate(arg->woofname, arg->el_size, arg->queue_depth);
	fprintf(stdout, "Child process: \t woofcreate returned: %d\n", retval);
	return retval;
}

struct wpcmd_woofput_arg {
	char woofname[PATH_MAX];
	char handlername[256];
	char *payload; // remember, it must be placed in shared memory!
};

static int wpcmd_woofput(WP *wp, WPJob* job) {
	struct wpcmd_woofput_arg *arg = job->arg;
	fprintf(stdout, "Child process: WooFPut('%s', '%s', %lx)\n", arg->woofname, arg->handlername, (unsigned long) arg->payload);
	return WooFPut(arg->woofname, arg->handlername, arg->payload);
}

struct wpcmd_waitforresult_arg {
	long timeout;
	char woofname[PATH_MAX];
}; // no result struct, the result is just a buffer

static int wpcmd_waitforresult(WP *wp, WPJob *job) {
	struct wpcmd_waitforresult_arg *arg = job->arg;
	char *resultbuffer = job->result;
	fprintf(stdout, "Child process: WaitForResult(%s)\n", arg->woofname);
	
	int seqno = 0;
	int startseqno = 0;
	long timeout = arg->timeout; // 30 seconds
	long sleep_time = 1L;

	while (timeout > 0 && (seqno = WooFGetLatestSeqno(arg->woofname)) == startseqno ) {
		if (nanosleep((const struct timespec[]){{0, sleep_time * 1000000L}}, NULL) != 0) {
			break ;
		}
		timeout -= sleep_time;
	}

	if (timeout < 0 || seqno < 0)
		return -1;

	WooFGet(arg->woofname, resultbuffer, seqno);
	return seqno;
}

union wpcmd_job_data_types {
	struct wpcmd_initdir_arg wpcmd_initdir_arg;
	struct wpcmd_woofcreate_arg wpcmd_woofcreate_arg;
	struct wpcmd_woofput_arg wpcmd_woofput_arg;
	struct wpcmd_waitforresult_arg wpcmd_waitforresult_arg;
};

static WPHandler wphandler_array[] = {
	wpcmd_initdir,
	wpcmd_woofcreate,
	wpcmd_woofput, 
	wpcmd_waitforresult,
	NULL
};

// Finally some book keeping related to our worker processes
SharedBufferPool *bp_jobobject_pool;
SharedBufferPool *bp_job_bigstringpool;

pthread_mutex_t worker_processes_lock;
struct hash_table *worker_processes;
void worker_process_manager_init() {
	fprintf(stdout, "worker process manager initializing\n");

	pthread_mutex_init(&worker_processes_lock, 0);
	worker_processes = hash_table_create(hash_string, hash_equals_string);

	fprintf(stdout, "shared memory object pools initializing\n");
	bp_jobobject_pool = sharedbuffpool_create(sizeof(union wpcmd_job_data_types), 16);
	fprintf(stdout, "\tcreated bp_jobobject_pool with chunk size of %lu\n", sizeof(union wpcmd_job_data_types));
	bp_job_bigstringpool = sharedbuffpool_create(MAX_WOOF_EL_SIZE, 16);
	fprintf(stdout, "\tcreated bp_job_bigstringpool with chunk size of %d\n", MAX_WOOF_EL_SIZE);

	if (bp_jobobject_pool == NULL) {
		fprintf(stderr, "Fatal error: failed to create bp_jobobject_pool\n");
		exit(1);
	}

	if (bp_job_bigstringpool == NULL) {
		fprintf(stderr, "Fatal error: failed to create bp_job_bigstringpool\n");
		exit(1);
	}
}

void worker_process_manager_shutdown() {
	fprintf(stdout, "worker process manager shutting down\n");
	pthread_mutex_lock(&worker_processes_lock);
	struct hash_entry *entry;
	hash_table_foreach(worker_processes, entry) {
		struct WP *process = (struct WP *)(entry->data);
		fprintf(stdout, "\tfree worker process: %s %lx", (char *)entry->key, (unsigned long)process);
		free_wp(process);
	}
	pthread_mutex_unlock(&worker_processes_lock);
	sharedbuffpool_free(bp_jobobject_pool);
	sharedbuffpool_free(bp_job_bigstringpool);

	// TODO: we just let the hashtable leak since the process will soon end in any case
	// remember: you must free both the keys and the values since we strdup the funcname 
	// when you get around to properly deallocating the hashtable
}

struct WP *worker_process_for_function(const char *funcname) {
	pthread_mutex_lock(&worker_processes_lock);
	struct hash_entry *result = hash_table_search(worker_processes, funcname);
	
	if (result != NULL) {
		fprintf(stdout, "Found an existing worker process for the function specified\n");
		
		pthread_mutex_unlock(&worker_processes_lock);
		return (struct WP *)result->data;
	} else {
		fprintf(stdout, "No existing worker process for the function specified, creating worker process\n");

		WP *wp = malloc(sizeof(struct WP));
		if (init_wp(wp, 16, wphandler_array, 4) < 0) {
			fprintf(stderr, "Fatal error: failed to construct the worker process\n");
			pthread_mutex_unlock(&worker_processes_lock);
			return NULL;
		}

		char funcdir[PATH_MAX];
		sprintf(funcdir, "./functions/%s/", funcname);

		// fork the namespace platform
		int pid = fork(); // TOOD: keep track of this pid and eventually garbage collect them
		if (pid == 0) {
			fprintf(stdout, "Subprocess: created the woofc-namespace-platform child process\n");

			if (chdir(funcdir) != 0) {
				fprintf(stdout, "Subprocess: Fatal error: failed to change to the function directory\n");
				exit(0);
			}
			fprintf(stdout, "WooFCNamespacePlatform running\n");
			execl("./woofc-namespace-platform", "./woofc-namespace-platform", "-m", "4", "-M", "4", NULL);
			exit(0);
		}

		{
			sleep(1); // TODO: find a better way of avoiding the race than this...
			WPJob* theJob = create_job_easy(wp, wpcmd_initdir);
			struct wpcmd_initdir_arg *arg = bp_getchunk(bp_jobobject_pool);
			strcpy(arg->dir, funcdir);
			theJob->arg = arg;
			if (wp_job_invoke(wp, theJob) < 0) {
				fprintf(stderr, "Fatal error: worker process failed to change directory to function dir\n");
				pthread_mutex_unlock(&worker_processes_lock);
				bp_freechunk(bp_jobobject_pool, (void *)arg);
				free_wp(wp);
				return NULL;
			}
			bp_freechunk(bp_jobobject_pool, (void *)arg);
			fprintf(stdout, "worker process changed directory to function dir '%s'\n", funcdir);
		}

		hash_table_insert(worker_processes, strdup(funcname), (void *)wp);
		pthread_mutex_unlock(&worker_processes_lock);

		return wp;
	}
}

int strIsValidFuncName(const char *str) {
	while (*str != '\0') {
		if (!((*str >= '0' && *str <= '9') || (*str >= 'a' && *str <= 'z') || (*str >= 'A' && *str <= 'Z') || *str == '-' || *str == '_')) return 0 ;
		str++;
	}
	return 1;
}

int copy_file(const char *dstfilename, const char *srcfilename, int perms) {
	FILE *srcfile = fopen(srcfilename, "rb");
	FILE *dstfile = fopen(dstfilename, "wb");
	if (!srcfile || !dstfile) {
		if (srcfile)
			fclose(srcfile);
		if (dstfile) 
			fclose(dstfile);
		return -1;
	}

	int chr;
	while ((chr = fgetc(srcfile)) != EOF) {
		fputc(chr, dstfile);
	}

	fclose(srcfile);
	fclose(dstfile);

	return chmod(dstfilename, perms);
}

/*
	API request handler for callback_function_create 
*/

int callback_function_create (const struct _u_request * httprequest, struct _u_response * httpresponse, void * user_data) {
	fprintf(stdout, "\n\nPOST REQUEST: callback_function_create\n");
	json_t* req_json = json_loadb(httprequest->binary_body, httprequest->binary_body_length, 0, NULL);
	json_dumpfd(req_json, 1, JSON_INDENT(4));
	fprintf(stdout, "\n");

	// get the function name & make sure its ascii only
	const char *funcname = json_string_value(json_object_get(req_json, "FunctionName"));
	if (funcname == NULL || !strIsValidFuncName(funcname)) {
		fprintf(stderr, "Fatal error: bad function name\n");
		json_decref(req_json);
		ulfius_set_string_body_response(httpresponse, 400, "InvalidParameterValueException");
		return U_CALLBACK_CONTINUE;
	}

	// find the function directory
	struct stat st = {0};
	char funcdir[PATH_MAX];
	sprintf(funcdir, "./functions/%s/", funcname);

	if (stat(funcdir, &st) != -1) {
		fprintf(stderr, "Fatal error: resource conflict, the function already exists\n");
		json_decref(req_json);
		ulfius_set_string_body_response(httpresponse, 409, "ResourceConflictException"); // the resource already exists
		return U_CALLBACK_CONTINUE;
	}

	// create the directory 
	fprintf(stdout, "created a directory for the function: %s\n", funcdir);
	if (mkdir(funcdir, 0700) == -1) {
		fprintf(stderr, "Fatal error: could not make directory for function: %s\n", funcdir);
		json_decref(req_json);
		ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
		return U_CALLBACK_CONTINUE;
	}

	// decode the base64 encoded payload string
	{
		// NOTE: this needs to be updated to remove the entire directory on failure
		const char *b64_encoded_func_zip = json_string_value(json_object_get(json_object_get(req_json, "Code"), "ZipFile"));
		if (!b64_encoded_func_zip) {
			fprintf(stderr, "Fatal error: ZipFile not provided\n");
			json_decref(req_json);
			ulfius_set_string_body_response(httpresponse, 400, "InvalidParameterValueException");
			return U_CALLBACK_CONTINUE;
		}

		int decoded_func_zip_len = Base64decode_len(b64_encoded_func_zip);
		char *decoded_func_zip = malloc(decoded_func_zip_len);
		if (!decoded_func_zip) {
			fprintf(stderr, "Fatal error: failed to malloc memory for decoding zipfile\n");
			json_decref(req_json);
			ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
			return U_CALLBACK_CONTINUE;
		}
		Base64decode(decoded_func_zip, b64_encoded_func_zip);

		char zipfilepath[PATH_MAX];
		sprintf(zipfilepath, "%sfunction.zip", funcdir);
		FILE *zipfile = fopen(zipfilepath, "wb");
		if (!zipfile) {
			fprintf(stderr, "Fatal error: failed to open zipfile for writing\n");
			json_decref(req_json);
			ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
			return U_CALLBACK_CONTINUE;
		}

		if (fwrite(decoded_func_zip, decoded_func_zip_len, 1, zipfile) < 0) {
			fclose(zipfile);
			fprintf(stderr, "Fatal error: failed to write the zipfile to the disk\n");
			json_decref(req_json);
			ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
			return U_CALLBACK_CONTINUE;
		}
		fclose(zipfile);

		// safe b/c the zipfilename can only contain a-z A-Z 0-9 '-' '_'
		char unzip_command[PATH_MAX + 128];
		sprintf(unzip_command, "/usr/bin/unzip '%s' -d %s", zipfilepath, funcdir);
		int retval = system(unzip_command);

		if (retval != 0) {
			fprintf(stderr, "Fatal error: unzip exited with non-zero return code, extraction was unsuccessful\n");
			json_decref(req_json);
			ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
			return U_CALLBACK_CONTINUE;
		}

		char buffer1[PATH_MAX];
		char buffer2[PATH_MAX];
		char buffer3[PATH_MAX];
		sprintf(buffer1, "%swoofc-namespace-platform", funcdir);
		sprintf(buffer2, "%swoofc-container", funcdir);
		sprintf(buffer3, "%sawspy_lambda", funcdir);
		if (copy_file(buffer1, "./woofc-namespace-platform", 777) < 0 ||
			copy_file(buffer2, "./woofc-container", 777) < 0 ||
			copy_file(buffer3, "./awspy_lambda", 777) < 0) {
			
			fprintf(stderr, "Fatal error: failed to copy a required binary dependency into the directory\n");
			json_decref(req_json);
			ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
			return U_CALLBACK_CONTINUE;
		}
	}

	WP *wp = worker_process_for_function(funcname);
	if (wp == NULL) {
		fprintf(stderr, "Fatal error: failed to fork off the worker process\n");
		json_decref(req_json);
		ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
		return U_CALLBACK_CONTINUE;
	}

	// dispatch a command to the subprocess to create the WooF
	{
		WPJob* theJob = create_job_easy(wp, wpcmd_woofcreate);
		struct wpcmd_woofcreate_arg *arg = theJob->arg = bp_getchunk(bp_jobobject_pool);
		arg->el_size = CALL_WOOF_EL_SIZE;
		arg->queue_depth = CALL_WOOF_QUEUE_DEPTH;
		strcpy(arg->woofname, CALL_WOOF_NAME);

		fprintf(stdout, "Invoking WooFCreate command\n");
		int retval = wp_job_invoke(wp, theJob);

		bp_freechunk(bp_jobobject_pool, (void *)arg);
		if (retval < 0) {
			fprintf(stderr, "Fatal error: failed to create the WooF '%s'\n", CALL_WOOF_NAME);
			ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
			json_decref(req_json);
			return U_CALLBACK_CONTINUE;
		}

		fprintf(stdout, "Created the WooF '%s' return code: %d\n", CALL_WOOF_NAME, retval);
	}
	

	// finally, write out the metadata file
	char metadata_file_path[PATH_MAX];
	sprintf(metadata_file_path, "./functions/%s/%s-metadata.json", funcname, funcname);
	FILE *metadata_file = fopen(metadata_file_path, "wb");

	json_object_del(req_json, "Code");
	const char *res_json_str = json_dumps(req_json, 0);

	if (metadata_file == NULL || fwrite(res_json_str, strlen(res_json_str), 1, metadata_file) < 0) {
		fclose(metadata_file);
		fprintf(stderr, "Fatal error: failed to write the metadata file\n");
		json_decref(req_json);
		free((void *)res_json_str);
		unlink(metadata_file_path);
		ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
		return U_CALLBACK_CONTINUE;
	}
	fclose(metadata_file);
	fprintf(stdout, "Created metadata file: %s\n", metadata_file_path);
	
	ulfius_set_string_body_response(httpresponse, 200, res_json_str);
	fprintf(stdout, "Successfully created function %s!\n", funcname);
	free((void *)res_json_str);
	json_decref(req_json);

	return U_CALLBACK_CONTINUE;
}

/*
	API request handler for callback_function_invoke
*/
int callback_function_invoke (const struct _u_request * httprequest, struct _u_response * httpresponse, void * user_data) {
	fprintf(stdout, "\n\nPOST REQUEST: callback_function_invoke\n");

	json_t* req_json = json_loadb(httprequest->binary_body, httprequest->binary_body_length, 0, NULL);
	json_dumpfd(req_json, 1, JSON_INDENT(2));
	fprintf(stdout, "\n");
	if (!req_json) {
		ulfius_set_string_body_response(httpresponse, 400, "InvalidRequestContentException");
		return U_CALLBACK_CONTINUE;
	}

	const char *funcname = u_map_get(httprequest->map_url, "name");
	char funcdir[PATH_MAX];
	sprintf(funcdir, "./functions/%s/", funcname);

	char metadata_file_path[PATH_MAX];
	FILE *metadata_file;
	sprintf(metadata_file_path, "./functions/%s/%s-metadata.json", funcname, funcname);

	if ((metadata_file = fopen(metadata_file_path, "r")) == NULL) {
		fprintf(stderr, "Fatal error: file '%s' not found\n", metadata_file_path);
		ulfius_set_string_body_response(httpresponse, 404, "ResourceNotFoundException");
		json_decref(req_json);
		return U_CALLBACK_CONTINUE;
	}
	fclose(metadata_file);

	// generate resultwoof if needed
	const char *invocation_type = u_map_get(httprequest->map_header, "X-Amz-Client-Context") ;
	if (!invocation_type) 
		invocation_type = "RequestResponse";
	
	char result_woof[256];
	char result_woof_path[PATH_MAX];
	result_woof[0] = 0;
	result_woof_path[0] = 0;
	if (strcmp(invocation_type, "RequestResponse") == 0) {
		sprintf(result_woof, "result-%02x%02x%02x%02x%02x%02x%02x%02x.woof",
			rand() % 256, rand() % 256, rand() % 256, rand() % 256,
			rand() % 256, rand() % 256, rand() % 256, rand() % 256);
		sprintf(result_woof_path, "%s%s", funcdir, result_woof);
	}

	fprintf(stdout, "function invocation options:\n"
		"\tfunction name: %s\n"
		"\tfunction dir: %s\n"
		"\tresult woof: %s\n"
		"\tresult woof path: %s\n",
		funcname, funcdir, result_woof, result_woof_path);

	// compose the payload to put in the WooF, first build the metadata objects and the 
	// payload string as provided by the client
	char *woofputbuffer = NULL;
	{
		json_t *metadata = json_object();
		json_object_set_new(metadata, "function", json_string(funcname));
		if (result_woof[0] != 0) {
			json_object_set_new(metadata, "result_woof", json_string(result_woof));
		} else {
			json_object_set_new(metadata, "result_woof", json_null());
		}

		const char *payload_str = json_dumps(req_json, JSON_COMPACT);
		json_decref(req_json);
		const char *metadata_str = json_dumps(metadata, JSON_COMPACT);
		json_decref(metadata);

		if (payload_str == NULL) {
			free((void *)payload_str);
			fprintf(stderr, "Fatal error: failed to dump the payload_str as JSON\n");
			ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
			return U_CALLBACK_CONTINUE;
		}

		if (metadata_str == NULL) {
			free((void *)payload_str);
			
			fprintf(stderr, "Fatal error: failed to dump the payload_str as JSON\n");
			ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
			return U_CALLBACK_CONTINUE;
		}

		int payload_str_len = strlen(payload_str);
		int metadata_str_len = strlen(metadata_str);
		if (payload_str_len + metadata_str_len + 2 > CALL_WOOF_EL_SIZE) {
			// the request was too large to fit in the buffer
			free((void *)payload_str);
			free((void *)metadata_str);
			ulfius_set_string_body_response(httpresponse, 413, "RequestTooLargeException");
			return U_CALLBACK_CONTINUE;
		}

		woofputbuffer = malloc(CALL_WOOF_EL_SIZE); 
		if (woofputbuffer == NULL) {
			free((void *)payload_str);
			free((void *)metadata_str);
			ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
			return U_CALLBACK_CONTINUE;
		}

		// should now look like metadata \0 payload \0 
		memset(woofputbuffer, 0, CALL_WOOF_EL_SIZE);
		strncpy(woofputbuffer, metadata_str, metadata_str_len);
		strncpy(woofputbuffer + metadata_str_len + 1, payload_str, payload_str_len);
		
		free((void *)payload_str);
		free((void *)metadata_str);
	}

	fprintf(stdout, "successfully constructed the payload for the WooFPut\n");

	WP *wp = worker_process_for_function(funcname);
	if (wp == NULL) {
		fprintf(stderr, "Fatal error: failed to get a handle on the worker process for this function\n");
		free(woofputbuffer);
		ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
		return U_CALLBACK_CONTINUE;
	}

	// if result_woof is defined, create the result woof so 
	// that we can pass back a, well, result :)
	if (result_woof[0] != 0) {
		WPJob* theJob = create_job_easy(wp, wpcmd_woofcreate);
		struct wpcmd_woofcreate_arg *arg = theJob->arg = bp_getchunk(bp_jobobject_pool);
		arg->el_size = CALL_WOOF_EL_SIZE;
		arg->queue_depth = CALL_WOOF_QUEUE_DEPTH;
		strcpy(arg->woofname, result_woof);
		
		fprintf(stdout, "Invoking WooFCreate command\n");
		int retval = wp_job_invoke(wp, theJob);

		bp_freechunk(bp_jobobject_pool, (void *)arg);
		if (retval < 0) {
			fprintf(stderr, "Fatal error: failed to create the WooF '%s'\n", result_woof);
			ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
			free(woofputbuffer);
			return U_CALLBACK_CONTINUE;
		}
		
		fprintf(stdout, "Created the result WooF '%s' %d\n", result_woof, retval);
	}
	
	// Do the WooF Put that invokes the lambda function
	{
		WPJob* theJob = create_job_easy(wp, wpcmd_woofput);
		struct wpcmd_woofput_arg *arg = theJob->arg = bp_getchunk(bp_jobobject_pool);
		strcpy(arg->woofname, CALL_WOOF_NAME); // set the woofname
		strcpy(arg->handlername, "awspy_lambda"); // set the handlername
		char *sharedbuff = bp_getchunk(bp_job_bigstringpool);
		arg->payload = sharedbuff; // set the payload
		memcpy(sharedbuff, woofputbuffer, CALL_WOOF_EL_SIZE);
		free(woofputbuffer);
		
		fprintf(stdout, "Invoking WooFPut command\n");
		int retval = wp_job_invoke(wp, theJob);

		bp_freechunk(bp_jobobject_pool, (void *)arg);
		bp_freechunk(bp_job_bigstringpool, (void *)sharedbuff);
		if (retval < 0) {
			if (result_woof_path[0] != 0) 
				unlink(result_woof_path);
			fprintf(stderr, "Fatal error: failed to put the invocation in WooF '%s'\n", CALL_WOOF_NAME);
			ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
			return U_CALLBACK_CONTINUE;
		}
		
		fprintf(stdout, "Put invocation in WooF '%s' at idx: %d\n", CALL_WOOF_NAME, retval);
	}

	if (result_woof[0]) {
		fprintf(stdout, "Result Woof was defined, so we are spinning until the result is available\n");

		WPJob* theJob = create_job_easy(wp, wpcmd_waitforresult);
		struct wpcmd_waitforresult_arg *arg = theJob->arg = bp_getchunk(bp_jobobject_pool);
		char *result = theJob->result = bp_getchunk(bp_job_bigstringpool);

		arg->timeout = 30000L;
		strcpy(arg->woofname, result_woof);
		
		int retval = wp_job_invoke(wp, theJob);

		bp_freechunk(bp_jobobject_pool, (void *)arg);
		unlink(result_woof_path);
		
		if (retval < 0) {
			bp_freechunk(bp_job_bigstringpool, (void *)result);
			fprintf(stderr, "Fatal error: failed to get result from lambda invocation, timed out or other error encountered\n");
			ulfius_set_string_body_response(httpresponse, 200, "{\"error\": \"function timed out\"}"); // TODO: improve this message to make it match that which you would get from AWS
			return U_CALLBACK_CONTINUE;
		}

		fprintf(stdout, "finished waiting for the result\n");

		ulfius_set_string_body_response(httpresponse, 200, result);
		bp_freechunk(bp_job_bigstringpool, (void *)result);
		return U_CALLBACK_CONTINUE;
	}

	ulfius_set_string_body_response(httpresponse, 200, "{\"status\": \"ok\"}");
	return U_CALLBACK_CONTINUE;
}

int main(int argc, char **argv)
{
	/*
		start the web server
	*/
	struct _u_instance instance;
	
	// Initialize the worker process manager
	worker_process_manager_init();

	// Setup the functions directory
	struct stat st = {0};
	if (stat("./functions", &st) == -1) {
		fprintf(stdout, "Created functions directory.\n");
		mkdir("./functions", 0700);
	}

	// Initialize instance with the port number
	if (ulfius_init_instance(&instance, PORT, NULL, NULL) != U_OK) {
		fprintf(stderr, "Error ulfius_init_instance, abort\n");
		return(1);
	}

	ulfius_add_endpoint_by_val(&instance, "POST", "/2015-03-31/", "/functions", 0, &callback_function_create, NULL);
	ulfius_add_endpoint_by_val(&instance, "POST", "/2015-03-31/", "/functions/:name/invocations", 0, &callback_function_invoke, NULL);

	// Start the framework
	if (ulfius_start_framework(&instance) == U_OK) {
		printf("Start framework on port %d\n", instance.port);

		fgetc(stdin); // block until input from user
	} else {
		fprintf(stderr, "Error starting framework\n");
	}
	printf("End framework\n");

	worker_process_manager_shutdown();

	ulfius_stop_framework(&instance);
	ulfius_clean_instance(&instance);

	// PyMem_RawFree(program);
	return 0;
}
