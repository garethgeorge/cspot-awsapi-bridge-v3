#ifndef NOTIFICATION_HELPERS_HPP
#define NOTIFICATION_HELPERS_HPP

// TODO: put this code in its own namespace

#include <array>
#include <vector>
#include <unordered_map>
#include <string>
#include <3rdparty/rapidxml/rapidxml.hpp>

using namespace std;
using namespace rapidxml;

const array<const char *, 4> eventTypes = {
	"s3:ObjectCreated:Put",
	"s3:ObjectCreated:Post",
	"s3:ObjectCreated:Copy",
	"s3:ObjectRemoved:Delete"
};


class EventFilter {
public:
	virtual bool filter(const std::string &eventName, json_t *event) = 0;

	virtual ~EventFilter() {
	}
};

struct EventHandler {
	virtual void handleEvent(const std::string &eventName, json_t *event) = 0;

	virtual ~EventHandler() {
	};
};

struct LambdaEventHandler : public EventHandler {
	std::string lambdaArn;
	std::shared_ptr<EventFilter> eventFilter;

	virtual void handleEvent(const std::string &eventName, json_t *event) override {
		if (!eventFilter->filter(eventName, event)) {
			fprintf(stdout, "not invoking lambda %s, did not pass filter\n", lambdaArn.c_str());
			return ;
		}

		// TODO: change this to use RabbitMQ to guarantee the delivery and execution
		// of event notifications

		std::unique_ptr<char[]> dump(json_dumps(event, 0));
		
		fprintf(stdout, "attempting to invoke lambda '%s'\n", this->lambdaArn.c_str());
		const std::string lambdaName = getNameFromLambdaArn(this->lambdaArn.c_str());
		
		struct _u_map req_headers;
		u_map_init(&req_headers);
		u_map_put(&req_headers, "Content-Type", "application/json");
		u_map_put(&req_headers, "X-Amz-Invocation-Type", "Event");
	
		struct _u_request request;
		ulfius_init_request(&request);
		request.http_verb = strdup("POST");
		std::string http_url = (
			std::string(LAMBDA_API_ENDPOINT "/2015-03-31/functions/") +
			lambdaName.c_str() + std::string("/invocations")).c_str();
		request.http_url = strdup(http_url.c_str());
		request.timeout = 30;
		u_map_copy_into(request.map_header, &req_headers);

		request.binary_body = (char *)dump.get();
		request.binary_body_length = strlen(dump.get());

		struct _u_response response;
		fprintf(stdout, "Making HTTP request to URL %s\n", request.http_url);
		ulfius_init_response(&response);
		int retval = ulfius_send_http_request(&request, &response);
		if (retval == U_OK) {
			fprintf(stdout, "The HTTP response was U_OK, successful\n");

			char *response_body = (char *) malloc(response.binary_body_length + 10);
			strncpy(response_body, (char *)response.binary_body, response.binary_body_length);
			response_body[response.binary_body_length] = '\0';
			fprintf(stdout, "RESPONSE BODY: %s\n", response_body);
			free(response_body);
		} else {
			fprintf(stderr, "Failed to invoke the handler lambda subscribed to this event\n");
		}
		ulfius_clean_response(&response);

		u_map_clean(&req_headers);
	}

	virtual ~LambdaEventHandler() {

	}
};


class EventFilterAnd : public EventFilter {
public:
	vector<shared_ptr<EventFilter>> filters;

	virtual bool filter(const std::string &eventName, json_t *event) override {
		fprintf(stdout, "evaluating EventAndFilter in response to event %s\n", eventName.c_str());
		for (auto& filter : this->filters) {
			if (!filter->filter(eventName, event))
				return false;
		}
		return true;
	}

	void addFilter(shared_ptr<EventFilter> &&filter) {
		this->filters.push_back(filter);
	}

	virtual ~EventFilterAnd() override {
	}
};

class S3EventFilterPrefix : public EventFilter {
public:
	const string prefix;

	S3EventFilterPrefix(const std::string& prefix) : prefix(prefix) {
	}

	virtual bool filter(const std::string &eventName, json_t *event) override {
		fprintf(stdout, "evaluating S3EventFilterPrefix in response to event %s\n", eventName.c_str());
		json_t *records = json_object_get(event, "Records");
		if (!records || json_array_size(records) != 1) 
			return false;
		json_t *firstRecord = json_array_get(records, (size_t)0);
		json_t *s3 = json_object_get(firstRecord, "s3");
		if (!s3) 
			return false;
		json_t *object = json_object_get(s3, "object");
		if (!object) 
			return false;
		const char *key = json_string_value(json_object_get(object, "key"));
		if (!key)
			return false;
		// check that the two strings have the same prefix
		fprintf(stdout, "comparing %s with %s\n", key, prefix.c_str());
		if (strncmp(key, prefix.c_str(), prefix.length()) != 0)
			return false;
		return true;
	}

	virtual ~S3EventFilterPrefix() {
	}
};

// {
//   "Records": [
//     {
//       "eventVersion": "2.0",
//       "eventSource": "aws:s3",
//       "awsRegion": "us-west-1",
//       "eventName": "s3:ObjectCreated:Put",
//       "eventTime": "2018-09-21T20:58:44Z",
//       "s3": {
//         "s3SchemaVersion": "1.0",
//         "bucket": {
//           "name": "myBucket",
//           "arn": "arn:aws:s3:::myBucket"
//         },
//         "object": {
//           "key": "dir/file.txt",
//           "size": 59
//         }
//       }
//     }
//   ]
// }

class S3NotificationConfiguration {
public:

	unordered_map<
		string, 
		vector<unique_ptr<EventHandler>>> handlerMap;

	S3NotificationConfiguration(std::istream& stream) {
		std::string str((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		fprintf(stdout, "read notification configuration from disk: %s\n", str.c_str());
		xml_document<> doc;
		doc.parse<0>((char *)str.c_str());

		this->loadFromXML(doc);
	}

	template<typename T>
	S3NotificationConfiguration(xml_document<T>& xmldocument) {
		this->loadFromXML(xmldocument);
	}

	void notify(const std::string& eventName, json_t *event) {
		if (handlerMap.find(eventName) != handlerMap.end()) {
			for (const auto& handler : handlerMap[eventName]) {
				handler->handleEvent(eventName, event);
			}
		}
	}

private:
	// list of allowable events found here: https://docs.aws.amazon.com/AmazonS3/latest/dev/NotificationHowTo.html
	template<typename T>
	void loadFromXML(xml_document<T>& xmldocument) {
		xml_node<> *nodeNotifConfig = xmldocument.first_node("NotificationConfiguration");
		if (nodeNotifConfig == nullptr) 
			throw AWSError(500, "Did not find the NotificationConfiguration node in the xml document");
		
		for (xml_node<> *cloudFuncConfig = nodeNotifConfig->first_node("CloudFunctionConfiguration");
			cloudFuncConfig != nullptr; 
			cloudFuncConfig = cloudFuncConfig->next_sibling("CloudFunctionConfiguration")) {
			this->loadCloudFunctionConfiguration(*cloudFuncConfig);
		}
	}
	
	template<typename T>
	void loadCloudFunctionConfiguration(xml_node<T>& cloudFuncConfig) {
		fprintf(stdout, "loading cloud function configuration from XML\n");

		xml_node<T>* funcArnNode = cloudFuncConfig.first_node("CloudFunction");
		if (funcArnNode == nullptr) 
			throw AWSError(500, "CloudFunctionConfiguration did not contain a 'CloudFunction' node specifying the function arn to invoke");

		const char *funcArn = funcArnNode->value();

		// try to decode any filters if possible
		xml_node<T> *filterNode = cloudFuncConfig.first_node("Filter");
		shared_ptr<EventFilter> eventFilter;
		if (filterNode != nullptr) {
			eventFilter = this->decodeFilter(*filterNode);
		} else {
			eventFilter = std::shared_ptr<EventFilter>(new EventFilterAnd);
		}

		for (xml_node<T>* node = cloudFuncConfig.first_node("Event"); node != nullptr; node = node->next_sibling("Event")) {
			const char *event = node->value();
			int event_len = strlen(event);

			// we just ignore the last character if its a wild card
			if (event[event_len - 1] == '*')
				event_len--;
			
			// check if it is a valid event type
			for (const char *eventType : eventTypes) {
				if (strncmp(event, eventType, event_len) == 0) {
					fprintf(stdout, "\tadded handler '%s' -> invoke '%s'\n", eventType, funcArn);
					unique_ptr<LambdaEventHandler> handler(new LambdaEventHandler());
					handler->lambdaArn = funcArn;
					handler->eventFilter = eventFilter;
					
					if (this->handlerMap.find(eventType) == this->handlerMap.end()) {
						this->handlerMap[eventType] = vector<unique_ptr<EventHandler>>();
					}
					this->handlerMap[eventType].push_back(std::move(handler));
				}
			}
		}
	}

	// takes the filter node as its argument
	template<typename T>
	shared_ptr<EventFilter> decodeFilter(xml_node<T>& filter_node) {
		shared_ptr<EventFilterAnd> andFilter = shared_ptr<EventFilterAnd>(new EventFilterAnd);
		
		fprintf(stdout, "\tdecoding a filter to apply to these events\n");
		xml_node<T> *s3key = filter_node.first_node("S3Key");
		if (s3key != nullptr) {
			// okay this means we are filtering on the S3Key clearly
			for (auto filterRule = s3key->first_node("FilterRule"); 
				filterRule != nullptr;
				filterRule = s3key->next_sibling("FilterRule")) {
				
				auto name = filterRule->first_node("Name");
				auto value = filterRule->first_node("Value");

				if (name == nullptr || value == nullptr) {
					fprintf(stderr, "Fatal error: malformatted filter expression");
					throw AWSError(500, "ServiceException").setDetails("malformatted filter expression");
				}

				if (strcmp(name->value(), "prefix") != 0) {
					throw AWSError(500, "ServiceException").setDetails("filters on prefix are the only types of filters supported");
				}
				
				fprintf(stdout, "\t\tfilter by prefix: %s\n", value->value());
				std::string filterPrefix = std::string(value->value());
				andFilter->addFilter(std::shared_ptr<EventFilter>(
					new S3EventFilterPrefix(filterPrefix)
				));
			}
		}

		return andFilter; // requires all filters added to it return true
	}
};

/*
<NotificationConfiguration xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
	<CloudFunctionConfiguration>
		<CloudFunction>arn:aws:lambda:function:handler</CloudFunction>
		<Event>s3:ObjectCreated:*</Event>
		<Filter>
			<S3Key>
				<FilterRule>
					<Name>prefix</Name>
					<Value>test</Value>
				</FilterRule>
			</S3Key>
		</Filter>
	</CloudFunctionConfiguration>
</NotificationConfiguration>
*/

#endif 