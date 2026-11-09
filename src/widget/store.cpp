#include "widget/store.h"

#include <windows.h>

namespace wg {
namespace {

constexpr wchar_t kBom = 0xFEFF;

std::wstring ReadAll(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";

    std::wstring text;
    wchar_t buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
        text.append(buf, got / sizeof(wchar_t));
    }
    CloseHandle(h);
    return text;
}

} // namespace

bool Store::Open(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mu_);
    path_ = path;
    values_.clear();

    std::wstring text = ReadAll(path);
    if (!text.empty() && text.front() == kBom) text.erase(0, 1);

    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = text.size();
        std::wstring line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();

        const size_t eq = line.find(L'=');
        if (eq != std::wstring::npos && eq > 0) {
            values_[line.substr(0, eq)] = line.substr(eq + 1);
        }
        pos = nl + 1;
    }
    return !text.empty();
}

bool Store::Has(const wchar_t* key) const {
    std::lock_guard<std::mutex> lock(mu_);
    return values_.find(key) != values_.end();
}

std::wstring Store::Get(const wchar_t* key, const wchar_t* fallback) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = values_.find(key);
    return it == values_.end() ? std::wstring(fallback) : it->second;
}

long long Store::GetInt(const wchar_t* key, long long fallback) const {
    const std::wstring v = Get(key, L"");
    if (v.empty()) return fallback;
    return wcstoll(v.c_str(), nullptr, 10);
}

void Store::Set(const wchar_t* key, std::wstring value) {
    std::lock_guard<std::mutex> lock(mu_);
    values_[key] = std::move(value);
}

void Store::SetInt(const wchar_t* key, long long value) {
    Set(key, std::to_wstring(value));
}

void Store::Remove(const wchar_t* key) {
    std::lock_guard<std::mutex> lock(mu_);
    values_.erase(key);
}

int Store::RemovePrefix(const std::wstring& prefix) {
    std::lock_guard<std::mutex> lock(mu_);
    int removed = 0;
    for (auto it = values_.begin(); it != values_.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            it = values_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::map<std::wstring, std::wstring> Store::Snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return values_;
}

namespace {

// The single writer shared by Store::Save and SaveSnapshotFile. Keeping one
// implementation matters: save semantics must be "the file equals these
// keys", never a merge with whatever was on disk.
bool WriteValues(const std::wstring& path,
                 const std::map<std::wstring, std::wstring>& values) {
    if (path.empty()) return false;

    // Keep the BOM: the file is UTF-16 with no code page of its own, and
    // Notepad / editors otherwise have to guess (and often guess wrong).
    std::wstring text(1, kBom);
    for (const auto& kv : values) {
        text += kv.first;
        text += L'=';
        text += kv.second;
        text += L'\n';
    }

    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const BOOL ok = WriteFile(h, text.data(), (DWORD)(text.size() * sizeof(wchar_t)),
                              &written, nullptr);
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

} // namespace

bool Store::Save() {
    std::lock_guard<std::mutex> lock(mu_);
    return WriteValues(path_, values_);
}

bool SaveSnapshotFile(const std::wstring& path,
                      const std::map<std::wstring, std::wstring>& values) {
    return WriteValues(path, values);
}

} // namespace wg
