#include "agent.h"

#include <windows.h>

#include <string>
#include <utility>

#include "text.h"

namespace agent {
namespace {

void Emit(Result& result, const Trace& onStep, Step step) {
    if (onStep) onStep(step);
    result.trace.push_back(std::move(step));
}

std::string FirstLine(const std::string& text) {
    const std::size_t newline = text.find('\n');
    return text.substr(0, newline == std::string::npos ? text.size() : newline);
}

} // namespace

std::string DefaultSystemPrompt() {
    return "You are AiMini, a tiny assistant living on a Windows 11 desktop. "
           "You can inspect this machine with the provided tools. Call a tool only when the "
           "answer depends on local facts; never guess values a tool can provide. "
           "After a tool call, answer with one short sentence using its result. "
           "Rules: answer in the same language as the user's question, plain text only, "
           "no markdown, at most 45 words (Chinese: at most 80 characters).";
}

Result Run(const std::string& question, llm::Transport& transport,
           const tools::Registry& registry, const Config& config, const Trace& onStep,
           const Delta& onDelta) {
    Result result;
    const ULONGLONG started = GetTickCount64();
    const std::string prompt =
        config.systemPrompt.empty() ? DefaultSystemPrompt() : config.systemPrompt;

    const std::string trimmed = text::Trim(question);
    if (trimmed.empty()) {
        result.error = "EMPTY QUESTION";
        Emit(result, onStep, Step{"error", "", result.error, 0});
        return result;
    }

    std::vector<llm::Message> messages;
    messages.emplace_back("system", prompt);
    messages.emplace_back("user", trimmed);

    const std::vector<llm::ToolSchema> schemas = registry.Schemas();

    for (int round = 0; round < config.maxRounds; ++round) {
        llm::Request request;
        request.host = config.host;
        request.path = config.path;
        request.model = config.model;
        request.messages = messages;
        request.tools = schemas;

        if (config.stream) {
            // The final-answer step would interrupt live deltas if printed
            // after the call; announce the round first instead.
            Emit(result, onStep, Step{"model", request.model, "streaming", 0});
        }

        llm::Result reply = config.stream ? transport.CompleteStream(request, onDelta)
                                          : transport.Complete(request);
        ++result.rounds;
        result.usage.prompt += reply.usage.prompt;
        result.usage.completion += reply.usage.completion;
        result.usage.total += reply.usage.total;

        if (!reply.ok) {
            result.error = reply.error.empty() ? "MODEL ERROR" : reply.error;
            Emit(result, onStep, Step{"error", "", result.error, reply.elapsedMs});
            result.elapsedMs = (int)(GetTickCount64() - started);
            return result;
        }

        if (!config.stream) {
            std::string names;
            for (const llm::ToolCall& call : reply.message.toolCalls) {
                if (!names.empty()) names += ",";
                names += call.name;
            }
            Emit(result, onStep,
                 Step{"model", request.model,
                      names.empty() ? "final answer" : "calls " + names +
                      " (in " + std::to_string(reply.usage.prompt) + " / out " +
                      std::to_string(reply.usage.completion) + " tokens)",
                      reply.elapsedMs});
        }

        if (reply.message.toolCalls.empty()) {
            const std::string answer = text::Trim(reply.message.content);
            if (answer.empty()) {
                result.error = "EMPTY REPLY";
                Emit(result, onStep, Step{"error", "", result.error, 0});
                result.elapsedMs = (int)(GetTickCount64() - started);
                return result;
            }
            result.ok = true;
            result.answer = answer;
            Emit(result, onStep, Step{"answer", "answer", text::Truncate(answer, 80), 0});
            result.elapsedMs = (int)(GetTickCount64() - started);
            return result;
        }

        // Echo the assistant turn first, then one "tool" message per call with
        // the matching id. This is the whole protocol the loop depends on.
        messages.push_back(reply.message);
        for (const llm::ToolCall& call : reply.message.toolCalls) {
            const ULONGLONG toolStarted = GetTickCount64();
            std::string error;
            std::string observation =
                registry.Invoke(call.name, call.arguments, &error);

            if (observation.size() > config.maxToolResult) {
                observation = text::Truncate(observation, config.maxToolResult) + " [truncated]";
            }
            ++result.toolsCalled;

            Step step;
            step.kind = "tool";
            step.name = call.name;
            step.detail = FirstLine(observation);
            if (step.detail.size() > 90) step.detail = text::Truncate(step.detail, 90);
            step.ms = (int)(GetTickCount64() - toolStarted);
            Emit(result, onStep, std::move(step));

            llm::Message toolMessage;
            toolMessage.role = "tool";
            toolMessage.content = observation;
            toolMessage.toolCallId = call.id;
            messages.push_back(std::move(toolMessage));
        }
    }

    result.error = "TOO MANY ROUNDS";
    Emit(result, onStep, Step{"error", "", result.error, 0});
    result.elapsedMs = (int)(GetTickCount64() - started);
    return result;
}

} // namespace agent
