// Which OpenAI-compatible endpoint to talk to. DeepSeek is the default; the
// experiment lets AIMINI_ENDPOINT / AIMINI_MODEL (or the widget's ini) point at
// any compatible gateway (Z.AI, OpenRouter, a local llama.cpp server, ...).
#pragma once

#include <string>

namespace aimini {

struct Endpoint {
    std::wstring host = L"api.deepseek.com";
    std::wstring path = L"/chat/completions";
    std::string model = "deepseek-chat";
};

// Accepts a full URL ("https://host/path/...") or "host/path". A bare host
// keeps the default /chat/completions path.
Endpoint ParseEndpoint(const std::wstring& url, const std::string& model);

// AIMINI_ENDPOINT + AIMINI_MODEL, else the DeepSeek defaults.
Endpoint FromEnv();

} // namespace aimini
