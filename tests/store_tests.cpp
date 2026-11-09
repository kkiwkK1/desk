// Persistence checks for wg::Store: the background writer must write exactly
// the snapshot it was given, so keys removed from memory (a cleared saved.*
// layout snapshot) cannot reappear after a restart.
#include "widget/store.h"

#include <windows.h>

#include <cstdio>
#include <map>
#include <string>

namespace {

int failures = 0;

void Check(bool ok, const wchar_t* what) {
    if (!ok) {
        std::wprintf(L"[store-test] FAIL %ls\n", what);
        ++failures;
    }
}

// Test file next to the test executable, not in %TEMP%: a restricted process
// token (common in CI/sandbox harnesses) may be able to run the binary but not
// write to the user's temp directory, which would look like a product failure.
std::wstring TempPath(const wchar_t* name) {
    wchar_t exe[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe, length);
    const std::size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) path.resize(slash + 1);
    path += L"wg-store-test-";
    path += name;
    path += L".ini";
    return path;
}

} // namespace

int wmain() {
    using namespace wg;

    const std::wstring path = TempPath(L"roundtrip");
    DeleteFileW(path.c_str());

    // A store that is saved normally writes exactly its own keys.
    std::map<std::wstring, std::wstring> first;
    first[L"board.x"] = L"1280";
    first[L"board.y"] = L"0";
    first[L"saved.pending"] = L"1";
    first[L"saved.board.w"] = L"760";
    Check(SaveSnapshotFile(path, first), L"snapshot file written");

    {
        Store store;
        Check(store.Open(path), L"written file opens");
        Check(store.Get(L"board.x", L"") == L"1280", L"board.x persisted");
        Check(store.GetInt(L"saved.pending", 0) == 1, L"saved.pending persisted");
    }

    // The UI clears the snapshot: in-memory keys go away, then the background
    // writer persists the new map. The cleared keys must be gone from disk.
    Store memory;
    Check(memory.Open(path), L"memory store opens");
    memory.RemovePrefix(L"saved.");
    const auto cleared = memory.Snapshot();
    Check(!cleared.count(L"saved.pending"), L"snapshot cleared in memory");
    Check(SaveSnapshotFile(path, cleared), L"cleared snapshot written");

    {
        Store reopened;
        Check(reopened.Open(path), L"reopened after clear");
        Check(!reopened.Has(L"saved.pending"), L"cleared saved.pending stays gone");
        Check(!reopened.Has(L"saved.board.w"), L"cleared saved.board.w stays gone");
        Check(reopened.Get(L"board.x", L"") == L"1280", L"unrelated keys survive the clear");
    }

    // The temp file used for the atomic replace must not be left behind.
    const std::wstring tmp = path + L".tmp";
    Check(GetFileAttributesW(tmp.c_str()) == INVALID_FILE_ATTRIBUTES,
          L"atomic replace leaves no temp file");

    // Empty path is a write failure, not a crash.
    Check(!SaveSnapshotFile(L"", first), L"empty path is rejected");

    // An empty map truncates the file to just the BOM: still readable, no keys.
    {
        const std::map<std::wstring, std::wstring> empty;
        Check(SaveSnapshotFile(path, empty), L"empty snapshot written");
        Store emptyStore;
        // Open reports "nothing to read" for a BOM-only file; what matters is
        // that no stale keys survive.
        emptyStore.Open(path);
        Check(emptyStore.Snapshot().empty(), L"BOM-only file has no keys");
    }

    DeleteFileW(path.c_str());

    std::wprintf(L"[store-test] %s\n", failures ? L"failed" : L"ok");
    return failures == 0 ? 0 : 1;
}
