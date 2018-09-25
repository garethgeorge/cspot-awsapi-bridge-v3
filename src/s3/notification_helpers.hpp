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

struct EventHandler {
	virtual void handleEvent(const std::string &eventName, json_t *event) = 0;

	virtual ~EventHandler() {
	};
};

struct LambdaEventHandler : public EventHandler {
	std::string lambdaArn;

	virtual void handleEvent(const std::string &eventName, json_t *event) override {
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
};

struct EventFilter {
	virtual bool filter(json_t *event) = 0;
};

struct EventFilterAnd : public EventFilter {
	vector<unique_ptr<EventFilter>> filters;

	virtual bool filter(json_t *event) override {
		for (unique_ptr<EventFilter>& filter : this->filters) {
			if (!filter->filter(event))
				return false;
		}
		return true;
	}

	void addFilter(unique_ptr<EventFilter> &&filter) {
		this->filters.push_back(std::move(filter));
	}
};

struct S3EventFilterPrefix : public EventFilter {
	const string prefix;

	S3EventFilterPrefix(std::string& prefix) : prefix(prefix) {
	}

	virtual bool filter(json_t *event) override {
		json_t *s3 = json_object_get(event, "s3");
		if (!s3) 
			return false;
		json_t *object = json_object_get(event, "object");
		if (!object) 
			return false;
		const char *key = json_string_value(json_object_get(object, "key"));
		if (!key)
			return false;
		// check that the two strings have the same prefix
		if (strncmp(key, prefix.c_str(), prefix.length()) != 0)
			return false;
		return true;
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

		for (xml_node<T>* node = cloudFuncConfig.first_node("Event"); node != nullptr; node = node->next_sibling("Event")) {
			const char *event = node->value();
			int event_len = strlen(event);

			// we just ignore the last character if its a wild card
			if (event[event_len - 1] == '*')
				event_len--;
			
			for (const char *eventType : eventTypes) {
				if (strncmp(event, eventType, event_len) == 0) {
					fprintf(stdout, "\tadded handler '%s' -> invoke '%s'\n", eventType, funcArn);
					unique_ptr<LambdaEventHandler> handler = make_unique<LambdaEventHandler>();
					handler->lambdaArn = funcArn;
					
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
	unique_ptr<EventFilter> decodeFilter(xml_node<T>& filter_node) {
		unique_ptr<EventFilterAnd> andFilter = make_unique<EventFilterAnd>();

		xml_node<T> *s3key = filter_node.first_node("S3Key");
		if (s3key != nullptr) {
			// okay this means we are filtering on the S3Key clearly
			for (auto filterRule = s3key.first_node("FilterRule"); 
				filterRule != nullptr;
				filterRule = s3key.next_sibling("FilterRule")) {
				
				auto name = filterRule.first_node("Name");
				auto value = filterRule.first_node("Value");

				if (name == nullptr || value == nullptr) {
					fprintf(stderr, "Fatal error: malformatted filter expression");
					throw AWSError(500, "ServiceException").setDetails("malformatted filter expression");
				}
				
				fprintf("\t\tfilter prefix: %s\n", value->value());
				andFilter->addFilter(make_unique<S3EventFilterPrefix>(value->value()));
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