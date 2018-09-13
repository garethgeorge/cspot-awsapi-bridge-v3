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



std::mutex used_function_name_lock;
std::unordered_set<std::string> used_function_names;
std::mutex lambda_functions_lock;
std::unordered_map<std::string, std::shared_ptr<FunctionProperties>> lambda_functions;

/*************************************************
    Function Properties method implementations
 *************************************************/

FunctionProperties::~FunctionProperties() {
	if (this->installation)
		delete this->installation;
}


void FunctionProperties::setSrcZipPath(std::string &zip_path) {
	this->src_zip_path = zip_path;

	char output_sha256[65];
	if (sha256_file((char *)zip_path.c_str(), output_sha256) != 0) {
		throw AWSError(500, "Failed to calculate SHA256 hash");
	}

	this->src_zip_sha256 = std::string(output_sha256);
}

FunctionProperties FunctionProperties::installFunction() const {
	FunctionProperties copy = *this;
	char install_path[2048];
	snprintf(install_path, sizeof(install_path), "./function_installs/%s-%s", 
		this->name.c_str(), this->src_zip_sha256.c_str());
	
	copy.installation = new FunctionInstallation(copy, install_path);

	return std::move(copy);
}

bool FunctionProperties::validateFunctionName(const char *str) {
	while (*str != '\0') {
		if (!((*str >= '0' && *str <= '9') || (*str >= 'a' && *str <= 'z') || (*str >= 'A' && *str <= 'Z') || *str == '-' || *str == '_')) return true;
		str++;
	}
	return false;
}

/*************************************************
    Function Installation method implementations
 *************************************************/
FunctionInstallation::FunctionInstallation(const FunctionProperties& func, const std::string& path)
		: function(&func), install_path(path) {
	
	FunctionManager *mgr = this->getManager();

	// create the directory if it does not exist
	if (mkdir(path.c_str(), 0700) != 0) {
		throw AWSError(500, "failed to create the directory for the function installation");
	}

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

	WP *wp = this->wp = new WP;
	if (init_wp(wp, WORKER_QUEUE_DEPTH, wphandler_array, 4) < 0) {
		throw AWSError(500, "failed to create the worker process");
	}

	// change the workdir of the worker process to the correct directory & call WooFInit
	sleep(1); // TODO: find a better way of avoiding the race with the ns process startup 
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
}

FunctionInstallation::~FunctionInstallation() {
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

bool FunctionManager::function_exists(const char *funcname) {
	this->lambda_functions_lock.lock();
	if (lambda_functions.find(funcname) != lambda_functions.end()) {
		this->lambda_functions_lock.unlock();
		return true;
	}
	this->lambda_functions_lock.unlock();

	if (!FunctionProperties::validateFunctionName(funcname)) {
		return false;
	}

	char path[PATH_MAX];
	this->metadata_path_for_function(funcname, path);

	return access(path, F_OK) != -1;
}

void FunctionManager::remove_function(const char *funcname) {
	this->lambda_functions.erase(funcname);
	char path[PATH_MAX];
	this->metadata_path_for_function(funcname, path);
	remove(path);
}