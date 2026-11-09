// Headless test for Server Status: parser, classifier, row layout and both
// panels. Writes build\server-preview.bmp and build\server-details.bmp.
#include "bmp.h"
#include "widget/layout.h"
#include "widget/view.h"
#include "data/server/details.h"
#include "data/server/providers.h"
#include "data/server/status_view.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const wchar_t* what) {
    if (!condition) {
        wprintf(L"[test] FAIL: %ls\n", what);
        ++g_failures;
    }
}

bool Same(const std::wstring& value, const wchar_t* expected) {
    return value == expected;
}

server::HostStatus UpHost(const wchar_t* target, int cpu, int mem, int disk, double load,
                          int cores, long long uptime, int rtt) {
    server::HostStatus host;
    host.target = target;
    host.name = server::DisplayName(target);
    host.ok = true;
    host.state = L"UP";
    host.cpuPercent = cpu;
    host.memPercent = mem;
    host.diskPercent = disk;
    host.load1 = load;
    host.cores = cores;
    host.uptimeSec = uptime;
    host.rttMs = rtt;
    return host;
}

server::HostStatus DownHost(const wchar_t* target, const wchar_t* state,
                            const wchar_t* error, int rtt) {
    server::HostStatus host;
    host.target = target;
    host.name = server::DisplayName(target);
    host.state = state;
    host.error = error;
    host.rttMs = rtt;
    return host;
}

void TestHosts() {
    const auto hosts = server::ParseHosts(
        L" web-01, db-01;web-01  root@10.0.0.5, -oProxyCommand=bad, , ");
    Check(hosts.size() == 3, L"ParseHosts dedupe / reject options");
    if (hosts.size() == 3) {
        Check(Same(hosts[0], L"web-01"), L"host 0");
        Check(Same(hosts[1], L"db-01"), L"host 1");
        Check(Same(hosts[2], L"root@10.0.0.5"), L"host 2");
    }

    Check(Same(server::DisplayName(L"root@10.0.0.5"), L"10.0.0.5"), L"DisplayName strips user");
    Check(Same(server::DisplayName(L"web-01"), L"web-01"), L"DisplayName keeps alias");

    std::wstring many;
    for (int i = 0; i < 12; ++i) many += L"h" + std::to_wstring(i) + L",";
    Check(server::ParseHosts(many).size() == server::kMaxHosts, L"host cap");

    Check(Same(server::JoinHosts({L"a", L"b"}), L"a, b"), L"JoinHosts");
}

void TestProbeParse() {
    const std::string fixture =
        "LOAD=0.00\n"
        "CORES=4\n"
        "MEMT=3812548\n"
        "MEMA=2801928\n"
        "DSK=24\n"
        "UP=1845781\n"
        "CPU=0\n";
    server::ProbeOutput out;
    Check(server::ParseProbeOutput(fixture, out), L"parse fixture");
    Check(out.cpuPercent == 0, L"cpu");
    Check(out.memPercent == 26, L"mem percent");
    Check(out.diskPercent == 24, L"disk");
    Check(out.load1 == 0.0, L"load");
    Check(out.cores == 4, L"cores");
    Check(out.uptimeSec == 1845781, L"uptime");

    server::ProbeOutput none;
    Check(!server::ParseProbeOutput("hello\nworld\n", none), L"reject non-probe output");
}

void TestClassify() {
    Check(Same(server::ClassifyError("root@1.2.3.4: Permission denied (publickey)."), L"AUTH"),
          L"classify auth");
    Check(Same(server::ClassifyError("ssh: Could not resolve hostname nope: Name or service not known"),
               L"DNS"),
          L"classify dns");
    Check(Same(server::ClassifyError("ssh: connect to host 1.2.3.4 port 22: Connection refused"),
               L"REFUSED"),
          L"classify refused");
    Check(Same(server::ClassifyError("Connection timed out during banner exchange\n"
                                     "Connection to 10.0.0.1 port 22 timed out"),
               L"TIMEOUT"),
          L"classify timeout");
    Check(Same(server::ClassifyError("Host key verification failed."), L"HOSTKEY"),
          L"classify host key");
    Check(Same(server::ClassifyError("No route to host"), L"UNREACH"), L"classify unreach");
    Check(Same(server::ClassifyError("something odd"), L"SSH ERR"), L"classify fallback");
}

void TestFormat() {
    Check(Same(server::FormatUptime(1845781), L"21D"), L"uptime days");
    Check(Same(server::FormatUptime(7200), L"2H"), L"uptime hours");
    Check(Same(server::FormatUptime(90), L"1M"), L"uptime minutes");
    Check(Same(server::FormatUptime(-1), L"--"), L"uptime none");
    Check(Same(server::FormatLoad(0.42), L"0.42"), L"load");
    Check(Same(server::FormatLoad(-1.0), L"--"), L"load none");
}

server::ServerData SampleData() {
    server::ServerData data;
    data.sshFound = true;
    data.sshPath = L"C:\\Windows\\System32\\OpenSSH\\ssh.exe";
    data.updated = L"01:52";
    data.hosts.push_back(UpHost(L"edge-01", 12, 26, 24, 0.00, 4, 1845781, 120));
    data.hosts.push_back(UpHost(L"build-02", 87, 71, 63, 3.40, 8, 345600, 980));
    data.hosts.push_back(DownHost(L"db-03", L"TIMEOUT", L"NO RESPONSE IN 8S", 8000));
    data.hosts.push_back(DownHost(L"media-04", L"AUTH",
                                  L"Permission denied (publickey).", 310));
    return data;
}

void TestRows(const server::ServerData& data) {
    const wg::Theme theme;
    const wg::ListStyle style;
    const auto rows = server::BuildRows(data, theme);
    Check(rows.size() == 4, L"row count");
    if (rows.size() != 4) return;

    Check(Same(rows[0].label, L"edge-01"), L"row label");
    Check(Same(rows[0].value, L"UP 120MS"), L"up value");
    Check(rows[0].valueColor == theme.ok, L"up color");
    Check(Same(rows[0].noteLeft, L"CPU 12%  MEM 26%  DSK 24%"), L"up metrics");
    Check(Same(rows[0].note, L"LOAD 0.00  UP 21D"), L"up load/uptime");

    Check(rows[1].valueColor == theme.warn, L"hot host warns");
    Check(rows[1].noteColor == theme.warn, L"hot note warns");

    Check(Same(rows[2].value, L"TIMEOUT"), L"timeout value");
    Check(Same(rows[2].noteLeft, L"CHECK HOST / NETWORK"), L"timeout hint");
    Check(Same(rows[2].note, L"NO RESPONSE IN 8S"), L"timeout detail");
    Check(rows[2].valueColor == theme.bad, L"timeout color");

    Check(Same(rows[3].value, L"AUTH"), L"auth value");
    Check(Same(rows[3].noteLeft, L"CHECK SSH KEY"), L"auth hint");
    Check(Same(rows[3].note, L"PERMISSION DENIED"), L"auth detail");

    // Same width budget the real panel uses: label + value on one line.
    wg::Canvas measure;
    const int inner = server::kPanelWidth - 2 * style.pad;
    for (const auto& row : rows) {
        const float line = measure.TextWidth(row.label) + measure.TextWidth(row.value);
        Check(line + 8 <= inner, L"label/value fit");
    }

    Check(wg::ListHeight(style, rows.size()) == 137, L"four-row height");
}

void TestEmpty() {
    const wg::Theme theme;
    server::ServerData data;
    data.sshFound = true;
    auto rows = server::BuildRows(data, theme);
    Check(rows.size() == 1 && Same(rows[0].value, L"NONE"), L"empty hosts row");
    data.sshFound = false;
    rows = server::BuildRows(data, theme);
    Check(Same(rows[0].value, L"NO SSH"), L"missing ssh row");
}

void TestPending() {
    const wg::Theme theme;
    server::ServerData data;
    data.sshFound = true;
    data.hosts.push_back(DownHost(L"new-01", L"WAIT", L"", 0));
    const auto rows = server::BuildRows(data, theme);
    Check(rows.size() == 1, L"pending row count");
    if (rows.empty()) return;
    Check(Same(rows[0].value, L"WAIT"), L"pending value");
    Check(Same(rows[0].noteLeft, L"PROBING"), L"pending hint");
    Check(rows[0].note.empty(), L"pending note empty");
    Check(rows[0].valueColor == theme.faint, L"pending color");
}

void TestLayout() {
    const RECT work{0, 0, 2560, 1400};

    POINT p = wg::FindFreeSpot(2220, 1300, 300, 38, {}, work, 8);
    Check(p.x == 2220 && p.y == 1300, L"layout keeps a free position");

    // AiMeter at 2240,1300 / 280x71. Server 300x38 must end up to its left.
    const std::vector<wg::Rect> aimeter = {{2240, 1300, 280, 71}};
    p = wg::FindFreeSpot(2220, 1300, 300, 38, aimeter, work, 8);
    Check(p.x == 1932 && p.y == 1300, L"layout moves left on same row");

    // The left slot is taken too: go above.
    const std::vector<wg::Rect> both = {{2240, 1300, 280, 71}, {1932, 1300, 300, 38}};
    p = wg::FindFreeSpot(2220, 1300, 300, 38, both, work, 8);
    Check(p.x == 2220 && p.y == 1254, L"layout stacks above when the row is full");

    // Off-screen requests are clamped into the work area.
    p = wg::FindFreeSpot(-100, -100, 300, 38, {}, work, 8);
    Check(p.x == 8 && p.y == 8, L"layout clamps into the work area");

    // A tiny work area has no room; the function must still return a sane
    // (clamped) point instead of looping or going negative.
    const RECT tiny{0, 0, 100, 50};
    p = wg::FindFreeSpot(0, 0, 300, 38, {}, tiny, 8);
    Check(p.x >= 0 && p.y >= 0, L"layout tiny work area stays positive");
}

bool WritePanels(const server::ServerData& data, const wchar_t* mainPath,
                 const wchar_t* detailsPath) {
    const wg::Theme theme;
    const wg::ListStyle style;
    const auto rows = server::BuildRows(data, theme);

    wg::Canvas panel;
    if (!panel.Resize(300, wg::ListHeight(style, rows.size()))) return false;
    panel.Clear();
    wg::DrawList(panel, rows, style, theme, false);
    if (!test::WriteBmp(mainPath, panel, wg::Rgba(0x20242C))) return false;

    wg::Canvas details;
    if (!details.Resize(440, DetailsHeight(data))) return false;
    details.Clear();
    DrawDetails(data, theme, details);
    return test::WriteBmp(detailsPath, details, wg::Rgba(0x20242C));
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring mainPath = argc > 1 ? argv[1] : L"build\\server-preview.bmp";
    const size_t slash = mainPath.find_last_of(L"\\/");
    const std::wstring detailsPath =
        (slash == std::wstring::npos ? L"" : mainPath.substr(0, slash + 1)) +
        L"server-details.bmp";

    TestHosts();
    TestProbeParse();
    TestClassify();
    TestFormat();

    const server::ServerData data = SampleData();
    TestRows(data);
    TestEmpty();
    TestPending();
    TestLayout();

    Check(DetailsHeight(data) == 218, L"details height");
    if (!WritePanels(data, mainPath.c_str(), detailsPath.c_str())) {
        wprintf(L"[test] cannot write previews\n");
        ++g_failures;
    } else {
        wprintf(L"[test] wrote %ls and %ls\n", mainPath.c_str(), detailsPath.c_str());
    }

    if (g_failures) {
        wprintf(L"[test] %d failure(s)\n", g_failures);
        return 1;
    }
    wprintf(L"[test] server ok\n");
    return 0;
}
