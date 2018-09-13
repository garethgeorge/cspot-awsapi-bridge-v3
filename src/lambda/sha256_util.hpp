#ifndef SHA256_UTIL_H
#define SHA256_UTIL_H

#include <openssl/sha.h>

inline void sha256(char *string, char outputBuffer[65])
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

inline int sha256_file(char *path, char outputBuffer[65])
{
	typedef char byte;

	// see https://stackoverflow.com/questions/2262386/generate-sha256-with-openssl-and-c
	FILE *file = fopen(path, "rb");
	if(!file) return -534;

	byte hash[SHA256_DIGEST_LENGTH];
	SHA256_CTX sha256;
	SHA256_Init(&sha256);
	const int bufSize = 32768;
	byte *buffer = (byte *)malloc(bufSize);
	int bytesRead = 0;
	if(!buffer) return ENOMEM;
	while((bytesRead = fread(buffer, 1, bufSize, file)))
	{
		SHA256_Update(&sha256, buffer, bytesRead);
	}
	SHA256_Final((unsigned char *)hash, &sha256);

	int i = 0;
    for(i = 0; i < SHA256_DIGEST_LENGTH; i++)
    {
        sprintf(outputBuffer + (i * 2), "%02x", hash[i]);
    }
    outputBuffer[64] = 0;

	fclose(file);
	free(buffer);
	return 0;
}

#endif 