// Minimal, thread-safe diagnostic logging.
//
// The framework has no opinion about where logs go: an application installs a
// sink (file, debugger, ring buffer) before starting a board. The default is no
// sink, so the disabled path stays cheap. Sinks may be called from the UI
// thread and from the refresh/save worker threads; implementations must be
// thread-safe.
#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace wg {

enum class LogLevel { Error, Warn, Info, Debug };

struct LogRecord {
    LogLevel level = LogLevel::Info;
    std::wstring category;
    std::wstring message;
};

using LogSink = std::function<void(const LogRecord&)>;

// Replaces the sink (empty/null disables logging). Thread-safe.
void SetLogSink(LogSink sink);

// Formats a record and forwards it to the current sink. Safe from any thread.
void Log(LogLevel level, std::wstring_view category, std::wstring_view message);

} // namespace wg
