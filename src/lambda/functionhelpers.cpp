#include "functionhelpers.hpp"

#include <openssl/sha.h>

void sha256(char *string, char outputBuffer[65])
{
	// see https://stackoverflow.com/questions/2262386/generate-sha256-with-openssl-and-c
	unsigned char hash[SHA256_DIGEST_LENGTH];
	SHA256_CTX sha256;
	SHA256_Init(&sha256);
	SHA256_Update(&sha256, string, strlen(string));
	SHA256_Final(hash, &sha256);
	int i = 0;
	for(i = 0; i < SHA256_DIGEST_LENGTH; i++)
	{
		sprintf(outputBuffer + (i * 2), "%02x", hash[i]);
	}
	outputBuffer[64] = 0;
}

int sha256_file(char *path, char outputBuffer[65])
{
	// see https://stackoverflow.com/questions/2262386/generate-sha256-with-openssl-and-c
	FILE *file = fopen(path, "rb");
	if(!file) return -534;

	byte hash[SHA256_DIGEST_LENGTH];
	SHA256_CTX sha256;
	SHA256_Init(&sha256);
	const int bufSize = 32768;
	byte *buffer = malloc(bufSize);
	int bytesRead = 0;
	if(!buffer) return ENOMEM;
	while((bytesRead = fread(buffer, 1, bufSize, file)))
	{
		SHA256_Update(&sha256, buffer, bytesRead);
	}
	SHA256_Final(hash, &sha256);

	sha256_hash_string(hash, outputBuffer);
	fclose(file);
	free(buffer);
	return 0;
}

/*************************************************
    Function Properties method implementations
 *************************************************/


FunctionProperties::setSrcZipPath(std::string &zip_path) {
	this->src_zip_path = zip_path;

	char output_sha256[65];
	if (sha256_file(zip_path.c_str(), output_sha256) != 0) {
		throw AWSError(500, "Failed to calculate SHA256 hash");
	}

	this->src_zip_sha256 = std::string(output_sha256);
}

FunctionProperties FunctionProperties::installFunction() const {
	FunctionProperties copy = *this;
	char buffer[2048];
	snprintf(buffer, sizeof(buffer), "./function_installs/%s/", this->src_zip_sha256);
	copy->installation_dir = buffer;

	// TODO: check if the directory exists, delete if it does.


	return copy;
}

FunctionProperties::validateFunctionName(const std::string& name) {
	const char *str = name.c_str();
	while (*str != '\0') {
		if (!((*str >= '0' && *str <= '9') || (*str >= 'a' && *str <= 'z') || (*str >= 'A' && *str <= 'Z') || *str == '-' || *str == '_')) return true;
		str++;
	}
	return false;
}


