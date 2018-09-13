#ifndef FUNCTIONHELPERS_HPP
#define FUNCTIONHELPERS_HPP

#include <json_fwd.hpp>

#include <string>
#include <exception>

// class for thrown errors
struct AWSError : public std::exception {
protected:
	int error_code;
	std::string msg;

public:
	AWSError(int error_code, std::string& message) : error_code(error_code), msg(message) {
	}
}

// function properties object
struct FunctionProperties { // the virtual representation of the function
	std::string function_name;
	std::string handler;
	std::string src_zip_path;
	std::string src_zip_sha256;
	std::string installation_dir; // length 0 if it is not installed yet

	void setSrcZipPath(std::string& path);

	void isInstalled() const {
		return installation_dir.length() != 0;
	}

	// returns a copy that should be used instead of the current 'uninstalled' copy of the function data
	FunctionProperties installFunction();

	static bool validateFunctionName(const char *funcname);
};



#endif 