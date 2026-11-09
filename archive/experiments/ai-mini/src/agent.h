// The loop that makes the model "drive" the system:
//
//   user question -> model -> tool_calls? -> run local tools -> model -> ...
//                           -> final answer
//
// Bounded by Config::maxRounds, transport-injectable, and free of UI code so it
// can be tested with scripted responses.
#pragma once

#include "llm.h"
#include "tools.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace agent {

struct Step {
    std::string kind;   // model | tool | answer | error
    std::string name;
    std::string detail;
    int ms = 0;
};

using Trace = std::function<void(const Step&)>;
using Delta = std::function<void(const std::string&)>;

struct Config {
    std::wstring host = L"api.deepseek.com";
    std::wstring path = L"/chat/completions";
    std::string model = "deepseek-chat";
    bool stream = true;
    int maxRounds = 4;
    std::size_t maxToolResult = 800;
    std::string systemPrompt;  // empty -> DefaultSystemPrompt()
};

struct Result {
    bool ok = false;
    std::string answer;
    std::string error;
    std::vector<Step> trace;
    llm::Usage usage;
    int rounds = 0;
    int toolsCalled = 0;
    int elapsedMs = 0;
};

std::string DefaultSystemPrompt();

Result Run(const std::string& question, llm::Transport& transport,
           const tools::Registry& registry, const Config& config = {},
           const Trace& onStep = nullptr, const Delta& onDelta = nullptr);

} // namespace agent
