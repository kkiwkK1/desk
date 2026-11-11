// M2.2 native-only proposal/file lifecycle core for CardSpec.
//
// Safety invariants kept by this translation unit:
//   * no printing, no logging, no network, no exec, no model, no ini and no
//     user-chosen source path: the caller supplies one absolute local
//     directory and only the three fixed leaf names are ever touched;
//   * no error text ever repeats a path or a payload byte - messages are
//     fixed strings, so a document (which may carry URLs or secrets) can
//     never leak through diagnostics;
//   * every kernel handle is owned by a small RAII wrapper: no detached
//     resources on any early return or exception;
//   * v1 inline only. Candidate bytes are re-validated and re-serialized to a
//     fixed point, and the existing active file must parse as v1 inline before
//     it can become the single backup slot;
//   * preflight rejects every unsafe shape (relative/UNC/device/remote
//     directory, final-directory reparse, leaf that is a directory/reparse/
//     multi-hardlink) *before* any temp or backup write, and a detected
//     baseline conflict performs no writes at all;
//   * the temp file is created with CREATE_NEW, written fully and flushed, so
//     promotion can be a single ReplaceFileW (existing active) or a no-replace
//     rename (first creation). A pre-existing temp is Busy and is never
//     truncated or deleted;
//   * this is not a strong filesystem CAS. A cooperating named mutex only
//     serializes this product inside the current Windows session; a
//     noncooperating writer can still change directory entries between
//     validation and promotion. Post-promotion reads therefore report
//     Raced/ChangedDuringWrite/Unknown instead of assuming success, and no
//     automatic rollback ever overwrites another writer's update.

#include "data/cardspec/store.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace cardspec::store {
namespace {

constexpr DWORD kMutexWaitMs = 2000;
constexpr std::size_t kDigestChars = 64;
// ReplaceFileW can fail after the target was already touched (1176/1177).
// Those codes are indeterminate, so the temp file is preserved instead of
// being deleted blindly.
constexpr DWORD kErrUnableToMoveReplacement = 1176;
constexpr DWORD kErrUnableToMoveReplacement2 = 1177;

bool Fail(std::wstring* error, std::wstring message) {
    if (error) *error = std::move(message);
    return false;
}

Result FailResult(Result result, std::wstring* error, std::wstring message) {
    if (error) *error = std::move(message);
    return result;
}

// --- RAII handle --------------------------------------------------------

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) : handle_(handle) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    void Reset(HANDLE handle = nullptr) {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = handle;
    }
    HANDLE get() const { return handle_; }
    bool valid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_ = nullptr;
};

// --- SHA-256 (lowercase hex) -------------------------------------------

// Local CSP only: CRYPT_VERIFYCONTEXT creates no key container and
// CRYPT_SILENT keeps any provider UI off the desktop. No key material and no
// network are involved, and the provider is released on every path.
bool Sha256Hex(std::string_view bytes, std::string* hex) {
    hex->clear();
    if (bytes.size() > 0xFFFFFFFFull) return false;

    HCRYPTPROV provider = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        return false;
    }
    HCRYPTHASH hash = 0;
    bool ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE;
    if (ok && !bytes.empty()) {
        ok = CryptHashData(hash, reinterpret_cast<const BYTE*>(bytes.data()),
                           static_cast<DWORD>(bytes.size()), 0) != FALSE;
    }
    unsigned char digest[32] = {};
    DWORD digest_size = static_cast<DWORD>(sizeof(digest));
    if (ok) {
        ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_size, 0) != FALSE;
    }
    if (ok && digest_size != static_cast<DWORD>(sizeof(digest))) ok = false;
    if (hash != 0) CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    if (!ok) return false;

    static constexpr char kHex[] = "0123456789abcdef";
    hex->reserve(kDigestChars);
    for (const unsigned char byte : digest) {
        hex->push_back(kHex[byte >> 4]);
        hex->push_back(kHex[byte & 0x0Fu]);
    }
    return true;
}

bool IsLowerHex64(std::string_view text) {
    if (text.size() != kDigestChars) return false;
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// --- Paths --------------------------------------------------------------

bool IsAsciiLetter(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

// Absolute "X:\..." form only: this rejects UNC (\\server\share), device
// (\\.\, \\?\), rooted (\foo), drive-relative (C:foo) and plain relative
// paths before any filesystem call.
bool LooksLocalAbsolute(const std::wstring& path) {
    if (path.size() < 3) return false;
    if (!IsAsciiLetter(path[0]) || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) {
        return false;
    }
    for (std::size_t i = 2; i < path.size(); ++i) {
        if (path[i] == L':') return false;  // alternate data stream or second drive
    }
    return true;
}

std::wstring JoinLeaf(const std::wstring& directory, const wchar_t* leaf) {
    if (!directory.empty() && directory.back() == L'\\') return directory + leaf;
    return directory + L"\\" + leaf;
}

// Resolves the caller's directory to one canonical form and refuses anything
// that is not an existing local directory whose final component is not a
// reparse point. Ancestor directories are trusted: this is not a sandbox
// against the same user deliberately swapping a parent mid-operation.
Result NormalizeDirectory(const std::wstring& directory, std::wstring* out,
                          std::wstring* error) {
    out->clear();
    if (directory.empty()) {
        return FailResult(Result::InvalidDirectory, error, L"配置目录为空");
    }
    if (directory.find(L'\0') != std::wstring::npos) {
        return FailResult(Result::InvalidDirectory, error, L"配置目录包含非法字符");
    }
    if (directory.size() > 32767) {
        return FailResult(Result::InvalidDirectory, error, L"配置目录过长");
    }
    if (!LooksLocalAbsolute(directory)) {
        return FailResult(Result::InvalidDirectory, error,
                          L"配置目录必须是绝对本地盘路径（拒绝 UNC/设备/相对路径）");
    }

    std::wstring buffer(512, L'\0');
    for (;;) {
        const DWORD needed = GetFullPathNameW(directory.c_str(),
                                              static_cast<DWORD>(buffer.size()),
                                              buffer.data(), nullptr);
        if (needed == 0) {
            return FailResult(Result::InvalidDirectory, error, L"无法解析配置目录");
        }
        if (needed < buffer.size()) {
            buffer.resize(needed);
            break;
        }
        buffer.assign(static_cast<std::size_t>(needed) + 1, L'\0');
    }

    std::wstring normalized = std::move(buffer);
    while (normalized.size() > 3 && normalized.back() == L'\\') normalized.pop_back();
    if (!LooksLocalAbsolute(normalized)) {
        return FailResult(Result::InvalidDirectory, error,
                          L"配置目录必须是绝对本地盘路径（拒绝 UNC/设备/相对路径）");
    }

    const std::wstring root = normalized.substr(0, 3);
    const UINT drive = GetDriveTypeW(root.c_str());
    if (drive == DRIVE_REMOTE || drive == DRIVE_UNKNOWN || drive == DRIVE_NO_ROOT_DIR) {
        return FailResult(Result::InvalidDirectory, error, L"配置目录不能是网络盘或未知盘");
    }

    const DWORD attributes = GetFileAttributesW(normalized.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return FailResult(Result::InvalidDirectory, error, L"配置目录不存在或不可访问");
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return FailResult(Result::InvalidDirectory, error, L"配置目录是链接或 reparse point");
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return FailResult(Result::InvalidDirectory, error, L"配置目录不是目录");
    }

    *out = std::move(normalized);
    return Result::Ok;
}

std::wstring UpperAscii(std::wstring text) {
    for (wchar_t& c : text) {
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    }
    return text;
}

std::wstring HexToWide(const std::string& hex) {
    std::wstring out;
    out.reserve(hex.size());
    for (const char c : hex) out.push_back(static_cast<wchar_t>(c));
    return out;
}

// --- Cooperating session mutex -----------------------------------------

// Serializes this product inside the current session (Local prefix), so two of
// our own writers cannot interleave. It is explicitly not an external edit
// lock: another process may still rewrite the file.
class SessionMutex {
public:
    SessionMutex() = default;
    ~SessionMutex() {
        if (handle_ != nullptr) {
            if (owned_) ReleaseMutex(handle_);
            CloseHandle(handle_);
        }
    }
    SessionMutex(const SessionMutex&) = delete;
    SessionMutex& operator=(const SessionMutex&) = delete;

    Result Acquire(const std::wstring& normalized_directory, std::wstring* error) {
        // Hash the UTF-16 bytes of the case-folded path: one name per
        // directory, no path text in the kernel object name beyond the hash.
        const std::wstring folded = UpperAscii(normalized_directory);
        const std::string_view folded_bytes(reinterpret_cast<const char*>(folded.data()),
                                            folded.size() * sizeof(wchar_t));
        std::string digest;
        if (!Sha256Hex(folded_bytes, &digest)) {
            return FailResult(Result::IoError, error, L"无法创建命名互斥体名称");
        }
        const std::wstring name = L"Local\\desk-cardspec-store-" + HexToWide(digest);
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (handle_ == nullptr) {
            return FailResult(Result::IoError, error, L"无法创建命名互斥体");
        }
        const DWORD wait = WaitForSingleObject(handle_, kMutexWaitMs);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            owned_ = true;
            return Result::Ok;
        }
        CloseHandle(handle_);
        handle_ = nullptr;
        if (wait == WAIT_TIMEOUT) {
            return FailResult(Result::Busy, error, L"等待命名互斥体超时");
        }
        return FailResult(Result::IoError, error, L"等待命名互斥体失败");
    }

private:
    HANDLE handle_ = nullptr;
    bool owned_ = false;
};

// --- Fixed leaves -------------------------------------------------------

struct LeafInfo {
    bool exists = false;
    DWORD attributes = 0;
};

Result QueryLeaf(const std::wstring& path, LeafInfo* out, std::wstring* error) {
    *out = LeafInfo{};
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return Result::Ok;
        if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION ||
            code == ERROR_BUSY) {
            return FailResult(Result::Busy, error, L"规格文件被其他进程占用");
        }
        return FailResult(Result::IoError, error, L"无法查询规格文件状态");
    }
    out->exists = true;
    out->attributes = attributes;
    return Result::Ok;
}

// Bounded, fail-closed read of one fixed leaf. Missing is Ok + !exists; an
// existing empty file is exists=true with bytes empty and the SHA-256 of the
// empty string. Unsafe leaves are refused instead of followed.
// *out is written only on success, so every failure leaves the caller's
// Snapshot exactly as it was.
Result ReadLeaf(const std::wstring& path, Snapshot* out, DWORD* attributes,
                std::wstring* error) {
    if (attributes) *attributes = 0;

    LeafInfo info;
    Result result = QueryLeaf(path, &info, error);
    if (result != Result::Ok) return result;
    if (!info.exists) {
        *out = Snapshot{};
        return Result::Ok;
    }
    if (attributes) *attributes = info.attributes;
    if ((info.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return FailResult(Result::Refused, error, L"规格文件是链接或 reparse point");
    }
    if ((info.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return FailResult(Result::Refused, error, L"规格路径是目录");
    }

    // FILE_FLAG_OPEN_REPARSE_POINT opens the link itself, so a leaf swapped
    // for a symlink/junction is seen and refused, never silently followed.
    // FILE_SHARE_WRITE is deliberately not granted: while this handle is open a
    // concurrent writer cannot open the file for writing, which is what makes
    // the size/timestamp recheck below meaningful. This is still not a
    // malicious-same-user CAS - it only blocks ordinary in-place writers.
    Handle file(CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file.valid()) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return FailResult(Result::Raced, error, L"读取期间规格文件消失");
        }
        if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION ||
            code == ERROR_BUSY) {
            return FailResult(Result::Busy, error, L"规格文件被其他进程占用");
        }
        return FailResult(Result::IoError, error, L"无法打开规格文件");
    }

    BY_HANDLE_FILE_INFORMATION handle_info{};
    if (!GetFileInformationByHandle(file.get(), &handle_info)) {
        return FailResult(Result::IoError, error, L"无法读取规格文件信息");
    }
    if ((handle_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return FailResult(Result::Refused, error, L"规格文件是链接或 reparse point");
    }
    if ((handle_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return FailResult(Result::Refused, error, L"规格路径是目录");
    }
    // A second name for the same bytes would let another location mutate the
    // baseline after we hashed it, so multi-hardlink leaves are refused.
    if (handle_info.nNumberOfLinks > 1) {
        return FailResult(Result::Refused, error, L"规格文件存在多个硬链接");
    }

    const ULONGLONG declared = (static_cast<ULONGLONG>(handle_info.nFileSizeHigh) << 32) |
                               handle_info.nFileSizeLow;
    if (declared > static_cast<ULONGLONG>(kMaxSpecBytes)) {
        return FailResult(Result::TooLarge, error, L"规格文件超过 32 KiB 上限");
    }

    std::string bytes;
    bytes.reserve(static_cast<std::size_t>(declared));
    for (;;) {
        char buffer[4096];
        const std::size_t room = kMaxSpecBytes + 1 - bytes.size();
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(sizeof(buffer), room));
        DWORD got = 0;
        if (!ReadFile(file.get(), buffer, want, &got, nullptr)) {
            const DWORD code = GetLastError();
            if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
                return FailResult(Result::Busy, error, L"规格文件被其他进程占用");
            }
            return FailResult(Result::IoError, error, L"读取规格文件失败");
        }
        if (got == 0) break;
        bytes.append(buffer, got);
        if (bytes.size() > kMaxSpecBytes) {
            // One byte past the cap is enough to prove the file is larger than
            // the documented limit; the prefix is never reported as a spec.
            return FailResult(Result::TooLarge, error, L"规格文件超过 32 KiB 上限");
        }
    }

    // Re-query size and last-write time on the same handle: a change of either
    // means the bytes we read may not be one document, so nothing is published.
    BY_HANDLE_FILE_INFORMATION after_info{};
    if (!GetFileInformationByHandle(file.get(), &after_info)) {
        return FailResult(Result::IoError, error, L"无法复核规格文件大小");
    }
    const ULONGLONG final_size = (static_cast<ULONGLONG>(after_info.nFileSizeHigh) << 32) |
                                 after_info.nFileSizeLow;
    if (final_size > static_cast<ULONGLONG>(kMaxSpecBytes)) {
        return FailResult(Result::TooLarge, error, L"规格文件超过 32 KiB 上限");
    }
    if (final_size != bytes.size() ||
        after_info.ftLastWriteTime.dwLowDateTime != handle_info.ftLastWriteTime.dwLowDateTime ||
        after_info.ftLastWriteTime.dwHighDateTime != handle_info.ftLastWriteTime.dwHighDateTime) {
        // Torn read / metadata change against another writer: publish nothing.
        return FailResult(Result::Raced, error, L"读取期间规格文件发生变化");
    }

    std::string digest;
    if (!Sha256Hex(bytes, &digest)) {
        return FailResult(Result::IoError, error, L"无法计算规格文件摘要");
    }
    Snapshot snapshot;
    snapshot.exists = true;
    snapshot.bytes = std::move(bytes);
    snapshot.digest = std::move(digest);
    *out = std::move(snapshot);
    return Result::Ok;
}

bool SameSnapshot(const Snapshot& a, const Snapshot& b) {
    if (a.exists != b.exists) return false;
    if (!a.exists) return true;
    return a.bytes == b.bytes && a.digest == b.digest;
}

// Caller-supplied baselines must be self-consistent before they can be
// compared with the disk: a digest that does not match its own bytes cannot
// prove anything about the file we are about to overwrite. InvalidBaseline is
// reserved for that internal inconsistency (including an over-limit baseline),
// never for a disagreement with what is on disk (that is Conflict).
Result ValidateBaseline(const Snapshot& baseline, std::wstring* error) {
    if (!baseline.exists) {
        if (!baseline.bytes.empty() || !baseline.digest.empty()) {
            return FailResult(Result::InvalidBaseline, error,
                              L"调用方基线内部不一致：缺失却带内容");
        }
        return Result::Ok;
    }
    if (baseline.bytes.size() > kMaxSpecBytes) {
        return FailResult(Result::InvalidBaseline, error, L"调用方基线超过 32 KiB 上限");
    }
    if (!IsLowerHex64(baseline.digest)) {
        return FailResult(Result::InvalidBaseline, error, L"调用方基线摘要格式无效");
    }
    std::string digest;
    if (!Sha256Hex(baseline.bytes, &digest)) {
        return FailResult(Result::IoError, error, L"无法计算调用方基线摘要");
    }
    if (digest != baseline.digest) {
        return FailResult(Result::InvalidBaseline, error, L"调用方基线摘要与内容不一致");
    }
    return Result::Ok;
}

// v1 inline only: cardspec::Parse rejects every other source type and every
// unknown key, and returns field/limit text that we deliberately drop.
bool IsValidV1Inline(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxSpecBytes) return false;
    Spec spec;
    std::wstring ignored;
    return cardspec::Parse(bytes, &spec, &ignored);
}

// --- Temp file ownership ------------------------------------------------

struct TempFile {
    std::wstring path;
    BY_HANDLE_FILE_INFORMATION identity{};
    bool created = false;
    bool identity_known = false;
    bool promoted = false;
};

// Deletes only the file this operation created: the name is re-opened, its
// volume serial + file index must still match the handle we wrote, and it must
// still be a plain file. Anything else (replaced, linked, unknown state) is
// left in place - a leftover temp is Busy for the next call, never data loss.
void DiscardOwnTemp(TempFile* temp) {
    if (!temp->created || temp->promoted || !temp->identity_known) return;
    Handle probe(CreateFileW(temp->path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!probe.valid()) return;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(probe.get(), &info)) return;
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return;
    }
    if (info.dwVolumeSerialNumber != temp->identity.dwVolumeSerialNumber ||
        info.nFileIndexHigh != temp->identity.nFileIndexHigh ||
        info.nFileIndexLow != temp->identity.nFileIndexLow) {
        return;
    }
    DeleteFileW(temp->path.c_str());
}

// FileDispositionInfo through SetFileInformationByHandle, resolved at run time
// so this translation unit does not depend on the toolchain's _WIN32_WINNT
// default. The enum ordinal (4) and the one-byte BOOLEAN layout are fixed by
// the Win32 ABI. Used only on a handle we still own, so the deletion is bound
// to that file identity instead of a re-resolved path.
bool MarkDeleteOnClose(HANDLE file) {
    using SetFileInformationByHandleFn = BOOL(WINAPI*)(HANDLE, int, void*, DWORD);
    static const SetFileInformationByHandleFn set_info = []() -> SetFileInformationByHandleFn {
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (kernel == nullptr) return nullptr;
        // Via void* so no function-pointer-to-function-pointer cast is needed.
        void* symbol = reinterpret_cast<void*>(GetProcAddress(kernel, "SetFileInformationByHandle"));
        return reinterpret_cast<SetFileInformationByHandleFn>(symbol);
    }();
    if (set_info == nullptr) return false;
    struct Disposition {
        BOOLEAN delete_file;  // FILE_DISPOSITION_INFO is one byte, not a BOOL
    };
    Disposition disposition{TRUE};
    constexpr int kFileDispositionInfo = 4;
    return set_info(file, kFileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
}

// CREATE_NEW + full write + FlushFileBuffers: the promoted file is never a
// partial document, and an existing temp is never truncated or reused. DELETE
// access lets every failure path dispose of the file through the handle we
// still own; if even that fails the temp is left in place and reported.
Result CreateTemp(const std::wstring& path, std::string_view bytes, TempFile* temp,
                  std::wstring* error) {
    *temp = TempFile{};
    temp->path = path;

    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
            return FailResult(Result::Busy, error, L"临时文件已存在");
        }
        if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION ||
            code == ERROR_BUSY) {
            return FailResult(Result::Busy, error, L"临时文件被其他进程占用");
        }
        return FailResult(Result::IoError, error, L"无法创建临时文件");
    }
    temp->created = true;

    if (!GetFileInformationByHandle(file.get(), &temp->identity)) {
        // Identity unknown, so a path-based delete could hit someone else's
        // file: dispose through the owned handle instead. If that also fails the
        // temp stays behind, which the static error text states.
        MarkDeleteOnClose(file.get());
        file.Reset();
        return FailResult(Result::IoError, error,
                          L"无法读取临时文件信息，临时文件可能残留");
    }
    temp->identity_known = true;

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(file.get(), bytes.data() + offset, chunk, &written, nullptr) ||
            written != chunk) {
            MarkDeleteOnClose(file.get());
            file.Reset();
            DiscardOwnTemp(temp);
            return FailResult(Result::IoError, error, L"写入临时文件失败");
        }
        offset += written;
    }
    if (!FlushFileBuffers(file.get())) {
        MarkDeleteOnClose(file.get());
        file.Reset();
        DiscardOwnTemp(temp);
        return FailResult(Result::IoError, error, L"刷新临时文件失败");
    }
    file.Reset();
    return Result::Ok;
}

// Test-only seam: called at most once, before any promotion, and an exception
// is contained so the temp file we own is still cleaned up.
bool RunHook(const Hooks& hooks, std::wstring* error) {
    if (!hooks.beforePromote) return true;
    try {
        hooks.beforePromote();
    } catch (...) {
        return Fail(error, L"测试钩子在提升前抛出异常");
    }
    return true;
}

Result MapPromoteError(DWORD code, bool creation_only) {
    switch (code) {
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_BUSY: return Result::Busy;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return Result::Raced;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: return creation_only ? Result::Raced : Result::IoError;
        case ERROR_ACCESS_DENIED:
        case ERROR_WRITE_PROTECT: return Result::Refused;
        default: return Result::IoError;
    }
}

bool IndeterminatePromotion(DWORD code) {
    return code == kErrUnableToMoveReplacement || code == kErrUnableToMoveReplacement2;
}

// Codes that prove this operation did not touch the target: the no-replace
// rename found an existing file, or the source/target entry was already gone.
// Everything else is judged by the readback only.
bool PromotionProvedUntouched(DWORD code, bool creation_only) {
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
    if (creation_only && (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS)) return true;
    return false;
}

void FillObserved(const std::wstring& active_path, Outcome* out) {
    Snapshot observed;
    std::wstring ignored;
    if (ReadLeaf(active_path, &observed, nullptr, &ignored) == Result::Ok) {
        out->observed = std::move(observed);
    }
}

bool ReadonlyAttribute(DWORD attributes) {
    return (attributes & FILE_ATTRIBUTE_READONLY) != 0;
}

} // namespace

bool Prepare(std::string_view utf8, Candidate* out, std::wstring* error) {
    if (error) error->clear();
    if (!out) return Fail(error, L"Prepare：输出对象为空");
    if (utf8.size() > kMaxSpecBytes) {
        return Fail(error, L"候选规格超过 " + std::to_wstring(kMaxSpecBytes) + L" 字节上限");
    }

    // Strict v1 inline schema: unknown/duplicate keys, non-inline source and
    // invalid UTF-8 all fail here, and the error text stays field/limit only.
    Spec parsed;
    if (!cardspec::Parse(utf8, &parsed, error)) return false;

    std::wstring serialize_error;
    std::string canonical = cardspec::Serialize(parsed, &serialize_error);
    if (canonical.empty()) return Fail(error, L"无法序列化候选规格");
    if (canonical.size() > kMaxSpecBytes) {
        return Fail(error, L"候选规格的规范形式超过 " + std::to_wstring(kMaxSpecBytes) +
                               L" 字节上限");
    }

    // The bytes handed to the filesystem must be a fixed point of
    // Parse+Serialize so a later reader round-trips exactly what Apply wrote.
    Spec reparsed;
    std::wstring ignored;
    if (!cardspec::Parse(canonical, &reparsed, &ignored)) {
        return Fail(error, L"候选规格的规范形式无法重新解析");
    }
    if (cardspec::Serialize(reparsed, &ignored) != canonical) {
        return Fail(error, L"候选规格的规范形式不稳定");
    }

    Candidate prepared;
    prepared.spec_ = std::move(reparsed);
    prepared.canonical_ = std::move(canonical);
    *out = std::move(prepared);  // failure above leaves *out untouched
    if (error) error->clear();
    return true;
}

Result ReadActive(const std::wstring& trustedDirectory, Snapshot* out,
                  std::wstring* error) {
    if (error) error->clear();
    if (!out) return FailResult(Result::InvalidDirectory, error, L"ReadActive：输出对象为空");
    std::wstring directory;
    const Result normalized = NormalizeDirectory(trustedDirectory, &directory, error);
    if (normalized != Result::Ok) return normalized;
    return ReadLeaf(JoinLeaf(directory, kActiveFile), out, nullptr, error);
}

Result ReadBackup(const std::wstring& trustedDirectory, Snapshot* out,
                  std::wstring* error) {
    if (error) error->clear();
    if (!out) return FailResult(Result::InvalidDirectory, error, L"ReadBackup：输出对象为空");
    std::wstring directory;
    const Result normalized = NormalizeDirectory(trustedDirectory, &directory, error);
    if (normalized != Result::Ok) return normalized;
    return ReadLeaf(JoinLeaf(directory, kBackupFile), out, nullptr, error);
}

Outcome Apply(const std::wstring& trustedDirectory, const Candidate& candidate,
              const Snapshot& expectedCurrent, const Hooks& hooks) {
    Outcome out;

    // 1. Re-validate the candidate: only v1 inline in canonical form is written.
    if (!candidate.valid()) {
        out.result = Result::InvalidCandidate;
        out.message = L"候选规格为空";
        return out;
    }
    std::string canonical = candidate.canonical();
    if (canonical.size() > kMaxSpecBytes) {
        out.result = Result::TooLarge;
        out.message = L"候选规格超过 32 KiB 上限";
        return out;
    }
    {
        Spec checked;
        std::wstring ignored;
        if (!cardspec::Parse(canonical, &checked, &ignored)) {
            out.result = Result::InvalidCandidate;
            out.message = L"候选规格无法解析为 v1 inline 规格";
            return out;
        }
        if (cardspec::Serialize(checked, &ignored) != canonical) {
            out.result = Result::InvalidCandidate;
            out.message = L"候选规格不是规范形式";
            return out;
        }
    }
    std::string canonical_digest;
    if (!Sha256Hex(canonical, &canonical_digest)) {
        out.result = Result::IoError;
        out.message = L"无法计算候选规格摘要";
        return out;
    }

    // 2. Directory, fixed leaf paths, then the cooperating session mutex.
    std::wstring directory;
    Result result = NormalizeDirectory(trustedDirectory, &directory, &out.message);
    if (result != Result::Ok) {
        out.result = result;
        return out;
    }

    const std::wstring active_path = JoinLeaf(directory, kActiveFile);
    const std::wstring backup_path = JoinLeaf(directory, kBackupFile);
    const std::wstring temp_path = JoinLeaf(directory, kTempFile);

    SessionMutex session;
    result = session.Acquire(directory, &out.message);
    if (result != Result::Ok) {
        out.result = result;
        FillObserved(active_path, &out);
        return out;
    }

    // 3. Baseline preflight. Everything up to the temp write is read-only, so a
    //    conflict or an unsafe slot returns without touching target/bak/tmp.
    result = ValidateBaseline(expectedCurrent, &out.message);
    if (result != Result::Ok) {
        out.result = result;
        FillObserved(active_path, &out);
        return out;
    }

    Snapshot current;
    DWORD current_attributes = 0;
    result = ReadLeaf(active_path, &current, &current_attributes, &out.message);
    if (result != Result::Ok) {
        out.result = result;
        FillObserved(active_path, &out);
        return out;
    }
    out.observed = current;

    if (current.exists != expectedCurrent.exists || !SameSnapshot(current, expectedCurrent)) {
        out.result = Result::Conflict;
        out.message = L"活动规格与调用方基线不一致";
        return out;
    }
    if (current.exists && current.bytes == canonical) {
        // Byte-identical canonical content: no write, no backup churn, and the
        // test hook is never invoked because nothing is promoted. This precedes
        // the readonly write-qualification: a readonly file that already holds
        // these exact bytes needs no write at all.
        out.effect = TargetEffect::None;
        out.result = Result::AlreadyMatches;
        out.message = L"活动规格与候选内容一致，无需写入";
        return out;
    }
    if (current.exists && !IsValidV1Inline(current.bytes)) {
        // Without a valid v1 predecessor the single backup slot could not hold
        // a restorable document, so replacement is refused, not attempted.
        out.result = Result::Refused;
        out.message = L"已存在的活动规格不是合法的 v1 inline 规格";
        return out;
    }
    if (current.exists && ReadonlyAttribute(current_attributes)) {
        out.result = Result::Refused;
        out.message = L"活动规格是只读文件";
        return out;
    }

    // The backup slot must be safe for reuse before anything is written - this
    // also covers the first-creation path, which never deletes it.
    LeafInfo backup_info;
    result = QueryLeaf(backup_path, &backup_info, &out.message);
    if (result != Result::Ok) {
        out.result = result;
        return out;
    }
    if (backup_info.exists) {
        if ((backup_info.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
            0) {
            out.result = Result::Refused;
            out.message = L"备份路径是目录或链接，拒绝写入";
            return out;
        }
        if (ReadonlyAttribute(backup_info.attributes)) {
            out.result = Result::Refused;
            out.message = L"备份文件是只读文件";
            return out;
        }
        Snapshot existing_backup;
        result = ReadLeaf(backup_path, &existing_backup, nullptr, &out.message);
        if (result != Result::Ok) {
            out.result = result;
            return out;
        }
    }

    // A pre-existing temp means some earlier or foreign operation is in an
    // unknown state: report Busy and leave it untouched.
    LeafInfo temp_info;
    result = QueryLeaf(temp_path, &temp_info, &out.message);
    if (result != Result::Ok) {
        out.result = result;
        return out;
    }
    if (temp_info.exists) {
        if (ReadonlyAttribute(temp_info.attributes)) {
            out.result = Result::Refused;
            out.message = L"临时文件是只读文件";
            return out;
        }
        out.result = Result::Busy;
        out.message = L"临时文件已存在";
        return out;
    }

    // 4. Owned temp: written fully and flushed before any promotion.
    TempFile temp;
    result = CreateTemp(temp_path, canonical, &temp, &out.message);
    if (result != Result::Ok) {
        DiscardOwnTemp(&temp);
        out.result = result;
        FillObserved(active_path, &out);
        return out;
    }

    if (!RunHook(hooks, &out.message)) {
        DiscardOwnTemp(&temp);
        out.result = Result::IoError;
        FillObserved(active_path, &out);
        return out;
    }

    // 5. Single promotion step.
    const bool had_current = current.exists;
    bool promoted = false;
    DWORD promote_code = 0;
    if (had_current) {
        // Atomic replace of the existing file; the replaced bytes land in the
        // single backup slot as part of the same operation.
        promoted = ReplaceFileW(active_path.c_str(), temp_path.c_str(), backup_path.c_str(), 0,
                                nullptr, nullptr) != FALSE;
    } else {
        // No REPLACE_EXISTING: a file created by a racing writer is reported,
        // never overwritten.
        promoted = MoveFileExW(temp_path.c_str(), active_path.c_str(), MOVEFILE_WRITE_THROUGH) !=
                   FALSE;
    }
    if (promoted) {
        temp.promoted = true;
        out.effect = TargetEffect::Replaced;
    } else {
        promote_code = GetLastError();
        // Never assume the target is untouched on failure: read it back and
        // classify what is actually observable now.
        Snapshot after;
        std::wstring ignored;
        const bool after_ok = ReadLeaf(active_path, &after, nullptr, &ignored) == Result::Ok;
        if (after_ok) out.observed = after;
        if (after_ok && after.exists && after.digest == canonical_digest &&
            after.bytes == canonical) {
            out.effect = TargetEffect::Replaced;
        } else if (PromotionProvedUntouched(promote_code, !had_current)) {
            out.effect = TargetEffect::None;
        } else if (after_ok && SameSnapshot(after, expectedCurrent)) {
            out.effect = TargetEffect::None;
        } else {
            out.effect = TargetEffect::Unknown;
        }

        if (out.effect != TargetEffect::Replaced) {
            if (!IndeterminatePromotion(promote_code)) {
                DiscardOwnTemp(&temp);
            }
            out.result = MapPromoteError(promote_code, !had_current);
            out.message = L"提升临时文件失败";
            FillObserved(active_path, &out);
            return out;
        }
        // The observable postcondition already holds; fall through to verify
        // the backup slot as well instead of reporting a spurious failure.
        // Identity-checked cleanup keeps a leftover temp from locking out the
        // next call; a temp already consumed by the promotion is simply gone.
        DiscardOwnTemp(&temp);
    }

    // 6. Post-promotion readback: external drift is reported, never "fixed" by
    //    rolling back over another writer's update.
    Snapshot after_active;
    result = ReadLeaf(active_path, &after_active, nullptr, &out.message);
    if (result != Result::Ok) {
        out.effect = TargetEffect::Unknown;  // cannot see the target now
        out.result = Result::ChangedDuringWrite;
        out.message = L"提升后无法复读活动规格";
        return out;
    }
    if (!after_active.exists || after_active.digest != canonical_digest ||
        after_active.bytes != canonical) {
        out.effect = TargetEffect::Replaced;
        out.result = Result::ChangedDuringWrite;
        out.message = L"提升后活动规格与候选不一致";
        FillObserved(active_path, &out);
        return out;
    }
    out.observed = after_active;

    if (had_current) {
        Snapshot after_backup;
        result = ReadLeaf(backup_path, &after_backup, nullptr, &out.message);
        if (result != Result::Ok) {
            out.effect = TargetEffect::Unknown;
            out.result = Result::ChangedDuringWrite;
            out.message = L"提升后无法复读备份文件";
            return out;
        }
        if (!after_backup.exists || !SameSnapshot(after_backup, expectedCurrent)) {
            out.effect = TargetEffect::Replaced;
            out.result = Result::ChangedDuringWrite;
            out.message = L"提升后备份与预期旧内容不一致";
            return out;
        }
    }

    out.result = Result::Ok;
    out.effect = TargetEffect::Replaced;
    out.message.clear();
    return out;
}

Outcome Rollback(const std::wstring& trustedDirectory, const Snapshot& expectedCurrent,
                 const Snapshot& expectedBackup, const Hooks& hooks) {
    Outcome out;

    std::wstring directory;
    Result result = NormalizeDirectory(trustedDirectory, &directory, &out.message);
    if (result != Result::Ok) {
        out.result = result;
        return out;
    }

    const std::wstring active_path = JoinLeaf(directory, kActiveFile);
    const std::wstring backup_path = JoinLeaf(directory, kBackupFile);
    const std::wstring temp_path = JoinLeaf(directory, kTempFile);

    SessionMutex session;
    result = session.Acquire(directory, &out.message);
    if (result != Result::Ok) {
        out.result = result;
        FillObserved(active_path, &out);
        return out;
    }

    // Both baselines are mandatory: a restore is only defined as a swap of two
    // known byte strings.
    result = ValidateBaseline(expectedCurrent, &out.message);
    if (result != Result::Ok) {
        out.result = result;
        FillObserved(active_path, &out);
        return out;
    }
    result = ValidateBaseline(expectedBackup, &out.message);
    if (result != Result::Ok) {
        out.result = result;
        FillObserved(active_path, &out);
        return out;
    }

    Snapshot current;
    DWORD current_attributes = 0;
    result = ReadLeaf(active_path, &current, &current_attributes, &out.message);
    if (result != Result::Ok) {
        out.result = result;
        FillObserved(active_path, &out);
        return out;
    }
    out.observed = current;
    if (!SameSnapshot(current, expectedCurrent)) {
        out.result = Result::Conflict;
        out.message = L"活动规格与调用方基线不一致";
        return out;
    }

    Snapshot backup;
    DWORD backup_attributes = 0;
    result = ReadLeaf(backup_path, &backup, &backup_attributes, &out.message);
    if (result != Result::Ok) {
        out.result = result;
        return out;
    }
    if (backup.exists != expectedBackup.exists) {
        out.result = Result::Conflict;
        out.message = L"备份文件与调用方基线不一致";
        return out;
    }
    if (backup.exists && !SameSnapshot(backup, expectedBackup)) {
        out.result = Result::Conflict;
        out.message = L"备份文件与调用方基线不一致";
        return out;
    }
    if (!backup.exists) {
        // No backup (including a first-ever creation): no implicit undo.
        out.result = Result::NoBackup;
        out.message = L"没有可用于回滚的备份文件";
        return out;
    }
    if (!current.exists) {
        // Only an existing active file is swapped in this stage; restoring a
        // missing active (delete-style undo) is out of scope and refused.
        out.result = Result::Refused;
        out.message = L"回滚只交换已存在的活动规格，不做删除式恢复";
        return out;
    }
    if (!IsValidV1Inline(backup.bytes)) {
        out.result = Result::InvalidBackup;
        out.message = L"备份文件不是合法的 v1 inline 规格";
        return out;
    }
    if (!IsValidV1Inline(current.bytes)) {
        out.result = Result::Refused;
        out.message = L"已存在的活动规格不是合法的 v1 inline 规格";
        return out;
    }
    if (ReadonlyAttribute(current_attributes) || ReadonlyAttribute(backup_attributes)) {
        out.result = Result::Refused;
        out.message = L"活动规格或备份文件是只读文件";
        return out;
    }

    LeafInfo temp_info;
    result = QueryLeaf(temp_path, &temp_info, &out.message);
    if (result != Result::Ok) {
        out.result = result;
        return out;
    }
    if (temp_info.exists) {
        if (ReadonlyAttribute(temp_info.attributes)) {
            out.result = Result::Refused;
            out.message = L"临时文件是只读文件";
            return out;
        }
        out.result = Result::Busy;
        out.message = L"临时文件已存在";
        return out;
    }

    // The exact backup bytes (BOM/spacing included) are what gets restored, so
    // the temp is a byte copy, not a re-serialization.
    TempFile temp;
    result = CreateTemp(temp_path, backup.bytes, &temp, &out.message);
    if (result != Result::Ok) {
        DiscardOwnTemp(&temp);
        out.result = result;
        FillObserved(active_path, &out);
        return out;
    }

    if (!RunHook(hooks, &out.message)) {
        DiscardOwnTemp(&temp);
        out.result = Result::IoError;
        FillObserved(active_path, &out);
        return out;
    }

    // Single-slot swap: the restored bytes become active, the former active
    // becomes the new backup.
    if (ReplaceFileW(active_path.c_str(), temp_path.c_str(), backup_path.c_str(), 0, nullptr,
                     nullptr)) {
        temp.promoted = true;
        out.effect = TargetEffect::Replaced;
    } else {
        const DWORD promote_code = GetLastError();
        Snapshot after;
        std::wstring ignored;
        const bool after_ok = ReadLeaf(active_path, &after, nullptr, &ignored) == Result::Ok;
        if (after_ok) out.observed = after;
        if (after_ok && after.exists && SameSnapshot(after, expectedBackup)) {
            out.effect = TargetEffect::Replaced;
        } else if (PromotionProvedUntouched(promote_code, false)) {
            out.effect = TargetEffect::None;
        } else if (after_ok && SameSnapshot(after, expectedCurrent)) {
            out.effect = TargetEffect::None;
        } else {
            out.effect = TargetEffect::Unknown;
        }
        if (out.effect != TargetEffect::Replaced) {
            if (!IndeterminatePromotion(promote_code)) {
                DiscardOwnTemp(&temp);
            }
            out.result = MapPromoteError(promote_code, false);
            out.message = L"回滚替换失败";
            FillObserved(active_path, &out);
            return out;
        }
        DiscardOwnTemp(&temp);
    }

    Snapshot after_active;
    result = ReadLeaf(active_path, &after_active, nullptr, &out.message);
    if (result != Result::Ok) {
        out.effect = TargetEffect::Unknown;
        out.result = Result::ChangedDuringWrite;
        out.message = L"回滚后无法复读活动规格";
        return out;
    }
    if (!after_active.exists || !SameSnapshot(after_active, expectedBackup)) {
        out.effect = TargetEffect::Replaced;
        out.result = Result::ChangedDuringWrite;
        out.message = L"回滚后活动规格与备份内容不一致";
        FillObserved(active_path, &out);
        return out;
    }
    out.observed = after_active;

    Snapshot after_backup;
    result = ReadLeaf(backup_path, &after_backup, nullptr, &out.message);
    if (result != Result::Ok) {
        out.effect = TargetEffect::Unknown;
        out.result = Result::ChangedDuringWrite;
        out.message = L"回滚后无法复读备份槽";
        return out;
    }
    if (!after_backup.exists || !SameSnapshot(after_backup, expectedCurrent)) {
        out.effect = TargetEffect::Replaced;
        out.result = Result::ChangedDuringWrite;
        out.message = L"回滚后备份槽与预期旧内容不一致";
        return out;
    }

    out.result = Result::Ok;
    out.effect = TargetEffect::Replaced;
    out.message.clear();
    return out;
}

} // namespace cardspec::store
