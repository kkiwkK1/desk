#include "widget/diag.h"

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wprintf(L"[diag-test] FAIL %ls\n", message);
        ++failures;
    }
}

struct Capture {
    void Add(const wg::LogRecord& record) {
        std::lock_guard<std::mutex> lock(mutex);
        records.push_back(record);
    }

    std::size_t Count() {
        std::lock_guard<std::mutex> lock(mutex);
        return records.size();
    }

    std::vector<wg::LogRecord> Snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        return records;
    }

    std::mutex mutex;
    std::vector<wg::LogRecord> records;
};

} // namespace

int wmain() {
    using namespace wg;

    // ---- capture level, category and message -------------------------------
    {
        Capture capture;
        SetLogSink([&capture](const LogRecord& record) { capture.Add(record); });
        Log(LogLevel::Error, L"account", L"request failed");
        Log(LogLevel::Warn, L"servers", L"one host is down");
        Log(LogLevel::Info, L"layout", L"cards placed");
        Log(LogLevel::Debug, L"trace", L"done");
        const std::vector<LogRecord> records = capture.Snapshot();
        Check(records.size() == 4, L"every level is forwarded to the sink");
        if (records.size() == 4) {
            Check(records[0].level == LogLevel::Error &&
                      records[0].category == L"account" &&
                      records[0].message == L"request failed",
                  L"error level, category and message are captured");
            Check(records[1].level == LogLevel::Warn && records[1].category == L"servers" &&
                      records[1].message == L"one host is down",
                  L"warn level, category and message are captured");
            Check(records[2].level == LogLevel::Info && records[2].category == L"layout" &&
                      records[2].message == L"cards placed",
                  L"info level, category and message are captured");
            Check(records[3].level == LogLevel::Debug && records[3].category == L"trace" &&
                      records[3].message == L"done",
                  L"debug level, category and message are captured");
        }
        // Empty strings are still valid records.
        Log(LogLevel::Info, L"", L"");
        Check(capture.Count() == 5, L"empty category/message is still logged");
    }

    // ---- reset the sink ----------------------------------------------------
    {
        Capture capture;
        SetLogSink([&capture](const LogRecord& record) { capture.Add(record); });
        Log(LogLevel::Info, L"cat", L"before reset");
        SetLogSink(nullptr);
        Log(LogLevel::Info, L"cat", L"after reset");
        Check(capture.Count() == 1, L"a null sink disables logging");
        SetLogSink(LogSink{});
        Log(LogLevel::Info, L"cat", L"after empty sink");
        Check(capture.Count() == 1, L"an empty sink disables logging");
        SetLogSink([&capture](const LogRecord& record) { capture.Add(record); });
        Log(LogLevel::Info, L"cat", L"after reinstall");
        Check(capture.Count() == 2, L"a sink can be installed again");
    }

    // ---- replacing the sink stops the old one ------------------------------
    {
        int first = 0;
        int second = 0;
        SetLogSink([&first](const LogRecord&) { ++first; });
        Log(LogLevel::Info, L"cat", L"first");
        SetLogSink([&second](const LogRecord&) { ++second; });
        Log(LogLevel::Info, L"cat", L"second");
        Check(first == 1 && second == 1, L"replacing the sink stops the previous one");
    }

    // ---- a throwing sink is swallowed --------------------------------------
    {
        SetLogSink([](const LogRecord&) { throw std::runtime_error("sink failed"); });
        bool threw = false;
        try {
            Log(LogLevel::Error, L"cat", L"boom");
        } catch (...) {
            threw = true;
        }
        Check(!threw, L"a throwing sink does not reach the caller");
        int after = 0;
        SetLogSink([&after](const LogRecord&) { ++after; });
        Log(LogLevel::Info, L"cat", L"still works");
        Check(after == 1, L"logging keeps working after a sink threw");
    }

    // ---- the sink runs outside the lock (re-entrant calls do not deadlock) --
    {
        std::atomic<int> calls{0};
        SetLogSink([&calls](const LogRecord&) {
            ++calls;
            SetLogSink(nullptr);
            Log(LogLevel::Info, L"nested", L"ignored");
        });
        Log(LogLevel::Info, L"outer", L"first");
        Check(calls.load() == 1, L"a sink may replace itself and log again without deadlock");
        Log(LogLevel::Info, L"outer", L"second");
        Check(calls.load() == 1, L"the re-entrant reset really disabled logging");
    }

    // ---- concurrent logging from several threads ---------------------------
    {
        Capture capture;
        SetLogSink([&capture](const LogRecord& record) { capture.Add(record); });
        constexpr int kThreads = 4;
        constexpr int kPerThread = 300;
        std::atomic<bool> start{false};
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int thread = 0; thread < kThreads; ++thread) {
            threads.emplace_back([&start, thread]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (int i = 0; i < kPerThread; ++i) {
                    const std::wstring message =
                        std::to_wstring(thread) + L":" + std::to_wstring(i);
                    Log(LogLevel::Info, L"thread", message);
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (std::thread& thread : threads) thread.join();

        const std::vector<LogRecord> records = capture.Snapshot();
        Check(records.size() == static_cast<std::size_t>(kThreads * kPerThread),
              L"concurrent logging delivers exactly N records");
        std::set<std::wstring> unique;
        bool categoriesOk = true;
        for (const LogRecord& record : records) {
            unique.insert(record.message);
            categoriesOk = categoriesOk && record.category == L"thread";
        }
        Check(categoriesOk, L"concurrent records keep their category");
        Check(unique.size() == static_cast<std::size_t>(kThreads * kPerThread),
              L"no concurrent record is duplicated or lost");
        SetLogSink(nullptr);
    }

    if (failures) {
        std::wprintf(L"[diag-test] %d failure(s)\n", failures);
        return 1;
    }
    std::wprintf(L"[diag-test] ok\n");
    return 0;
}
