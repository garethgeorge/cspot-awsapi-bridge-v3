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

#include "function_helpers.hpp"
#include "sha256_util.hpp"

#define PORT 8080

// manages all of the lambda functions etc.
FunctionManager *funcMgr = nullptr;

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

int zip_from_base64_string(const char *b64string, char *zipfilepath, char *sha256outparam) {
	int decoded_zip_len = Base64decode_len(b64string);
	char *decoded_zip = (char *)malloc(decoded_zip_len);
	if (!decoded_zip) {
		fprintf(stderr, "Fatal error: failed to malloc memory for decoding zipfile\n");
		throw AWSError(500, "ServiceException");
	}

	Base64decode(decoded_zip, b64string);

	char sha256src[65];
	sha256(decoded_zip, sha256src);
	
	sprintf(zipfilepath, "./functions/zips/%s.zip", sha256src);
	FILE *zipfile = fopen(zipfilepath, "wb");
	if (!zipfile) {
		fprintf(stderr, "Fatal error: failed to open zipfile for writing\n");
		free(decoded_zip);
		throw AWSError(500, "ServiceException");
	}

	if (fwrite(decoded_zip, decoded_zip_len, 1, zipfile) < 0) {
		free(decoded_zip);
		fclose(zipfile);
		fprintf(stderr, "Fatal error: failed to write the zipfile to the disk\n");
		throw AWSError(500, "ServiceException");
	}

	free(decoded_zip);
	fclose(zipfile);

	if (sha256outparam != NULL) {
		strcpy(sha256outparam, sha256src);
	}

	return decoded_zip_len;
}

int callback_function_create (const struct _u_request * httprequest, struct _u_response * httpresponse, void * user_data) {
	fprintf(stdout, "\n\nPOST REQUEST: callback_function_create\n");
	json_t* req_json = json_loadb((const char *)httprequest->binary_body, httprequest->binary_body_length, 0, NULL);
	json_dumpfd(req_json, 1, JSON_INDENT(4));
	fprintf(stdout, "\n");

	// guard against the function locking.
	std::lock_guard<std::mutex> guard(funcMgr->create_function_lock);

	try {
		fprintf(stdout, "decoding create function request\n");
		const char *funcname = json_string_value(json_object_get(req_json, "FunctionName"));
		
		fprintf(stdout, "create function: %s\n", funcname);

		if (!FunctionProperties::validateFunctionName(funcname)) {
			fprintf(stderr, "Fatal error: bad function name\n");
			throw AWSError(400, "InvalidParameterValueException");
		}

		if (funcMgr->functionExists(funcname)) {
			fprintf(stderr, "Fatal error: a function with that name already exists\n");
			throw AWSError(409, "ResourceConflictException");
		}
		
		std::shared_ptr<FunctionProperties> func = std::make_shared<FunctionProperties>();
		func->name = funcname;
		const char *handler = json_string_value(json_object_get(req_json, "Handler"));
		fprintf(stdout, "function handler: %s\n", handler);
		if (handler == NULL || strstr(handler, ".") == NULL) {
			fprintf(stderr, "Fatal error: handler name not specified or did not contain a '.'\n");
			throw AWSError(409, "InvalidParameterValueException");
		}
		func->handler = handler;

		// decode the zip file
		fprintf(stdout, "decoding the source zip file and writing it to disk\n");
		char sha256src[65];
	
		// NOTE: this needs to be updated to remove the entire directory on failure
		const char *b64_encoded_func_zip = json_string_value(json_object_get(json_object_get(req_json, "Code"), "ZipFile"));
		if (!b64_encoded_func_zip) {
			fprintf(stderr, "Fatal error: ZipFile not provided\n");
			throw AWSError(400, "InvalidParameterValueException");
		}

		char zipfilepath[PATH_MAX];
		int code_size = zip_from_base64_string(b64_encoded_func_zip, zipfilepath, sha256src);

		func->src_zip_path = zipfilepath;
		func->src_zip_sha256 = sha256src;

		json_object_del(req_json, "Code");
		json_object_set_new(req_json, "CodeSha256", json_string(sha256src));
		json_object_set_new(req_json, "CodeSize", json_integer(code_size));
		fprintf(stdout, "\tdecoded and wrote out zipfile, hash: %s\n", sha256src);

		// done creating the function, write it out
		fprintf(stdout, "now adding the function to the table");
		funcMgr->addFunction(func);

		const char *res_json_str = json_dumps(req_json, 0);
		if (res_json_str == NULL) {
			throw AWSError(500, "failed to stringify req json");
		}

		ulfius_set_string_body_response(httpresponse, 200, res_json_str);
		fprintf(stdout, "Successfully created function %s!\n", funcname);
		free((void *)res_json_str);
		json_decref(req_json);

		return U_CALLBACK_CONTINUE;
	} catch (const AWSError &e) {
		fprintf(stderr, "Caught error: %s\n", e.msg.c_str());
		json_decref(req_json);
		ulfius_set_string_body_response(httpresponse, e.error_code, e.msg.c_str());
		return U_CALLBACK_CONTINUE;
	}
}

int callback_update_function_code(const struct _u_request * httprequest, struct _u_response * httpresponse, void * user_data) {
	fprintf(stdout, "\n\nPOST REQUEST: callback_update_function_code\n");
	json_t* req_json = json_loadb((const char *)httprequest->binary_body, httprequest->binary_body_length, 0, NULL);
	json_dumpfd(req_json, 1, JSON_INDENT(4));
	fprintf(stdout, "\n");

	// guard against the function locking.
	std::lock_guard<std::mutex> guard(funcMgr->create_function_lock);

	try {
		fprintf(stdout, "decoding update function code request\n");
		const char *funcname = u_map_get(httprequest->map_url, "name");
		
		fprintf(stdout, "create function: %s\n", funcname);

		if (!FunctionProperties::validateFunctionName(funcname)) {
			fprintf(stderr, "Fatal error: bad function name\n");
			throw AWSError(400, "InvalidParameterValueException");
		}

		if (!funcMgr->functionExists(funcname)) {
			fprintf(stderr, "Fatal error: no such function\n");
			throw AWSError(404, "ResourceNotFoundException");
		}
		
		std::shared_ptr<FunctionProperties> func = std::make_shared<FunctionProperties>(*(funcMgr->getFunction(funcname)));

		// decode the zip file
		fprintf(stdout, "decoding the source zip file and writing it to disk\n");
		char sha256src[65];
	
		// NOTE: this needs to be updated to remove the entire directory on failure
		const char *b64_encoded_func_zip = json_string_value(json_object_get(req_json, "ZipFile"));
		if (!b64_encoded_func_zip) {
			fprintf(stderr, "Fatal error: ZipFile not provided\n");
			throw AWSError(400, "InvalidParameterValueException");
		}

		char zipfilepath[PATH_MAX];
		int code_size = zip_from_base64_string(b64_encoded_func_zip, zipfilepath, sha256src);

		func->src_zip_path = zipfilepath;
		func->src_zip_sha256 = sha256src;

		json_object_del(req_json, "Code");
		json_object_set_new(req_json, "CodeSha256", json_string(sha256src));
		json_object_set_new(req_json, "CodeSize", json_integer(code_size));
		fprintf(stdout, "\tdecoded and wrote out zipfile, hash: %s\n", sha256src);

		// done creating the function, write it out
		fprintf(stdout, "now adding the function to the table");
		funcMgr->addFunction(func);

		const char *res_json_str = json_dumps(req_json, 0);
		if (res_json_str == NULL) {
			throw AWSError(500, "failed to stringify req json");
		}

		ulfius_set_string_body_response(httpresponse, 200, res_json_str);
		fprintf(stdout, "Successfully updated function %s!\n", funcname);
		free((void *)res_json_str);
		json_decref(req_json);

		return U_CALLBACK_CONTINUE;
	} catch (const AWSError &e) {
		fprintf(stderr, "Caught error: %s\n", e.msg.c_str());
		json_decref(req_json);
		ulfius_set_string_body_response(httpresponse, e.error_code, e.msg.c_str());
		return U_CALLBACK_CONTINUE;
	}
}

int callback_function_delete(const struct _u_request * httprequest, struct _u_response * httpresponse, void * user_data) {
	fprintf(stdout, "\n\nPOST REQUEST: callback_function_delete\n");
	json_t* req_json = json_loadb((const char *)httprequest->binary_body, httprequest->binary_body_length, 0, NULL);
	json_dumpfd(req_json, 1, JSON_INDENT(4));
	fprintf(stdout, "\n");

	// guard against the function locking.
	std::lock_guard<std::mutex> guard(funcMgr->create_function_lock);

	try {
		fprintf(stdout, "decoding delete function code request\n");
		const char *funcname = u_map_get(httprequest->map_url, "name");
		
		fprintf(stdout, "delete function: %s\n", funcname);

		if (!FunctionProperties::validateFunctionName(funcname)) {
			fprintf(stderr, "Fatal error: bad function name\n");
			throw AWSError(400, "InvalidParameterValueException");
		}

		if (!funcMgr->functionExists(funcname)) {
			fprintf(stderr, "Fatal error: no such function\n");
			throw AWSError(404, "ResourceNotFoundException");
		}

		funcMgr->removeFunction(funcname);

		ulfius_set_string_body_response(httpresponse, 204, "");
		fprintf(stdout, "deleted function %s successfully\n", funcname);
		return U_CALLBACK_CONTINUE;
	} catch (const AWSError &e) {
		fprintf(stderr, "Caught error: %s\n", e.msg.c_str());
		json_decref(req_json);
		ulfius_set_string_body_response(httpresponse, e.error_code, e.msg.c_str());
		return U_CALLBACK_CONTINUE;
	}
}

/*
	API request handler for callback_function_invoke
*/
int callback_function_invoke (const struct _u_request * httprequest, struct _u_response * httpresponse, void * user_data) {
	fprintf(stdout, "\n\nPOST REQUEST: callback_function_invoke\n");
	
	json_t* req_json = json_loadb((char *)httprequest->binary_body, httprequest->binary_body_length, 0, NULL);

	const char *funcname;
	std::shared_ptr<const FunctionProperties> func = nullptr;
	bool request_response = false;

	try {
		if (!req_json) {
			throw AWSError(400, "InvalidRequestContentException");
		}

		funcname = u_map_get(httprequest->map_url, "name");
		fprintf(stdout, "invoking function: %s\n", funcname);
		if (!FunctionProperties::validateFunctionName(funcname)) {
			fprintf(stderr, "Fatal error: bad function name\n");
			throw AWSError(400, "InvalidParameterValueException");
		}

		if (!funcMgr->functionExists(funcname)) {
			fprintf(stderr, "Fatal error: no such function\n");
			throw AWSError(404, "ResourceNotFoundException");
		}

		// generate resultwoof if needed
		const char *invocation_type = u_map_get(httprequest->map_header, "X-Amz-Invocation-Type") ;
		if (!invocation_type) 
			invocation_type = "RequestResponse";
		
		request_response = strcmp(invocation_type, "RequestResponse") == 0;

		fprintf(stdout, "getting function object for function %s\n", funcname);
		func = funcMgr->getFunction(funcname);
		fprintf(stdout, "installing function if not already installed %s\n", funcname);
		if (!func->isInstalled()) {
			fprintf(stdout, "\tfunction not installed, installing\n");
			funcMgr->installFunction(func);
			func = funcMgr->getFunction(funcname);
		}

	} catch (const AWSError &e) {
		fprintf(stderr, "Caught error: %s\n", e.msg.c_str());
		json_decref(req_json);
		ulfius_set_string_body_response(httpresponse, e.error_code, e.msg.c_str());
		return U_CALLBACK_CONTINUE;
	}

	std::shared_ptr<FunctionInstallation> installation = func->installation;
	WP *wp = installation->wp;
	struct ResultWooF resultwoof;

	try {
		if (request_response) {
			queue_get(&(installation->result_woof_queue), &resultwoof);
			
			WPJob* theJob = create_job_easy(wp, wpcmd_woofgetlatestseqno);
			struct wpcmd_woofgetlatestseqno_arg *arg = 
				(struct wpcmd_woofgetlatestseqno_arg *)(theJob->arg = bp_getchunk(funcMgr->bp_jobobject_pool));
			strcpy(arg->woofname, resultwoof.woofname);
			int retval = wp_job_invoke(wp, theJob);
			bp_freechunk(funcMgr->bp_jobobject_pool, (void *)arg);
			if (retval < 0) {
				fprintf(stderr, "Fatal error: failed to get the seqno from the namespace\n");
				throw AWSError(500, "ServiceException");
			}

			resultwoof.seqno = retval;
		}

		fprintf(stdout, "function invocation options:\n"
			"\tfunction name: %s\n"
			"\tfunction dir: %s\n"
			"\tresult woof: %s (seqno: %d)\n"
			"\trequest response: %d\n",
			funcname, installation->install_path.c_str(), 
			request_response ? resultwoof.woofname : NULL, 
			request_response ? resultwoof.seqno : -1, 
			request_response);
		
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
			json_object_set_new(metadata, "metadata", func->dumpJson());

			const char *payload_str = json_dumps(req_json, JSON_COMPACT);
			json_decref(req_json);
			const char *metadata_str = json_dumps(metadata, JSON_COMPACT);
			json_decref(metadata);

			if (payload_str == NULL) {
				free((void *)payload_str);
				throw AWSError(500, "ServiceException");
			}

			if (metadata_str == NULL) {
				free((void *)payload_str);
				fprintf(stderr, "Fatal error: failed to dump the payload_str as JSON\n");
				throw AWSError(500, "ServiceException");
			}

			int payload_str_len = strlen(payload_str);
			int metadata_str_len = strlen(metadata_str);
			if (payload_str_len + metadata_str_len + 2 > CALL_WOOF_EL_SIZE) {
				// the request was too large to fit in the buffer
				free((void *)payload_str);
				free((void *)metadata_str);
				throw AWSError(500, "ServiceException");
			}

			woofputbuffer = (char *)malloc(CALL_WOOF_EL_SIZE); 
			if (woofputbuffer == NULL) {
				free((void *)payload_str);
				free((void *)metadata_str);
				throw AWSError(500, "ServiceException");
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
				(struct wpcmd_woofput_arg *)(theJob->arg = bp_getchunk(funcMgr->bp_jobobject_pool));
			strcpy(arg->woofname, CALL_WOOF_NAME); // set the woofname
			strcpy(arg->handlername, "awspy_lambda"); // set the handlername
			char *sharedbuff = (char *)bp_getchunk(funcMgr->bp_job_bigstringpool);
			arg->payload = sharedbuff; // set the payload
			memcpy(sharedbuff, woofputbuffer, CALL_WOOF_EL_SIZE);
			free(woofputbuffer);
			
			fprintf(stdout, "Invoking WooFPut command\n");
			int retval = wp_job_invoke(wp, theJob);

			bp_freechunk(funcMgr->bp_jobobject_pool, (void *)arg);
			bp_freechunk(funcMgr->bp_job_bigstringpool, (void *)sharedbuff);
			if (retval < 0) {
				fprintf(stderr, "Fatal error: failed to put the invocation in WooF '%s'\n", CALL_WOOF_NAME);
				throw AWSError(500, "ServiceException");
			}
			
			fprintf(stdout, "Put invocation in WooF '%s' at idx: %d\n", CALL_WOOF_NAME, retval);
		}
		
		if (request_response) {
			fprintf(stdout, "Result Woof was defined, so we are spinning until the result is available\n");

			WPJob* theJob = create_job_easy(wp, wpcmd_waitforresult);
			struct wpcmd_waitforresult_arg *arg = 
				(struct wpcmd_waitforresult_arg *)(theJob->arg = bp_getchunk(funcMgr->bp_jobobject_pool));
			char *result = (char *)(theJob->result = bp_getchunk(funcMgr->bp_job_bigstringpool));
			arg->timeout = 30000L;
			memcpy(&(arg->resultwoof), &resultwoof, sizeof(struct ResultWooF));
			
			int retval = wp_job_invoke(wp, theJob);
			bp_freechunk(funcMgr->bp_jobobject_pool, (void *)arg);
			
			if (retval < 0) {
				if (request_response)
					queue_put(&(installation->result_woof_queue), &resultwoof);
				
				bp_freechunk(funcMgr->bp_job_bigstringpool, (void *)result);
				fprintf(stderr, "Fatal error: failed to get result from lambda invocation, timed out or other error encountered\n");
				ulfius_set_string_body_response(httpresponse, 200, "{\"error\": \"function timed out\"}"); // TODO: improve this message to make it match that which you would get from AWS
				return U_CALLBACK_CONTINUE;
			}

			fprintf(stdout, "finished waiting for the result\n");
			resultwoof.seqno = retval;
			queue_put(&(installation->result_woof_queue), &resultwoof);

			ulfius_set_string_body_response(httpresponse, 200, result);
			bp_freechunk(funcMgr->bp_job_bigstringpool, (void *)result);
			return U_CALLBACK_CONTINUE;
		}
		
		ulfius_set_string_body_response(httpresponse, 200, "{\"status\": \"ok\"}");
		return U_CALLBACK_CONTINUE;

	} catch (const AWSError &e) {
		fprintf(stderr, "Caught error: %s\n", e.msg.c_str());
		if (request_response) 
			queue_put(&(installation->result_woof_queue), &resultwoof);
		
		json_decref(req_json);
		ulfius_set_string_body_response(httpresponse, e.error_code, e.msg.c_str());
		return U_CALLBACK_CONTINUE;
	}
}

void sig_handler(int sig) {
	switch (sig) {
	case SIGINT:
		fprintf(stderr, "\n\nCAUGHT SIGNAL: shutting down\n");
		delete funcMgr;
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

	funcMgr = new FunctionManager;
	funcMgr->install_base_dir = "./functions/installs";
	funcMgr->metadata_base_dir = "./functions/metadata";

	// Setup the functions directory
	struct stat st = {0};
	if (stat("./functions", &st) == -1) {
		fprintf(stdout, "Created functions directory.\n");
		mkdir("./functions", 0700);
		mkdir("./functions/installs", 0700);
		mkdir("./functions/metadata", 0700);
		mkdir("./functions/zips", 0700);
	}

	// Initialize instance with the port number
	if (ulfius_init_instance(&instance, PORT, NULL, NULL) != U_OK) {
		fprintf(stderr, "Error ulfius_init_instance, abort\n");
		return(1);
	}
	
	ulfius_add_endpoint_by_val(&instance, "POST", "/2015-03-31/", "/functions", 0, &callback_function_create, NULL);
	ulfius_add_endpoint_by_val(&instance, "POST", "/2015-03-31/", "/functions/:name/invocations", 0, &callback_function_invoke, NULL);
	ulfius_add_endpoint_by_val(&instance, "PUT", "/2015-03-31/", "/functions/:name/code", 0, &callback_update_function_code, NULL);
	ulfius_add_endpoint_by_val(&instance, "DELETE", "/2015-03-31/", "/functions/:name", 0, &callback_function_delete, NULL);

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
