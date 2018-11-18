#ifndef S3FILESYSTEM_HPP
#define S3FILESYSTEM_HPP

#include <random>

struct S3LogRef {
	int64_t logId = -1;
	int64_t recordIdx = -1; // index of the record in the log
	S3LogRef() : logId(-1), recordIdx(-1) {};
	S3LogRef(uint64_t logId, uint64_t recordIdx) : logId(logId), recordIdx(recordIdx) {};
};

template<size_t record_size>
class S3StorageLog {
	std::mutex lock;
	uint64_t capacity = 0;
	uint64_t used = 0;
	uint64_t logId = 0;
	std::string logName;

	bool initialized = false;

	void initialize() {
		if (!initialized) {
			WooFCreate((char *)this->logName.c_str(), record_size, this->capacity);
			initialized = true;
		}
	}
public:
	constexpr static uint64_t recordSize = record_size;

	struct OutOfSpaceException : public std::exception {
	};
	
	S3StorageLog(uint64_t recordCount) {
		fprintf(stdout, "Constructed S3StorageLog with capacity %lu records of size %lu (total storage %lu)\n", recordCount, recordSize, recordCount * recordSize);
		this->capacity = recordCount;

		// generate an ID for the next log with a very low probability of collision
		std::random_device rd;
		std::default_random_engine generator(rd());
		std::uniform_int_distribution<long long unsigned> distribution(0,0xFFFFFFFFFFFFFFFF);

		while (true) {
			this->logId = distribution(generator);
			this->logName = getLogName(this->logId);

			struct stat st = {0};
			try {
				if (stat(this->logName.c_str(), &st) != -1) {
					throw AWSError(500, "S3StorageLog picked an identifier that was already in use. The probability of this is INCREDIBLY low. Oops.");
				}
				break ;
			} catch (const AWSError error) {
				fprintf(stderr, "Wow, you must be very (un)lucky, a name collision occured. Ranodmly picking another one\n");
			}
		}
	}

	uint64_t getLogID() {
		return logId;
	}

	uint64_t getRecordSize() {
		return recordSize;
	}

	S3LogRef append(void *data) {
		std::lock_guard<std::mutex> guard(this->lock);
		
		if (this->used >= this->capacity) {
			throw OutOfSpaceException();
		}

		this->initialize();
		this->used++;
		long idx = WooFPut((char *)this->logName.c_str(), NULL, (char *)data);
		if (WooFInvalid(idx)) {
			throw AWSError(500, "failed to WooFPut record into the log");
		}
		// fprintf(stdout, "appended record to log %s at idx %li\n", this->logName.c_str(), idx);

		return S3LogRef(this->logId, idx);
	}

	static std::string getLogName(uint64_t logid) {
		char buffer[128];
		snprintf(buffer, sizeof(buffer) - 1, "%lx.shard.log", logid);
		return buffer;
	}

	static void get(const S3LogRef logref, void *result) {
		std::string logName = getLogName(logref.logId);
		if (WooFGet((char *)logName.c_str(), (char *)result, logref.recordIdx) != 1) {
			throw AWSError(500, "Bad LogId when attempting to get element from log");
		}
	}
};

template<size_t record_size>
class S3LogWriter {
public:
	uint64_t objectsPerLog;
	std::unique_ptr<S3StorageLog<record_size>> storageLog = nullptr;

	S3LogWriter() : S3LogWriter(256) {
	}

	S3LogWriter(uint64_t objectsPerLog) {
		this->objectsPerLog = objectsPerLog;
		this->refreshLog();
	}

	void refreshLog() {
		this->storageLog = std::unique_ptr<S3StorageLog<record_size>>(new S3StorageLog<record_size>(objectsPerLog));
	}

	S3LogRef append(void *data) {
		try {
			return this->storageLog->append(data);
		} catch (typename S3StorageLog<record_size>::OutOfSpaceException& e) {
			fprintf(stdout, "current log (id: %lu) ran out of space, replacing with a new log\n", this->storageLog->getLogID());
			this->refreshLog();
		}
		return this->storageLog->append(data);
	}

	void get(const S3LogRef logref, void *result) {
		S3StorageLog<record_size>::get(logref, result);
	}
};

struct S3FileSystem {
	// each log holds 16 megabytes of data
	constexpr static size_t S3FILE_SHARD_BYTES = 16 * 1024;
	constexpr static size_t S3OBJECTS_PER_LOG = 1024;

	struct FileExistsException : public std::exception { };
	struct FileDoesNotExistException : public std::exception { };

	struct S3Shard {
		S3LogRef nextShard; // may be initialized as some sort of null value
		uint64_t data_remaining = 0;
		uint8_t data[S3FILE_SHARD_BYTES];
	};

	std::unordered_map<std::string, S3LogRef> files;
	S3LogWriter<sizeof(S3Shard)> theShardWriter = S3LogWriter<sizeof(S3Shard)>(S3OBJECTS_PER_LOG);

	S3LogRef writeBuffer(void *data, size_t data_len) {
		// fprintf(stdout, "Writing ... data remaining ... %lu\n", data_len);
		if (data_len > S3FILE_SHARD_BYTES) {
			// fprintf(stdout, "\twrite large chunk and set nextShard to the appropriate ref\n");
			auto nextShardRef = writeBuffer((void *)((uint8_t *)data + S3FILE_SHARD_BYTES), data_len - S3FILE_SHARD_BYTES);
			// fprintf(stdout, "\t\twrote the chunk as ref %lx:%lu\n", nextShardRef.logId, nextShardRef.recordIdx);

			std::unique_ptr<S3Shard> shard(new S3Shard);
			shard->data_remaining = data_len;
			memcpy(&(shard->data), data, S3FILE_SHARD_BYTES);
			shard->nextShard = nextShardRef;
			return this->theShardWriter.append((void *)shard.get());
		} else {
			// fprintf(stdout, "\twrite small and final chunk\n");
			std::unique_ptr<S3Shard> shard(new S3Shard);
			shard->data_remaining = data_len;
			memcpy(&(shard->data), data, data_len);
			return this->theShardWriter.append((void *)shard.get());
		}
	}

	std::string readBuffer(S3LogRef ref) {
		std::stringstream ss(std::stringstream::binary | std::stringstream::out);
		
		while (ref.logId != -1) {
			// fprintf(stdout, "reading ... read from ref %lx:%lu\n", ref.logId, ref.recordIdx);
			std::unique_ptr<S3Shard> shard(new S3Shard);
			S3StorageLog<sizeof(S3Shard)>::get(ref, (void *)shard.get());
			size_t data_in_shard = 
				shard->data_remaining > S3FileSystem::S3FILE_SHARD_BYTES ? 
				S3FileSystem::S3FILE_SHARD_BYTES : shard->data_remaining;
			// fprintf(stdout, "\tdata in shard: %d\n", data_in_shard);
			ss.write((char const*)shard->data, data_in_shard);
			ref = shard->nextShard;
		}

		return ss.str();
	}
};


/*
struct TRIE {
	uint64_t id; // if it has an id, then it is on disk

	struct ValueType {
		uint64_t children;
	};

	std::vector<std::string> keys;
	std::vector<uint64_t> children; // stored by the sha256 of the node
	std::vector<S3LogRef> values; // stores an S3LogRef to the file with this exact name

	// returns the string to replace the TRIE at this position with

	void markModified() {
		id = 0;
	}

	size_t common_prefix(const char *a, const char *b) {
		const char *ao = a;
		while(*a != 0 && *b != 0 && *a == *b) {
			a++;
			b++;
		}
		return (size_t)(a - ao);
	}

	void insert(const char *key, const S3LogRef& file) {
		for (size_t i = 0; i < this->keys.size(); ++i) {
			if (this->keys[i].c_str()[0] == key[0]) {
				// set the current key to the prefix
				size_t common_prefix_len = this->common_prefix(this->keys[i].c_str(), key);
				this->keys[i] = std::string(this->keys[i].c_str(), common_prefix_len);
				
				// construct the keys for the children
				TRIE newNode;
				newNode->insert(this->keys[i].c_str() + common_prefix_len, this->)
				newNode->insert(key + common_prefix_len, file);

				this->values[i]
			} else if (key[0] < this->keys[i].c_str()[0]) {
				// we have passed the region of strings that could have common prefixes
				// we should just go ahead and insert
			}
		}
	}
};
*/


// BTree implementation: https://en.wikibooks.org/wiki/Algorithm_Implementation/Trees/B%2B_tree
// this should be easy enough to modify to support the operations we need

// template<size_t B>
// struct BTreeEntry {
// public:
// 	bool isLeaf = true;
// 	BTreeEntry *parent = nullptr;

// 	std::vector<std::string> keys;
// 	std::vector<std::unique_ptr<BTreeEntry<B>>> children;
// 	std::vector<S3LogRef> values;

// 	bool isRoot() {
// 		return parent == nullptr;
// 	}

// 	void insert(const std::string& key, std::unique_ptr<BTreeEntry>&& entry) {
// 		assert(this->isLeaf == false);

// 		auto insertInto = this;
// 		if (this->keys.size() == B) {
// 			insertInto = split();
// 		}
		
// 		size_t idx = std::distance(insertInto->keys.begin(), std::lower_bound(insertInto->keys.begin(), insertInto->keys.end(), file));
// 		this->keys.insert(this->keys.begin() + idx, key);
// 		this->children.insert(this->children.begin() + idx, std::move(entry));
// 	}


// 	// TODO: better generalize this class through use of templates
// 	void insert(const std::string& key, const S3LogRef& value) {
// 		if (this->isLeaf) {
// 			// we are ACTUALLY inserting on this node, so figure out if a split is 
// 			// required and if a split is required then do the split
// 			auto insertInto = split(key);
			
// 			size_t idx = std::distance(insertInto->keys.begin(), std::lower_bound(insertInto->keys.begin(), insertInto->keys.end(), key));
// 			insertInto->keys.insert(this->keys.begin() + idx, key);
// 			insertInto->values.insert(this->values.begin() + idx, value);
// 		} else {
			
// 			size_t idx = std::distance(this->keys.begin(), std::lower_bound(this->keys.begin(), this->keys.end(), key));
// 			this->children[idx]->insert(key, value);
// 		}
// 	}

// 	BTreeEntry* split(const std::string& key) {
// 		if (this->keys.size() < B) {
// 			return this;
// 		}
		
// 		if (this->isRoot()) {
// 			// okay, we need to copy ourself into a child node, then run split on that
// 			unique_ptr<BTreeEntry> newChild(new BTreeEntry<B>(*this));
// 			newChild->parent = this;
// 			this->isLeaf = false;

// 			this->keys.clear();
// 			this->children.clear();

// 			newChild->split(key);
// 		} else {
// 			// it is an interior node that is not the root 
// 		}


// 		// THE HALF IT RETURNS WILL BE THE HALF THAT SHOULD CONTAIN 'KEY'
// 		// SHOULD BE PRETTY EASY TO FIGURE THAT OUT

// 		// TODO: IMPLEMENT THE SPLIT
// 		return nullptr;
// 	}
// }

#endif