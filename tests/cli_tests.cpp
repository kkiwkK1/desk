// Headless tests for srvctl: argument parsing, targets, table layout, JSON and
// config round-trips. No network and no SSH.
#include "cli/cli.h"
#include "cli/config.h"
#include "cli/output.h"
#include "cli/targets.h"
#include "widget/store.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

int g_failed = 0;
int g_checks = 0;

void Check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) {
        ++g_failed;
        std::printf("FAIL  %s\n", what);
    }
}

#define CHECK(expr) Check((expr), #expr)

std::vector<std::wstring> Args(std::initializer_list<const wchar_t*> items) {
    std::vector<std::wstring> out;
    for (const wchar_t* item : items) out.emplace_back(item);
    return out;
}

void TestArgs() {
    {
        const auto r = srv::ParseArgs(
            Args({L"srvctl", L"status", L"web-01", L"db-01", L"--json", L"--timeout", L"5"}));
        CHECK(r.ok);
        CHECK(r.options.command == L"status");
        CHECK(r.options.targets.size() == 2);
        CHECK(r.options.targets[0] == L"web-01");
        CHECK(r.options.json);
        CHECK(r.options.timeoutSec == 5);
    }
    {
        const auto r = srv::ParseArgs(
            Args({L"srvctl", L"exec", L"--timeout", L"30", L"web-01", L"--", L"uname", L"-a"}));
        CHECK(r.ok);
        CHECK(r.options.command == L"exec");
        CHECK(r.options.targets.size() == 1 && r.options.targets[0] == L"web-01");
        CHECK(r.options.remoteArgs.size() == 2);
        CHECK(r.options.remoteArgs[1] == L"-a");
        CHECK(r.options.timeoutSec == 30);
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"exec", L"web-01", L"uname", L"-a"}));
        CHECK(r.ok);
        CHECK(r.options.remoteArgs.size() == 2);
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"exec", L"web-01"}));
        CHECK(!r.ok);
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"ssh"}));
        CHECK(r.ok);
        CHECK(r.options.command == L"ssh");
        CHECK(r.options.targets.empty());
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"exec"}));
        CHECK(!r.ok);
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"--config", L"x.ini", L"status",
                                            L"--watch", L"2", L"--color", L"never"}));
        CHECK(r.ok);
        CHECK(r.options.command == L"status");
        CHECK(r.options.configPath == L"x.ini");
        CHECK(r.options.watch && r.options.watchSec == 2);
        CHECK(r.options.color == srv::ColorMode::Never);
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"status", L"--watch"}));
        CHECK(r.ok);
        CHECK(r.options.watch && r.options.watchSec == 5);
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"status", L"--watch=3"}));
        CHECK(r.ok);
        CHECK(r.options.watchSec == 3);
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"status", L"--nope"}));
        CHECK(!r.ok);
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"frobnicate"}));
        CHECK(!r.ok);
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"hosts", L"add", L"a,b", L"c"}));
        CHECK(r.ok);
        CHECK(r.options.command == L"hosts");
        CHECK(r.options.hostsAction == L"add");
        CHECK(r.options.targets.size() == 2);
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"hosts"}));
        CHECK(r.ok);
        CHECK(r.options.hostsAction == L"list");
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"hosts", L"rm", L"web-01"}));
        CHECK(r.ok);
        CHECK(r.options.hostsAction == L"remove");
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"help", L"status"}));
        CHECK(r.ok);
        CHECK(r.options.command == L"help");
        CHECK(r.options.targets.size() == 1 && r.options.targets[0] == L"status");
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"--version"}));
        CHECK(r.ok);
        CHECK(r.options.version);
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"--tty", L"exec", L"web-01", L"htop"}));
        CHECK(r.ok);
        CHECK(r.options.tty);
        CHECK(r.options.command == L"exec");
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"doctor", L"--json"}));
        CHECK(!r.ok);
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"hosts", L"--watch"}));
        CHECK(!r.ok);
    }
    {
        const auto r = srv::ParseArgs(Args({L"srvctl", L"status", L"-oProxyCommand=x"}));
        CHECK(!r.ok);
    }
}

void TestTargets() {
    const auto tokens = srv::TokenizeTargets(L"web-01, db-01; root@10.0.0.5  [::1]");
    CHECK(tokens.size() == 4);
    CHECK(tokens[3] == L"[::1]");

    const auto parsed = srv::ParseTargets(L"web-01, WEB-01, db-01");
    CHECK(parsed.size() == 2);
    CHECK(parsed[0] == L"web-01");
    CHECK(parsed[1] == L"db-01");

    std::wstring why;
    CHECK(!srv::ValidTarget(L"-oProxyCommand=x", &why));
    CHECK(!srv::ValidTarget(L"", &why));
    CHECK(srv::ValidTarget(L"root@10.0.0.5", &why));

    CHECK(srv::JoinTargets({L"a", L"b"}) == L"a, b");
    CHECK(srv::ParseTargets(L"a b c d", 2).size() == 2);
}

void TestOutput() {
    CHECK(srv::DisplayWidth(L"abc") == 3);
    CHECK(srv::DisplayWidth(L"服务器") == 6);
    CHECK(srv::DisplayWidth(L"a\u0301") == 1);
    CHECK(srv::TruncateTo(L"abcdef", 5) == L"ab...");
    CHECK(srv::DisplayWidth(srv::TruncateTo(L"abcdef", 5)) == 5);
    CHECK(srv::FormatPercent(-1) == L"--");
    CHECK(srv::FormatPercent(12) == L"12%");
    CHECK(srv::FormatRtt(1500) == L"1.5s");
    CHECK(srv::FormatUptimeValue(90061) == L"1D");
    CHECK(srv::FormatLoadValue(0.4) == L"0.40");

    CHECK(srv::JsonString(L"a\"b\\c") == "\"a\\\"b\\\\c\"");
    CHECK(srv::JsonString(L"line\nnext") == "\"line\\nnext\"");
    CHECK(srv::JsonString(L"服务器").find("\xE6\x9C\x8D") != std::string::npos);
    CHECK(srv::JsonDouble(0.4) == "0.40");
    CHECK(srv::JsonIntOrNull(-1) == "null");
    CHECK(srv::JsonLongOrNull(12) == "12");

    std::vector<std::wstring> headers = {L"HOST", L"STATE"};
    std::vector<std::vector<srv::Cell>> rows = {
        {srv::Cell{L"web-01", srv::Color::None, false}, srv::Cell{L"UP", srv::Color::Green, false}},
        {srv::Cell{L"db-01", srv::Color::None, false},
         srv::Cell{L"AUTH", srv::Color::Red, false}},
    };
    const std::wstring plain = srv::RenderTable(headers, rows, false, 80);
    CHECK(plain.find(L"HOST") != std::wstring::npos);
    CHECK(plain.find(L"web-01") != std::wstring::npos);
    CHECK(plain.find(L"\x1b[") == std::wstring::npos);
    CHECK(std::count(plain.begin(), plain.end(), L'\n') == 3);  // header, rule, 2 rows -> 3

    const std::wstring colored = srv::RenderTable(headers, rows, true, 80);
    CHECK(colored.find(L"\x1b[") != std::wstring::npos);

    std::vector<std::wstring> three = {L"HOST", L"STATE", L"DETAIL"};
    std::vector<std::vector<srv::Cell>> detailRows = {
        {srv::Cell{L"web-01", srv::Color::None, false},
         srv::Cell{L"UP", srv::Color::Green, false},
         srv::Cell{L"a long detail that will not fit", srv::Color::Dim, false}},
    };
    const std::wstring narrow = srv::RenderTable(three, detailRows, false, 16);
    CHECK(narrow.find(L"DETAIL") == std::wstring::npos);

    const std::wstring wide = srv::RenderTable(three, detailRows, false, 120);
    CHECK(wide.find(L"DETAIL") != std::wstring::npos);
}

void TestConfig() {
    const std::wstring path = L"build\\tmp\\cli-test-config-" +
                              std::to_wstring(GetCurrentProcessId()) + L".ini";
    DeleteFileW(path.c_str());

    {
        wg::Store store;
        store.Open(path);
        store.Set(L"hosts", L"web-01, db-01");
        store.Set(L"ssh", L"C:\\Windows\\System32\\OpenSSH\\ssh.exe");
        store.SetInt(L"timeout", 12);
        CHECK(store.Save());
    }

    srv::Config config;
    std::wstring error;
    CHECK(config.Load(path, false, error));
    CHECK(config.exists);
    CHECK(!config.inherited);
    CHECK(config.hosts.size() == 2);
    CHECK(config.timeoutSec == 12);
    CHECK(!config.ssh.empty());

    config.hosts.push_back(L"db-02");
    CHECK(config.Save(error));
    srv::Config reloaded;
    CHECK(reloaded.Load(path, false, error));
    CHECK(reloaded.hosts.size() == 3);
    CHECK(reloaded.hosts[2] == L"db-02");

    DeleteFileW(path.c_str());

    srv::Config missing;
    CHECK(!missing.Load(L"build\\tmp\\definitely-not-here.ini", false, error));
}

} // namespace

int wmain() {
    TestArgs();
    TestTargets();
    TestOutput();
    TestConfig();
    std::printf("cli tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
