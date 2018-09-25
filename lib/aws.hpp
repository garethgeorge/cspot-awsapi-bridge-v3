#ifndef AWS_HPP
#define AWS_HPP

#include <sstream>

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

inline std::string getArnForBucketName(const char *bucketname) {
	std::stringstream ss;
	ss << "arn:aws:s3:::" << bucketname;
	return std::move(ss.str());
}

inline std::string getNameFromBucketArn(const char *bucketarn) {
	const constexpr size_t min_len = sizeof("arn:aws:s3:::") - 1;
	if (strlen(bucketarn) <= min_len) {
		throw AWSError(500, "Invalid ARN, too short to contain name");
	}
	return std::string(bucketarn + min_len + 1);
}

inline std::string getArnForLambdaName(const char *lambdaName) {
	return std::string("arn:aws:lambda:function:") + lambdaName;
}

inline std::string getNameFromLambdaArn(const char *lambdaArn) {
	const constexpr size_t min_len = sizeof("arn:aws:lambda:function:") - 1;
	if (strlen(lambdaArn) <= min_len) {
		char buff[1024];
		memset(buff, 0, sizeof(buff));
		snprintf(buff, sizeof(buff)-1, "Invalid ARN (%s), too short to contain function name", buff);
		throw AWSError(500, buff);
	}
	return std::string(lambdaArn + min_len);
}

#endif