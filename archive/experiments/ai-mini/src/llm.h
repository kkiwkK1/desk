// Chat-completions client for the AI-mini experiment.
//
// The wire format is the OpenAI-compatible subset DeepSeek speaks: messages
// with an optional tool_calls array, tool results echoed back by id, and a
// "tools" array of JSON schemas. No SDK: one blocking call per model turn,
// optionally consumed as an SSE stream, always from a worker thread.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace llm {

// Called for each content delta of a streamed answer.
using DeltaFn = std::function<void(const std::string&)>;

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;  // raw JSON text
};

struct Message {
    std::string role;       // system | user | assistant | tool
    std::string content;
    std::vector<ToolCall> toolCalls;
    std::string toolCallId;  // role == "tool"

    Message() = default;
    Message(std::string r, std::string c) : role(std::move(r)), content(std::move(c)) {}
};

struct ToolSchema {
    std::string name;
    std::string description;
    std::string parameters;  // JSON text, e.g. {"type":"object","properties":{}}
};

struct Request {
    std::wstring host = L"api.deepseek.com";
    std::wstring path = L"/chat/completions";
    std::string model = "deepseek-chat";
    std::vector<Message> messages;
    std::vector<ToolSchema> tools;
    double temperature = 0.2;
    int maxTokens = 700;
    int timeoutMs = 60000;  // per-read timeout, not whole-call
    bool stream = false;
};

struct Usage {
    int prompt = 0;
    int completion = 0;
    int total = 0;
};

struct Result {
    bool ok = false;
    std::string error;   // "NO KEY" / "NET ERR" / "BAD KEY" / "RATE LIMIT" / ...
    Message message;
    Usage usage;
    int httpStatus = 0;
    int elapsedMs = 0;
};

std::string BuildRequestBody(const Request& request);

// Pure parsers: no I/O, so both wire shapes can be tested offline.
Result ParseResponse(const std::string& body, int httpStatus = 200, int elapsedMs = 0);
Result ParseSse(const std::string& body, const DeltaFn& onDelta = {});

std::string ErrorText(int httpStatus);

// Blocking HTTPS POST to the configured endpoint. Call from a worker thread.
Result Chat(const std::wstring& apiKey, const Request& request);

// Same request with stream:true. Deltas arrive as the model generates; the
// returned Result holds the accumulated content and any tool calls.
Result ChatStream(const std::wstring& apiKey, const Request& request, const DeltaFn& onDelta);

// Injectable seam: tests script responses without a network or a key.
class Transport {
public:
    virtual ~Transport() = default;
    virtual Result Complete(const Request& request) = 0;

    // Tests and simple transports can ignore streaming: the default runs the
    // blocking call and never invokes onDelta.
    virtual Result CompleteStream(const Request& request, const DeltaFn& onDelta) {
        (void)onDelta;
        return Complete(request);
    }
};

class HttpTransport : public Transport {
public:
    explicit HttpTransport(std::wstring apiKey) : key_(std::move(apiKey)) {}
    Result Complete(const Request& request) override;
    Result CompleteStream(const Request& request, const DeltaFn& onDelta) override;

private:
    std::wstring key_;
};

} // namespace llm
