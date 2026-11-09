#include "status_view.h"

#include <cwchar>
#include <string>

namespace server {
namespace {

constexpr size_t kMaxLabel = 16;
constexpr size_t kMaxNote = 20;

std::wstring Truncate(std::wstring text, size_t max) {
    if (text.size() > max) text.resize(max);
    return text;
}

std::wstring Percent(int value) {
    if (value < 0) return L"--";
    wchar_t buf[8];
    swprintf(buf, 8, L"%d%%", value);
    return buf;
}

std::wstring Rtt(int ms) {
    wchar_t buf[16];
    if (ms >= 1000) swprintf(buf, 16, L"%.1fS", ms / 1000.0);
    else swprintf(buf, 16, L"%dMS", ms);
    return buf;
}

int Worst(const HostStatus& host) {
    int worst = -1;
    if (host.cpuPercent > worst) worst = host.cpuPercent;
    if (host.memPercent > worst) worst = host.memPercent;
    if (host.diskPercent > worst) worst = host.diskPercent;
    return worst;
}

wg::Color Severity(const HostStatus& host, const wg::Theme& theme) {
    const int worst = Worst(host);
    if (worst >= 90) return theme.bad;
    if (worst >= 75) return theme.warn;
    return theme.ok;
}

// What to do about a failed probe, in the same 5x7 vocabulary as the value.
std::wstring Hint(const std::wstring& state) {
    if (state == L"WAIT") return L"PROBING";
    if (state == L"TIMEOUT") return L"CHECK HOST / NETWORK";
    if (state == L"AUTH") return L"CHECK SSH KEY";
    if (state == L"DNS") return L"CHECK HOSTNAME";
    if (state == L"REFUSED") return L"CHECK SSHD";
    if (state == L"HOSTKEY") return L"CHECK KNOWN_HOSTS";
    if (state == L"UNREACH") return L"NO ROUTE TO HOST";
    if (state == L"CLOSED") return L"REMOTE CLOSED";
    if (state == L"NO SSH") return L"INSTALL OPENSSH";
    if (state == L"NO DATA") return L"LINUX /PROC ONLY";
    return L"SEE DETAILS";
}

// Right-hand caption. ssh's raw stderr often starts with "user@host: ", which
// wastes the 20-character budget, so known failures get a clean phrase and
// only unknown ones fall back to the captured line.
std::wstring NoteFor(const HostStatus& host) {
    if (host.state == L"AUTH") return L"PERMISSION DENIED";
    if (host.state == L"DNS") return L"CANNOT RESOLVE HOST";
    if (host.state == L"REFUSED") return L"CONNECTION REFUSED";
    if (host.state == L"HOSTKEY") return L"VERIFY KNOWN_HOSTS";
    if (host.state == L"CLOSED") return L"CONNECTION CLOSED";
    if (host.state == L"NO SSH") return L"OPENSSH NOT FOUND";
    if (host.state == L"NO DATA") return L"/PROC NOT AVAILABLE";
    return Truncate(host.error, kMaxNote);
}

} // namespace

std::vector<wg::Row> BuildRows(const ServerData& data, const wg::Theme& theme) {
    std::vector<wg::Row> rows;
    rows.reserve(data.hosts.empty() ? 1 : data.hosts.size());

    if (data.hosts.empty()) {
        wg::Row row;
        row.label = L"SERVERS";
        row.value = data.sshFound ? L"NONE" : L"NO SSH";
        row.noteLeft = L"TRAY > SETTINGS";
        row.note = data.sshFound ? L"ADD SSH HOSTS" : L"INSTALL OPENSSH";
        row.valueColor = data.sshFound ? theme.warn : theme.bad;
        row.noteColor = theme.faint;
        rows.push_back(std::move(row));
        return rows;
    }

    for (const HostStatus& host : data.hosts) {
        wg::Row row;
        row.label = Truncate(host.name, kMaxLabel);
        row.noteColor = theme.faint;

        if (host.ok) {
            row.value = L"UP " + Rtt(host.rttMs);
            row.valueColor = Severity(host, theme);
            row.noteLeft = L"CPU " + Percent(host.cpuPercent) + L"  MEM " +
                           Percent(host.memPercent) + L"  DSK " + Percent(host.diskPercent);
            row.note = L"LOAD " + FormatLoad(host.load1) + L"  UP " +
                       FormatUptime(host.uptimeSec);

            const int worst = Worst(host);
            if (worst >= 90) row.noteColor = theme.bad;
            else if (worst >= 75) row.noteColor = theme.warn;
        } else {
            const bool pending = host.state == L"WAIT";
            row.value = host.state.empty() ? L"DOWN" : host.state;
            row.valueColor = pending ? theme.faint : theme.bad;
            row.noteLeft = Hint(host.state);
            row.note = pending ? std::wstring() : NoteFor(host);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace server
