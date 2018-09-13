#include <cstring>

#include "function_helpers.hpp"
#include "sha256_util.hpp"

#include <lib/fsutil.hpp>

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <linux/limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <iostream>
#include <cassert>

#include <src/constants.h>

std::mutex used_function_name_lock;
std::unordered_set<std::string> used_function_names;
std::mutex lambda_functions_lock;
std::unordered_map<std::string, std::shared_ptr<FunctionProperties>> lambda_functions;

/*************************************************
    Function Properties method implementations
 *************************************************/

FunctionProperties::~FunctionProperties() {
	
}

FunctionProperties::FunctionProperties(const FunctionProperties& orig, std::shared_ptr<FunctionInstallation> install) {
	*this = orig;
	this->installation = install;
}


FunctionProperties::FunctionProperties(json_t *json) {
	const char *name = json_string_value(json_object_get(json, "FunctionName"));
	if (name == nullptr)
		throw AWSError(500, "failed to find name in metadata");
	this->name = name;

	const char *handler = json_string_value(json_object_get(json, "Handler"));
	if (handler == nullptr)
		throw AWSError(500, "failed to find handler in metadata");
	this->handler = handler;

	const char *src_zip_path = json_string_value(json_object_get(json, "src_zip_path"));
	if (src_zip_path == nullptr)
		throw AWSError(500, "failed to find src_zip_path in metadata");
	this->src_zip_path = src_zip_path;

	const char *src_zip_sha256 = json_string_value(json_object_get(json, "src_zip_sha256"));
	if (src_zip_sha256 == nullptr)
		throw AWSError(500, "failed to find src_zip_sha256 in metadata");
	this->src_zip_sha256 = src_zip_sha256;
}

json_t *FunctionProperties::dumpJson() const {
	json_t *json = json_object(); // new object with ref count 1
	json_object_set_new(json, "FunctionName", json_string(this->name.c_str()));
	json_object_set_new(json, "Handler", json_string(this->handler.c_str()));
	json_object_set_new(json, "src_zip_path", json_string(this->src_zip_path.c_str()));
	json_object_set_new(json, "src_zip_sha256", json_string(this->src_zip_sha256.c_str()));
	return json;
}

void FunctionProperties::setSrcZipPath(std::string &zip_path) {
	this->src_zip_path = zip_path;

	char output_sha256[65];
	if (sha256_file((char *)zip_path.c_str(), output_sha256) != 0) {
		throw AWSError(500, "Failed to calculate SHA256 hash");
	}

	this->src_zip_sha256 = std::string(output_sha256);
}

std::shared_ptr<FunctionProperties> FunctionProperties::installFunction() const {
	FunctionManager *mgr = this->getManager();
	assert(mgr != NULL);

	FunctionProperties copy = *this;
	char install_path[2048];
	snprintf(install_path, sizeof(install_path), "%s/%s-%s", mgr->install_base_dir.c_str(),
		this->name.c_str(), this->src_zip_sha256.c_str());
	
	std::shared_ptr<FunctionInstallation> install = std::make_shared<FunctionInstallation>(copy, install_path);
	return std::make_shared<FunctionProperties>(*this, std::move(install));
}

bool FunctionProperties::validateFunctionName(const char *str) {
	while (*str != '\0') {
		if (!((*str >= '0' && *str <= '9') || (*str >= 'a' && *str <= 'z') || (*str >= 'A' && *str <= 'Z') || *str == '-' || *str == '_')) return false;
		str++;
	}
	return true;
}

/*************************************************
    Function Installation method implementations
 *************************************************/

static int copy_file(const char *dstfilename, const char *srcfilename, int perms) {
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

FunctionInstallation::FunctionInstallation(const FunctionProperties& func, const std::string& path)
		: function(&func), install_path(path) {
	
	// initialize shared queue's 
	queue_init(&(this->result_woof_queue), sizeof(struct ResultWooF), RESULT_WOOF_COUNT);

	FunctionManager *mgr = this->getManager();

	std::cout << "cleaning out install location if it already exists" << std::endl;
	rmrfdir(this->install_path.c_str());

	// create the directory if it does not exist
	std::cout << "function install: creating directory for the function" << std::endl;
	if (mkdir(path.c_str(), 0700) != 0) {
		char error[1024];
		snprintf(error, sizeof(error), "failed to create directory %s for the function installation", path.c_str());
		throw AWSError(500, error);
	}

	// unzip the source code for the function
	std::cout << "function install: unzipping the function source code" << std::endl;
	{
		char unzip_command[PATH_MAX + 128];
		sprintf(unzip_command, "/usr/bin/unzip '%s' -d %s", this->function->src_zip_path.c_str(), this->install_path.c_str());
		int retval = system(unzip_command);
		if (retval != 0) {
			throw AWSError(500, "unzip exited with non-zero return code, extraction was unsuccessful\n");
		}
	}

	// copy the binary's into the directory
	char buffer1[PATH_MAX];
	char buffer2[PATH_MAX];
	char buffer3[PATH_MAX];
	fprintf(stdout, "copying binaries into installation directory %s\n", this->install_path.c_str());
	sprintf(buffer1, "%s/woofc-namespace-platform", this->install_path.c_str());
	sprintf(buffer2, "%s/woofc-container", this->install_path.c_str());
	sprintf(buffer3, "%s/awspy_lambda", this->install_path.c_str());
	if (copy_file(buffer1, "./woofc-namespace-platform", 777) < 0 ||
		copy_file(buffer2, "./woofc-container", 777) < 0 ||
		copy_file(buffer3, "./awspy_lambda", 777) < 0) {
		
		throw AWSError(500, "failed to copy one of the binaries into the installation namespace");
	}

	// spawn the woofc-namespace-platform process
	std::cout << "function install: fork woofc-namespace-platform" << std::endl;
	int nspid = this->woofcnamespace_pid = fork();
	if (nspid < 0) {
		throw AWSError(500, "failed to fork the woofcnamespace platform");
	} else if (nspid == 0) {
		fprintf(stdout, "Subprocess: created the woofc-namespace-platform child process\n");

		if (chdir(this->install_path.c_str()) != 0) {
			fprintf(stdout, "Subprocess: Fatal error: failed to change to the function directory\n");
			exit(0);
		}

		fprintf(stdout, "WooFCNamespacePlatform running for dir %s\n", this->install_path.c_str());
		execl("./woofc-namespace-platform", "./woofc-namespace-platform", "-m", "4", "-M", "8", NULL);
		exit(0);
	} else {
		fprintf(stdout, "WoofCNamespacePlatform PID: %d\n", nspid);
	}

	sleep(1); // TODO: avoid race conditions, give the woofc-namespace-platform a bit of time to start up

	// spawn the worker process
	std::cout << "function install: creating the worker process" << std::endl;
	WP *wp = this->wp = new WP;
	if (init_wp(wp, WORKER_QUEUE_DEPTH, wphandler_array, 4) < 0) {
		throw AWSError(500, "failed to create the worker process");
	}

	// change the workdir of the worker process to the correct directory & call WooFInit
	{
		WPJob* theJob = create_job_easy(wp, wpcmd_initdir);
		struct wpcmd_initdir_arg *arg = (struct wpcmd_initdir_arg *)bp_getchunk(mgr->bp_jobobject_pool);
		strcpy(arg->dir, this->install_path.c_str());
		theJob->arg = arg;
		int retval = wp_job_invoke(wp, theJob);
		bp_freechunk(mgr->bp_jobobject_pool, (void *)arg);
		if (retval < 0) {
			throw AWSError(500, "failed to change the working directory of the worker process");
		}
		fprintf(stdout, "worker process changed directory to function dir '%s'\n", this->install_path.c_str());
	}
	
	// finally, create the result woof's 
	int i;
	for (i = 0; i < RESULT_WOOF_COUNT; ++i) {
		char woofpath[PATH_MAX];
		char woofname[256];
		sprintf(woofname, "result-%d.woof", i);
		sprintf(woofpath, "%s/%s", this->install_path.c_str(), woofname);

		fprintf(stdout, "seting up resultwoof %s (%s)\n", woofname, woofpath);

		unlink(woofpath);

		{
			fprintf(stderr, "Result WooF '%s' does not exist, making it\n", woofpath);

			WPJob* theJob = create_job_easy(wp, wpcmd_woofcreate);
			struct wpcmd_woofcreate_arg *arg =
				(struct wpcmd_woofcreate_arg *)(theJob->arg = bp_getchunk(mgr->bp_jobobject_pool));
			arg->el_size = RESULT_WOOF_EL_SIZE;
			arg->queue_depth = 1; // result woof only needs to hold one result at a time
			strcpy(arg->woofname, woofname);
			fprintf(stdout, "Creating Result WooF %s\n", woofpath);
			int retval = wp_job_invoke(wp, theJob);
			bp_freechunk(mgr->bp_jobobject_pool, (void *)arg);
			if (retval < 0) {
				char error[1024];
				sprintf(error, "Fatal error: failed to create result woof '%s'\n", woofname);
				throw AWSError(500, error);
			}
		}
		
		WPJob* theJob = create_job_easy(wp, wpcmd_woofgetlatestseqno);
		struct wpcmd_woofgetlatestseqno_arg *arg = 
			(struct wpcmd_woofgetlatestseqno_arg *)(theJob->arg = bp_getchunk(mgr->bp_jobobject_pool));
		strcpy(arg->woofname, woofname);
		int retval = wp_job_invoke(wp, theJob);
		bp_freechunk(mgr->bp_jobobject_pool, (void *)arg);
		if (retval < 0) {
			char error[1024];
			sprintf(error, "Fatal error: failed to get seqno for result woof '%s' retval: %d\n", woofname, retval);
			throw AWSError(500, error);
		}

		// now get the latest seqno and insert it into the table
		struct ResultWooF woof;
		woof.seqno = retval;
		strcpy(woof.woofname, woofname);
		queue_put(&(this->result_woof_queue), &woof);
	}

	// and the invocation woof
	{
		WPJob* theJob = create_job_easy(wp, wpcmd_woofcreate);
		struct wpcmd_woofcreate_arg *arg = 
			(struct wpcmd_woofcreate_arg *)(theJob->arg = bp_getchunk(mgr->bp_jobobject_pool));
		arg->el_size = CALL_WOOF_EL_SIZE;
		arg->queue_depth = CALL_WOOF_QUEUE_DEPTH;
		strcpy(arg->woofname, CALL_WOOF_NAME);

		fprintf(stdout, "Invoking WooFCreate command\n");
		int retval = wp_job_invoke(wp, theJob);

		bp_freechunk(mgr->bp_jobobject_pool, (void *)arg);
		if (retval < 0) {
			char error[1024];
			snprintf(error, sizeof(error), "Fatal error: failed to create the WooF '%s'", CALL_WOOF_NAME);
			std::cout << error << std::endl;
			throw AWSError(500, error);
		}

		fprintf(stdout, "Created the WooF '%s' return code: %d\n", CALL_WOOF_NAME, retval);
	}
}

FunctionInstallation::~FunctionInstallation() {
	std::cout << "cleanup installation for function: '" << this->function->name << "' location: " << this->install_path << std::endl;
	rmrfdir(this->install_path.c_str());
	
	queue_free(&(this->result_woof_queue));
	if (this->wp != NULL)
		free_wp(this->wp);
	if (woofcnamespace_pid != -1)
		kill(this->woofcnamespace_pid, SIGTERM);
}


FunctionManager::FunctionManager() {
	this->bp_jobobject_pool = sharedbuffpool_create(sizeof(union wpcmd_job_data_types), OBJECT_POOL_SIZE);
	this->bp_job_bigstringpool = sharedbuffpool_create(MAX_WOOF_EL_SIZE, OBJECT_POOL_SIZE);
}
	
FunctionManager::~FunctionManager() {
	sharedbuffpool_free(this->bp_jobobject_pool);
	sharedbuffpool_free(this->bp_job_bigstringpool);
}

bool FunctionManager::functionExists(const char *funcname) {
	std::lock_guard<std::mutex> g(this->lambda_functions_lock);
	if (lambda_functions.find(funcname) != lambda_functions.end()) {
		return true;
	}

	if (!FunctionProperties::validateFunctionName(funcname)) {
		return false;
	}
	
	char path[PATH_MAX];
	this->metadata_path_for_function(funcname, path);
	fprintf(stdout, "checking if function metadata exists at path: %s\n", path);
	
	return access(path, F_OK) != -1;
}

void FunctionManager::removeFunction(const char *funcname) {
	std::lock_guard<std::mutex> g(this->lambda_functions_lock);

	this->lambda_functions.erase(funcname);
	char path[PATH_MAX];
	this->metadata_path_for_function(funcname, path);
	remove(path);
}

void FunctionManager::addFunction(std::shared_ptr<FunctionProperties>& func) {
	func->manager = this;

	json_t *json = func->dumpJson();

	char *metadata_str = json_dumps(json, 0);
	json_decref(json);

	if (metadata_str == NULL) {
		throw AWSError(500, "failed to dump the metadata JSON as a string");
	}

	char output_path[PATH_MAX];
	this->metadata_path_for_function(func->name.c_str(), output_path);

	FILE *metadata_file = fopen(output_path, "wb");
	if (!metadata_file) {
		free(metadata_str);
		throw AWSError(500, "failed to open metadata file for writing");
	}

	fprintf(stdout, "wrote out metadata file %s\n", output_path);
	if (fwrite(metadata_str, strlen(metadata_str), 1, metadata_file) < 0) {
		free(metadata_str);
		throw AWSError(500, "failed to write the metadata file");
	}

	fclose(metadata_file);
	free(metadata_str);

	std::lock_guard<std::mutex> g(this->lambda_functions_lock);
	lambda_functions[func->name] = func;
}

std::shared_ptr<const FunctionProperties> FunctionManager::getFunction(const char *funcname) {
	std::lock_guard<std::mutex> g(this->lambda_functions_lock);

	auto result = this->lambda_functions.find(funcname);
	if (result != this->lambda_functions.end()) {
		return result->second;
	}
	
	char metadata_path[PATH_MAX];
	this->metadata_path_for_function(funcname, metadata_path);

	json_t *metadata = json_load_file(metadata_path, 0, NULL);
	if (metadata == NULL) {
		throw AWSError(500, "failed to read the metadata file, function probably corrupted");
	}

	try {
		auto func = std::make_shared<FunctionProperties>(metadata);
		func->manager = this;
		json_decref(metadata);

		this->lambda_functions[funcname] = func;
		return func;
	} catch (const AWSError& e) {
		json_decref(metadata);
		throw e;
	}
}

void FunctionManager::installFunction(std::shared_ptr<const FunctionProperties>& func) {
	if (func->isInstalled()) 
		return ;
	std::lock_guard<std::mutex> g(this->create_function_lock);
	std::shared_ptr<const FunctionProperties> funcNow = this->getFunction(func->name.c_str());

	// check if another thread jumped in and installed the function before we got to hold the create function lock
	if (!funcNow->isInstalled()) {
		// finally, install the function while holding the lock
		std::lock_guard<std::mutex> g2(this->lambda_functions_lock);
		this->lambda_functions[funcNow->name] = funcNow->installFunction();
	}
}