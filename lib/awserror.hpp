#ifndef AWSERROR_HPP
#define AWSERROR_HPP

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

#endif