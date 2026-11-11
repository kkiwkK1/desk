// M2.2a proposal-store regression tests. Native, headless, offline.
//
// Result mapping follows the frozen M2.2a API: an existing active file that
// is not valid v1 is Refused; InvalidBaseline is reserved for an internally
// inconsistent caller Snapshot. Requires an existing active for Rollback;
// with a backup but no active it is Refused, and with no backup it is NoBackup.
// Read-only ordinary files are readable; Apply/Rollback refuse read-only
// leaves before writing. Read failures are always checked with a preloaded
// non-empty Snapshot so TooLarge/Refused/Busy/Raced must preserve exists/bytes/
// digest instead of clearing the caller's value. The read-side concurrency
// stress is bounded: if the size-change window is not observed it records a
// skip for that branch, while any illegal result, torn snapshot or leaked leaf
// is still a hard failure.
//
// Everything lives under one synthetic PID directory below the test exe
// directory. The suite never reads a real cardspec configuration, never uses
// a real credential, and only the three fixed leaf names in store.h are ever
// written. Failure labels are static: no synthetic payload or path text is
// printed, and every failure message is checked for payload/path echo.
#include "data/cardspec/store.h"

#include "data/cardspec/cardspec.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace cs = cardspec::store;

int checks = 0;
int failures = 0;
int skips = 0;

constexpr wchar_t kCanaryW[] = L"CANARY9f3";
constexpr char kEmptySha[] = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

// Independent SHA-256 vector for exactly the bytes below (length 102). The
// implementation's output is never used to compute this expectation.
constexpr char kRowsDoc[] =
    "{\"version\":1,\"title\":\"t\",\"source\":{\"type\":\"inline\"},"
    "\"layout\":\"rows\",\"rows\":[{\"label\":\"l\",\"text\":\"t\"}]}";
static_assert(sizeof(kRowsDoc) - 1 == 102, "hard-coded SHA vector is for 102 bytes");
constexpr char kRowsSha[] = "08c019f43d240e4a4b3f53e70091c7384dcbb804f8eec00f05febb615a68fb02";

constexpr char kBarDoc[] =
    "{\"version\":1,\"title\":\"bar\",\"source\":{\"type\":\"inline\"},"
    "\"layout\":\"bar\",\"rows\":[{\"label\":\"n\",\"value\":3,\"max\":5}]}";
constexpr char kOldDoc[] =
    "{\"version\":1,\"title\":\"old\",\"source\":{\"type\":\"inline\"},"
    "\"layout\":\"rows\",\"rows\":[{\"label\":\"a\",\"text\":\"b\"}]}";
constexpr char kNewDoc[] =
    "{\"version\":1,\"title\":\"new\",\"source\":{\"type\":\"inline\"},"
    "\"layout\":\"bar\",\"rows\":[{\"label\":\"n\",\"value\":3,\"max\":5}]}";
constexpr char kOtherDoc[] =
    "{\"version\":1,\"title\":\"ext\",\"source\":{\"type\":\"inline\"},"
    "\"layout\":\"rows\",\"rows\":[{\"label\":\"e\",\"text\":\"x\"}]}";
constexpr char kBackupDoc[] =
    "{\"version\":1,\"title\":\"bak\",\"source\":{\"type\":\"inline\"},"
    "\"layout\":\"rows\",\"rows\":[{\"label\":\"b\",\"text\":\"k\"}]}";
constexpr char kRowsV2Doc[] =
    "{\"version\":2,\"title\":\"t\",\"source\":{\"type\":\"inline\"},"
    "\"layout\":\"rows\",\"rows\":[{\"label\":\"l\",\"text\":\"t\"}]}";
constexpr char kCanaryDoc[] =
    "{\"version\":2,\"title\":\"CANARY9f3\",\"source\":{\"type\":\"inline\"},"
    "\"layout\":\"rows\",\"rows\":[{\"label\":\"l\",\"text\":\"t\"}]}";
constexpr char kUnknownDoc[] =
    "{\"version\":1,\"title\":\"t\",\"source\":{\"type\":\"inline\"},"
    "\"layout\":\"rows\",\"rows\":[{\"label\":\"l\",\"text\":\"t\"}],"
    "\"extra\":1}";
constexpr char kDuplicateDoc[] =
    "{\"version\":1,\"version\":1,\"title\":\"t\",\"source\":{\"type\":\"inline\"},"
    "\"layout\":\"rows\",\"rows\":[{\"label\":\"l\",\"text\":\"t\"}]}";
constexpr char kHttpDoc[] =
    "{\"version\":1,\"title\":\"t\",\"source\":{\"type\":\"http\"},"
    "\"layout\":\"rows\",\"rows\":[{\"label\":\"l\",\"text\":\"t\"}]}";
constexpr char kNaNValueDoc[] =
    "{\"version\":1,\"title\":\"t\",\"source\":{\"type\":\"inline\"},"
    "\"layout\":\"bar\",\"rows\":[{\"label\":\"l\",\"value\":1e999,\"max\":5}]}";
constexpr char kUnknownRowDoc[] =
    "{\"version\":1,\"title\":\"t\",\"source\":{\"type\":\"inline\"},"
    "\"layout\":\"rows\",\"rows\":[{\"label\":\"l\",\"text\":\"t\",\"value\":1}]}";
constexpr char kTempSentinel[] = "TEMP-SENTINEL-CANARY9f3";

std::string OldBytes() {
    return std::string("\xEF\xBB\xBF") + " \r\n\t" + kOldDoc + "  \n";
}

std::string BackupBytes() {
    return std::string("\xEF\xBB\xBF") + " \n" + kBackupDoc + "\t\r\n";
}

std::string NewInput() {
    return std::string("\xEF\xBB\xBF") + "  " + kNewDoc + " \r\n";
}

bool SameSpec(const cardspec::Spec& a, const cardspec::Spec& b) {
    if (a.title != b.title || a.layout != b.layout || a.rows.size() != b.rows.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.rows.size(); ++i) {
        if (a.rows[i].label != b.rows[i].label) return false;
        if (a.rows[i].text != b.rows[i].text) return false;
        if (a.rows[i].value != b.rows[i].value) return false;
        if (a.rows[i].max != b.rows[i].max) return false;
    }
    return true;
}

std::uint32_t Rotr(std::uint32_t value, std::uint32_t bits) {
    return (value >> bits) | (value << (32 - bits));
}

// Small, self-contained SHA-256 so the test does not depend on the store's
// hashing path or on any extra library.
std::string Sha256Hex(std::string_view data) {
    static const std::uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
        0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
        0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
        0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
        0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
        0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
        0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
        0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    std::uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

    std::vector<std::uint8_t> message(data.begin(), data.end());
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) message.push_back(0);
    for (int i = 7; i >= 0; --i) {
        message.push_back(static_cast<std::uint8_t>((bit_length >> (i * 8)) & 0xff));
    }

    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::uint32_t w[64] = {};
        for (std::size_t i = 0; i < 16; ++i) {
            const std::size_t p = offset + i * 4;
            w[i] = (static_cast<std::uint32_t>(message[p]) << 24) |
                   (static_cast<std::uint32_t>(message[p + 1]) << 16) |
                   (static_cast<std::uint32_t>(message[p + 2]) << 8) |
                   static_cast<std::uint32_t>(message[p + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h[0];
        std::uint32_t b = h[1];
        std::uint32_t c = h[2];
        std::uint32_t d = h[3];
        std::uint32_t e = h[4];
        std::uint32_t f = h[5];
        std::uint32_t g = h[6];
        std::uint32_t hh = h[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint32_t word : h) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            out.push_back(kHex[(word >> shift) & 0x0fu]);
        }
    }
    return out;
}

void Check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        std::printf("[cardspec-store-test] FAIL %s\n", what.c_str());
        ++failures;
    }
}

void Skip(const char* what, const std::string& reason) {
    ++skips;
    std::printf("[cardspec-store-test] SKIP %s: %s\n", what, reason.c_str());
}

std::string SymlinkSkipReason(DWORD code) {
    if (code == ERROR_PRIVILEGE_NOT_HELD) {
        return "symlink privilege unavailable (ERROR_PRIVILEGE_NOT_HELD)";
    }
    char buffer[96] = {};
    std::snprintf(buffer, sizeof(buffer),
                  "CreateSymbolicLinkW unavailable (GetLastError=%lu)",
                  static_cast<unsigned long>(code));
    return buffer;
}

void CheckStaticW(const std::string& label, const std::wstring& text) {
    ++checks;
    if (text.find(kCanaryW) != std::wstring::npos || text.find(L'\\') != std::wstring::npos) {
        std::printf("[cardspec-store-test] FAIL %s\n", label.c_str());
        ++failures;
    }
}

void CheckOutcomeStatic(const std::string& label, const cs::Outcome& out) {
    CheckStaticW(label, out.message);
}

void CheckResultValue(const std::string& label, cs::Result result,
                      std::initializer_list<cs::Result> allowed) {
    ++checks;
    bool ok = false;
    for (const cs::Result candidate : allowed) {
        if (result == candidate) ok = true;
    }
    if (!ok) {
        std::printf("[cardspec-store-test] FAIL %s (result=%d)\n", label.c_str(),
                    static_cast<int>(result));
        ++failures;
    }
}

void CheckResult(const std::string& label, const cs::Outcome& out,
                 std::initializer_list<cs::Result> allowed) {
    CheckResultValue(label, out.result, allowed);
}

void CheckSuccess(const std::string& label, const cs::Outcome& out) {
    CheckResult(label, out, {cs::Result::Ok});
    CheckOutcomeStatic(label + ": static message", out);
}

void CheckFailure(const std::string& label, const cs::Outcome& out,
                  std::initializer_list<cs::Result> allowed) {
    CheckResult(label, out, allowed);
    Check(!out.ok(), label + ": outcome is not ok");
    Check(!out.message.empty(), label + ": failure message is set");
    CheckOutcomeStatic(label + ": static message", out);
}

void CheckEffect(const std::string& label, const cs::Outcome& out,
                 cs::TargetEffect expected) {
    Check(out.effect == expected, label + ": target effect");
}

void CheckEffectOneOf(const std::string& label, const cs::Outcome& out,
                      std::initializer_list<cs::TargetEffect> allowed) {
    ++checks;
    bool ok = false;
    for (const cs::TargetEffect candidate : allowed) {
        if (out.effect == candidate) ok = true;
    }
    if (!ok) {
        std::printf("[cardspec-store-test] FAIL %s (effect=%d)\n", label.c_str(),
                    static_cast<int>(out.effect));
        ++failures;
    }
}

void CheckReadOk(const std::string& label, cs::Result result, const std::wstring& error) {
    CheckResultValue(label, result, {cs::Result::Ok});
    Check(error.empty(), label + ": read clears error");
}

void CheckReadFailure(const std::string& label, cs::Result result,
                      const std::wstring& error,
                      std::initializer_list<cs::Result> allowed) {
    CheckResultValue(label, result, allowed);
    Check(!error.empty(), label + ": error is set");
    CheckStaticW(label + ": static error", error);
}

bool ReadActiveOk(const std::string& label, const std::wstring& dir, cs::Snapshot* out) {
    std::wstring error = L"stale error";
    const cs::Result result = cs::ReadActive(dir, out, &error);
    CheckReadOk(label, result, error);
    return result == cs::Result::Ok;
}

bool ReadBackupOk(const std::string& label, const std::wstring& dir, cs::Snapshot* out) {
    std::wstring error = L"stale error";
    const cs::Result result = cs::ReadBackup(dir, out, &error);
    CheckReadOk(label, result, error);
    return result == cs::Result::Ok;
}

void CheckSnapshot(const std::string& label, const cs::Snapshot& snapshot, bool exists,
                   std::string_view bytes, std::string_view digest);
cs::Snapshot ReadSentinel();

void CheckReadActiveFailure(const std::string& label, const std::wstring& dir,
                            std::initializer_list<cs::Result> allowed) {
    cs::Snapshot snapshot = ReadSentinel();
    const cs::Snapshot sentinel = snapshot;
    std::wstring error = L"stale error";
    const cs::Result result = cs::ReadActive(dir, &snapshot, &error);
    CheckReadFailure(label, result, error, allowed);
    CheckSnapshot(label + ": failure keeps caller snapshot", snapshot, true,
                  sentinel.bytes, sentinel.digest);
}

void CheckReadBackupFailure(const std::string& label, const std::wstring& dir,
                            std::initializer_list<cs::Result> allowed) {
    cs::Snapshot snapshot = ReadSentinel();
    const cs::Snapshot sentinel = snapshot;
    std::wstring error = L"stale error";
    const cs::Result result = cs::ReadBackup(dir, &snapshot, &error);
    CheckReadFailure(label, result, error, allowed);
    CheckSnapshot(label + ": failure keeps caller snapshot", snapshot, true,
                  sentinel.bytes, sentinel.digest);
}

void CheckSnapshot(const std::string& label, const cs::Snapshot& snapshot, bool exists,
                   std::string_view bytes, std::string_view digest) {
    Check(snapshot.exists == exists, label + ": exists");
    Check(snapshot.bytes == std::string(bytes), label + ": bytes");
    Check(snapshot.digest == std::string(digest), label + ": digest");
}

void CheckSnapshotMissing(const std::string& label, const cs::Snapshot& snapshot) {
    CheckSnapshot(label, snapshot, false, "", "");
}

cs::Snapshot SnapshotFor(std::string_view bytes) {
    cs::Snapshot snapshot;
    snapshot.exists = true;
    snapshot.bytes = std::string(bytes);
    snapshot.digest = Sha256Hex(bytes);
    return snapshot;
}

cs::Snapshot ReadSentinel() { return SnapshotFor("READ-SENTINEL"); }

std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0) return L".";
    const std::wstring text(path, length);
    const std::size_t slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : text.substr(0, slash);
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ReadBytes(const std::wstring& path, std::string* out) {
    out->clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char buffer[4096];
    DWORD got = 0;
    BOOL read_ok = TRUE;
    while ((read_ok = ReadFile(file, buffer, sizeof(buffer), &got, nullptr)) != FALSE &&
           got > 0) {
        out->append(buffer, got);
    }
    CloseHandle(file);
    return read_ok != FALSE;
}

bool WriteBytes(const std::wstring& path, std::string_view bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = TRUE;
    if (!bytes.empty()) {
        ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    }
    const bool good = ok != FALSE && static_cast<std::size_t>(written) == bytes.size();
    CloseHandle(file);
    return good;
}

bool GetWriteTime(const std::wstring& path, FILETIME* out) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    *out = data.ftLastWriteTime;
    return true;
}

bool SetOldWriteTime(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    SYSTEMTIME old_time = {};
    old_time.wYear = 2020;
    old_time.wMonth = 1;
    old_time.wDay = 2;
    old_time.wHour = 3;
    old_time.wMinute = 4;
    old_time.wSecond = 5;
    FILETIME file_time = {};
    const BOOL converted = SystemTimeToFileTime(&old_time, &file_time);
    const BOOL set = converted && SetFileTime(file, nullptr, nullptr, &file_time);
    CloseHandle(file);
    return set != FALSE;
}

bool SameFileTime(const FILETIME& a, const FILETIME& b) {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

void DeleteTree(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            RemoveDirectoryW(path.c_str());
        } else {
            SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(path.c_str());
        }
        return;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(path.c_str());
        return;
    }
    WIN32_FIND_DATAW data = {};
    HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            DeleteTree(path + L"\\" + name);
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryW(path.c_str());
}

std::vector<std::wstring> ListNames(const std::wstring& dir) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW data = {};
    HANDLE search = FindFirstFileW((dir + L"\\*").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return names;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        names.push_back(name);
    } while (FindNextFileW(search, &data));
    FindClose(search);
    std::sort(names.begin(), names.end());
    return names;
}

void CheckLeaves(const std::string& label, const std::wstring& dir,
                 std::initializer_list<const wchar_t*> expected) {
    std::vector<std::wstring> want;
    for (const wchar_t* name : expected) want.emplace_back(name);
    std::sort(want.begin(), want.end());
    Check(ListNames(dir) == want, label + ": only the expected fixed leaves exist");
}

std::wstring ActivePath(const std::wstring& dir) {
    return dir + L"\\" + cs::kActiveFile;
}

std::wstring BackupPath(const std::wstring& dir) {
    return dir + L"\\" + cs::kBackupFile;
}

std::wstring TempPath(const std::wstring& dir) {
    return dir + L"\\" + cs::kTempFile;
}

void CheckFileBytes(const std::string& label, const std::wstring& path,
                    std::string_view expected) {
    std::string actual;
    const bool ok = ReadBytes(path, &actual);
    Check(ok, label + ": file is readable");
    if (ok) Check(actual == std::string(expected), label + ": exact bytes");
}

void CheckMissing(const std::string& label, const std::wstring& path) {
    Check(!Exists(path), label + ": path is absent");
}

void CheckIsDirectory(const std::string& label, const std::wstring& path) {
    Check(IsDirectory(path), label + ": path is a directory");
}

bool NewCase(const std::wstring& root, const wchar_t* name, std::wstring* out) {
    *out = root + L"\\" + name;
    const bool ok = CreateDirectoryW(out->c_str(), nullptr) != 0;
    Check(ok, "synthetic case directory created");
    return ok;
}

bool PrepareOk(const std::string& label, std::string_view text, cs::Candidate* out) {
    std::wstring error = L"stale error";
    const bool ok = cs::Prepare(text, out, &error);
    Check(ok, label + ": prepares");
    Check(error.empty(), label + ": success clears error");
    return ok;
}

void PrepareReject(const std::string& label, std::string_view text) {
    cs::Candidate sentinel;
    if (!PrepareOk(label + ": sentinel", kRowsDoc, &sentinel)) return;
    const std::string before = sentinel.canonical();
    const cardspec::Spec before_spec = sentinel.spec();

    std::wstring error = L"stale error";
    const bool ok = cs::Prepare(text, &sentinel, &error);
    Check(!ok, label + ": rejected");
    if (ok) return;
    Check(!error.empty(), label + ": error is set");
    Check(error != L"stale error", label + ": error replaces a stale value");
    Check(sentinel.canonical() == before, label + ": failure leaves canonical untouched");
    Check(SameSpec(sentinel.spec(), before_spec), label + ": failure leaves spec untouched");
    CheckStaticW(label + ": static error", error);
}

void TestPrepare() {
    cs::Candidate fresh;
    Check(!fresh.valid(), "default Candidate is invalid");
    Check(fresh.canonical().empty(), "default Candidate has no canonical bytes");
    Check(!cardspec::Validate(fresh.spec()), "default Candidate spec is invalid");

    std::wstring null_error = L"stale error";
    Check(!cs::Prepare(kRowsDoc, nullptr, &null_error), "Prepare rejects a null output");
    Check(!null_error.empty(), "null output reports an error");
    CheckStaticW("null output message", null_error);

    cs::Candidate rows;
    if (PrepareOk("v1 rows", kRowsDoc, &rows)) {
        Check(rows.valid(), "v1 rows candidate is valid");
        Check(rows.spec().title == L"t", "v1 rows title");
        Check(rows.spec().layout == cardspec::Layout::Rows, "v1 rows layout");
        Check(rows.spec().rows.size() == 1 && rows.spec().rows[0].label == L"l" &&
                  rows.spec().rows[0].text == L"t",
              "v1 rows fields");
        Check(!rows.canonical().empty(), "v1 rows canonical is not empty");

        cardspec::Spec parsed;
        std::wstring parse_error = L"stale error";
        Check(cardspec::Parse(rows.canonical(), &parsed, &parse_error),
              "canonical output reparses");
        Check(parse_error.empty(), "canonical reparse clears error");
        Check(cardspec::Serialize(parsed) == rows.canonical(),
              "canonical output is a fixed point");

        cs::Candidate round_trip;
        if (PrepareOk("canonical round trip", rows.canonical(), &round_trip)) {
            Check(round_trip.canonical() == rows.canonical(),
                  "canonical round trip is byte identical");
            Check(SameSpec(round_trip.spec(), rows.spec()),
                  "canonical round trip preserves typed values");
        }
    }

    cs::Candidate bar;
    if (PrepareOk("v1 bar", kBarDoc, &bar)) {
        Check(bar.spec().layout == cardspec::Layout::Bar, "v1 bar layout");
        Check(bar.spec().rows.size() == 1 && bar.spec().rows[0].value == 3.0 &&
                  bar.spec().rows[0].max == 5.0,
              "v1 bar numbers");
    }

    const std::string bom_doc = std::string("\xEF\xBB\xBF") + "  " + kRowsDoc + " \r\n\t";
    cs::Candidate bom;
    Check(PrepareOk("BOM and whitespace", bom_doc, &bom), "BOM/whitespace candidate builds");

    PrepareReject("v2 document", kRowsV2Doc);
    PrepareReject("http source", kHttpDoc);
    PrepareReject("unknown root key", kUnknownDoc);
    PrepareReject("duplicate root key", kDuplicateDoc);
    PrepareReject("unknown row key", kUnknownRowDoc);
    PrepareReject("malformed JSON", "{");
    PrepareReject("empty document", "");
    PrepareReject("trailing data", std::string(kRowsDoc) + "x");
    PrepareReject("infinite bar value", kNaNValueDoc);

    std::string too_large(kRowsDoc);
    too_large.append(cardspec::kMaxSpecBytes, ' ');
    Check(too_large.size() > cardspec::kMaxSpecBytes, "oversized fixture is over the limit");
    PrepareReject("oversized document", too_large);

    PrepareReject("payload canary", kCanaryDoc);

    cs::Candidate untouched;
    std::wstring error = L"stale error";
    Check(!cs::Prepare(kRowsV2Doc, &untouched, &error), "invalid input is rejected");
    Check(!untouched.valid() && untouched.canonical().empty(),
          "fresh invalid candidate stays invalid after a rejected Prepare");
    CheckStaticW("fresh rejection message", error);
}

void TestReadSnapshots(const std::wstring& root) {
    {
        std::wstring dir;
        if (!NewCase(root, L"read-missing", &dir)) return;
        cs::Snapshot snapshot;
        if (ReadActiveOk("missing active", dir, &snapshot)) {
            CheckSnapshotMissing("missing active snapshot", snapshot);
        }
        snapshot = cs::Snapshot{};
        if (ReadBackupOk("missing backup", dir, &snapshot)) {
            CheckSnapshotMissing("missing backup snapshot", snapshot);
        }
        CheckLeaves("missing directory", dir, {});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"read-empty", &dir)) return;
        Check(WriteBytes(ActivePath(dir), ""), "empty active fixture writes");
        Check(WriteBytes(BackupPath(dir), ""), "empty backup fixture writes");
        cs::Snapshot active;
        if (ReadActiveOk("empty active", dir, &active)) {
            CheckSnapshot("empty active snapshot", active, true, "", kEmptySha);
        }
        cs::Snapshot backup;
        if (ReadBackupOk("empty backup", dir, &backup)) {
            CheckSnapshot("empty backup snapshot", backup, true, "", kEmptySha);
        }
        CheckLeaves("empty directory", dir, {cs::kActiveFile, cs::kBackupFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"read-hash", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kRowsDoc), "hash active fixture writes");
        cs::Snapshot active;
        if (ReadActiveOk("known-vector active", dir, &active)) {
            CheckSnapshot("known-vector active snapshot", active, true, kRowsDoc, kRowsSha);
        }
        CheckLeaves("hash directory", dir, {cs::kActiveFile});
    }

    {
        struct DirCase {
            const char* label;
            std::wstring path;
        };
        const std::wstring missing = root + L"\\missing-" +
                                     std::to_wstring(GetCurrentProcessId());
        const DirCase cases[] = {
            {"relative directory", L"relative-cardspec-store"},
            {"UNC directory", L"\\\\server\\share\\cardspec-store"},
            {"device directory", L"\\\\.\\cardspec-store-device"},
            {"nonexistent directory", missing},
        };
        for (const DirCase& item : cases) {
            CheckReadActiveFailure(item.label, item.path, {cs::Result::InvalidDirectory});
        }
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"read-too-large", &dir)) return;
        const std::string oversized(cardspec::kMaxSpecBytes + 1, ' ');
        Check(WriteBytes(ActivePath(dir), oversized), "oversized active fixture writes");
        Check(WriteBytes(BackupPath(dir), oversized), "oversized backup fixture writes");
        CheckReadActiveFailure("oversized active", dir, {cs::Result::TooLarge});
        CheckReadBackupFailure("oversized backup", dir, {cs::Result::TooLarge});
        CheckLeaves("oversized directory", dir, {cs::kActiveFile, cs::kBackupFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"read-leaf-directory", &dir)) return;
        Check(CreateDirectoryW(ActivePath(dir).c_str(), nullptr) != 0,
              "active leaf directory fixture creates");
        Check(CreateDirectoryW(BackupPath(dir).c_str(), nullptr) != 0,
              "backup leaf directory fixture creates");
        CheckReadActiveFailure("active leaf directory", dir, {cs::Result::Refused});
        CheckReadBackupFailure("backup leaf directory", dir, {cs::Result::Refused});
        CheckLeaves("leaf directory case", dir, {cs::kActiveFile, cs::kBackupFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"read-hardlink", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kRowsDoc), "hardlink active fixture writes");
        const std::wstring extra = dir + L"\\extra-link.json";
        const bool linked = CreateHardLinkW(extra.c_str(), ActivePath(dir).c_str(), nullptr) != 0;
        Check(linked, "hardlink fixture creates a second name");
        if (linked) {
            CheckReadActiveFailure("multi-hardlink active", dir, {cs::Result::Refused});
            CheckFileBytes("hardlink active remains readable as fixture", ActivePath(dir),
                           kRowsDoc);
            CheckFileBytes("hardlink second name remains readable", extra, kRowsDoc);
        }
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"read-symlink-file", &dir)) return;
        const std::wstring target = dir + L"\\target.json";
        Check(WriteBytes(target, kRowsDoc), "symlink file target writes");
        const DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        if (!CreateSymbolicLinkW(ActivePath(dir).c_str(), target.c_str(), flags)) {
            Skip("symlink file leaf", SymlinkSkipReason(GetLastError()));
        } else {
            CheckReadActiveFailure("symlink file leaf", dir, {cs::Result::Refused});
            CheckFileBytes("symlink file target unchanged", target, kRowsDoc);
        }
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"read-reparse-dir", &dir)) return;
        const std::wstring real = dir + L"\\real";
        const std::wstring link = dir + L"\\link";
        Check(CreateDirectoryW(real.c_str(), nullptr) != 0, "reparse real directory creates");
        const DWORD flags =
            SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        if (!CreateSymbolicLinkW(link.c_str(), real.c_str(), flags)) {
            Skip("reparse directory", SymlinkSkipReason(GetLastError()));
        } else {
            CheckReadActiveFailure("reparse directory", link, {cs::Result::InvalidDirectory});
        }
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"read-readonly", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kRowsDoc), "readonly read active writes");
        Check(WriteBytes(BackupPath(dir), kOldDoc), "readonly read backup writes");
        Check(SetFileAttributesW(ActivePath(dir).c_str(), FILE_ATTRIBUTE_READONLY) != 0,
              "readonly read active marks the file");
        Check(SetFileAttributesW(BackupPath(dir).c_str(), FILE_ATTRIBUTE_READONLY) != 0,
              "readonly read backup marks the file");
        cs::Snapshot active;
        if (ReadActiveOk("readonly active is readable", dir, &active)) {
            CheckSnapshot("readonly active snapshot", active, true, kRowsDoc, kRowsSha);
        }
        cs::Snapshot backup;
        if (ReadBackupOk("readonly backup is readable", dir, &backup)) {
            CheckSnapshot("readonly backup snapshot", backup, true, kOldDoc,
                          Sha256Hex(kOldDoc));
        }
        SetFileAttributesW(ActivePath(dir).c_str(), FILE_ATTRIBUTE_NORMAL);
        SetFileAttributesW(BackupPath(dir).c_str(), FILE_ATTRIBUTE_NORMAL);
        CheckLeaves("readonly read leaves", dir, {cs::kActiveFile, cs::kBackupFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"read-race", &dir)) return;
        Check(WriteBytes(ActivePath(dir), "x"), "read race fixture writes");
        std::atomic<bool> stop{false};
        std::atomic<bool> writer_ok{false};
        std::atomic<unsigned long> writer_error{0};
        std::thread writer([&]() {
            HANDLE file = CreateFileW(
                ActivePath(dir).c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                writer_error.store(static_cast<unsigned long>(GetLastError()));
                stop.store(true);
                return;
            }
            writer_ok.store(true);
            while (!stop.load()) {
                LARGE_INTEGER start = {};
                SetFilePointerEx(file, start, nullptr, FILE_BEGIN);
                SetEndOfFile(file);
                const char byte = 'x';
                DWORD written = 0;
                WriteFile(file, &byte, 1, &written, nullptr);
                LARGE_INTEGER end = {};
                end.QuadPart = 1024;
                SetFilePointerEx(file, end, nullptr, FILE_BEGIN);
                SetEndOfFile(file);
            }
            CloseHandle(file);
        });
        bool raced = false;
        bool bad = false;
        bool busy = false;
        for (int attempt = 0; attempt < 5000 && !raced && !bad && !busy; ++attempt) {
            if (stop.load()) break;  // writer failed; do not spin the full budget
            cs::Snapshot snapshot = ReadSentinel();
            const cs::Snapshot sentinel = snapshot;
            std::wstring error = L"stale error";
            const cs::Result result = cs::ReadActive(dir, &snapshot, &error);
            if (result == cs::Result::Raced) {
                raced = true;
                CheckReadFailure("concurrent size change is raced", result, error,
                                 {cs::Result::Raced});
                CheckSnapshot("raced read keeps caller snapshot", snapshot, true,
                              sentinel.bytes, sentinel.digest);
            } else if (result == cs::Result::Ok) {
                // A successful concurrent read must still be a whole snapshot,
                // never a torn prefix or an inconsistent digest.
                if (!error.empty() || !snapshot.exists ||
                    snapshot.bytes.size() > cardspec::kMaxSpecBytes ||
                    snapshot.bytes == sentinel.bytes ||
                    snapshot.digest != Sha256Hex(snapshot.bytes)) {
                    Check(false, "concurrent read returned a partial or inconsistent snapshot");
                    bad = true;
                }
            } else if (result == cs::Result::Busy) {
                // A writer holding the fixed leaf is a legal safe refusal: the
                // caller's Snapshot must stay exactly as preloaded.
                CheckReadFailure("concurrent read is busy", result, error,
                                 {cs::Result::Busy});
                CheckSnapshot("busy read keeps caller snapshot", snapshot, true,
                              sentinel.bytes, sentinel.digest);
                busy = true;
            } else {
                Check(false, "concurrent read returned an illegal result");
                bad = true;
            }
            if ((attempt & 15) == 0) ::Sleep(0);
        }
        stop.store(true);
        writer.join();
        const unsigned long code = writer_error.load();
        if (!raced && !bad) {
            if (!writer_ok.load()) {
                if (code == static_cast<unsigned long>(ERROR_SHARING_VIOLATION)) {
                    char reason[96] = {};
                    std::snprintf(reason, sizeof(reason),
                                  "writer CreateFileW sharing violation (GetLastError=%lu)",
                                  code);
                    Skip("concurrent read Raced branch", reason);
                } else {
                    std::printf("[cardspec-store-test] writer GetLastError=%lu\n", code);
                    Check(false, "concurrent read writer failed with an unexpected Win32 error");
                }
            } else {
                Skip("concurrent read Raced branch",
                     busy ? "ReadActive Busy while the writer held the leaf; Raced not observed"
                          : "size-change window not observed in this bounded run");
            }
        }
        CheckLeaves("concurrent read leaves", dir, {cs::kActiveFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"read-sharing", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kRowsDoc), "sharing fixture writes");
        HANDLE held = CreateFileW(ActivePath(dir).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(held != INVALID_HANDLE_VALUE, "sharing fixture opens an exclusive handle");
        if (held != INVALID_HANDLE_VALUE) {
            CheckReadActiveFailure("sharing-denied active", dir,
                                   {cs::Result::Busy, cs::Result::IoError,
                                    cs::Result::Refused});
            CloseHandle(held);
        }
        CheckFileBytes("sharing fixture bytes unchanged", ActivePath(dir), kRowsDoc);
        CheckLeaves("sharing leaves", dir, {cs::kActiveFile});
    }
}

void TestApplyCreate(const std::wstring& root) {
    std::wstring dir;
    if (!NewCase(root, L"apply-create", &dir)) return;
    cs::Candidate candidate;
    if (!PrepareOk("create candidate", NewInput(), &candidate)) return;

    cs::Snapshot expected;
    if (!ReadActiveOk("create expected missing", dir, &expected)) return;
    CheckSnapshotMissing("create expected snapshot", expected);

    const cs::Outcome out = cs::Apply(dir, candidate, expected);
    CheckSuccess("first creation", out);
    CheckEffect("first creation", out, cs::TargetEffect::Replaced);
    CheckFileBytes("first creation active", ActivePath(dir), candidate.canonical());
    CheckMissing("first creation backup", BackupPath(dir));
    CheckMissing("first creation temp", TempPath(dir));
    CheckLeaves("first creation leaves", dir, {cs::kActiveFile});

    cs::Snapshot after;
    if (ReadActiveOk("first creation readback", dir, &after)) {
        CheckSnapshot("first creation readback", after, true, candidate.canonical(),
                      Sha256Hex(candidate.canonical()));
    }
}

void TestApplyExistingAndRollback(const std::wstring& root) {
    std::wstring dir;
    if (!NewCase(root, L"apply-existing", &dir)) return;

    const std::string old_bytes = OldBytes();
    Check(WriteBytes(ActivePath(dir), old_bytes), "existing active fixture writes");
    cs::Candidate candidate;
    if (!PrepareOk("existing candidate", NewInput(), &candidate)) return;

    cs::Snapshot expected;
    if (!ReadActiveOk("existing expected", dir, &expected)) return;
    CheckSnapshot("existing expected snapshot", expected, true, old_bytes,
                  Sha256Hex(old_bytes));

    const cs::Outcome applied = cs::Apply(dir, candidate, expected);
    CheckSuccess("existing apply", applied);
    CheckEffect("existing apply", applied, cs::TargetEffect::Replaced);
    CheckFileBytes("existing apply active", ActivePath(dir), candidate.canonical());
    CheckFileBytes("existing apply backup keeps old bytes", BackupPath(dir), old_bytes);
    CheckMissing("existing apply temp", TempPath(dir));
    CheckLeaves("existing apply leaves", dir, {cs::kActiveFile, cs::kBackupFile});

    cs::Snapshot rollback_current;
    cs::Snapshot rollback_backup;
    if (!ReadActiveOk("rollback expected current", dir, &rollback_current)) return;
    if (!ReadBackupOk("rollback expected backup", dir, &rollback_backup)) return;
    CheckSnapshot("rollback expected current snapshot", rollback_current, true,
                  candidate.canonical(), Sha256Hex(candidate.canonical()));
    CheckSnapshot("rollback expected backup snapshot", rollback_backup, true, old_bytes,
                  Sha256Hex(old_bytes));

    const cs::Outcome rolled = cs::Rollback(dir, rollback_current, rollback_backup);
    CheckSuccess("rollback restore", rolled);
    CheckEffect("rollback restore", rolled, cs::TargetEffect::Replaced);
    CheckFileBytes("rollback restores exact old bytes", ActivePath(dir), old_bytes);
    CheckFileBytes("rollback swaps prior active into backup", BackupPath(dir),
                   candidate.canonical());
    CheckMissing("rollback temp", TempPath(dir));
    CheckLeaves("rollback leaves", dir, {cs::kActiveFile, cs::kBackupFile});

    cs::Snapshot after_active;
    cs::Snapshot after_backup;
    if (ReadActiveOk("rollback readback active", dir, &after_active)) {
        CheckSnapshot("rollback readback active", after_active, true, old_bytes,
                      Sha256Hex(old_bytes));
    }
    if (ReadBackupOk("rollback readback backup", dir, &after_backup)) {
        CheckSnapshot("rollback readback backup", after_backup, true, candidate.canonical(),
                      Sha256Hex(candidate.canonical()));
    }
}

void TestAlreadyMatches(const std::wstring& root) {
    std::wstring dir;
    if (!NewCase(root, L"already-matches", &dir)) return;
    cs::Candidate candidate;
    if (!PrepareOk("already candidate", kNewDoc, &candidate)) return;

    Check(WriteBytes(ActivePath(dir), candidate.canonical()), "already active fixture writes");
    Check(SetOldWriteTime(ActivePath(dir)), "already active time fixture sets");
    FILETIME active_before = {};
    Check(GetWriteTime(ActivePath(dir), &active_before), "already active time reads");

    cs::Snapshot expected;
    if (!ReadActiveOk("already expected", dir, &expected)) return;
    int calls = 0;
    cs::Hooks hooks;
    hooks.beforePromote = [&]() {
        ++calls;
        Check(false, "hook must not run for AlreadyMatches");
    };
    const cs::Outcome first = cs::Apply(dir, candidate, expected, hooks);
    CheckResult("already matches", first, {cs::Result::AlreadyMatches});
    CheckEffect("already matches", first, cs::TargetEffect::None);
    CheckOutcomeStatic("already matches static message", first);
    Check(calls == 0, "AlreadyMatches does not call beforePromote");
    CheckFileBytes("already matches active bytes", ActivePath(dir), candidate.canonical());
    CheckMissing("already matches backup", BackupPath(dir));
    CheckMissing("already matches temp", TempPath(dir));
    CheckLeaves("already matches leaves", dir, {cs::kActiveFile});
    FILETIME active_after = {};
    Check(GetWriteTime(ActivePath(dir), &active_after), "already active time rereads");
    Check(SameFileTime(active_before, active_after), "already matches active write time unchanged");

    Check(WriteBytes(BackupPath(dir), kOldDoc), "already backup fixture writes");
    Check(SetOldWriteTime(BackupPath(dir)), "already backup time fixture sets");
    FILETIME backup_before = {};
    Check(GetWriteTime(BackupPath(dir), &backup_before), "already backup time reads");

    cs::Snapshot with_backup;
    if (!ReadActiveOk("already with backup expected", dir, &with_backup)) return;
    const cs::Outcome second = cs::Apply(dir, candidate, with_backup);
    CheckResult("already matches with backup", second, {cs::Result::AlreadyMatches});
    CheckEffect("already matches with backup", second, cs::TargetEffect::None);
    CheckFileBytes("already backup bytes unchanged", BackupPath(dir), kOldDoc);
    CheckMissing("already with backup temp", TempPath(dir));
    CheckLeaves("already with backup leaves", dir, {cs::kActiveFile, cs::kBackupFile});
    FILETIME backup_after = {};
    Check(GetWriteTime(BackupPath(dir), &backup_after), "already backup time rereads");
    Check(SameFileTime(backup_before, backup_after),
          "already matches backup write time unchanged");

    // Byte equality matters: the same typed spec with a BOM/spacing is a real
    // replacement, not AlreadyMatches.
    {
        std::wstring byte_dir;
        if (!NewCase(root, L"already-bytes-differ", &byte_dir)) return;
        const std::string padded = std::string("\xEF\xBB\xBF") + "  " + kNewDoc + " \r\n";
        Check(WriteBytes(ActivePath(byte_dir), padded), "byte-differ active writes");
        cs::Snapshot byte_expected;
        if (!ReadActiveOk("byte-differ expected", byte_dir, &byte_expected)) return;
        const cs::Outcome out = cs::Apply(byte_dir, candidate, byte_expected);
        CheckSuccess("same spec but different bytes is applied", out);
        CheckEffect("same spec but different bytes is applied", out,
                    cs::TargetEffect::Replaced);
        CheckFileBytes("byte-differ active becomes canonical", ActivePath(byte_dir),
                       candidate.canonical());
        CheckFileBytes("byte-differ backup keeps padded bytes", BackupPath(byte_dir),
                       padded);
        CheckMissing("byte-differ temp", TempPath(byte_dir));
        CheckLeaves("byte-differ leaves", byte_dir,
                    {cs::kActiveFile, cs::kBackupFile});
    }

    // Read-only is irrelevant when the bytes already equal canonical: this is
    // AlreadyMatches with no write and no hook, not the read-only Refused path.
    {
        std::wstring readonly_dir;
        if (!NewCase(root, L"already-readonly-canonical", &readonly_dir)) return;
        Check(WriteBytes(ActivePath(readonly_dir), candidate.canonical()),
              "readonly canonical active writes");
        Check(SetFileAttributesW(ActivePath(readonly_dir).c_str(),
                                 FILE_ATTRIBUTE_READONLY) != 0,
              "readonly canonical active marks the file");
        cs::Snapshot readonly_expected;
        if (!ReadActiveOk("readonly canonical expected", readonly_dir, &readonly_expected)) {
            return;
        }
        int readonly_calls = 0;
        cs::Hooks readonly_hooks;
        readonly_hooks.beforePromote = [&]() {
            ++readonly_calls;
            Check(false, "hook must not run for readonly AlreadyMatches");
        };
        const cs::Outcome out =
            cs::Apply(readonly_dir, candidate, readonly_expected, readonly_hooks);
        CheckResult("readonly canonical already matches", out,
                    {cs::Result::AlreadyMatches});
        CheckEffect("readonly canonical already matches", out, cs::TargetEffect::None);
        CheckOutcomeStatic("readonly canonical already matches", out);
        Check(readonly_calls == 0, "readonly AlreadyMatches does not call beforePromote");
        CheckFileBytes("readonly canonical bytes unchanged", ActivePath(readonly_dir),
                       candidate.canonical());
        CheckMissing("readonly canonical backup", BackupPath(readonly_dir));
        CheckMissing("readonly canonical temp", TempPath(readonly_dir));
        CheckLeaves("readonly canonical leaves", readonly_dir, {cs::kActiveFile});
        const DWORD attributes = GetFileAttributesW(ActivePath(readonly_dir).c_str());
        Check(attributes != INVALID_FILE_ATTRIBUTES &&
                  (attributes & FILE_ATTRIBUTE_READONLY) != 0,
              "readonly canonical keeps the read-only attribute");
        SetFileAttributesW(ActivePath(readonly_dir).c_str(), FILE_ATTRIBUTE_NORMAL);
    }
}

void TestApplyRejections(const std::wstring& root) {
    cs::Candidate candidate;
    if (!PrepareOk("rejection candidate", kNewDoc, &candidate)) return;

    {
        std::wstring dir;
        if (!NewCase(root, L"reject-default-candidate", &dir)) return;
        const cs::Outcome out = cs::Apply(dir, cs::Candidate{}, cs::Snapshot{});
        CheckFailure("default candidate", out, {cs::Result::InvalidCandidate});
        CheckEffect("default candidate", out, cs::TargetEffect::None);
        CheckLeaves("default candidate leaves", dir, {});
    }

    {
        struct DirCase {
            const char* label;
            std::wstring path;
        };
        const std::wstring missing = root + L"\\apply-missing-" +
                                     std::to_wstring(GetCurrentProcessId());
        const DirCase cases[] = {
            {"relative apply directory", L"relative-cardspec-store"},
            {"UNC apply directory", L"\\\\server\\share\\cardspec-store"},
            {"device apply directory", L"\\\\.\\cardspec-store-device"},
            {"nonexistent apply directory", missing},
        };
        for (const DirCase& item : cases) {
            const cs::Outcome out = cs::Apply(item.path, candidate, cs::Snapshot{});
            CheckFailure(item.label, out, {cs::Result::InvalidDirectory});
            CheckEffect(item.label, out, cs::TargetEffect::None);
        }
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"reject-v2-active", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kRowsV2Doc), "v2 active fixture writes");
        cs::Snapshot expected;
        if (!ReadActiveOk("v2 active expected", dir, &expected)) return;
        const cs::Outcome out = cs::Apply(dir, candidate, expected);
        CheckFailure("v2 active", out, {cs::Result::Refused});
        CheckEffect("v2 active", out, cs::TargetEffect::None);
        CheckFileBytes("v2 active unchanged", ActivePath(dir), kRowsV2Doc);
        CheckMissing("v2 active backup", BackupPath(dir));
        CheckMissing("v2 active temp", TempPath(dir));
        CheckLeaves("v2 active leaves", dir, {cs::kActiveFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"reject-malformed-active", &dir)) return;
        Check(WriteBytes(ActivePath(dir), "{"), "malformed active fixture writes");
        cs::Snapshot expected;
        if (!ReadActiveOk("malformed active expected", dir, &expected)) return;
        const cs::Outcome out = cs::Apply(dir, candidate, expected);
        CheckFailure("malformed active", out, {cs::Result::Refused});
        CheckEffect("malformed active", out, cs::TargetEffect::None);
        CheckFileBytes("malformed active unchanged", ActivePath(dir), "{");
        CheckMissing("malformed active backup", BackupPath(dir));
        CheckMissing("malformed active temp", TempPath(dir));
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"reject-conflicts", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "conflict active fixture writes");

        const cs::Outcome missing_expected =
            cs::Apply(dir, candidate, cs::Snapshot{});
        CheckFailure("active exists but expected missing", missing_expected,
                     {cs::Result::Conflict});
        CheckEffect("active exists but expected missing", missing_expected,
                    cs::TargetEffect::None);
        CheckFileBytes("missing expected active unchanged", ActivePath(dir), kOldDoc);
        CheckMissing("missing expected backup", BackupPath(dir));
        CheckMissing("missing expected temp", TempPath(dir));

        const cs::Outcome bytes_mismatch =
            cs::Apply(dir, candidate, SnapshotFor(kOtherDoc));
        CheckFailure("expected bytes/digest mismatch", bytes_mismatch,
                     {cs::Result::Conflict});
        CheckEffect("expected bytes/digest mismatch", bytes_mismatch,
                    cs::TargetEffect::None);
        CheckFileBytes("bytes mismatch active unchanged", ActivePath(dir), kOldDoc);
        CheckMissing("bytes mismatch backup", BackupPath(dir));
        CheckMissing("bytes mismatch temp", TempPath(dir));

        cs::Snapshot inconsistent = SnapshotFor(kOldDoc);
        inconsistent.digest.assign(64, '0');
        const cs::Outcome invalid_baseline = cs::Apply(dir, candidate, inconsistent);
        CheckFailure("expected digest is internally inconsistent", invalid_baseline,
                     {cs::Result::InvalidBaseline});
        CheckEffect("expected digest is internally inconsistent", invalid_baseline,
                    cs::TargetEffect::None);
        CheckFileBytes("invalid baseline active unchanged", ActivePath(dir), kOldDoc);
        CheckMissing("invalid baseline backup", BackupPath(dir));
        CheckMissing("invalid baseline temp", TempPath(dir));

        cs::Snapshot missing_with_bytes;
        missing_with_bytes.exists = false;
        missing_with_bytes.bytes = "payload";
        missing_with_bytes.digest = Sha256Hex(missing_with_bytes.bytes);
        const cs::Outcome inconsistent_missing = cs::Apply(dir, candidate, missing_with_bytes);
        CheckFailure("missing expected carries bytes", inconsistent_missing,
                     {cs::Result::InvalidBaseline});
        CheckEffect("missing expected carries bytes", inconsistent_missing,
                    cs::TargetEffect::None);
        CheckFileBytes("inconsistent missing active unchanged", ActivePath(dir), kOldDoc);
        CheckMissing("inconsistent missing backup", BackupPath(dir));
        CheckMissing("inconsistent missing temp", TempPath(dir));

        cs::Snapshot preview = SnapshotFor(kOldDoc);
        Check(WriteBytes(ActivePath(dir), kOtherDoc), "external pre-preview edit writes");
        const cs::Outcome external = cs::Apply(dir, candidate, preview);
        CheckFailure("external edit before apply", external, {cs::Result::Conflict});
        CheckEffect("external edit before apply", external, cs::TargetEffect::None);
        CheckFileBytes("external pre-preview edit preserved", ActivePath(dir), kOtherDoc);
        CheckMissing("external pre-preview backup", BackupPath(dir));
        CheckMissing("external pre-preview temp", TempPath(dir));
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"reject-missing-vs-expected", &dir)) return;
        const cs::Outcome out = cs::Apply(dir, candidate, SnapshotFor(kOldDoc));
        CheckFailure("active missing but expected exists", out, {cs::Result::Conflict});
        CheckEffect("active missing but expected exists", out, cs::TargetEffect::None);
        CheckLeaves("active missing conflict leaves", dir, {});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"reject-temp-busy", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "busy active fixture writes");
        Check(WriteBytes(TempPath(dir), kTempSentinel), "busy temp fixture writes");
        cs::Snapshot expected;
        if (!ReadActiveOk("busy expected", dir, &expected)) return;
        const cs::Outcome out = cs::Apply(dir, candidate, expected);
        CheckFailure("preexisting temp", out, {cs::Result::Busy});
        CheckEffect("preexisting temp", out, cs::TargetEffect::None);
        CheckFileBytes("busy active unchanged", ActivePath(dir), kOldDoc);
        CheckFileBytes("busy temp untouched", TempPath(dir), kTempSentinel);
        CheckMissing("busy backup untouched", BackupPath(dir));
        CheckLeaves("busy leaves", dir, {cs::kActiveFile, cs::kTempFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"reject-backup-directory", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "backup-directory active fixture writes");
        Check(CreateDirectoryW(BackupPath(dir).c_str(), nullptr) != 0,
              "backup directory fixture creates");
        cs::Snapshot expected;
        if (!ReadActiveOk("backup-directory expected", dir, &expected)) return;
        const cs::Outcome out = cs::Apply(dir, candidate, expected);
        CheckFailure("backup directory", out,
                     {cs::Result::Refused, cs::Result::InvalidBackup});
        CheckEffect("backup directory", out, cs::TargetEffect::None);
        CheckFileBytes("backup-directory active unchanged", ActivePath(dir), kOldDoc);
        CheckIsDirectory("backup directory preserved", BackupPath(dir));
        CheckMissing("backup-directory temp", TempPath(dir));
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"reject-create-backup-directory", &dir)) return;
        Check(CreateDirectoryW(BackupPath(dir).c_str(), nullptr) != 0,
              "creation backup directory fixture creates");
        const cs::Outcome out = cs::Apply(dir, candidate, cs::Snapshot{});
        CheckFailure("creation with unsafe backup", out,
                     {cs::Result::Refused, cs::Result::InvalidBackup});
        CheckEffect("creation with unsafe backup", out, cs::TargetEffect::None);
        CheckMissing("creation unsafe backup active", ActivePath(dir));
        CheckIsDirectory("creation unsafe backup preserved", BackupPath(dir));
        CheckMissing("creation unsafe backup temp", TempPath(dir));
        CheckLeaves("creation unsafe backup leaves", dir, {cs::kBackupFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"reject-active-directory", &dir)) return;
        Check(CreateDirectoryW(ActivePath(dir).c_str(), nullptr) != 0,
              "active directory fixture creates");
        const cs::Outcome out = cs::Apply(dir, candidate, cs::Snapshot{});
        CheckFailure("active leaf is a directory", out, {cs::Result::Refused});
        CheckEffect("active leaf is a directory", out, cs::TargetEffect::None);
        CheckIsDirectory("active directory preserved", ActivePath(dir));
        CheckMissing("active directory backup", BackupPath(dir));
        CheckMissing("active directory temp", TempPath(dir));
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"reject-hardlink-active", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "apply hardlink fixture writes");
        const std::wstring extra = dir + L"\\extra-link.json";
        const bool linked = CreateHardLinkW(extra.c_str(), ActivePath(dir).c_str(), nullptr) != 0;
        Check(linked, "apply hardlink fixture creates a second name");
        if (linked) {
            const cs::Outcome out = cs::Apply(dir, candidate, SnapshotFor(kOldDoc));
            CheckFailure("apply multi-hardlink active", out, {cs::Result::Refused});
            CheckEffect("apply multi-hardlink active", out, cs::TargetEffect::None);
            CheckFileBytes("apply hardlink active unchanged", ActivePath(dir), kOldDoc);
            CheckFileBytes("apply hardlink second name unchanged", extra, kOldDoc);
            CheckMissing("apply hardlink backup", BackupPath(dir));
            CheckMissing("apply hardlink temp", TempPath(dir));
        }
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"reject-symlink-active", &dir)) return;
        const std::wstring target = dir + L"\\target.json";
        Check(WriteBytes(target, kOldDoc), "apply symlink target writes");
        const DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        if (!CreateSymbolicLinkW(ActivePath(dir).c_str(), target.c_str(), flags)) {
            Skip("apply symlink active", SymlinkSkipReason(GetLastError()));
        } else {
            const cs::Outcome out = cs::Apply(dir, candidate, SnapshotFor(kOldDoc));
            CheckFailure("apply symlink active", out, {cs::Result::Refused});
            CheckEffect("apply symlink active", out, cs::TargetEffect::None);
            CheckFileBytes("apply symlink target unchanged", target, kOldDoc);
            CheckMissing("apply symlink backup", BackupPath(dir));
            CheckMissing("apply symlink temp", TempPath(dir));
        }
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"reject-readonly-active", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "readonly active fixture writes");
        Check(SetFileAttributesW(ActivePath(dir).c_str(), FILE_ATTRIBUTE_READONLY) != 0,
              "readonly active fixture marks the file");
        cs::Snapshot expected;
        if (!ReadActiveOk("readonly active expected", dir, &expected)) return;
        const cs::Outcome out = cs::Apply(dir, candidate, expected);
        CheckFailure("readonly active", out, {cs::Result::Refused});
        CheckEffect("readonly active", out, cs::TargetEffect::None);
        CheckFileBytes("readonly active bytes unchanged", ActivePath(dir), kOldDoc);
        CheckMissing("readonly active backup", BackupPath(dir));
        CheckMissing("readonly active temp", TempPath(dir));
        SetFileAttributesW(ActivePath(dir).c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"reject-readonly-backup", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "readonly backup active fixture writes");
        Check(WriteBytes(BackupPath(dir), kBackupDoc), "readonly backup fixture writes");
        Check(SetFileAttributesW(BackupPath(dir).c_str(), FILE_ATTRIBUTE_READONLY) != 0,
              "readonly backup fixture marks the file");
        cs::Snapshot expected;
        if (!ReadActiveOk("readonly backup expected", dir, &expected)) return;
        const cs::Outcome out = cs::Apply(dir, candidate, expected);
        CheckFailure("readonly backup", out, {cs::Result::Refused});
        CheckEffect("readonly backup", out, cs::TargetEffect::None);
        CheckFileBytes("readonly backup active unchanged", ActivePath(dir), kOldDoc);
        CheckFileBytes("readonly backup bytes unchanged", BackupPath(dir), kBackupDoc);
        CheckMissing("readonly backup temp", TempPath(dir));
        SetFileAttributesW(BackupPath(dir).c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"reject-sharing-active", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "apply sharing fixture writes");
        HANDLE held = CreateFileW(ActivePath(dir).c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(held != INVALID_HANDLE_VALUE, "apply sharing fixture opens an exclusive handle");
        if (held != INVALID_HANDLE_VALUE) {
            const cs::Outcome out = cs::Apply(dir, candidate, SnapshotFor(kOldDoc));
            CheckFailure("apply sharing-denied active", out,
                         {cs::Result::Busy, cs::Result::IoError, cs::Result::Refused});
            CheckEffect("apply sharing-denied active", out, cs::TargetEffect::None);
            CheckMissing("apply sharing backup", BackupPath(dir));
            CheckMissing("apply sharing temp", TempPath(dir));
            CloseHandle(held);
        }
        CheckFileBytes("apply sharing active unchanged", ActivePath(dir), kOldDoc);
    }
}

void TestRollback(const std::wstring& root) {
    cs::Candidate candidate;
    if (!PrepareOk("rollback candidate", kNewDoc, &candidate)) return;

    {
        std::wstring dir;
        if (!NewCase(root, L"rollback-no-backup", &dir)) return;
        const cs::Outcome created = cs::Apply(dir, candidate, cs::Snapshot{});
        CheckSuccess("rollback creation", created);
        cs::Snapshot current;
        cs::Snapshot backup;
        if (!ReadActiveOk("rollback no-backup current", dir, &current)) return;
        if (!ReadBackupOk("rollback no-backup backup", dir, &backup)) return;
        CheckSnapshotMissing("rollback no-backup snapshot", backup);
        const cs::Outcome out = cs::Rollback(dir, current, backup);
        CheckFailure("rollback without backup", out, {cs::Result::NoBackup});
        CheckEffect("rollback without backup", out, cs::TargetEffect::None);
        CheckFileBytes("rollback without backup keeps active", ActivePath(dir),
                       candidate.canonical());
        CheckMissing("rollback without backup temp", TempPath(dir));
        CheckLeaves("rollback without backup leaves", dir, {cs::kActiveFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"rollback-swap", &dir)) return;
        const std::string old_bytes = OldBytes();
        const std::string backup_bytes = BackupBytes();
        Check(WriteBytes(ActivePath(dir), old_bytes), "rollback swap active writes");
        Check(WriteBytes(BackupPath(dir), backup_bytes), "rollback swap backup writes");
        cs::Snapshot current;
        cs::Snapshot backup;
        if (!ReadActiveOk("rollback swap current", dir, &current)) return;
        if (!ReadBackupOk("rollback swap backup", dir, &backup)) return;
        const cs::Outcome out = cs::Rollback(dir, current, backup);
        CheckSuccess("rollback swap", out);
        CheckEffect("rollback swap", out, cs::TargetEffect::Replaced);
        CheckFileBytes("rollback swap restores exact backup bytes", ActivePath(dir),
                       backup_bytes);
        CheckFileBytes("rollback swap stores prior active in backup", BackupPath(dir),
                       old_bytes);
        CheckMissing("rollback swap temp", TempPath(dir));
        CheckLeaves("rollback swap leaves", dir, {cs::kActiveFile, cs::kBackupFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"rollback-missing-current", &dir)) return;
        Check(WriteBytes(BackupPath(dir), kBackupDoc), "missing-current backup writes");
        cs::Snapshot current;
        cs::Snapshot backup;
        current.exists = false;
        if (!ReadBackupOk("missing-current backup snapshot", dir, &backup)) return;
        const cs::Outcome out = cs::Rollback(dir, current, backup);
        CheckFailure("rollback with backup but missing current", out,
                     {cs::Result::Refused});
        CheckEffect("rollback with backup but missing current", out,
                    cs::TargetEffect::None);
        CheckMissing("missing-current active", ActivePath(dir));
        CheckFileBytes("missing-current backup unchanged", BackupPath(dir), kBackupDoc);
        CheckMissing("missing-current temp", TempPath(dir));
        CheckLeaves("missing-current leaves", dir, {cs::kBackupFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"rollback-readonly-active", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "rollback readonly active writes");
        Check(WriteBytes(BackupPath(dir), kBackupDoc), "rollback readonly backup writes");
        Check(SetFileAttributesW(ActivePath(dir).c_str(), FILE_ATTRIBUTE_READONLY) != 0,
              "rollback readonly active marks the file");
        cs::Snapshot current;
        cs::Snapshot backup;
        if (!ReadActiveOk("rollback readonly current", dir, &current)) return;
        if (!ReadBackupOk("rollback readonly backup", dir, &backup)) return;
        const cs::Outcome out = cs::Rollback(dir, current, backup);
        CheckFailure("rollback readonly active", out, {cs::Result::Refused});
        CheckEffect("rollback readonly active", out, cs::TargetEffect::None);
        CheckFileBytes("rollback readonly active unchanged", ActivePath(dir), kOldDoc);
        CheckFileBytes("rollback readonly backup unchanged", BackupPath(dir), kBackupDoc);
        CheckMissing("rollback readonly active temp", TempPath(dir));
        SetFileAttributesW(ActivePath(dir).c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"rollback-readonly-backup", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "rollback readonly-bak active writes");
        Check(WriteBytes(BackupPath(dir), kBackupDoc), "rollback readonly-bak backup writes");
        Check(SetFileAttributesW(BackupPath(dir).c_str(), FILE_ATTRIBUTE_READONLY) != 0,
              "rollback readonly-bak marks the file");
        cs::Snapshot current;
        cs::Snapshot backup;
        if (!ReadActiveOk("rollback readonly-bak current", dir, &current)) return;
        if (!ReadBackupOk("rollback readonly-bak backup", dir, &backup)) return;
        const cs::Outcome out = cs::Rollback(dir, current, backup);
        CheckFailure("rollback readonly backup", out, {cs::Result::Refused});
        CheckEffect("rollback readonly backup", out, cs::TargetEffect::None);
        CheckFileBytes("rollback readonly-bak active unchanged", ActivePath(dir), kOldDoc);
        CheckFileBytes("rollback readonly-bak backup unchanged", BackupPath(dir),
                       kBackupDoc);
        CheckMissing("rollback readonly-bak temp", TempPath(dir));
        SetFileAttributesW(BackupPath(dir).c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"rollback-invalid-backup", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "invalid backup active writes");
        Check(WriteBytes(BackupPath(dir), kRowsV2Doc), "v2 backup fixture writes");
        cs::Snapshot current;
        cs::Snapshot backup;
        if (!ReadActiveOk("invalid backup current", dir, &current)) return;
        if (!ReadBackupOk("invalid backup snapshot", dir, &backup)) return;
        const cs::Outcome out = cs::Rollback(dir, current, backup);
        CheckFailure("rollback v2 backup", out, {cs::Result::InvalidBackup});
        CheckEffect("rollback v2 backup", out, cs::TargetEffect::None);
        CheckFileBytes("rollback v2 active unchanged", ActivePath(dir), kOldDoc);
        CheckFileBytes("rollback v2 backup unchanged", BackupPath(dir), kRowsV2Doc);
        CheckMissing("rollback v2 temp", TempPath(dir));
        CheckLeaves("rollback v2 leaves", dir, {cs::kActiveFile, cs::kBackupFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"rollback-malformed-backup", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "malformed backup active writes");
        Check(WriteBytes(BackupPath(dir), "{"), "malformed backup fixture writes");
        cs::Snapshot current;
        cs::Snapshot backup;
        if (!ReadActiveOk("malformed backup current", dir, &current)) return;
        if (!ReadBackupOk("malformed backup snapshot", dir, &backup)) return;
        const cs::Outcome out = cs::Rollback(dir, current, backup);
        CheckFailure("rollback malformed backup", out, {cs::Result::InvalidBackup});
        CheckEffect("rollback malformed backup", out, cs::TargetEffect::None);
        CheckFileBytes("rollback malformed active unchanged", ActivePath(dir), kOldDoc);
        CheckFileBytes("rollback malformed backup unchanged", BackupPath(dir), "{");
        CheckMissing("rollback malformed temp", TempPath(dir));
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"rollback-conflict-current", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "current-conflict active writes");
        Check(WriteBytes(BackupPath(dir), kBackupDoc), "current-conflict backup writes");
        cs::Snapshot backup;
        if (!ReadBackupOk("current-conflict backup snapshot", dir, &backup)) return;
        const cs::Outcome out = cs::Rollback(dir, SnapshotFor(kOtherDoc), backup);
        CheckFailure("rollback current mismatch", out, {cs::Result::Conflict});
        CheckEffect("rollback current mismatch", out, cs::TargetEffect::None);
        CheckFileBytes("rollback current mismatch active unchanged", ActivePath(dir), kOldDoc);
        CheckFileBytes("rollback current mismatch backup unchanged", BackupPath(dir),
                       kBackupDoc);
        CheckMissing("rollback current mismatch temp", TempPath(dir));
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"rollback-conflict-backup", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "backup-conflict active writes");
        Check(WriteBytes(BackupPath(dir), kBackupDoc), "backup-conflict backup writes");
        cs::Snapshot current;
        if (!ReadActiveOk("backup-conflict current snapshot", dir, &current)) return;
        const cs::Outcome out = cs::Rollback(dir, current, SnapshotFor(kOtherDoc));
        CheckFailure("rollback backup mismatch", out, {cs::Result::Conflict});
        CheckEffect("rollback backup mismatch", out, cs::TargetEffect::None);
        CheckFileBytes("rollback backup mismatch active unchanged", ActivePath(dir), kOldDoc);
        CheckFileBytes("rollback backup mismatch backup unchanged", BackupPath(dir),
                       kBackupDoc);
        CheckMissing("rollback backup mismatch temp", TempPath(dir));
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"rollback-invalid-baseline", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "baseline active writes");
        Check(WriteBytes(BackupPath(dir), kBackupDoc), "baseline backup writes");
        cs::Snapshot current;
        if (!ReadActiveOk("baseline current snapshot", dir, &current)) return;
        cs::Snapshot backup = SnapshotFor(kBackupDoc);
        backup.digest.assign(64, 'f');
        const cs::Outcome out = cs::Rollback(dir, current, backup);
        CheckFailure("rollback inconsistent backup baseline", out,
                     {cs::Result::InvalidBaseline});
        CheckEffect("rollback inconsistent backup baseline", out, cs::TargetEffect::None);
        CheckFileBytes("rollback inconsistent active unchanged", ActivePath(dir), kOldDoc);
        CheckFileBytes("rollback inconsistent backup unchanged", BackupPath(dir), kBackupDoc);
        CheckMissing("rollback inconsistent temp", TempPath(dir));
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"rollback-temp-busy", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "rollback busy active writes");
        Check(WriteBytes(BackupPath(dir), kBackupDoc), "rollback busy backup writes");
        Check(WriteBytes(TempPath(dir), kTempSentinel), "rollback busy temp writes");
        cs::Snapshot current;
        cs::Snapshot backup;
        if (!ReadActiveOk("rollback busy current", dir, &current)) return;
        if (!ReadBackupOk("rollback busy backup", dir, &backup)) return;
        const cs::Outcome out = cs::Rollback(dir, current, backup);
        CheckFailure("rollback preexisting temp", out, {cs::Result::Busy});
        CheckEffect("rollback preexisting temp", out, cs::TargetEffect::None);
        CheckFileBytes("rollback busy active unchanged", ActivePath(dir), kOldDoc);
        CheckFileBytes("rollback busy backup unchanged", BackupPath(dir), kBackupDoc);
        CheckFileBytes("rollback busy temp untouched", TempPath(dir), kTempSentinel);
        CheckLeaves("rollback busy leaves", dir,
                    {cs::kActiveFile, cs::kBackupFile, cs::kTempFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"rollback-invalid-directory", &dir)) return;
        const cs::Outcome out =
            cs::Rollback(L"relative-cardspec-store", SnapshotFor(kOldDoc),
                         SnapshotFor(kBackupDoc));
        CheckFailure("rollback relative directory", out, {cs::Result::InvalidDirectory});
        CheckEffect("rollback relative directory", out, cs::TargetEffect::None);
        CheckLeaves("rollback invalid directory leaves", dir, {});
    }
}

// These hooks demonstrate the documented non-CAS window: an external writer
// can act between validation and promotion. They do not claim a strong
// filesystem CAS, and no case auto-rolls back over the other writer.
void TestHooks(const std::wstring& root) {
    cs::Candidate candidate;
    if (!PrepareOk("hook candidate", kNewDoc, &candidate)) return;

    {
        std::wstring dir;
        if (!NewCase(root, L"hook-create-race", &dir)) return;
        cs::Snapshot expected;
        if (!ReadActiveOk("hook-create expected", dir, &expected)) return;
        int calls = 0;
        cs::Hooks hooks;
        hooks.beforePromote = [&]() {
            ++calls;
            Check(WriteBytes(ActivePath(dir), kOtherDoc), "hook external target writes");
        };
        const cs::Outcome out = cs::Apply(dir, candidate, expected, hooks);
        CheckFailure("hook creates target during first creation", out,
                     {cs::Result::Raced});
        CheckEffectOneOf("hook creates target during first creation", out,
                         {cs::TargetEffect::None, cs::TargetEffect::Unknown});
        Check(calls == 1, "hook is called exactly once for first creation");
        CheckFileBytes("hook raced target is not overwritten", ActivePath(dir), kOtherDoc);
        CheckMissing("hook raced backup", BackupPath(dir));
        CheckMissing("hook raced temp is cleaned", TempPath(dir));
        CheckLeaves("hook raced leaves", dir, {cs::kActiveFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"hook-existing-race", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "hook-existing active writes");
        cs::Snapshot expected;
        if (!ReadActiveOk("hook-existing expected", dir, &expected)) return;
        int calls = 0;
        cs::Hooks hooks;
        hooks.beforePromote = [&]() {
            ++calls;
            Check(WriteBytes(ActivePath(dir), kOtherDoc), "hook external edit writes");
        };
        const cs::Outcome out = cs::Apply(dir, candidate, expected, hooks);
        CheckFailure("hook edits target during existing apply", out,
                     {cs::Result::ChangedDuringWrite});
        CheckEffect("hook edits target during existing apply", out,
                    cs::TargetEffect::Replaced);
        Check(calls == 1, "hook is called exactly once for existing apply");
        CheckFileBytes("hook edited apply active", ActivePath(dir), candidate.canonical());
        CheckFileBytes("hook edited target bytes become backup", BackupPath(dir),
                       kOtherDoc);
        CheckMissing("hook edited apply temp", TempPath(dir));
        CheckLeaves("hook edited apply leaves", dir, {cs::kActiveFile, cs::kBackupFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"hook-throws", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "throwing hook active writes");
        cs::Snapshot expected;
        if (!ReadActiveOk("throwing hook expected", dir, &expected)) return;
        int calls = 0;
        cs::Hooks hooks;
        hooks.beforePromote = [&]() {
            ++calls;
            throw std::runtime_error("synthetic hook failure");
        };
        bool threw = false;
        cs::Outcome out;
        try {
            out = cs::Apply(dir, candidate, expected, hooks);
        } catch (...) {
            threw = true;
        }
        Check(calls == 1, "throwing hook is called exactly once");
        if (!threw) CheckFailure("throwing hook", out, {cs::Result::IoError});
        CheckFileBytes("throwing hook leaves active untouched", ActivePath(dir), kOldDoc);
        CheckMissing("throwing hook backup", BackupPath(dir));
        CheckMissing("throwing hook temp is cleaned", TempPath(dir));
        CheckLeaves("throwing hook leaves", dir, {cs::kActiveFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"hook-creates-backup", &dir)) return;
        cs::Snapshot expected;
        if (!ReadActiveOk("hook-backup expected", dir, &expected)) return;
        int calls = 0;
        cs::Hooks hooks;
        hooks.beforePromote = [&]() {
            ++calls;
            Check(WriteBytes(BackupPath(dir), kOtherDoc), "hook external backup writes");
        };
        const cs::Outcome out = cs::Apply(dir, candidate, expected, hooks);
        CheckResult("hook creates backup during first creation", out,
                    {cs::Result::Ok, cs::Result::ChangedDuringWrite, cs::Result::Raced,
                     cs::Result::Conflict});
        Check(calls == 1, "hook backup case is called exactly once");
        CheckFileBytes("hook-created backup is preserved", BackupPath(dir), kOtherDoc);
        CheckMissing("hook-created backup temp is cleaned", TempPath(dir));
        if (out.effect == cs::TargetEffect::Replaced) {
            CheckFileBytes("hook-created backup active", ActivePath(dir),
                           candidate.canonical());
        }
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"hook-not-on-conflict", &dir)) return;
        Check(WriteBytes(ActivePath(dir), kOldDoc), "preflight conflict active writes");
        cs::Snapshot expected;
        if (!ReadActiveOk("preflight conflict expected", dir, &expected)) return;
        Check(WriteBytes(ActivePath(dir), kOtherDoc), "preflight conflict external edit writes");
        int calls = 0;
        cs::Hooks hooks;
        hooks.beforePromote = [&]() {
            ++calls;
            Check(false, "hook must not run on a detected conflict");
        };
        const cs::Outcome out = cs::Apply(dir, candidate, expected, hooks);
        CheckFailure("hook is not reached on conflict", out, {cs::Result::Conflict});
        Check(calls == 0, "hook is not called when preflight detects a conflict");
        CheckFileBytes("preflight conflict target preserved", ActivePath(dir), kOtherDoc);
        CheckMissing("preflight conflict backup", BackupPath(dir));
        CheckMissing("preflight conflict temp", TempPath(dir));
        CheckLeaves("preflight conflict leaves", dir, {cs::kActiveFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"rollback-hook-throws", &dir)) return;
        const std::string active_bytes = OldBytes();
        const std::string backup_bytes = BackupBytes();
        Check(WriteBytes(ActivePath(dir), active_bytes), "rollback throwing active writes");
        Check(WriteBytes(BackupPath(dir), backup_bytes), "rollback throwing backup writes");
        cs::Snapshot current;
        cs::Snapshot backup;
        if (!ReadActiveOk("rollback throwing current", dir, &current)) return;
        if (!ReadBackupOk("rollback throwing backup", dir, &backup)) return;
        int calls = 0;
        cs::Hooks hooks;
        hooks.beforePromote = [&]() {
            ++calls;
            throw std::runtime_error("synthetic rollback hook failure");
        };
        bool threw = false;
        cs::Outcome out;
        try {
            out = cs::Rollback(dir, current, backup, hooks);
        } catch (...) {
            threw = true;
        }
        Check(!threw, "rollback throwing hook is contained");
        Check(calls == 1, "rollback throwing hook is called exactly once");
        if (!threw) CheckFailure("rollback throwing hook", out, {cs::Result::IoError});
        CheckEffect("rollback throwing hook", out, cs::TargetEffect::None);
        CheckFileBytes("rollback throwing active unchanged", ActivePath(dir), active_bytes);
        CheckFileBytes("rollback throwing backup unchanged", BackupPath(dir), backup_bytes);
        CheckMissing("rollback throwing temp is cleaned", TempPath(dir));
        CheckLeaves("rollback throwing leaves", dir,
                    {cs::kActiveFile, cs::kBackupFile});
    }

    {
        std::wstring dir;
        if (!NewCase(root, L"rollback-hook-edits-active", &dir)) return;
        const std::string active_bytes = OldBytes();
        const std::string backup_bytes = BackupBytes();
        Check(WriteBytes(ActivePath(dir), active_bytes), "rollback edit active writes");
        Check(WriteBytes(BackupPath(dir), backup_bytes), "rollback edit backup writes");
        cs::Snapshot current;
        cs::Snapshot backup;
        if (!ReadActiveOk("rollback edit current", dir, &current)) return;
        if (!ReadBackupOk("rollback edit backup", dir, &backup)) return;
        int calls = 0;
        cs::Hooks hooks;
        hooks.beforePromote = [&]() {
            ++calls;
            Check(WriteBytes(ActivePath(dir), kOtherDoc),
                  "rollback hook external active writes");
        };
        const cs::Outcome out = cs::Rollback(dir, current, backup, hooks);
        CheckFailure("rollback hook edits active", out,
                     {cs::Result::ChangedDuringWrite});
        CheckEffect("rollback hook edits active", out, cs::TargetEffect::Replaced);
        Check(calls == 1, "rollback editing hook is called exactly once");
        CheckFileBytes("rollback hook restores old backup", ActivePath(dir), backup_bytes);
        CheckFileBytes("rollback hook stores external bytes in backup", BackupPath(dir),
                       kOtherDoc);
        CheckMissing("rollback hook edit temp is cleaned", TempPath(dir));
        CheckLeaves("rollback hook edit leaves", dir,
                    {cs::kActiveFile, cs::kBackupFile});
    }
}

} // namespace

int wmain() {
    std::wstring root = ExeDir() + L"\\cardspec-store-" + kCanaryW + L"-" +
                        std::to_wstring(GetCurrentProcessId());
    DeleteTree(root);
    const bool root_ok = CreateDirectoryW(root.c_str(), nullptr) != 0;
    Check(root_ok, "synthetic root directory created");
    if (root_ok) {
        TestPrepare();
        TestReadSnapshots(root);
        TestApplyCreate(root);
        TestApplyExistingAndRollback(root);
        TestAlreadyMatches(root);
        TestApplyRejections(root);
        TestRollback(root);
        TestHooks(root);
        DeleteTree(root);
        Check(!Exists(root), "synthetic root cleanup removes the synthetic tree");
    }

    std::printf("[cardspec-store-test] checks=%d failed=%d skips=%d\n", checks, failures,
                skips);
    return failures == 0 ? 0 : 1;
}
