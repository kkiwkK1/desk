// Offline tests for the AI-mini core. No network, no key, no window:
// JSON, request encoding, response decoding, tool wrapping and the agent loop
// are all exercised through the injectable transport.
#include "../src/agent.h"
#include "../src/endpoint.h"
#include "../src/json.h"
#include "../src/llm.h"
#include "../src/text.h"
#include "../src/tools.h"
#include "widget/cjk.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::printf("[FAIL] %s (line %d)\n", expression, line);
        ++g_failures;
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

int Run(const char* name, void (*test)()) {
    const int before = g_failures;
    test();
    if (g_failures == before) std::printf("[ok] %s\n", name);
    return g_failures - before;
}

class ScriptedTransport : public llm::Transport {
public:
    std::vector<llm::Result> script;
    std::vector<llm::Request> requests;
    std::size_t next = 0;

    llm::Result Complete(const llm::Request& request) override {
        requests.push_back(request);
        if (next >= script.size()) {
            llm::Result missing;
            missing.error = "NO SCRIPT";
            return missing;
        }
        return script[next++];
    }
};

llm::Result Reply(const std::string& content) {
    llm::Result result;
    result.ok = true;
    result.message.role = "assistant";
    result.message.content = content;
    result.usage.prompt = 10;
    result.usage.completion = 5;
    result.usage.total = 15;
    result.elapsedMs = 12;
    return result;
}

llm::Result ToolReply(const std::string& id, const std::string& name,
                      const std::string& arguments) {
    llm::Result result;
    result.ok = true;
    result.message.role = "assistant";
    result.message.toolCalls.push_back(llm::ToolCall{id, name, arguments});
    result.usage.prompt = 20;
    result.usage.completion = 8;
    result.usage.total = 28;
    result.elapsedMs = 20;
    return result;
}

void TestJson() {
    jsonlite::Value value;
    std::string error;
    const std::string text =
        R"({"a":[1,2.5,true,null,"x\"y\\z\n"],"u":"\u4f60\u597d","n":-3})";
    CHECK(jsonlite::Parse(text, value, &error));
    if (!error.empty()) std::printf("  parse error: %s\n", error.c_str());

    const jsonlite::Value* array = value.Get("a");
    CHECK(array != nullptr);
    if (array) {
        CHECK(array->size() == 5);
        CHECK(array->At(0)->AsInt() == 1);
        CHECK(array->At(1)->AsNumber() == 2.5);
        CHECK(array->At(2)->AsBool());
        CHECK(array->At(3)->isNull());
        CHECK(array->At(4)->AsString() == "x\"y\\z\n");
    }
    CHECK(value.Get("u")->AsString() == "\xE4\xBD\xA0\xE5\xA5\xBD");
    CHECK(value.Get("n")->AsInt() == -3);

    const std::string dumped = value.Dump();
    jsonlite::Value again;
    CHECK(jsonlite::Parse(dumped, again));
    CHECK(again.Dump() == dumped);
    CHECK(jsonlite::Value(1.5).Dump() == "1.5");
    CHECK(jsonlite::Value(2.0).Dump() == "2");

    jsonlite::Value bad;
    CHECK(!jsonlite::Parse("{bad", bad));
    CHECK(!jsonlite::Parse("{\"a\":1,}", bad));
}

void TestRequestEncoding() {
    llm::Request request;
    request.messages.emplace_back("system", "sys");
    request.messages.emplace_back("user", "hi");

    llm::Message assistant;
    assistant.role = "assistant";
    assistant.toolCalls.push_back(llm::ToolCall{"c1", "disk_free", R"({"drive":"C:"})"});
    request.messages.push_back(assistant);

    llm::Message tool;
    tool.role = "tool";
    tool.content = "C: 10 GB free";
    tool.toolCallId = "c1";
    request.messages.push_back(tool);

    request.tools.push_back(
        llm::ToolSchema{"disk_free", "disk", R"({"type":"object","properties":{}})"});

    jsonlite::Value root;
    CHECK(jsonlite::Parse(llm::BuildRequestBody(request), root));
    CHECK(root.Get("model")->AsString() == "deepseek-chat");
    CHECK(root.Get("stream")->AsBool() == false);

    request.stream = true;
    jsonlite::Value streamed;
    CHECK(jsonlite::Parse(llm::BuildRequestBody(request), streamed));
    CHECK(streamed.Get("stream")->AsBool());
    CHECK(streamed.Get("stream_options")->Get("include_usage")->AsBool());

    const jsonlite::Value* messages = root.Get("messages");
    CHECK(messages != nullptr && messages->size() == 4);
    if (messages && messages->size() == 4) {
        const jsonlite::Value* call = messages->At(2)->Get("tool_calls");
        CHECK(call != nullptr && call->size() == 1);
        CHECK(messages->At(2)->Get("content")->isNull());
        if (call) {
            CHECK(call->At(0)->Get("id")->AsString() == "c1");
            CHECK(call->At(0)->Get("function")->Get("name")->AsString() == "disk_free");
        }
        CHECK(messages->At(3)->Get("tool_call_id")->AsString() == "c1");
        CHECK(messages->At(3)->Get("content")->AsString() == "C: 10 GB free");
    }

    const jsonlite::Value* tools = root.Get("tools");
    CHECK(tools != nullptr && tools->size() == 1);
    if (tools) {
        CHECK(tools->At(0)->Get("function")->Get("parameters")->isObject());
    }
}

void TestResponseParsing() {
    const char* body =
        R"({"choices":[{"message":{"role":"assistant","content":null,)"
        R"("tool_calls":[{"id":"call_1","type":"function","function":)"
        R"({"name":"local_time","arguments":"{}"}}]}}],)"
        R"("usage":{"prompt_tokens":10,"completion_tokens":3,"total_tokens":13}})";

    const llm::Result result = llm::ParseResponse(body);
    CHECK(result.ok);
    CHECK(result.message.toolCalls.size() == 1);
    if (result.message.toolCalls.size() == 1) {
        CHECK(result.message.toolCalls[0].id == "call_1");
        CHECK(result.message.toolCalls[0].name == "local_time");
    }
    CHECK(result.usage.total == 13);

    const llm::Result empty = llm::ParseResponse("{}");
    CHECK(!empty.ok);
    CHECK(empty.error == "NO MESSAGE");

    const llm::Result denied = llm::ParseResponse(
        R"({"error":{"message":"Authentication Fails"}})", 401, 5);
    CHECK(!denied.ok);
    CHECK(denied.error == "BAD KEY");

    CHECK(llm::ErrorText(401) == "BAD KEY");
    CHECK(llm::ErrorText(402) == "NO CREDIT");
    CHECK(llm::ErrorText(429) == "RATE LIMIT");
    CHECK(llm::ErrorText(418) == "HTTP 418");
}

void TestCjk() {
    CHECK(wg::cjk::IsWide(L'中'));
    CHECK(wg::cjk::IsWide(L'。'));
    CHECK(!wg::cjk::IsWide(L'A'));
    CHECK(wg::cjk::TextWidth(L"AB") == 17.0f);
    CHECK(wg::cjk::TextWidth(L"中") == 13.0f);
    CHECK(wg::cjk::LineHeight(L"AB") == 13);
    CHECK(wg::cjk::LineHeight(L"中") == 15);

    const std::vector<std::wstring> chinese =
        wg::cjk::Wrap(L"一二三四五六七", 42, 5);
    CHECK(chinese.size() == 3);
    if (chinese.size() == 3) {
        CHECK(chinese[0] == L"一二三");
        CHECK(chinese[1] == L"四五六");
        CHECK(chinese[2] == L"七");
    }

    const std::vector<std::wstring> ascii = wg::cjk::Wrap(L"one two three", 70, 2);
    CHECK(ascii.size() == 2);
    if (ascii.size() == 2) {
        CHECK(ascii[0] == L"one two");
        CHECK(ascii[1] == L"three");
    }
}

void TestSse() {
    const std::string body =
        R"(data: {"choices":[{"delta":{"content":"你"}}]})" "\n"
        R"(data: {"choices":[{"delta":{"content":"好"}}]})" "\n"
        R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"c1","function":{"name":"disk_free","arguments":"{\"drive\":"}}]}}]})" "\n"
        R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"C:\"}"}}]}}]})" "\n"
        R"(data: {"usage":{"prompt_tokens":11,"completion_tokens":3,"total_tokens":14}})" "\n"
        R"(data: [DONE])" "\n";

    std::string deltas;
    const llm::Result result = llm::ParseSse(body, [&](const std::string& piece) {
        deltas += piece;
    });

    CHECK(result.ok);
    CHECK(result.message.content == "你好");
    CHECK(deltas == "你好");
    CHECK(result.message.toolCalls.size() == 1);
    if (result.message.toolCalls.size() == 1) {
        CHECK(result.message.toolCalls[0].name == "disk_free");
        CHECK(result.message.toolCalls[0].arguments == R"({"drive":"C:"})");
    }
    CHECK(result.usage.total == 14);

    CHECK(!llm::ParseSse("data: {broken}\n").ok);
}

void TestEndpoint() {
    const aimini::Endpoint full = aimini::ParseEndpoint(
        L"https://api.z.ai/api/coding/paas/v4/chat/completions", "glm-4.6");
    CHECK(full.host == L"api.z.ai");
    CHECK(full.path == L"/api/coding/paas/v4/chat/completions");
    CHECK(full.model == "glm-4.6");

    const aimini::Endpoint bare = aimini::ParseEndpoint(L"api.deepseek.com", "");
    CHECK(bare.host == L"api.deepseek.com");
    CHECK(bare.path == L"/chat/completions");
    CHECK(bare.model == "deepseek-chat");

    const aimini::Endpoint trailing =
        aimini::ParseEndpoint(L"https://example.com/v1/", "m");
    CHECK(trailing.host == L"example.com");
    CHECK(trailing.path == L"/v1");
    CHECK(trailing.model == "m");
}

void TestText() {
    CHECK(text::Trim("  hi \n") == "hi");
    CHECK(text::Ascii("hi \xE4\xBD\xA0") == "hi ?");
    CHECK(text::Truncate("abcdef", 5) == "ab...");

    const std::vector<std::string> lines = text::Wrap("one two three four", 7);
    CHECK(lines.size() == 3);
    if (lines.size() == 3) {
        CHECK(lines[0] == "one two");
        CHECK(lines[1] == "three");
        CHECK(lines[2] == "four");
    }

    const std::vector<std::string> split = text::Wrap("abcdefgh", 3);
    CHECK(split.size() == 3);
    if (split.size() == 3) {
        CHECK(split[0] == "abc");
        CHECK(split[1] == "def");
        CHECK(split[2] == "gh");
    }
}

tools::Registry EchoRegistry() {
    tools::Registry registry;
    registry.Add(tools::Tool{
        "echo", "echo text", R"({"type":"object","properties":{"text":{"type":"string"}}})",
        [](const std::string& arguments) {
            jsonlite::Value parsed;
            if (!jsonlite::Parse(arguments, parsed)) return std::string("ECHO:bad-json");
            const jsonlite::Value* text = parsed.Get("text");
            return std::string("ECHO:") + (text ? text->AsString() : "");
        }});
    return registry;
}

class StreamingTransport : public llm::Transport {
public:
    llm::Result Complete(const llm::Request&) override { return Reply("fallback"); }
    llm::Result CompleteStream(const llm::Request&, const llm::DeltaFn& onDelta) override {
        if (onDelta) {
            onDelta("Hel");
            onDelta("lo");
        }
        return Reply("Hello");
    }
};

void TestAgentStream() {
    StreamingTransport transport;
    std::string deltas;
    const agent::Result result = agent::Run(
        "hi", transport, EchoRegistry(), agent::Config{}, nullptr,
        [&](const std::string& piece) { deltas += piece; });

    CHECK(result.ok);
    CHECK(result.answer == "Hello");
    CHECK(deltas == "Hello");

    agent::Config blocking;
    blocking.stream = false;
    const agent::Result plain = agent::Run("hi", transport, EchoRegistry(), blocking);
    CHECK(plain.ok);
    CHECK(plain.answer == "fallback");
}

void TestAgentToolLoop() {
    ScriptedTransport transport;
    transport.script.push_back(ToolReply("c1", "echo", R"({"text":"hi"})"));
    transport.script.push_back(Reply("DONE."));

    agent::Config config;
    config.maxRounds = 3;

    std::vector<agent::Step> seen;
    const agent::Result result =
        agent::Run("say hi", transport, EchoRegistry(), config,
                   [&seen](const agent::Step& step) { seen.push_back(step); });

    CHECK(result.ok);
    CHECK(result.answer == "DONE.");
    CHECK(result.rounds == 2);
    CHECK(result.toolsCalled == 1);
    CHECK(result.usage.total == 43);
    CHECK(transport.requests.size() == 2);
    CHECK(seen.size() == result.trace.size());
    if (seen.size() >= 2) {
        CHECK(seen[0].kind == "model");
        CHECK(seen[1].kind == "tool");
        CHECK(seen[1].name == "echo");
        CHECK(seen[1].detail == "ECHO:hi");
    }

    if (transport.requests.size() == 2) {
        const std::vector<llm::Message>& messages = transport.requests[1].messages;
        CHECK(messages.size() == 4);
        if (messages.size() == 4) {
            CHECK(messages[0].role == "system");
            CHECK(messages[2].role == "assistant");
            CHECK(messages[2].toolCalls.size() == 1);
            CHECK(messages[3].role == "tool");
            CHECK(messages[3].content == "ECHO:hi");
            CHECK(messages[3].toolCallId == "c1");
        }
    }
}

void TestAgentUnknownTool() {
    ScriptedTransport transport;
    transport.script.push_back(ToolReply("c9", "nope", "{}"));
    transport.script.push_back(Reply("OK."));

    const agent::Result result = agent::Run("go", transport, EchoRegistry());
    CHECK(result.ok);
    CHECK(result.answer == "OK.");
    CHECK(transport.requests.size() == 2);
    if (transport.requests.size() == 2 && transport.requests[1].messages.size() == 4) {
        const std::string& observation = transport.requests[1].messages[3].content;
        CHECK(observation.find("unknown tool") != std::string::npos);
    }
}

void TestAgentLimits() {
    ScriptedTransport transport;
    transport.script.push_back(ToolReply("c1", "echo", "{\"text\":\"a\"}"));
    transport.script.push_back(ToolReply("c2", "echo", "{\"text\":\"b\"}"));

    agent::Config config;
    config.maxRounds = 2;

    const agent::Result result = agent::Run("loop", transport, EchoRegistry(), config);
    CHECK(!result.ok);
    CHECK(result.error == "TOO MANY ROUNDS");
    CHECK(result.rounds == 2);
    CHECK(transport.requests.size() == 2);

    ScriptedTransport failing;
    llm::Result failure;
    failure.error = "NET ERR";
    failing.script.push_back(failure);
    const agent::Result failed = agent::Run("x", failing, EchoRegistry());
    CHECK(!failed.ok);
    CHECK(failed.error == "NET ERR");
    CHECK(failed.rounds == 1);

    ScriptedTransport none;
    const agent::Result empty = agent::Run("   ", none, EchoRegistry());
    CHECK(!empty.ok);
    CHECK(empty.error == "EMPTY QUESTION");
    CHECK(none.requests.empty());
}

void TestLocalTools() {
    const tools::Registry registry = tools::LocalTools();
    CHECK(registry.Find("local_time") != nullptr);
    CHECK(registry.Find("system_info") != nullptr);
    CHECK(registry.Find("disk_free") != nullptr);
    CHECK(registry.Find("copy_text") != nullptr);

    std::string error;
    const std::string time = registry.Invoke("local_time", "{}", &error);
    CHECK(error.empty());
    CHECK(!time.empty());
    CHECK(time.find("Local time") != std::string::npos);

    const std::string info = registry.Invoke("system_info", "{}");
    CHECK(info.find("Memory") != std::string::npos);

    const std::string disks = registry.Invoke("disk_free", "{}");
    CHECK(!disks.empty());

    const std::string unknown = registry.Invoke("does_not_exist", "{}", &error);
    CHECK(error == "unknown tool");
    CHECK(unknown.find("unknown tool") != std::string::npos);

    const std::string badArgs = registry.Invoke("copy_text", "{not json");
    CHECK(badArgs.find("ERROR") == 0);
}

} // namespace

int wmain() {
    int failed = 0;
    failed += Run("json", TestJson);
    failed += Run("request encoding", TestRequestEncoding);
    failed += Run("response parsing", TestResponseParsing);
    failed += Run("endpoint", TestEndpoint);
    failed += Run("sse", TestSse);
    failed += Run("cjk", TestCjk);
    failed += Run("text", TestText);
    failed += Run("agent stream", TestAgentStream);
    failed += Run("agent tool loop", TestAgentToolLoop);
    failed += Run("agent unknown tool", TestAgentUnknownTool);
    failed += Run("agent limits", TestAgentLimits);
    failed += Run("local tools", TestLocalTools);

    if (failed == 0) {
        std::printf("\nall tests passed\n");
        return 0;
    }
    std::printf("\n%d test group(s) failed\n", failed);
    return 1;
}
