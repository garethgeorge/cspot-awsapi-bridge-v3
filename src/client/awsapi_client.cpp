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

#include "wpcmds.h"

// Finally some book keeping related to our worker processes
SharedBufferPool *bp_jobobject_pool;
SharedBufferPool *bp_job_bigstringpool;

struct CSPOTNamespace {
	WP *worker_process = NULL;
	int woofcnamespace_pid = -1;

	// a queue containing result WooFs for reuse
	Queue result_woof_queue;

	CSPOTNamespace() {
		queue_init(&(this->result_woof_queue), sizeof(struct ResultWooF), RESULT_WOOF_COUNT);
	}

	~CSPOTNamespace() {
		queue_free(&(this->result_woof_queue));
		if (worker_process != NULL)
			free_wp(worker_process);
		if (woofcnamespace_pid != -1)
			kill(this->woofcnamespace_pid, SIGTERM);
	}
};

std::mutex namespaces_mtx;
std::unordered_map<std::string, CSPOTNamespace*> namespaces;

void worker_process_manager_init() {
	fprintf(stdout, "worker process manager initializing\n");

	std::lock_guard<std::mutex> lock(namespaces_mtx);

	fprintf(stdout, "shared memory object pools initializing\n");
	bp_jobobject_pool = sharedbuffpool_create(sizeof(union wpcmd_job_data_types), WORKER_QUEUE_DEPTH);
	fprintf(stdout, "\tcreated bp_jobobject_pool with chunk size of %lu\n", sizeof(union wpcmd_job_data_types));
	bp_job_bigstringpool = sharedbuffpool_create(MAX_WOOF_EL_SIZE, WORKER_QUEUE_DEPTH);
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
	std::lock_guard<std::mutex> lock(namespaces_mtx);
	
	auto it = namespaces.begin();

	while (it != namespaces.end()) {
		fprintf(stdout, "\tfree namespace for function: %s\n", it->first.c_str());
		delete it->second;
		it = namespaces.erase(it);
	}

	fprintf(stdout, "free'd all namespaces\n");

	sharedbuffpool_free(bp_jobobject_pool);
	sharedbuffpool_free(bp_job_bigstringpool);
}

struct CSPOTNamespace *namespace_for_function(const char *funcname) {
	std::lock_guard<std::mutex> lock(namespaces_mtx);
	
	if (namespaces.find(funcname) != namespaces.end()) {
		fprintf(stdout, "Found an existing worker process for the function specified\n");
		return namespaces[funcname];
	} else {
		fprintf(stdout, "No existing worker process for the function specified, creating worker process\n");

		WP *wp = new WP;
		if (init_wp(wp, WORKER_QUEUE_DEPTH, wphandler_array, 4) < 0) {
			fprintf(stderr, "Fatal error: failed to construct the worker process\n");
			return NULL;
		}

		char funcdir[PATH_MAX];
		sprintf(funcdir, "./functions/%s/", funcname);

		// fork the namespace platform
		int nspid = fork(); // TOOD: keep track of this pid and eventually garbage collect them
		if (nspid < 0) {
			fprintf(stderr, "Fatal error: failed to fork woofc-namespace-platform executable\n");
			return NULL;
		} else if (nspid == 0) {
			fprintf(stdout, "Subprocess: created the woofc-namespace-platform child process\n");

			if (chdir(funcdir) != 0) {
				fprintf(stdout, "Subprocess: Fatal error: failed to change to the function directory\n");
				exit(0);
			}
			fprintf(stdout, "WooFCNamespacePlatform running\n");
			execl("./woofc-namespace-platform", "./woofc-namespace-platform", "-m", "4", "-M", "8", NULL);
			exit(0);
		} else {
			fprintf(stdout, "WoofCNamespacePlatform PID: %d\n", nspid);
		}
		
		{
			sleep(1); // TODO: find a better way of avoiding the race than this...
			WPJob* theJob = create_job_easy(wp, wpcmd_initdir);
			struct wpcmd_initdir_arg *arg = (struct wpcmd_initdir_arg *)bp_getchunk(bp_jobobject_pool);
			strcpy(arg->dir, funcdir);
			theJob->arg = arg;
			if (wp_job_invoke(wp, theJob) < 0) {
				fprintf(stderr, "Fatal error: worker process failed to change directory to function dir\n");
				bp_freechunk(bp_jobobject_pool, (void *)arg);
				free_wp(wp);
				return NULL;
			}
			bp_freechunk(bp_jobobject_pool, (void *)arg);
			fprintf(stdout, "worker process changed directory to function dir '%s'\n", funcdir);
		}

		CSPOTNamespace *ns = new CSPOTNamespace;
		ns->worker_process = wp;
		ns->woofcnamespace_pid = nspid;
		
		// create the result woofs for the process
		{
			int i;
			for (i = 0; i < RESULT_WOOF_COUNT; ++i) {
				char woofpath[PATH_MAX];
				char woofname[256];
				sprintf(woofname, "result-%d.woof", i);
				sprintf(woofpath, "./functions/%s/%s", funcname, woofname);

				fprintf(stdout, "seting up resultwoof %s (%s)\n", woofname, woofpath);

				struct stat st = {0};
				if (stat(woofpath, &st) == -1) {
					fprintf(stderr, "Result WooF '%s' does not exist, making it\n", woofpath);

					WPJob* theJob = create_job_easy(wp, wpcmd_woofcreate);
					struct wpcmd_woofcreate_arg *arg =
						(struct wpcmd_woofcreate_arg *)(theJob->arg = bp_getchunk(bp_jobobject_pool));
					arg->el_size = RESULT_WOOF_EL_SIZE;
					arg->queue_depth = 1; // result woof only needs to hold one result at a time
					strcpy(arg->woofname, woofname);
					fprintf(stdout, "Creating Result WooF %s\n", woofpath);
					int retval = wp_job_invoke(wp, theJob);
					bp_freechunk(bp_jobobject_pool, (void *)arg);
					if (retval < 0) {
						fprintf(stderr, "Fatal error: failed to create result woof '%s'\n", woofname);
						delete ns;
						return NULL;
					}
				}
				
				WPJob* theJob = create_job_easy(wp, wpcmd_woofgetlatestseqno);
				struct wpcmd_woofgetlatestseqno_arg *arg = 
					(struct wpcmd_woofgetlatestseqno_arg *)(theJob->arg = bp_getchunk(bp_jobobject_pool));
				strcpy(arg->woofname, woofname);
				int retval = wp_job_invoke(wp, theJob);
				bp_freechunk(bp_jobobject_pool, (void *)arg);
				if (retval < 0) {
					fprintf(stderr, "Fatal error: failed to get seqno for result woof '%s' retval: %d\n", woofname, retval);
					delete ns;
					return NULL;
				}

				// now get the latest seqno and insert it into the table
				struct ResultWooF woof;
				woof.seqno = retval;
				strcpy(woof.woofname, woofname);
				queue_put(&(ns->result_woof_queue), &woof);
			}
		}

		namespaces[funcname] = ns;

		return ns;
	}
}

int strIsValidFuncName(const char *str) {
	// TODO: replace this with a C++11 regex match
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
	json_t* req_json = json_loadb((const char *)httprequest->binary_body, httprequest->binary_body_length, 0, NULL);
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
		char *decoded_func_zip = (char *)malloc(decoded_func_zip_len);
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

	CSPOTNamespace *ns = namespace_for_function(funcname);
	if (ns == NULL) {
		fprintf(stderr, "Fatal error: failed to get namespace for function %s\n", funcname);
		json_decref(req_json);
		ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
		return U_CALLBACK_CONTINUE;
	}
	WP *wp = ns->worker_process;

	// dispatch a command to the subprocess to create the WooF
	{
		WPJob* theJob = create_job_easy(wp, wpcmd_woofcreate);
		struct wpcmd_woofcreate_arg *arg = 
			(struct wpcmd_woofcreate_arg *)(theJob->arg = bp_getchunk(bp_jobobject_pool));
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
	
	json_t* req_json = json_loadb((char *)httprequest->binary_body, httprequest->binary_body_length, 0, NULL);

	json_dumpfd(req_json, 1, JSON_INDENT(2));
	fprintf(stdout, "\n");
	if (!req_json) {
		ulfius_set_string_body_response(httpresponse, 400, "InvalidRequestContentException");
		json_decref(req_json);
		return U_CALLBACK_CONTINUE;
	}

	const char *funcname = u_map_get(httprequest->map_url, "name");
	if (!strIsValidFuncName(funcname)) {
		fprintf(stderr, "Fatal error: invalid function name was provided\n");
		ulfius_set_string_body_response(httpresponse, 400, "InvalidRequestContentException");
		json_decref(req_json);
		return U_CALLBACK_CONTINUE;
	}

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
	const char *invocation_type = u_map_get(httprequest->map_header, "X-Amz-Invocation-Type") ;
	if (!invocation_type) 
		invocation_type = "RequestResponse";
	
	int request_response = (strcmp(invocation_type, "RequestResponse") == 0) ? 1 : 0;

	fprintf(stdout, "fetching the namespace for this function\n");
	
	CSPOTNamespace *ns = namespace_for_function(funcname);
	if (ns == NULL) {
		fprintf(stderr, "Fatal error: failed to get the CSPOT Namespace for this function\n");
		ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
		json_decref(req_json);
		return U_CALLBACK_CONTINUE;
	}

	WP *wp = ns->worker_process;
	if (wp == NULL) {
		fprintf(stderr, "Fatal error: bad worker process should never happen\n");
		exit(0);
	}

	struct ResultWooF resultwoof;
	if (request_response) {
		// to be sure the WooF is ready before invoking the function
		queue_get(&(ns->result_woof_queue), &resultwoof);

		WPJob* theJob = create_job_easy(wp, wpcmd_woofgetlatestseqno);
		struct wpcmd_woofgetlatestseqno_arg *arg = 
			(struct wpcmd_woofgetlatestseqno_arg *)(theJob->arg = bp_getchunk(bp_jobobject_pool));
		strcpy(arg->woofname, resultwoof.woofname);
		int retval = wp_job_invoke(wp, theJob);
		bp_freechunk(bp_jobobject_pool, (void *)arg);
		if (retval < 0) {
			fprintf(stderr, "Fatal error: failed to get the seqno from the namespace\n");
			ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
			json_decref(req_json);
			return U_CALLBACK_CONTINUE;
		}

		resultwoof.seqno = retval;
	}

	fprintf(stdout, "function invocation options:\n"
		"\tfunction name: %s\n"
		"\tfunction dir: %s\n"
		"\tresult woof: %s (seqno: %d)\n"
		"\trequest response: %d\n",
		funcname, funcdir, resultwoof.woofname, resultwoof.seqno, request_response);
	
	// compose the payload to put in the WooF, first build the metadata objects and the 
	// payload string as provided by the client
	char *woofputbuffer = NULL;
	{
		json_t *metadata = json_object();
		json_object_set_new(metadata, "function", json_string(funcname));
		if (request_response) {
			json_object_set_new(metadata, "result_woof", json_string(resultwoof.woofname));
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
			if (request_response) 
				queue_put(&(ns->result_woof_queue), &resultwoof);
			return U_CALLBACK_CONTINUE;
		}

		if (metadata_str == NULL) {
			free((void *)payload_str);
			fprintf(stderr, "Fatal error: failed to dump the payload_str as JSON\n");
			ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
			if (request_response) 
				queue_put(&(ns->result_woof_queue), &resultwoof);
			return U_CALLBACK_CONTINUE;
		}

		int payload_str_len = strlen(payload_str);
		int metadata_str_len = strlen(metadata_str);
		if (payload_str_len + metadata_str_len + 2 > CALL_WOOF_EL_SIZE) {
			// the request was too large to fit in the buffer
			free((void *)payload_str);
			free((void *)metadata_str);
			ulfius_set_string_body_response(httpresponse, 413, "RequestTooLargeException");
			if (request_response) 
				queue_put(&(ns->result_woof_queue), &resultwoof);
			return U_CALLBACK_CONTINUE;
		}

		woofputbuffer = (char *)malloc(CALL_WOOF_EL_SIZE); 
		if (woofputbuffer == NULL) {
			free((void *)payload_str);
			free((void *)metadata_str);
			ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
			if (request_response) 
				queue_put(&(ns->result_woof_queue), &resultwoof);
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

	// Do the WooF Put that invokes the lambda function
	{
		WPJob* theJob = create_job_easy(wp, wpcmd_woofput);
		struct wpcmd_woofput_arg *arg = 
			(struct wpcmd_woofput_arg *)(theJob->arg = bp_getchunk(bp_jobobject_pool));
		strcpy(arg->woofname, CALL_WOOF_NAME); // set the woofname
		strcpy(arg->handlername, "awspy_lambda"); // set the handlername
		char *sharedbuff = (char *)bp_getchunk(bp_job_bigstringpool);
		arg->payload = sharedbuff; // set the payload
		memcpy(sharedbuff, woofputbuffer, CALL_WOOF_EL_SIZE);
		free(woofputbuffer);
		
		fprintf(stdout, "Invoking WooFPut command\n");
		int retval = wp_job_invoke(wp, theJob);

		bp_freechunk(bp_jobobject_pool, (void *)arg);
		bp_freechunk(bp_job_bigstringpool, (void *)sharedbuff);
		if (retval < 0) {
			fprintf(stderr, "Fatal error: failed to put the invocation in WooF '%s'\n", CALL_WOOF_NAME);
			ulfius_set_string_body_response(httpresponse, 500, "ServiceException");
			if (request_response)
				queue_put(&(ns->result_woof_queue), &resultwoof);
			return U_CALLBACK_CONTINUE;
		}
		
		fprintf(stdout, "Put invocation in WooF '%s' at idx: %d\n", CALL_WOOF_NAME, retval);
	}

	if (request_response) {
		fprintf(stdout, "Result Woof was defined, so we are spinning until the result is available\n");

		WPJob* theJob = create_job_easy(wp, wpcmd_waitforresult);
		struct wpcmd_waitforresult_arg *arg = 
			(struct wpcmd_waitforresult_arg *)(theJob->arg = bp_getchunk(bp_jobobject_pool));
		char *result = (char *)(theJob->result = bp_getchunk(bp_job_bigstringpool));
		arg->timeout = 30000L;
		memcpy(&(arg->resultwoof), &resultwoof, sizeof(struct ResultWooF));
		
		int retval = wp_job_invoke(wp, theJob);
		bp_freechunk(bp_jobobject_pool, (void *)arg);
		
		if (retval < 0) {
			if (request_response) // TODO: we should really try to update the seqno in the woof with the real seqno if an error occurs, who knows what actually happened
				queue_put(&(ns->result_woof_queue), &resultwoof);
			
			bp_freechunk(bp_job_bigstringpool, (void *)result);
			fprintf(stderr, "Fatal error: failed to get result from lambda invocation, timed out or other error encountered\n");
			ulfius_set_string_body_response(httpresponse, 200, "{\"error\": \"function timed out\"}"); // TODO: improve this message to make it match that which you would get from AWS
			return U_CALLBACK_CONTINUE;
		}

		fprintf(stdout, "finished waiting for the result\n");
		resultwoof.seqno = retval;
		queue_put(&(ns->result_woof_queue), &resultwoof);

		ulfius_set_string_body_response(httpresponse, 200, result);
		bp_freechunk(bp_job_bigstringpool, (void *)result);
		return U_CALLBACK_CONTINUE;
	}

	ulfius_set_string_body_response(httpresponse, 200, "{\"status\": \"ok\"}");
	return U_CALLBACK_CONTINUE;
}

void sig_handler(int sig) {
	switch (sig) {
	case SIGINT:
		fprintf(stderr, "\n\nCAUGHT SIGNAL: shutting down\n");
		worker_process_manager_shutdown();
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
	signal(SIGINT, sig_handler);

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
