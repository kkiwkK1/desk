// wg::Store - tiny UTF-16 key=value file next to the exe.
//
// Deliberately not INI/registry/JSON: the widgets only need a handful of
// scalars (window position, last snapshot) and a plain text file stays
// inspectable with Notepad. Safe to call from the UI and worker threads.
#pragma once

#include <map>
#include <mutex>
#include <string>

namespace wg {

class Store {
public:
    // Reads the file if it exists; returns false when there was nothing to read.
    bool Open(const std::wstring& path);
    bool Has(const wchar_t* key) const;
    std::wstring Get(const wchar_t* key, const wchar_t* fallback = L"") const;
    long long GetInt(const wchar_t* key, long long fallback = 0) const;
    void Set(const wchar_t* key, std::wstring value);
    void SetInt(const wchar_t* key, long long value);
    void Remove(const wchar_t* key);
    // Erases every key starting with 'prefix'; returns how many were removed.
    int RemovePrefix(const std::wstring& prefix);
    // Write-through via a temp file + rename, so a crash cannot truncate it.
    bool Save();

    // Copy of the current key/value map, for a background writer.
    std::map<std::wstring, std::wstring> Snapshot() const;

    const std::wstring& path() const { return path_; }

private:
    mutable std::mutex mu_;
    std::wstring path_;
    std::map<std::wstring, std::wstring> values_;
};

// Writes exactly `values` to `path` (UTF-16LE with BOM, temp file + atomic
// replace). Keys that are not in `values` are removed from the file.
//
// This is what a background writer must use: merging a snapshot into the old
// file would resurrect keys the UI removed (for example a cleared saved.*
// layout snapshot) after a restart. Thread-safe with respect to Store.
bool SaveSnapshotFile(const std::wstring& path,
                      const std::map<std::wstring, std::wstring>& values);

} // namespace wg
