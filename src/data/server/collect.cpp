#include "collect.h"

#include "providers.h"
#include "status_view.h"

#include <algorithm>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

namespace server {
namespace {

std::wstring Now() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buf[8];
    swprintf(buf, 8, L"%02d:%02d", now.wHour, now.wMinute);
    return buf;
}

} // namespace

void Load(ServerData& data, wg::Store& store, const wg::Theme& theme) {
    data.sshPath = SshPath(store.Get(L"ssh"));
    data.sshFound = !data.sshPath.empty();

    for (const auto& target : ParseHosts(store.Get(L"hosts"))) {
        HostStatus host;
        host.target = target;
        host.name = DisplayName(target);
        host.state = L"WAIT";
        data.hosts.push_back(std::move(host));
    }
    data.rows = BuildRows(data, theme);
}

void Refresh(ServerData& data, wg::Store& store, const wg::Theme& theme) {
    // Pick up host edits made by srvctl without a restart. Read a separate
    // Store so the widget's x/y and other live state stay intact; copy just
    // the three shared keys into the main store before Save().
    wg::Store disk;
    disk.Open(store.path());
    auto shared = [&](const wchar_t* key) {
        return disk.Has(key) ? disk.Get(key) : store.Get(key);
    };
    const std::wstring hostsText = shared(L"hosts");
    const std::wstring sshSetting = shared(L"ssh");
    const std::wstring identity = shared(L"key");
    store.Set(L"hosts", hostsText);
    store.Set(L"ssh", sshSetting);
    store.Set(L"key", identity);

    const std::vector<std::wstring> targets = ParseHosts(hostsText);
    const std::wstring ssh = SshPath(sshSetting);

    data = ServerData{};
    data.sshPath = ssh;
    data.sshFound = !ssh.empty();
    data.hosts.reserve(targets.size());
    for (const auto& target : targets) {
        HostStatus host;
        host.target = target;
        host.name = DisplayName(target);
        data.hosts.push_back(std::move(host));
    }

    if (!data.sshFound) {
        for (auto& host : data.hosts) {
            host.state = L"NO SSH";
            host.error = L"OPENSSH CLIENT NOT FOUND";
        }
    } else if (!data.hosts.empty()) {
        // One thread per host: a dead machine costs its own timeout, not
        // every other machine's. kMaxHosts (8) bounds the fan-out.
        std::vector<std::thread> workers;
        workers.reserve(data.hosts.size());
        for (size_t i = 0; i < data.hosts.size(); ++i) {
            try {
                workers.emplace_back([&data, &targets, &ssh, &identity, i] {
                    try {
                        data.hosts[i] = ProbeHost(targets[i], ssh, identity,
                                                  kProbeTimeoutMs);
                    } catch (...) {
                        data.hosts[i].state = L"ERROR";
                        data.hosts[i].error = L"PROBE FAILED";
                    }
                });
            } catch (...) {
                // Thread creation failed (extremely unlikely); stay correct.
                data.hosts[i] =
                    ProbeHost(targets[i], ssh, identity, kProbeTimeoutMs);
            }
        }
        for (auto& worker : workers) worker.join();
    }

    data.rows = BuildRows(data, theme);
    data.updated = Now();
    store.Save();
}

} // namespace server
