// The tools the model may call. Everything here is local, dependency-free,
// and deliberately small: three read-only probes of this Windows machine plus
// one harmless write (clipboard). A registry is just a vector of {schema, fn}
// so tests can inject their own tools.
#pragma once

#include "llm.h"

#include <functional>
#include <string>
#include <vector>

namespace tools {

struct Tool {
    std::string name;
    std::string description;
    std::string parameters;  // JSON schema text
    std::function<std::string(const std::string& arguments)> run;
};

class Registry {
public:
    void Add(Tool tool);

    std::vector<llm::ToolSchema> Schemas() const;

    // Never throws and always returns text for the model. 'error' (optional)
    // gets a short machine-readable reason when the call failed.
    std::string Invoke(const std::string& name, const std::string& arguments,
                       std::string* error = nullptr) const;

    const Tool* Find(const std::string& name) const;
    const std::vector<Tool>& all() const { return tools_; }

private:
    std::vector<Tool> tools_;
};

// local_time, system_info, disk_free, copy_text.
Registry LocalTools();

} // namespace tools
