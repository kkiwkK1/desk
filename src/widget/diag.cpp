#include "widget/diag.h"

#include <mutex>
#include <utility>

namespace wg {
namespace {

std::mutex g_sinkMutex;
LogSink g_sink;  // guarded by g_sinkMutex; copied before being invoked

} // namespace

void SetLogSink(LogSink sink) {
    std::lock_guard<std::mutex> lock(g_sinkMutex);
    g_sink = std::move(sink);
}

void Log(LogLevel level, std::wstring_view category, std::wstring_view message) {
    LogSink sink;
    {
        std::lock_guard<std::mutex> lock(g_sinkMutex);
        if (!g_sink) return;  // disabled path: no allocation, no callback
        sink = g_sink;
    }

    // The sink runs outside the lock, so it may log again or replace itself;
    // a throwing sink must never reach the caller.
    LogRecord record;
    record.level = level;
    record.category.assign(category.data(), category.size());
    record.message.assign(message.data(), message.size());
    try {
        if (sink) sink(record);
    } catch (...) {
    }
}

} // namespace wg
