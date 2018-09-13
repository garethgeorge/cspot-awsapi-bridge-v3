#ifndef FUNCTIONHELPERS_HPP
#define FUNCTIONHELPERS_HPP

#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <string>
#include <exception>
#include <memory>

#include <lib/utility.h>
#include <src/constants.h>
#include "wpcmds.h"

#include <jansson.h>

// class for thrown errors
struct AWSError : public std::exception {
	const int error_code;
	const std::string msg;
	std::string details;

	AWSError(int error_code, const char *message) : AWSError(error_code, std::string(message)) { };
	AWSError(int error_code, const std::string message) : error_code(error_code), msg(message) {
		this->details = message;
	};

	AWSError setDetails(std::string details) {
		this->details = details;
		return *this;
	}
};

struct FunctionProperties;
struct FunctionInstallation;
struct FunctionManager;

// class for managing the properties of functions we create
struct FunctionProperties { // the virtual representation of the function
	FunctionManager *manager = nullptr;
	
	std::string name;
	std::string handler;
	std::string src_zip_path;
	std::string src_zip_sha256;
	std::shared_ptr<FunctionInstallation> installation = nullptr;
	
	FunctionProperties() {};
	FunctionProperties(const FunctionProperties& orig, std::shared_ptr<FunctionInstallation> install);
	FunctionProperties(json_t *json);
	
	~FunctionProperties();

	void setSrcZipPath(std::string& path);

	bool isInstalled() const {
		return installation != nullptr;
	}

	// returns a copy that should be used instead of the current 'uninstalled' copy of the function data
	// will also initialize the namespace, throws if any error is encountered
	// NOTE: this should not be called manually, see FunctionManager::installFunction as a better way 
	std::shared_ptr<FunctionProperties> installFunction() const;

	static bool validateFunctionName(const char *funcname);

	inline FunctionManager* getManager() const {
		return this->manager;
	}

	json_t *dumpJson() const;
};

struct FunctionInstallation {
	const FunctionProperties *function;

	std::string install_path;

	WP *wp = nullptr;
	int woofcnamespace_pid = -1;
	Queue result_woof_queue;
	
	FunctionInstallation(const FunctionProperties& func, const std::string& path);
	~FunctionInstallation();

	inline FunctionManager* getManager()  {
		return this->function->manager;
	}
};

struct FunctionManager {
protected:
	// these are private to the implementation, may be acquired by various operations
	std::mutex lambda_functions_lock;
	std::unordered_map<std::string, std::shared_ptr<const FunctionProperties>> lambda_functions;

public:
	
	SharedBufferPool *bp_jobobject_pool;
	SharedBufferPool *bp_job_bigstringpool;

	// long held lock, only one thread can be creating a function at a time
	// also used to prevent multiple threads from installing a function at the same time
	// and any other long running operation
	// TODO: this REALLY needs a better name, it is basically used any place an operation can not be done in parallel
	// such as installing functions on the local machine, for example.
	std::mutex create_function_lock;

	// you must set these values, no trailing slashes 
	std::string install_base_dir;
	std::string metadata_base_dir;

	FunctionManager();
	virtual ~FunctionManager();

	// NOTE: you must NOT be holding the lambda_functions_lock when you call this or 
	// dead lock will occur 
	virtual bool functionExists(const char *funcname);

	// throws AWSError
	virtual void removeFunction(const char *funcname);
	
	// you must have acquired both lambda_functions_lock and create_function_lock to run this 
	// throws AWSError
	virtual void addFunction(std::shared_ptr<FunctionProperties>& func);

	// load function
	// throws AWSError
	virtual std::shared_ptr<const FunctionProperties> getFunction(const char *funcname);

	// install the function on the local machine so that execution can begin
	// throws AWSError 
	virtual void installFunction(std::shared_ptr<const FunctionProperties>& func);

private:
	void metadata_path_for_function(const char *funcname, char *path) {
		sprintf(path, "%s/%s.metadata.json", this->metadata_base_dir.c_str(), funcname);
	}
};

#endif 
