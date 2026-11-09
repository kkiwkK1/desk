// ai-mini-cli - the same agent loop as the widget, without a window.
//
//   ai-mini-cli "how much space is free on C:?"
//   ai-mini-cli --model deepseek-chat --json "what time is it?"
//
// The key comes from DEEPSEEK_API_KEY, Credential Manager (AiMini/DeepSeek) or
// ~/.dsh/.credentials.yaml; it is never accepted on the command line, so it
// cannot end up in a shell history.
#include "agent.h"
#include "apikey.h"
#include "endpoint.h"
#include "json.h"
#include "text.h"
#include "tools.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void Print(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

void Usage() {
    Print(
        "ai-mini-cli - minimal AI agent for this Windows machine\n"
        "\n"
        "usage: ai-mini-cli [options] <question>\n"
        "       echo <question> | ai-mini-cli [options]\n"
        "\n"
        "options:\n"
        "  --model NAME       model id (default deepseek-chat)\n"
        "  --endpoint URL     OpenAI-compatible endpoint, e.g.\n"
        "                     https://api.z.ai/api/coding/paas/v4/chat/completions\n"
        "  --max-rounds N     model turns before giving up (default 4)\n"
        "  --no-stream        wait for the whole answer instead of SSE deltas\n"
        "  --json             print a machine-readable result\n"
        "  -h, --help         this text\n"
        "\n"
        "Environment: AIMINI_ENDPOINT, AIMINI_MODEL, AIMINI_API_KEY.\n"
        "The key is read from AIMINI_API_KEY, DEEPSEEK_API_KEY, Windows\n"
        "Credential Manager (AiMini/DeepSeek), then ~/.dsh/.credentials.yaml.\n");
}

std::string Join(const std::vector<std::string>& words) {
    std::string out;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i) out.push_back(' ');
        out += words[i];
    }
    return out;
}

std::string ReadPipedStdin() {
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (GetFileType(input) == FILE_TYPE_CHAR) return std::string();  // console, not a pipe

    std::string out;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(input, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        out.append(buffer, got);
    }
    return out;
}

void PrintStep(const agent::Step& step) {
    std::string line = "[" + step.kind;
    line.resize(std::max<std::size_t>(line.size(), 7), ' ');
    line += "] ";
    line += step.name;
    if (!step.detail.empty()) {
        line += " ";
        line += step.detail;
    }
    line += " (" + std::to_string(step.ms) + " ms)\n";
    Print(line);
}

std::string ResultJson(const agent::Result& result, const std::string& model,
                       const std::string& question, int firstTokenMs) {
    using jsonlite::Value;
    Value root = Value::MakeObject();
    root.Set("ok", Value(result.ok));
    root.Set("question", Value(question));
    root.Set("model", Value(model));
    root.Set("answer", Value(result.answer));
    root.Set("error", Value(result.error));
    root.Set("rounds", Value(result.rounds));
    root.Set("tool_calls", Value(result.toolsCalled));
    root.Set("elapsed_ms", Value(result.elapsedMs));
    root.Set("first_token_ms", Value(firstTokenMs));

    Value usage = Value::MakeObject();
    usage.Set("prompt", Value(result.usage.prompt));
    usage.Set("completion", Value(result.usage.completion));
    usage.Set("total", Value(result.usage.total));
    root.Set("usage", usage);

    Value trace = Value::MakeArray();
    for (const agent::Step& step : result.trace) {
        Value item = Value::MakeObject();
        item.Set("kind", Value(step.kind));
        item.Set("name", Value(step.name));
        item.Set("detail", Value(step.detail));
        item.Set("ms", Value(step.ms));
        trace.Push(item);
    }
    root.Set("trace", trace);
    return root.Dump();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    bool jsonOut = false;
    bool stream = true;
    std::string modelArg;
    std::wstring endpointArg;
    int maxRounds = 4;
    std::vector<std::string> words;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--json") {
            jsonOut = true;
        } else if (arg == L"--model" && i + 1 < argc) {
            modelArg = text::Narrow(argv[++i]);
        } else if (arg == L"--endpoint" && i + 1 < argc) {
            endpointArg = argv[++i];
        } else if (arg == L"--max-rounds" && i + 1 < argc) {
            maxRounds = std::max(1, _wtoi(argv[++i]));
        } else if (arg == L"--no-stream") {
            stream = false;
        } else if (arg == L"--help" || arg == L"-h") {
            Usage();
            return 0;
        } else {
            words.push_back(text::Narrow(arg));
        }
    }

    std::string question = text::Trim(Join(words));
    if (question.empty()) question = text::Trim(ReadPipedStdin());
    if (question.empty()) {
        Usage();
        return 2;
    }

    const std::wstring key = aimini::LoadApiKey();
    if (key.empty()) {
        std::fprintf(stderr,
                     "error: NO KEY. Set DEEPSEEK_API_KEY, or set the key from the "
                     "widget (tray menu), or add DEEPSEEK_API_KEY to "
                     "~/.dsh/.credentials.yaml.\n");
        return 1;
    }

    aimini::Endpoint endpoint = aimini::FromEnv();
    if (!endpointArg.empty()) {
        endpoint = aimini::ParseEndpoint(endpointArg, endpoint.model);
    }
    if (!modelArg.empty()) endpoint.model = modelArg;
    const std::string model = endpoint.model;

    llm::HttpTransport transport(key);
    const tools::Registry registry = tools::LocalTools();

    agent::Config config;
    config.host = endpoint.host;
    config.path = endpoint.path;
    config.model = endpoint.model;
    config.maxRounds = maxRounds;
    config.stream = stream;

    const ULONGLONG started = GetTickCount64();
    int firstTokenMs = -1;
    auto onDelta = [&](const std::string& delta) {
        if (firstTokenMs < 0) firstTokenMs = (int)(GetTickCount64() - started);
        if (!jsonOut) Print(delta);
    };

    const agent::Result result = agent::Run(
        question, transport, registry, config,
        jsonOut ? agent::Trace() : agent::Trace([stream](const agent::Step& step) {
            // Deltas already printed the answer; the answer step would repeat it.
            if (stream && step.kind == "answer") return;
            PrintStep(step);
        }),
        onDelta);

    if (jsonOut) {
        Print(ResultJson(result, model, question, firstTokenMs) + "\n");
    } else if (result.ok) {
        if (stream) {
            // A provider that ignored stream:true only has the final text.
            if (firstTokenMs < 0) Print("\n" + result.answer);
            Print("\n\n");
        } else {
            Print("\n" + result.answer + "\n\n");
        }
        const std::string first =
            firstTokenMs >= 0 ? std::to_string(firstTokenMs) + "ms" : "-";
        char summary[200];
        std::snprintf(summary, sizeof(summary),
                      "%d round(s), %d tool call(s), %.1fs, %d+%d tokens, first token %s\n",
                      result.rounds, result.toolsCalled, result.elapsedMs / 1000.0,
                      result.usage.prompt, result.usage.completion, first.c_str());
        Print(summary);
    } else {
        std::fprintf(stderr, "\nerror: %s\n", result.error.c_str());
    }

    return result.ok ? 0 : 1;
}
