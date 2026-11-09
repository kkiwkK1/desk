#include "llm.h"

#include "json.h"
#include "text.h"
#include "widget/http.h"

#include <windows.h>

#include <map>
#include <string>
#include <utility>

namespace llm {
namespace {

std::string ErrorMessageFromBody(const std::string& body) {
    jsonlite::Value root;
    if (jsonlite::Parse(body, root)) {
        if (const jsonlite::Value* err = root.Get("error")) {
            if (err->isString()) return err->AsString();
            if (const jsonlite::Value* msg = err->Get("message")) return msg->StringOr();
        }
    }
    return text::Truncate(text::CollapseWhitespace(body), 120);
}

void AddToolCalls(jsonlite::Value& item, const std::vector<ToolCall>& calls) {
    using jsonlite::Value;
    Value array = Value::MakeArray();
    for (const ToolCall& call : calls) {
        Value entry = Value::MakeObject();
        entry.Set("id", Value(call.id));
        entry.Set("type", Value("function"));
        Value fn = Value::MakeObject();
        fn.Set("name", Value(call.name));
        fn.Set("arguments", Value(call.arguments.empty() ? "{}" : call.arguments));
        entry.Set("function", fn);
        array.Push(entry);
    }
    item.Set("tool_calls", array);
}

jsonlite::Value MessageToJson(const Message& message) {
    using jsonlite::Value;
    Value item = Value::MakeObject();
    item.Set("role", Value(message.role));

    if (message.role == "tool") {
        item.Set("content", Value(message.content));
        item.Set("tool_call_id", Value(message.toolCallId));
        return item;
    }

    // An assistant turn that only calls tools has content: null, and the
    // tool_calls must be echoed exactly - DeepSeek validates the ids when the
    // following "tool" messages arrive.
    if (message.role == "assistant" && !message.toolCalls.empty()) {
        if (message.content.empty()) item.Set("content", Value());
        else item.Set("content", Value(message.content));
        AddToolCalls(item, message.toolCalls);
        return item;
    }

    item.Set("content", Value(message.content));
    return item;
}

void ReadUsage(const jsonlite::Value* usage, Usage& out) {
    if (!usage) return;
    if (const jsonlite::Value* v = usage->Get("prompt_tokens")) out.prompt = (int)v->AsInt();
    if (const jsonlite::Value* v = usage->Get("completion_tokens"))
        out.completion = (int)v->AsInt();
    if (const jsonlite::Value* v = usage->Get("total_tokens")) out.total = (int)v->AsInt();
    if (out.total == 0) out.total = out.prompt + out.completion;
}

// Incremental SSE reader. Feed() accepts arbitrary byte boundaries (WinHTTP
// chunks do not line up with events); Finish() produces the same Result shape
// as the non-streaming parser, including tool calls assembled from fragments.
struct SseState {
    std::string buffer;
    std::string raw;
    std::vector<ToolCall> calls;
    std::map<int, ToolCall> pendingCalls;
    Usage usage;
    std::string content;
    std::string error;
    bool failed = false;
    bool done = false;
    bool sawEvent = false;
    DeltaFn onDelta;

    void Feed(const char* data, std::size_t size) {
        raw.append(data, size);
        if (raw.size() > 256 * 1024) raw.resize(256 * 1024);
        buffer.append(data, size);

        std::size_t newline = 0;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            HandleLine(line);
        }
    }

    void HandleLine(const std::string& line) {
        if (line.compare(0, 5, "data:") != 0) return;  // event:, id:, comments
        sawEvent = true;
        const std::string payload = text::Trim(line.substr(5));
        if (payload.empty()) return;
        if (payload == "[DONE]") {
            done = true;
            return;
        }
        HandlePayload(payload);
    }

    void HandlePayload(const std::string& payload) {
        jsonlite::Value root;
        if (!jsonlite::Parse(payload, root)) {
            failed = true;
            error = "BAD SSE JSON";
            return;
        }

        if (const jsonlite::Value* err = root.Get("error")) {
            failed = true;
            const jsonlite::Value* message = err->Get("message");
            error = "API: " + (message ? text::Truncate(message->StringOr(), 100)
                                       : std::string("stream error"));
            return;
        }

        ReadUsage(root.Get("usage"), usage);

        const jsonlite::Value* choices = root.Get("choices");
        const jsonlite::Value* choice = choices ? choices->At(0) : nullptr;
        const jsonlite::Value* delta = choice ? choice->Get("delta") : nullptr;
        if (!delta) return;

        if (const jsonlite::Value* piece = delta->Get("content")) {
            if (piece->isString() && !piece->AsString().empty()) {
                content += piece->AsString();
                if (onDelta) onDelta(piece->AsString());
            }
        }

        if (const jsonlite::Value* calls = delta->Get("tool_calls")) {
            for (std::size_t i = 0; i < calls->size(); ++i) {
                const jsonlite::Value* call = calls->At(i);
                if (!call) continue;

                int index = 0;
                if (const jsonlite::Value* at = call->Get("index")) index = (int)at->AsInt();
                ToolCall& pending = pendingCalls[index];

                if (const jsonlite::Value* id = call->Get("id")) {
                    if (id->isString()) pending.id = id->AsString();
                }
                if (const jsonlite::Value* function = call->Get("function")) {
                    if (const jsonlite::Value* name = function->Get("name")) {
                        if (name->isString()) pending.name = name->AsString();
                    }
                    if (const jsonlite::Value* args = function->Get("arguments")) {
                        if (args->isString()) pending.arguments += args->AsString();
                    }
                }
            }
        }
    }

    Result Finish(int httpStatus, int elapsedMs) {
        Result result;
        result.httpStatus = httpStatus;
        result.elapsedMs = elapsedMs;
        if (failed) {
            result.error = error.empty() ? "STREAM ERROR" : error;
            return result;
        }

        result.message.role = "assistant";
        result.message.content = content;
        for (auto& entry : pendingCalls) {
            ToolCall call = std::move(entry.second);
            if (call.name.empty()) continue;
            if (call.id.empty()) call.id = "call_" + std::to_string(entry.first);
            result.message.toolCalls.push_back(std::move(call));
        }
        result.usage = usage;
        result.ok = !result.message.content.empty() || !result.message.toolCalls.empty();
        if (!result.ok) result.error = "EMPTY STREAM";
        return result;
    }
};

} // namespace

std::string BuildRequestBody(const Request& request) {
    using jsonlite::Value;

    Value root = Value::MakeObject();
    root.Set("model", Value(request.model));

    Value messages = Value::MakeArray();
    for (const Message& message : request.messages) {
        messages.Push(MessageToJson(message));
    }
    root.Set("messages", messages);

    if (!request.tools.empty()) {
        Value schemas = Value::MakeArray();
        for (const ToolSchema& tool : request.tools) {
            Value entry = Value::MakeObject();
            entry.Set("type", Value("function"));

            Value fn = Value::MakeObject();
            fn.Set("name", Value(tool.name));
            fn.Set("description", Value(tool.description));

            Value params;
            std::string error;
            if (!jsonlite::Parse(tool.parameters, params, &error) || !params.isObject()) {
                params = Value::MakeObject();
                params.Set("type", Value("object"));
                params.Set("properties", Value::MakeObject());
            }
            fn.Set("parameters", params);

            entry.Set("function", fn);
            schemas.Push(entry);
        }
        root.Set("tools", schemas);
        root.Set("tool_choice", Value("auto"));
    }

    root.Set("temperature", Value(request.temperature));
    root.Set("max_tokens", Value(request.maxTokens));
    root.Set("stream", Value(request.stream));
    if (request.stream) {
        // DeepSeek/Z.AI put the final usage in a trailing chunk when asked.
        // Providers that ignore this field simply omit it.
        Value options = Value::MakeObject();
        options.Set("include_usage", Value(true));
        root.Set("stream_options", options);
    }
    return root.Dump();
}

Result ParseResponse(const std::string& body, int httpStatus, int elapsedMs) {
    using jsonlite::Value;

    Result result;
    result.httpStatus = httpStatus;
    result.elapsedMs = elapsedMs;

    if (httpStatus != 200) {
        result.error = ErrorText(httpStatus);
        const std::string detail = ErrorMessageFromBody(body);
        if (!detail.empty() && result.error.rfind("HTTP ", 0) == 0) {
            result.error += ": " + detail;
        }
        return result;
    }

    Value root;
    std::string parseError;
    if (!jsonlite::Parse(body, root, &parseError)) {
        result.error = "BAD JSON: " + parseError;
        return result;
    }

    if (const Value* error = root.Get("error")) {
        const Value* msg = error->Get("message");
        result.error = "API: " + (msg ? text::Truncate(msg->StringOr(), 100)
                                      : std::string("unknown error"));
        return result;
    }

    const Value* choices = root.Get("choices");
    const Value* choice = choices ? choices->At(0) : nullptr;
    const Value* message = choice ? choice->Get("message") : nullptr;
    if (!message) {
        result.error = "NO MESSAGE";
        return result;
    }

    if (const Value* role = message->Get("role")) result.message.role = role->StringOr();
    if (result.message.role.empty()) result.message.role = "assistant";

    if (const Value* content = message->Get("content")) {
        if (content->isString()) result.message.content = content->AsString();
    }

    if (const Value* calls = message->Get("tool_calls")) {
        for (std::size_t i = 0; i < calls->size(); ++i) {
            const Value* call = calls->At(i);
            if (!call) continue;

            ToolCall parsed;
            if (const Value* id = call->Get("id")) parsed.id = id->StringOr();
            if (const Value* fn = call->Get("function")) {
                if (const Value* name = fn->Get("name")) parsed.name = name->StringOr();
                if (const Value* args = fn->Get("arguments")) {
                    parsed.arguments = args->isString() ? args->AsString() : args->Dump();
                }
            }
            if (parsed.name.empty()) continue;
            if (parsed.id.empty()) parsed.id = "call_" + std::to_string(i);
            result.message.toolCalls.push_back(std::move(parsed));
        }
    }

    if (const Value* usage = root.Get("usage")) {
        if (const Value* v = usage->Get("prompt_tokens")) result.usage.prompt = (int)v->AsInt();
        if (const Value* v = usage->Get("completion_tokens"))
            result.usage.completion = (int)v->AsInt();
        if (const Value* v = usage->Get("total_tokens"))
            result.usage.total = (int)v->AsInt();
    }
    if (result.usage.total == 0) {
        result.usage.total = result.usage.prompt + result.usage.completion;
    }

    result.ok = true;
    return result;
}

Result ParseSse(const std::string& body, const DeltaFn& onDelta) {
    SseState state;
    state.onDelta = onDelta;
    state.Feed(body.data(), body.size());
    if (!state.buffer.empty()) {
        std::string tail = state.buffer;
        state.buffer.clear();
        state.HandleLine(tail);
    }
    return state.Finish(200, 0);
}

std::string ErrorText(int httpStatus) {
    switch (httpStatus) {
    case 400: return "BAD REQUEST";
    case 401: return "BAD KEY";
    case 402: return "NO CREDIT";
    case 403: return "FORBIDDEN";
    case 404: return "NOT FOUND";
    case 429: return "RATE LIMIT";
    case 500:
    case 502:
    case 503: return "SERVER BUSY";
    default: return "HTTP " + std::to_string(httpStatus);
    }
}

Result Chat(const std::wstring& apiKey, const Request& request) {
    if (apiKey.empty()) {
        Result result;
        result.error = "NO KEY";
        return result;
    }

    const std::string body = BuildRequestBody(request);
    const ULONGLONG started = GetTickCount64();
    const wg::HttpResponse response =
        wg::HttpPost(request.host.c_str(), request.path.c_str(), apiKey, body,
                     request.timeoutMs, L"AiMini/0.1");
    const int elapsed = (int)(GetTickCount64() - started);

    if (!response.transport) {
        Result result;
        result.error = "NET ERR";
        result.elapsedMs = elapsed;
        return result;
    }
    return ParseResponse(response.body, (int)response.status, elapsed);
}

Result ChatStream(const std::wstring& apiKey, const Request& request, const DeltaFn& onDelta) {
    if (apiKey.empty()) {
        Result result;
        result.error = "NO KEY";
        return result;
    }

    Request streamed = request;
    streamed.stream = true;
    const std::string body = BuildRequestBody(streamed);

    SseState state;
    state.onDelta = onDelta;

    const ULONGLONG started = GetTickCount64();
    const wg::HttpResponse response = wg::HttpPostStream(
        streamed.host.c_str(), streamed.path.c_str(), apiKey, body,
        [&state](const char* data, std::size_t size) {
            state.Feed(data, size);
            return !state.failed;  // stop pulling on malformed JSON
        },
        streamed.timeoutMs, L"AiMini/0.1");
    const int elapsed = (int)(GetTickCount64() - started);

    if (!response.transport) {
        Result result;
        result.error = "NET ERR";
        result.elapsedMs = elapsed;
        return result;
    }
    if (response.status != 200) {
        return ParseResponse(response.body, (int)response.status, elapsed);
    }

    if (!state.buffer.empty()) {
        std::string tail = state.buffer;
        state.buffer.clear();
        state.HandleLine(tail);
    }

    Result result = state.Finish(200, elapsed);
    if (!result.ok && !state.sawEvent) {
        // Endpoint ignored stream:true and answered with a normal JSON body.
        const Result fallback = ParseResponse(state.raw, 200, elapsed);
        if (fallback.ok) return fallback;
    }
    return result;
}

Result HttpTransport::Complete(const Request& request) { return Chat(key_, request); }

Result HttpTransport::CompleteStream(const Request& request, const DeltaFn& onDelta) {
    return ChatStream(key_, request, onDelta);
}

} // namespace llm
