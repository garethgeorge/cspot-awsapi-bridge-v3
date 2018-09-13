#ifndef FUNCTIONHELPERS_HPP
#define FUNCTIONHELPERS_HPP

#include <3rdparty/json_fwd.hpp>

#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <string>
#include <exception>
#include <memory>

#include <lib/utility.h>
#include <src/constants.h>
#include "wpcmds.h"

// class for thrown errors
struct AWSError : public std::exception {
protected:
	int error_code;
	std::string msg;

public:
	AWSError(int error_code, const char *message) : AWSError(error_code, std::string(message)) { };
	AWSError(int error_code, const std::string&& message) : error_code(error_code), msg(message) { };
};

struct FunctionProperties;
struct FunctionInstallation;
struct FunctionManager;

// class for managing the properties of functions we create
struct FunctionProperties { // the virtual representation of the function
	FunctionManager *manager;
	
	std::string name;
	std::string handler;
	std::string src_zip_path;
	std::string src_zip_sha256;
	FunctionInstallation* installation;
	
	~FunctionProperties();

	void setSrcZipPath(std::string& path);

	bool isInstalled() {
		return installation != nullptr;
	}

	// returns a copy that should be used instead of the current 'uninstalled' copy of the function data
	// will also initialize the namespace, throws if any error is encountered
	FunctionProperties installFunction() const;

	static bool validateFunctionName(const char *funcname);

	inline FunctionManager* getManager() {
		return this->manager;
	}
};

struct FunctionInstallation {
	const FunctionProperties *function;

	std::string install_path;

	WP *wp = nullptr;
	int woofcnamespace_pid = -1;
	Queue result_woof_queue;
	
	FunctionInstallation(const FunctionProperties& func, const std::string& path);
	~FunctionInstallation();

	inline FunctionManager* getManager() {
		return this->function->manager;
	}
};

struct FunctionManager {
	SharedBufferPool *bp_jobobject_pool;
	SharedBufferPool *bp_job_bigstringpool;

	// long held lock, only one thread can be creating a function at a time
	std::mutex create_function_lock;

	// acquire for as short a time as possible to read the unordered_map
	std::mutex lambda_functions_lock;
	std::unordered_map<std::string, std::shared_ptr<FunctionProperties>> lambda_functions;

	// you must set these values, no trailing slashes 
	std::string install_base_dir;
	std::string metadata_base_dir;

	FunctionManager();
	~FunctionManager();

	// NOTE: you must NOT be holding the lambda_functions_lock when you call this or 
	// dead lock will occur 
	virtual bool function_exists(const char *funcname);


	virtual void remove_function(const char *funcname);

private:
	void metadata_path_for_function(const char *funcname, char *path) {
		sprintf(path, "%s/%s.metadata.json", install_base_dir.c_str(), funcname);
	}
};

#endif 
