#include <linux/limits.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <libgen.h>
#include <string.h>
#include <Python.h>

#include "woofc.h"
#include "3rdparty/json.h"

#include "constants.h"

#define NAMESPACE 

#define DEBUG

#ifdef DEBUG 
#define fdebugf(args...) fprintf(args)
#else
#define fdebugf(args...) 
#endif

struct InvocationArguments {
	const char *function_name;
	const char *payload_str;
	const char *result_woof;
};


struct FunctionMetadata {
	const char *function_directory;
	const char *function_name; // the name of the lambda
	const char *handler_name; // the name of the handler as provided in the metadata,
	                          // null if not defined, then the following values get defaults
	const char *python_package_name; // the name of the python package
	const char *python_function_name; // the name of the function in the python package
};


// helper function
json_value *json_object_value_of_key(json_value *obj, const char *key) {
	long idx;
	for (idx = 0; idx < obj->u.object.length; idx++) {
		if (strcmp(key, obj->u.object.values[idx].name) == 0)
			return obj->u.object.values[idx].value;
	}
	return NULL;
}

const char *json_get_str_for_key(json_value *obj, const char *key) {
	json_value *value = json_object_value_of_key(obj, key);
	if (value == NULL) 
		return NULL;
	if (value->type != json_string)
		return NULL;
	return value->u.string.ptr;
}

json_value *read_json_file(const char *fname) {
	char *file_content = NULL;
	FILE *f = fopen(fname, "r");
	if (f == NULL) {
		fdebugf(stdout, "Fatal error: file '%s' did not exist\n", fname);
		return NULL;
	}

	// determine the file length
	fseek(f, 0, SEEK_END);
	long file_length = ftell(f);
	fseek(f, 0, SEEK_SET);

	// read & null terminate the file
	file_content = malloc(file_length);
	fread(file_content, 1, file_length, f);

	// parse the json data
	json_value *parsed_json = json_parse(file_content, file_length);
	free(file_content);

	return parsed_json;
}

struct FunctionMetadata* load_function_metadata(const char *ns, const char *function_name) {
	char func_defn_path[PATH_MAX];
	sprintf(func_defn_path, "%s/%s-metadata.json", ns, function_name);
	fdebugf(stdout, "function metadata directory: %s\n", func_defn_path);
	json_value *func_metadata = read_json_file(func_defn_path);

	if (!func_metadata) {
		fdebugf(stderr, "Fatal error: function metadata corrupted or file did not exist\n");
		return NULL;
	}

	struct FunctionMetadata *metadata = malloc(sizeof(struct FunctionMetadata));
	
	metadata->function_directory = malloc(PATH_MAX);
	sprintf((char *)metadata->function_directory, "%s/", ns);
	metadata->function_name = strdup(json_get_str_for_key(func_metadata, "FunctionName"));
	metadata->handler_name = json_get_str_for_key(func_metadata, "Handler");
	if (metadata->handler_name) {
		metadata->handler_name = strdup(metadata->handler_name);

		const char* dot_position = strchr(metadata->handler_name, '.');
		if (dot_position == NULL) {
			fdebugf(stderr, "Fatal error: malformatted handler, expected . but handler was: %s\n", metadata->handler_name);
			free(metadata);
			return NULL;
		}

		metadata->python_package_name = strndup(metadata->handler_name, dot_position - metadata->handler_name);
		metadata->python_function_name = strdup(dot_position + 1);
	} else {
		metadata->python_function_name = strdup(metadata->function_name);
		metadata->python_package_name = strdup("main");
	}

	return metadata;
}

void free_function_metadata(struct FunctionMetadata *metadata) {
	free((void *)metadata->function_directory);
	free((void *)metadata->function_name);
	if (metadata->handler_name != NULL)
		free((void *)metadata->handler_name);
	free((void *)metadata->python_package_name);
	free((void *)metadata->python_function_name);
	free((void *)metadata);
}

int create_python_helpers() {
	// see: https://stackoverflow.com/questions/3286448/calling-a-python-method-from-c-c-and-extracting-its-return-value 
	// for the work around used to avoid creating a temporary file

	PyObject *pModuleMain = PyImport_Import(PyUnicode_DecodeFSDefault("__main__"));
	if (!pModuleMain) {
		fdebugf(stderr, "Fatal error: failed to get a handle on the main module __main__\n");
		fflush(stderr);
		exit(1);
	}

	fdebugf(stdout, "Initializing the helper functions used to manage the payload and process the result\n");
	fflush(stdout);
	PyRun_SimpleString(
		"import json as _json \n"
		// "print(_json.dumps({'hello': 'world'}, indent=2))\n"
		"def __pylambda_parse_payload(payload_str):\n"
		"	parsed = _json.loads(payload_str)\n"
		// "	print(_json.dumps(parsed, indent=2))\n"
		"	return parsed, {}\n"
		"def __pylambda_package_result(result):\n"
		"	return _json.dumps(result)\n"
	);
	fdebugf(stdout, "now invoking the lambda\n");
	fflush(stdout);
}

PyObject* get_python_method(PyObject *module, const char *method_name) {
	PyObject *method = PyObject_GetAttrString(module, method_name);

	if (!method || !PyCallable_Check(method)) {
		fdebugf(stderr, "Fatal error: attempted to get function, but got none or a non-callable\n");
		return NULL;
	}
	
	return method;
}


int awspy_lambda(WOOF *wf, unsigned long seq_no, void *ptr) {
	struct InvocationArguments arguments;
	struct FunctionMetadata *function_metadata;
	
	char *input_data_buffer = ptr;

	// STEP 1) determine the location of the namespace we are running from 
	//         and change the working directory
	const char *ns = getenv("WOOFC_DIR");
	chdir(ns); // NOTE at the moment this gets immeditaely overriden once we determine the function
	fdebugf(stdout, "awspy_lambda invocation starting:\n");
	fdebugf(stdout, "namespace directory: %s\n", getenv("WOOFC_DIR"));

	// STEP 2) decode the function name & json data sections
	fdebugf(stdout, "metadata: %s\n", input_data_buffer);
	json_value *json_data = json_parse(input_data_buffer, strlen(input_data_buffer));

	if (!json_data) {
		fdebugf(stderr, "Fatal error: failed to decode the JSON payload\n");
		return -1;
	}

	// read a few keys from the json data and initialize the arguments struct 

	arguments.function_name = json_get_str_for_key(json_data, "function");
	arguments.result_woof = json_get_str_for_key(json_data, "result_woof");
	arguments.payload_str = input_data_buffer + strlen(input_data_buffer) + 1;
	fdebugf(stdout, "the payload is: %s\n", arguments.payload_str);

	if (!arguments.function_name || !arguments.payload_str) {
		fdebugf(stderr, "Fatal error: invocation json_data is missing a required property 'function_name' or 'payload_str'\n");
		return -1;
	}

	fdebugf(stdout,
		"arguments:\n"
		"\tfunction name: %s\n"
		"\tresult woof: %s\n"
		"\tpayload.length: %d\n",
		arguments.function_name,
		arguments.result_woof ? arguments.result_woof : "<none>",
		strlen(arguments.payload_str)
		);


	// load the function's metadata
	if ((function_metadata = load_function_metadata(ns, arguments.function_name)) == NULL) {
		fdebugf(stderr, "Fatal error encountered while loading metadata, aborting");
		return -1;
	}
	assert(function_metadata->function_name == arguments.function_name);
	fdebugf(stdout, 
		"metadata:\n"
		"\tfunction name: %s\n"
		"\tfunction directory: %s\n"
		"\thandler: %s\n"
		"\tpython package: %s\n"
		"\tpython function: %s\n", 
		function_metadata->function_name,
		function_metadata->function_directory,
		function_metadata->handler_name ? function_metadata->handler_name : "<none>",
		function_metadata->python_package_name,
		function_metadata->python_function_name);

	//
	// BEGIN INTERACTING WITH PYTHON BY SETTING UP OUR HELPER FUNCTIONS
	//
	if (chdir(function_metadata->function_directory) < 0) {
		fdebugf(stderr, "Fatal error: failed to change directory to the function directory: '%s'\n", 
			function_metadata->function_directory);
		return -1;
	}

	// update the PYTHONPATH to include the current working directory
	{
		const char *pypath = getenv("PYTHONPATH");
		char newpypath[1024];
		sprintf(newpypath, "PYTHONPATH=%s:.", pypath);
		putenv(newpypath);
	}
	fdebugf(stdout, "updated python path to include the current directory\n");

	wchar_t *program_name = Py_DecodeLocale("main.py", NULL);
	Py_SetProgramName(program_name);
	Py_Initialize();

	PyObject *main_module = PyImport_Import(PyUnicode_DecodeFSDefault("__main__"));
	if (!main_module) {
		fdebugf(stderr, "Fatal error: failed to get a handle on the main module __main__\n");
		fflush(stderr);
		return -1;
	}

	fdebugf(stdout, "initializing helper functions\n");
	create_python_helpers();
	PyObject *pyfunc_parse_payload = get_python_method(main_module, "__pylambda_parse_payload");
	PyObject *pyfunc_package_result = get_python_method(main_module, "__pylambda_package_result");
	if (!pyfunc_parse_payload || !pyfunc_package_result) {
		fdebugf(stderr, "Failed to get one of the helper functions, aborting\n");
		return -1;
	}

	fdebugf(stdout, "getting a handle on the handler: %s.%s\n", 
		function_metadata->python_package_name, 
		function_metadata->python_function_name);

	PyObject *lambda_module = PyImport_Import(PyUnicode_DecodeFSDefault(function_metadata->python_package_name));
	if (!lambda_module) {
		fdebugf(stderr, "Fatal error: the module '%s' did not exist or failed to load\n", 
			function_metadata->python_package_name);
		return -1;
	}
	PyObject *lambda_function = get_python_method(lambda_module, function_metadata->python_function_name);
	if (!lambda_function) {
		fdebugf(stderr, 
			"Fatal error: the function '%s' could not be found in package '%s'\n", 
			function_metadata->python_function_name, function_metadata->python_package_name);
		return -1;
	}

	// Packaging the arguments for the lambda function
	fdebugf(stdout, "parsing the payload\n");
	PyObject *args = PyTuple_New(1);
	PyTuple_SetItem(args, 0, PyUnicode_DecodeFSDefault(arguments.payload_str));
	PyObject *py_payload = PyObject_CallObject(pyfunc_parse_payload, args);
	Py_DECREF(args);
	if (!py_payload) {
		PyErr_Print();
		fdebugf(stderr, "Fatal error: failed to JSON decode the payload");
		return -1;
	}
	fdebugf(stdout, "\tparsed payload: ");
#ifdef DEBUG 
	PyObject_Print(py_payload, stdout, Py_PRINT_RAW);
#endif 
	fdebugf(stdout, "\n");

	// Invoking the lambda function
	fdebugf(stdout, "invoking the lambda\n");
	args = py_payload;
	PyObject *py_lambda_result = PyObject_CallObject(lambda_function, args);
	// NO DECREF here b/c we just copied it
	fdebugf(stdout, "\tlambda result: ");
#ifdef DEBUG
	PyObject_Print(py_lambda_result, stdout, Py_PRINT_RAW);
#endif
	fdebugf(stdout, "\n");

	// Invoking the packaging function
	args = PyTuple_New(1);
	Py_INCREF(py_lambda_result);
	PyTuple_SetItem(args, 0, py_lambda_result); // SetItem does not aquire a ref, it steals one
	PyObject *py_packaged_result = PyObject_CallObject(pyfunc_package_result, args);
	if (!py_packaged_result) {
		PyErr_Print();
		fdebugf(stderr, "Fatal error: failed to JSON encode the result\n");
		return -1;
	}

	//
	// finally, write the packaged result out to the 'result woof'
	//

	PyObject *py_packaged_result_unicode = PyUnicode_AsEncodedString(py_packaged_result, "utf-8", "~E~");
	const char *result_str = PyBytes_AS_STRING(py_packaged_result_unicode);
	fdebugf(stdout, "\tpackaged result: %s\n", result_str);
	Py_DECREF(py_packaged_result_unicode);

	if (arguments.result_woof) {
		if (strlen(result_str) > RESULT_WOOF_EL_SIZE) {
			fdebugf(stderr, "Fatal error: result object can not fit in ELEMENT_SIZE (%d bytes)\n", RESULT_WOOF_EL_SIZE);
			return -1;
		}

		char result_element_buffer[RESULT_WOOF_EL_SIZE];
		memset(result_element_buffer, 0, RESULT_WOOF_EL_SIZE);
		strcpy(result_element_buffer, result_str);

		int idx = WooFPut((char *)arguments.result_woof, NULL, result_element_buffer);
		if (idx < 0) {
			fdebugf(stderr, "Fatal error: failed to place the result in the woof specified\n");
			return -1;
		}
		fdebugf(stdout, "Result successfully placed in result woof: '%s'\n", arguments.result_woof);
	} else {
		fdebugf(stdout, "No result woof specified. Nothing to do with the result, discarding it\n");
	}

	// free our memory
	Py_DECREF(py_packaged_result);
	Py_DECREF(py_lambda_result);
	Py_DECREF(py_payload);
	Py_DECREF(main_module);
	Py_DECREF(lambda_module);
	Py_DECREF(lambda_function);
	Py_DECREF(pyfunc_parse_payload);
	Py_DECREF(pyfunc_package_result);

	if (Py_FinalizeEx() < 0) {
		return 120;
	}
	PyMem_RawFree(program_name);

	free_function_metadata(function_metadata);
	
	fdebugf(stdout, "Done.\n");

	return 0;
}
