// Long-term assistant memory (M1.1) - see memory.h for the contract.
//
// Files are UTF-8 JSON Lines. Facts are rewritten through a temp file +
// MoveFileEx so a crash can never truncate the only copy; episodes are only
// appended and never rewritten. The parser is deliberately tiny and strict:
// it only needs the flat objects this module writes, so it does not pull in a
// general JSON dependency (m1-pi's json.h covers the pi protocol instead).
//
// assistant-memory-reset.txt is a one-line reset id: the durable witness that
// the user cleared memory, so the engine's session digest changes even when
// facts and episodes were already empty and across a restart. It is written
// (atomically) before ClearAll touches the data files.
#include "data/assistant/memory.h"

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
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace assistant {

const wchar_t* const kFactSensitive = L"sensitive path or credential - not stored";
const wchar_t* const kFactEmpty = L"text is empty";
const wchar_t* const kFactNotFound = L"fact id not found";
const wchar_t* const kFactWriteFailed = L"cannot write memory file";
const wchar_t* const kFactLoadFailed = L"cannot read memory file";
const wchar_t* const kMemoryDirEmpty = L"config directory is empty";

namespace {

constexpr int kEpisodeMemory = 500;                 // episodes kept in RAM
constexpr std::size_t kEpisodeReadBytes = 4u << 20; // tail read on Load
constexpr std::size_t kFactsReadBytes = 16u << 20;
// The reset marker is one id; a bounded read keeps a corrupted file cheap.
constexpr std::size_t kResetReadBytes = 4096;

// --- small text helpers ---------------------------------------------------

std::wstring Utf8ToWide(const std::string& bytes) {
    if (bytes.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                   static_cast<int>(bytes.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    if (size <= 0) {
        codePage = CP_ACP;  // tolerate a hand-edited file saved in the ANSI page
        size = MultiByteToWideChar(codePage, 0, bytes.data(),
                                   static_cast<int>(bytes.size()), nullptr, 0);
    }
    if (size <= 0) return L"";
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()),
                        out.data(), size);
    return out;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

bool IsSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\f';
}

std::wstring Trim(std::wstring_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && IsSpace(text[begin])) ++begin;
    while (end > begin && IsSpace(text[end - 1])) --end;
    return std::wstring(text.substr(begin, end - begin));
}

// Facts live in one-line files, so a pasted multi-line memory is folded.
std::wstring CollapseToLine(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (wchar_t c : text) {
        if (c == L'\r' || c == L'\n' || c == L'\t') {
            pendingSpace = !out.empty();
            continue;
        }
        if (pendingSpace) {
            out += L' ';
            pendingSpace = false;
        }
        out += c;
    }
    return Trim(out);
}

wchar_t LowerChar(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c - L'A' + L'a');
    return c;
}

std::wstring Lower(std::wstring_view text) {
    std::wstring out(text);
    for (wchar_t& c : out) c = LowerChar(c);
    return out;
}

bool ContainsInsensitive(std::wstring_view haystack, std::wstring_view needle) {
    if (needle.empty()) return false;
    const std::wstring lowerHaystack = Lower(haystack);
    const std::wstring lowerNeedle = Lower(needle);
    return lowerHaystack.find(lowerNeedle) != std::wstring::npos;
}

// ASCII "word" character for token-boundary purposes ([A-Za-z0-9_]).
bool IsAsciiWordChar(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
           (c >= L'0' && c <= L'9') || c == L'_';
}

// Case-insensitive search that only accepts a hit at a token start: nothing
// ASCII-word-like may precede it. A non-ASCII character (CJK, full-width
// punctuation) is a boundary, so "密钥sk-..." still matches while "desk-",
// "task-", "risk-" and "disk-" do not. Lower() only folds ASCII, so the code
// unit indices stay aligned with the original text.
bool ContainsInsensitiveAtTokenStart(std::wstring_view haystack,
                                     std::wstring_view needle) {
    if (needle.empty() || haystack.size() < needle.size()) return false;
    const std::wstring lowerHaystack = Lower(haystack);
    const std::wstring lowerNeedle = Lower(needle);
    std::size_t pos = lowerHaystack.find(lowerNeedle);
    while (pos != std::wstring::npos) {
        if (pos == 0 || !IsAsciiWordChar(lowerHaystack[pos - 1])) return true;
        pos = lowerHaystack.find(lowerNeedle, pos + 1);
    }
    return false;
}

// Words used by Query(): ASCII/CJK punctuation and whitespace split tokens.
std::vector<std::wstring> Tokenize(std::wstring_view text) {
    static const std::wstring kSeparators =
        L" \t\r\n\f,.;:!?\"'`()[]{}<>|/\\-+*=&%$#@~^，。：；！？、（）【】「」『』…—";
    std::vector<std::wstring> tokens;
    std::wstring current;
    for (wchar_t c : text) {
        if (kSeparators.find(c) != std::wstring::npos) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current += LowerChar(c);
        }
    }
    if (!current.empty()) tokens.push_back(current);
    // A whole Chinese question may be one long token; also keep 2-char grams so
    // "这个项目目标" still hits "项目" and "目标" facts.
    std::vector<std::wstring> extra;
    for (const std::wstring& token : tokens) {
        for (std::size_t i = 0; i + 1 < token.size(); ++i) {
            extra.push_back(token.substr(i, 2));
        }
    }
    tokens.insert(tokens.end(), extra.begin(), extra.end());
    // Drop duplicates while keeping order; cap the token list.
    std::vector<std::wstring> unique;
    for (const std::wstring& token : tokens) {
        if (std::find(unique.begin(), unique.end(), token) == unique.end()) {
            unique.push_back(token);
            if (unique.size() >= 24) break;
        }
    }
    return unique;
}

long long NowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<long long>(u.QuadPart / 10000ULL - 11644473600000LL);
}

std::wstring Hex64(unsigned long long value) {
    static const wchar_t kDigits[] = L"0123456789abcdef";
    wchar_t digits[16];
    for (int i = 15; i >= 0; --i) {
        digits[i] = kDigits[value & 0xFULL];
        value >>= 4;
    }
    return std::wstring(digits, 16);
}

std::wstring NewId(const wchar_t* prefix) {
    static std::atomic<unsigned long long> counter{0};
    const unsigned long long salt = ++counter * 0x9E3779B97F4A7C15ULL;
    std::wstring id(prefix);
    id += Hex64(static_cast<unsigned long long>(NowMs()) ^ salt);
    return id;
}

int ClampInt(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

// --- shared store registry ------------------------------------------------

// "C:\\data\\Memory\\\\" and "c:/data/memory" must name the same store. The key
// is the absolute path (when Windows can normalize it - it does not need to
// exist), with '/' folded to '\\', trailing separators removed and the whole
// path lower-cased. A drive root keeps its separator ("c:\"), because a bare
// "c:" is the drive-relative path (the current directory on that drive) in
// Win32: collapsing the root into it would let two directories share a key.
std::wstring NormalizeDirectoryKey(const std::wstring& configDir) {
    if (configDir.empty()) return std::wstring();
    std::wstring path;
    const DWORD needed = GetFullPathNameW(configDir.c_str(), 0, nullptr, nullptr);
    if (needed > 0) {
        std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
        const DWORD written = GetFullPathNameW(
            configDir.c_str(), needed, buffer.data(), nullptr);
        if (written > 0 && written < needed) path.assign(buffer.data(), written);
    }
    if (path.empty()) path = configDir;
    for (wchar_t& c : path) {
        if (c == L'/') c = L'\\';
    }
    while (path.size() > 1 && path.back() == L'\\') path.pop_back();
    if (path.size() == 2 && path[1] == L':') path += L'\\';  // keep the drive root
    return Lower(path);
}

// Only weak_ptr: a shared store is owned by its callers, never by the registry.
// A store whose Load failed is not registered at all (see OpenSharedMemory).

// Function-local statics so the registry is usable from static initializers
// without depending on translation-unit construction order.
std::mutex& SharedRegistryMu() {
    static std::mutex mu;
    return mu;
}

std::unordered_map<std::wstring, std::weak_ptr<MemoryStore>>& SharedRegistry() {
    static std::unordered_map<std::wstring, std::weak_ptr<MemoryStore>> registry;
    return registry;
}

// --- files ----------------------------------------------------------------

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    std::wstring partial;
    for (std::size_t i = 0; i < path.size(); ++i) {
        partial += path[i];
        const bool separator = path[i] == L'\\' || path[i] == L'/';
        const bool last = i + 1 == path.size();
        if (!separator && !last) continue;
        std::wstring current = partial;
        while (!current.empty() && (current.back() == L'\\' || current.back() == L'/')) {
            current.pop_back();
        }
        if (current.empty()) continue;
        if (current.size() == 2 && current[1] == L':') continue;  // drive root
        if (!CreateDirectoryW(current.c_str(), nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) return false;
        }
    }
    return true;
}

// Reads at most maxBytes from the end of the file (0 = whole file).
bool ReadFileBytes(const std::wstring& path, std::size_t maxBytes, std::string* out,
                   DWORD* error) {
    out->clear();
    HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return false;
    }

    if (maxBytes > 0) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(handle, &size) &&
            size.QuadPart > static_cast<LONGLONG>(maxBytes)) {
            LARGE_INTEGER offset{};
            offset.QuadPart = size.QuadPart - static_cast<LONGLONG>(maxBytes);
            SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN);
            // Do not start in the middle of a UTF-8 sequence.
            while (offset.QuadPart < size.QuadPart) {
                char first = 0;
                DWORD got = 0;
                if (!ReadFile(handle, &first, 1, &got, nullptr) || got == 0) break;
                if ((static_cast<unsigned char>(first) & 0xC0) != 0x80) {
                    SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN);
                    break;
                }
                ++offset.QuadPart;
            }
        }
    }

    char buffer[65536];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(handle, buffer, sizeof(buffer), &got, nullptr)) {
            *error = GetLastError();
            CloseHandle(handle);
            return false;
        }
        if (got == 0) break;
        out->append(buffer, got);
    }
    CloseHandle(handle);
    return true;
}

bool WriteFileBytesAtomic(const std::wstring& path, const std::string& bytes,
                          DWORD* error) {
    const std::wstring temp = path + L".tmp";
    HANDLE handle = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return false;
    }

    bool ok = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, 1u << 20));
        DWORD wrote = 0;
        if (!WriteFile(handle, bytes.data() + offset, chunk, &wrote, nullptr) ||
            wrote == 0) {
            *error = GetLastError();
            ok = false;
            break;
        }
        offset += wrote;
    }
    if (ok) ok = FlushFileBuffers(handle) != FALSE;
    CloseHandle(handle);
    if (!ok) {
        DeleteFileW(temp.c_str());
        return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        *error = GetLastError();
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

void SetError(std::wstring* error, const wchar_t* text) {
    if (error != nullptr) *error = text;
}

// --- minimal flat JSON (UTF-16 in memory) ---------------------------------

struct FlatValue {
    std::wstring key;
    bool isString = false;
    std::wstring stringValue;
    bool isNumber = false;
    long long numberValue = 0;
};

bool ParseJsonString(const std::wstring& text, std::size_t* pos, std::wstring* out) {
    std::size_t i = *pos;
    if (i >= text.size() || text[i] != L'"') return false;
    ++i;
    std::wstring result;
    while (i < text.size()) {
        wchar_t c = text[i++];
        if (c == L'"') {
            *pos = i;
            *out = std::move(result);
            return true;
        }
        if (c != L'\\') {
            if (static_cast<unsigned>(c) < 0x20) return false;
            result += c;
            continue;
        }
        if (i >= text.size()) return false;
        const wchar_t escaped = text[i++];
        switch (escaped) {
            case L'"': result += L'"'; break;
            case L'\\': result += L'\\'; break;
            case L'/': result += L'/'; break;
            case L'b': result += L'\b'; break;
            case L'f': result += L'\f'; break;
            case L'n': result += L'\n'; break;
            case L'r': result += L'\r'; break;
            case L't': result += L'\t'; break;
            case L'u': {
                if (i + 4 > text.size()) return false;
                unsigned value = 0;
                for (int k = 0; k < 4; ++k) {
                    const wchar_t h = text[i++];
                    unsigned digit = 0;
                    if (h >= L'0' && h <= L'9') {
                        digit = static_cast<unsigned>(h - L'0');
                    } else if (h >= L'a' && h <= L'f') {
                        digit = static_cast<unsigned>(h - L'a' + 10);
                    } else if (h >= L'A' && h <= L'F') {
                        digit = static_cast<unsigned>(h - L'A' + 10);
                    } else {
                        return false;
                    }
                    value = value * 16 + digit;
                }
                result += static_cast<wchar_t>(value);
                break;
            }
            default: return false;
        }
    }
    return false;
}

bool ParseFlatObject(const std::wstring& text, std::vector<FlatValue>* out) {
    out->clear();
    std::size_t i = 0;
    auto skipSpace = [&]() {
        while (i < text.size() && IsSpace(text[i])) ++i;
    };
    skipSpace();
    if (i >= text.size() || text[i] != L'{') return false;
    ++i;
    skipSpace();
    if (i < text.size() && text[i] == L'}') {
        ++i;
        skipSpace();
        return i == text.size();
    }
    while (i < text.size()) {
        skipSpace();
        if (i >= text.size() || text[i] != L'"') return false;
        FlatValue value;
        if (!ParseJsonString(text, &i, &value.key)) return false;
        skipSpace();
        if (i >= text.size() || text[i] != L':') return false;
        ++i;
        skipSpace();
        if (i >= text.size()) return false;
        if (text[i] == L'"') {
            if (!ParseJsonString(text, &i, &value.stringValue)) return false;
            value.isString = true;
        } else if (text[i] == L'-' || (text[i] >= L'0' && text[i] <= L'9')) {
            const std::size_t start = i;
            if (text[i] == L'-') ++i;
            while (i < text.size() && text[i] >= L'0' && text[i] <= L'9') ++i;
            if (i == start) return false;
            value.numberValue =
                wcstoll(text.substr(start, i - start).c_str(), nullptr, 10);
            value.isNumber = true;
        } else if (text.compare(i, 4, L"null") == 0) {
            i += 4;
        } else if (text.compare(i, 4, L"true") == 0) {
            i += 4;
            value.isNumber = true;
            value.numberValue = 1;
        } else if (text.compare(i, 5, L"false") == 0) {
            i += 5;
            value.isNumber = true;
            value.numberValue = 0;
        } else {
            return false;
        }
        out->push_back(std::move(value));
        skipSpace();
        if (i >= text.size()) return false;
        if (text[i] == L',') {
            ++i;
            continue;
        }
        if (text[i] == L'}') {
            ++i;
            skipSpace();
            return i == text.size();
        }
        return false;
    }
    return false;
}

const FlatValue* FindValue(const std::vector<FlatValue>& values, const wchar_t* key) {
    for (const FlatValue& value : values) {
        if (value.key == key) return &value;
    }
    return nullptr;
}

std::wstring StringValue(const std::vector<FlatValue>& values, const wchar_t* key) {
    const FlatValue* value = FindValue(values, key);
    return value != nullptr && value->isString ? value->stringValue : std::wstring();
}

long long NumberValue(const std::vector<FlatValue>& values, const wchar_t* key,
                      long long fallback) {
    const FlatValue* value = FindValue(values, key);
    return value != nullptr && value->isNumber ? value->numberValue : fallback;
}

std::wstring JsonEscape(std::wstring_view text) {
    static const wchar_t kHex[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(text.size() + 8);
    for (wchar_t c : text) {
        switch (c) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (static_cast<unsigned>(c) < 0x20) {
                    out += L"\\u";
                    out += kHex[(c >> 12) & 0xF];
                    out += kHex[(c >> 8) & 0xF];
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

std::wstring SerializeFact(const Fact& fact) {
    std::wstring out = L"{\"id\":\"";
    out += JsonEscape(fact.id);
    out += L"\",\"kind\":\"";
    out += JsonEscape(fact.kind);
    out += L"\",\"text\":\"";
    out += JsonEscape(fact.text);
    out += L"\",\"source\":\"";
    out += JsonEscape(fact.sourceRef);
    out += L"\",\"confidence\":";
    out += std::to_wstring(fact.confidence);
    out += L",\"status\":\"";
    out += JsonEscape(fact.status);
    out += L"\",\"created\":";
    out += std::to_wstring(fact.createdMs);
    out += L",\"updated\":";
    out += std::to_wstring(fact.updatedMs);
    out += L",\"expires\":";
    out += std::to_wstring(fact.expiresMs);
    // Appended last so a file written by this build stays readable by older
    // ones: their flat parser ignores keys it does not ask for.
    out += L",\"pinned\":";
    out += (fact.pinned ? L"true" : L"false");
    out += L"}";
    return out;
}

bool ParseFact(const std::wstring& line, Fact* fact) {
    std::vector<FlatValue> values;
    if (!ParseFlatObject(line, &values)) return false;
    fact->id = StringValue(values, L"id");
    fact->kind = StringValue(values, L"kind");
    fact->text = StringValue(values, L"text");
    fact->sourceRef = StringValue(values, L"source");
    fact->status = StringValue(values, L"status");
    fact->confidence =
        ClampInt(static_cast<int>(NumberValue(values, L"confidence", 80)), 0, 100);
    fact->createdMs = NumberValue(values, L"created", 0);
    fact->updatedMs = NumberValue(values, L"updated", 0);
    fact->expiresMs = NumberValue(values, L"expires", 0);
    // Files written before pinning existed have no "pinned" key: false.
    fact->pinned = NumberValue(values, L"pinned", 0) != 0;
    if (fact->id.empty() || fact->text.empty()) return false;
    if (fact->kind.empty()) fact->kind = L"fact";
    if (fact->status.empty()) fact->status = L"active";
    if (fact->updatedMs == 0) fact->updatedMs = fact->createdMs;
    if (fact->createdMs == 0) fact->createdMs = fact->updatedMs;
    return true;
}

std::wstring SerializeEpisode(const Episode& episode) {
    std::wstring out = L"{\"id\":\"";
    out += JsonEscape(episode.id);
    out += L"\",\"created\":";
    out += std::to_wstring(episode.createdMs);
    out += L",\"question\":\"";
    out += JsonEscape(episode.question);
    out += L"\",\"answer\":\"";
    out += JsonEscape(episode.answer);
    out += L"\",\"sources\":\"";
    out += JsonEscape(episode.sources);
    out += L"\"}";
    return out;
}

bool ParseEpisode(const std::wstring& line, Episode* episode) {
    std::vector<FlatValue> values;
    if (!ParseFlatObject(line, &values)) return false;
    episode->id = StringValue(values, L"id");
    episode->question = StringValue(values, L"question");
    episode->answer = StringValue(values, L"answer");
    episode->sources = StringValue(values, L"sources");
    episode->createdMs = NumberValue(values, L"created", 0);
    return !episode->id.empty();
}

std::vector<std::wstring> SplitLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t next = text.find(L'\n', pos);
        if (next == std::wstring::npos) next = text.size();
        std::wstring line = text.substr(pos, next - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!Trim(line).empty()) lines.push_back(std::move(line));
        if (next == text.size()) break;
        pos = next + 1;
    }
    return lines;
}

} // namespace

// --- sensitive material ---------------------------------------------------

bool MemoryStore::IsSensitive(std::wstring_view text) {
    static const wchar_t* const kPatterns[] = {
        L".ssh",          L".credentials", L"id_rsa",     L"id_ed25519",
        L".pem",          L"private key",  L"bearer ",    L"api_key",
        L"apikey",        L"secret_key",   L"access_token", L"refresh_token",
        L"credential",    L"password",     L"cmdkey",
    };
    for (const wchar_t* pattern : kPatterns) {
        if (ContainsInsensitive(text, pattern)) return true;
    }
    // "sk-" is an API-key prefix, not a substring: "desk-render-test",
    // "task-...", "risk-..." and "disk-..." are ordinary hyphenated words and
    // must reach memory. Requiring a token start still catches a leading
    // "sk-", "sk-" after whitespace / quotes / ':' / '=', and CJK directly
    // before it (e.g. "密钥sk-...").
    return ContainsInsensitiveAtTokenStart(text, L"sk-");
}

// --- paths ----------------------------------------------------------------

std::wstring MemoryStore::FactsPath() const {
    return configDir_ + L"\\assistant-facts.jsonl";
}

std::wstring MemoryStore::EpisodesPath() const {
    return configDir_ + L"\\assistant-episodes.jsonl";
}

std::wstring MemoryStore::ResetPath() const {
    return configDir_ + L"\\assistant-memory-reset.txt";
}

// --- load -----------------------------------------------------------------

bool MemoryStore::Load(const std::wstring& configDir, std::wstring* error) {
    std::lock_guard<std::mutex> lock(mu_);
    configDir_ = configDir;
    while (!configDir_.empty() &&
           (configDir_.back() == L'\\' || configDir_.back() == L'/')) {
        configDir_.pop_back();
    }
    facts_.clear();
    recentEpisodes_.clear();
    resetRevision_.clear();
    // Reset metadata first: if it cannot be read the store must not look like
    // "never cleared" (that would let an old Pi session survive a restart).
    const bool resetOk = LoadResetRevision(error);
    const bool factsOk = LoadFacts(error);
    LoadEpisodes();
    return resetOk && factsOk;
}

bool MemoryStore::LoadFacts(std::wstring* error) {
    std::string bytes;
    DWORD winError = 0;
    if (!ReadFileBytes(FactsPath(), kFactsReadBytes, &bytes, &winError)) {
        if (winError == ERROR_FILE_NOT_FOUND || winError == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        SetError(error, kFactLoadFailed);
        return false;
    }

    bool badLine = false;
    for (const std::wstring& line : SplitLines(Utf8ToWide(bytes))) {
        Fact fact;
        if (ParseFact(line, &fact)) {
            facts_.push_back(std::move(fact));
        } else {
            badLine = true;
        }
    }
    TrimFactsToCap();
    if (badLine) {
        SetError(error, kFactLoadFailed);
        return false;
    }
    return true;
}

bool MemoryStore::LoadEpisodes() {
    std::string bytes;
    DWORD winError = 0;
    if (!ReadFileBytes(EpisodesPath(), kEpisodeReadBytes, &bytes, &winError)) {
        return winError == ERROR_FILE_NOT_FOUND || winError == ERROR_PATH_NOT_FOUND;
    }
    for (const std::wstring& line : SplitLines(Utf8ToWide(bytes))) {
        Episode episode;
        if (ParseEpisode(line, &episode)) recentEpisodes_.push_back(std::move(episode));
    }
    if (recentEpisodes_.size() > static_cast<std::size_t>(kEpisodeMemory)) {
        recentEpisodes_.erase(
            recentEpisodes_.begin(),
            recentEpisodes_.begin() +
                static_cast<std::ptrdiff_t>(recentEpisodes_.size() - kEpisodeMemory));
    }
    return true;
}

// --- reset revision --------------------------------------------------------

bool MemoryStore::LoadResetRevision(std::wstring* error) {
    std::string bytes;
    DWORD winError = 0;
    if (!ReadFileBytes(ResetPath(), kResetReadBytes, &bytes, &winError)) {
        if (winError == ERROR_FILE_NOT_FOUND || winError == ERROR_PATH_NOT_FOUND) {
            resetRevision_.clear();   // first run: the user never cleared
            return true;
        }
        SetError(error, kFactLoadFailed);
        return false;
    }
    // One id; a hand-edited trailing newline or spaces are folded away.
    resetRevision_ = CollapseToLine(Utf8ToWide(bytes));
    return true;
}

std::wstring MemoryStore::ResetRevision() const {
    std::lock_guard<std::mutex> lock(mu_);
    return resetRevision_;
}

// --- facts persistence ----------------------------------------------------

bool MemoryStore::SaveFacts(std::wstring* error) {
    std::wstring text;
    for (const Fact& fact : facts_) {
        text += SerializeFact(fact);
        text += L'\n';
    }
    const std::string bytes = WideToUtf8(text);
    DWORD winError = 0;
    if (!WriteFileBytesAtomic(FactsPath(), bytes, &winError)) {
        SetError(error, kFactWriteFailed);
        return false;
    }
    return true;
}

bool MemoryStore::SaveFactsOrRollback(const std::vector<Fact>& before,
                                     std::wstring* error) {
    if (SaveFacts(error)) return true;
    // The caller already mutated facts_; undo it so Facts()/Query() keep
    // matching assistant-facts.jsonl. Otherwise a forgotten fact reappears
    // after a restart and a failed insert looks stored until it is not.
    facts_ = before;
    return false;
}

void MemoryStore::TrimFactsToCap() {
    if (facts_.size() <= static_cast<std::size_t>(kFactCap)) return;
    const long long now = NowMs();
    while (facts_.size() > static_cast<std::size_t>(kFactCap)) {
        auto weakest = facts_.begin();
        for (auto it = facts_.begin(); it != facts_.end(); ++it) {
            const Fact& a = *it;
            const Fact& b = *weakest;
            const bool aGone = a.status != L"active";
            const bool bGone = b.status != L"active";
            const bool aExpired = a.expiresMs > 0 && a.expiresMs < now;
            const bool bExpired = b.expiresMs > 0 && b.expiresMs < now;
            int order = 0;  // < 0 -> a is weaker
            if (aGone != bGone) {
                order = aGone ? -1 : 1;
            } else if (aExpired != bExpired) {
                order = aExpired ? -1 : 1;
            } else if (a.confidence != b.confidence) {
                order = a.confidence < b.confidence ? -1 : 1;
            } else if (a.updatedMs != b.updatedMs) {
                order = a.updatedMs < b.updatedMs ? -1 : 1;
            }
            if (order < 0) weakest = it;
        }
        facts_.erase(weakest);
    }
}

// --- episodes -------------------------------------------------------------

bool MemoryStore::AppendEpisode(const std::wstring& question, const std::wstring& answer,
                                const std::wstring& sources, std::wstring* error) {
    std::lock_guard<std::mutex> lock(mu_);
    if (configDir_.empty()) {
        SetError(error, kFactWriteFailed);
        return false;
    }

    Episode episode;
    episode.id = NewId(L"ep-");
    episode.createdMs = NowMs();
    episode.question = CollapseToLine(question);
    episode.answer = CollapseToLine(answer);
    episode.sources = CollapseToLine(sources);
    if (episode.question.empty() && episode.answer.empty()) {
        SetError(error, kFactEmpty);
        return false;
    }
    if (IsSensitive(episode.question) || IsSensitive(episode.answer)) {
        SetError(error, kFactSensitive);
        return false;
    }

    if (!EnsureDirectory(configDir_)) {
        SetError(error, kFactWriteFailed);
        return false;
    }

    const std::string line = WideToUtf8(SerializeEpisode(episode) + L"\n");
    HANDLE handle = CreateFileW(
        EpisodesPath().c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        SetError(error, kFactWriteFailed);
        return false;
    }
    DWORD wrote = 0;
    const BOOL ok = WriteFile(handle, line.data(), static_cast<DWORD>(line.size()),
                              &wrote, nullptr);
    if (ok && wrote == line.size()) FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok || wrote != line.size()) {
        SetError(error, kFactWriteFailed);
        return false;
    }

    recentEpisodes_.push_back(std::move(episode));
    if (recentEpisodes_.size() > static_cast<std::size_t>(kEpisodeMemory)) {
        recentEpisodes_.erase(recentEpisodes_.begin());
    }
    return true;
}

std::vector<Episode> MemoryStore::RecentEpisodes(int limit) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (limit <= 0 || static_cast<std::size_t>(limit) >= recentEpisodes_.size()) {
        return recentEpisodes_;
    }
    return std::vector<Episode>(
        recentEpisodes_.end() - static_cast<std::ptrdiff_t>(limit),
        recentEpisodes_.end());
}

// --- facts ----------------------------------------------------------------

// Every mutator below runs in two phases: mutate + persist under mu_, then
// notify with mu_ released. Subscribers are allowed to call Facts(), so
// notifying under the data lock would deadlock the MEMORY card repaint.

bool MemoryStore::AddFact(const Fact& fact, std::wstring* error) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (configDir_.empty()) {
            SetError(error, kFactWriteFailed);
            return false;
        }

        Fact incoming = fact;
        incoming.text = CollapseToLine(fact.text);
        incoming.kind = CollapseToLine(fact.kind);
        if (incoming.kind.empty()) incoming.kind = L"fact";
        if (incoming.text.empty()) {
            SetError(error, kFactEmpty);
            return false;
        }
        if (IsSensitive(incoming.text) || IsSensitive(incoming.sourceRef)) {
            SetError(error, kFactSensitive);
            return false;
        }

        const long long now = NowMs();
        const std::wstring lowerText = Lower(incoming.text);
        const std::wstring lowerKind = Lower(incoming.kind);
        const std::vector<Fact> before = facts_;  // rollback point for a failed save

        for (Fact& existing : facts_) {
            // A soft-deleted tombstone is history, not a merge target: the user
            // forgot that id, so re-remembering the same text must create a
            // fresh active fact instead of silently reviving it.
            if (existing.status != L"active") continue;
            if (Lower(existing.text) != lowerText || Lower(existing.kind) != lowerKind) {
                continue;
            }
            existing.status = L"active";
            existing.confidence =
                std::max(existing.confidence, ClampInt(incoming.confidence, 0, 100));
            existing.updatedMs = now;
            if (!incoming.sourceRef.empty()) existing.sourceRef = incoming.sourceRef;
            if (incoming.expiresMs > 0) existing.expiresMs = incoming.expiresMs;
            // A re-observed fact must not silently unpin what the user pinned;
            // only SetPinned(false) does that.
            existing.pinned = existing.pinned || incoming.pinned;
            TrimFactsToCap();
            changed = SaveFactsOrRollback(before, error);
            if (!changed) return false;
            break;
        }

        if (!changed) {
            incoming.id = incoming.id.empty() ? NewId(L"mem-") : incoming.id;
            // Ids address facts (Facts/SetPinned/DeleteFact); a tombstone keeps
            // its id, so an insert must never reuse one. A caller-provided id
            // that is already taken gets a fresh one.
            for (const Fact& fact : facts_) {
                if (fact.id == incoming.id) {
                    incoming.id = NewId(L"mem-");
                    break;
                }
            }
            incoming.status = L"active";
            incoming.confidence = ClampInt(incoming.confidence, 0, 100);
            incoming.createdMs = incoming.createdMs > 0 ? incoming.createdMs : now;
            incoming.updatedMs = incoming.updatedMs > 0 ? incoming.updatedMs : now;
            if (incoming.updatedMs < incoming.createdMs) {
                incoming.updatedMs = incoming.createdMs;
            }
            facts_.push_back(std::move(incoming));
            TrimFactsToCap();
            changed = SaveFactsOrRollback(before, error);
            if (!changed) return false;
        }
    }
    NotifyChanged();
    return true;
}

bool MemoryStore::UpdateFact(const std::wstring& id, const std::wstring& text,
                             std::wstring* error) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const std::wstring newText = CollapseToLine(text);
        if (newText.empty()) {
            SetError(error, kFactEmpty);
            return false;
        }
        if (IsSensitive(newText)) {
            SetError(error, kFactSensitive);
            return false;
        }

        Fact* target = nullptr;
        for (Fact& fact : facts_) {
            if (fact.id == id) {
                target = &fact;
                break;
            }
        }
        if (target == nullptr || target->status != L"active") {
            SetError(error, kFactNotFound);
            return false;
        }

        const std::vector<Fact> before = facts_;  // rollback point
        const long long now = NowMs();
        const std::wstring lowerText = Lower(newText);
        bool folded = false;
        for (Fact& other : facts_) {
            if (&other == target || other.status != L"active") continue;
            if (Lower(other.kind) != Lower(target->kind) ||
                Lower(other.text) != lowerText) {
                continue;
            }
            // The corrected text already exists: fold this fact into that one.
            other.confidence =
                std::max(other.confidence, ClampInt(target->confidence + 5, 0, 100));
            other.updatedMs = now;
            other.pinned = other.pinned || target->pinned;
            facts_.erase(std::remove_if(facts_.begin(), facts_.end(),
                                        [&](const Fact& fact) { return fact.id == id; }),
                         facts_.end());
            folded = true;
            break;
        }
        if (!folded) {
            target->text = newText;
            target->confidence = ClampInt(target->confidence + 5, 0, 100);
            target->updatedMs = now;
        }
        changed = SaveFactsOrRollback(before, error);
        if (!changed) return false;
    }
    NotifyChanged();
    return true;
}

bool MemoryStore::DeleteFact(const std::wstring& id, std::wstring* error) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        bool found = false;
        for (Fact& fact : facts_) {
            if (fact.id != id) continue;
            found = true;
            if (fact.status != L"deleted") {
                const std::vector<Fact> before = facts_;  // rollback point
                fact.status = L"deleted";
                fact.updatedMs = NowMs();
                changed = SaveFactsOrRollback(before, error);
                if (!changed) return false;
            }
            break;
        }
        if (!found) {
            SetError(error, kFactNotFound);
            return false;
        }
    }
    if (changed) NotifyChanged();  // an already-deleted fact is not a change
    return true;
}

bool MemoryStore::SetPinned(const std::wstring& id, bool pinned, std::wstring* error) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        Fact* target = nullptr;
        for (Fact& fact : facts_) {
            if (fact.id == id) {
                target = &fact;
                break;
            }
        }
        if (target == nullptr || target->status != L"active") {
            SetError(error, kFactNotFound);
            return false;
        }
        if (target->pinned == pinned) return true;  // no rewrite, no notification

        const std::vector<Fact> before = facts_;  // rollback point
        target->pinned = pinned;
        // updatedMs is intentionally untouched (memory.h): pinning is a display
        // flag, so it must not refresh Query()'s recency/decay score.
        changed = SaveFactsOrRollback(before, error);
        if (!changed) return false;
    }
    NotifyChanged();
    return true;
}

bool MemoryStore::ClearAll(std::wstring* error) {
    // Caller-facing output: start from empty so a successful clear (or an early
    // rejection that sets its own message) never returns a message left over
    // from an earlier call on the same std::wstring.
    if (error != nullptr) error->clear();
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (configDir_.empty()) {
            SetError(error, kFactWriteFailed);
            return false;
        }
        // First run (or a store whose directory was removed): clearing an empty
        // memory must still succeed instead of failing on a missing path.
        if (!EnsureDirectory(configDir_)) {
            SetError(error, kFactWriteFailed);
            return false;
        }

        DWORD winError = 0;
        // Step 1: persist a new reset id first. The reset marker is the durable
        // "the user cleared memory" witness the engine folds into its session
        // digest, so an old Pi session is dropped even when facts and episodes
        // are already empty (and across a restart). If this write fails, nothing
        // else is touched and the revision stays as it was.
        const std::wstring resetId = NewId(L"reset-");
        if (!WriteFileBytesAtomic(ResetPath(), WideToUtf8(resetId + L"\n"), &winError)) {
            SetError(error, kFactWriteFailed);
            return false;
        }
        // From here on the revision has advanced: that is a conservative session
        // invalidation even if a data file below cannot be cleared (the call
        // still fails and notifies nobody, so no data clear is faked).
        resetRevision_ = resetId;

        // Step 2/3: the data files, facts first, episodes second. Their rollback
        // rules are unchanged (a failed episode rewrite restores the facts file
        // from the untouched snapshot); no third restore is needed because the
        // already-advanced revision keeps the old session invalidated.
        if (!WriteFileBytesAtomic(FactsPath(), std::string(), &winError)) {
            // The replace never happened, so both the file and the in-memory
            // snapshot are untouched - report and keep everything.
            SetError(error, kFactWriteFailed);
            return false;
        }
        if (!WriteFileBytesAtomic(EpisodesPath(), std::string(), &winError)) {
            // The facts file is already empty. Put the snapshot back so a
            // failed clear does not silently forget half of the memory. SaveFacts
            // serializes facts_, which was never cleared, so this restores the
            // pre-clear content; if the restore itself fails the two files stay
            // out of step, which is why this is documented as best effort and
            // the call still reports the failure.
            SaveFacts(nullptr);
            SetError(error, kFactWriteFailed);
            return false;
        }
        // The revision and both data rewrites returned success: only now is the
        // in-memory state allowed to claim the clear. There is deliberately no
        // "both lists were already empty" early return - a successful clear
        // always notifies, even with no data change, because it is also the
        // user's explicit forget barrier that lets the engine drop an
        // old/cancelled Pi session.
        facts_.clear();
        recentEpisodes_.clear();
    }
    NotifyChanged();
    return true;
}

std::vector<Fact> MemoryStore::Facts() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Fact> out;
    for (const Fact& fact : facts_) {
        if (fact.status == L"active") out.push_back(fact);
    }
    std::sort(out.begin(), out.end(), [](const Fact& a, const Fact& b) {
        if (a.pinned != b.pinned) return a.pinned;  // pinned facts list first
        if (a.updatedMs != b.updatedMs) return a.updatedMs > b.updatedMs;
        if (a.createdMs != b.createdMs) return a.createdMs > b.createdMs;
        // Deterministic tie-break: the MEMORY card keeps its selection by id,
        // so a stable order keeps the highlighted row from jumping.
        return a.id < b.id;
    });
    return out;
}

std::vector<Fact> MemoryStore::AllFacts() const {
    std::lock_guard<std::mutex> lock(mu_);
    return facts_;
}

std::vector<Fact> MemoryStore::Query(std::wstring_view text, int limit,
                                     long long nowMs) const {
    std::lock_guard<std::mutex> lock(mu_);
    const long long now = nowMs > 0 ? nowMs : NowMs();
    const std::vector<std::wstring> tokens = Tokenize(text);

    struct Scored {
        const Fact* fact = nullptr;
        int score = 0;
    };
    std::vector<Scored> scored;
    for (const Fact& fact : facts_) {
        if (fact.status != L"active") continue;
        if (fact.expiresMs > 0 && fact.expiresMs < now) continue;

        int hits = 0;
        int kindHits = 0;
        int sourceHits = 0;
        for (const std::wstring& token : tokens) {
            if (token.empty()) continue;
            if (ContainsInsensitive(fact.text, token)) {
                ++hits;
            } else if (ContainsInsensitive(fact.kind, token)) {
                ++kindHits;
            } else if (ContainsInsensitive(fact.sourceRef, token)) {
                ++sourceHits;
            }
        }
        if (!tokens.empty() && hits + kindHits + sourceHits == 0) continue;

        const long long age = now - fact.updatedMs;
        int score = hits * 10 + kindHits * 6 + sourceHits * 2;
        const long long decayAge = age > 0 ? age : 0;
        if (decayAge > kFactDecayMs) {
            // 90+ days unused: the fact keeps half its relevance instead of a
            // recency bonus (memory.h / plan §3 "90 天降权").
            score = score >= 2 ? score / 2 : 1;
        } else {
            score += static_cast<int>(10LL * (kFactDecayMs - decayAge) / kFactDecayMs);
        }
        score += (ClampInt(fact.confidence, 0, 100) - 50) / 10;
        scored.push_back(Scored{&fact, score});
    }

    std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.fact->updatedMs > b.fact->updatedMs;
    });

    std::vector<Fact> out;
    const std::size_t take =
        limit > 0 ? static_cast<std::size_t>(limit) : scored.size();
    for (std::size_t i = 0; i < scored.size() && i < take; ++i) {
        out.push_back(*scored[i].fact);
    }
    return out;
}

// --- change notification --------------------------------------------------

void MemoryStore::NotifyChanged() {
    // Snapshot under the subscriber lock, then call with no store lock held:
    // a handler is allowed to call Facts()/Query() and to Subscribe/Unsubscribe
    // itself. Unsubscribing during the loop therefore cannot break the walk -
    // the token is simply absent from the next snapshot.
    std::vector<ChangeHandler> handlers;
    {
        std::lock_guard<std::mutex> lock(subscribersMu_);
        handlers.reserve(subscribers_.size());
        for (const auto& entry : subscribers_) handlers.push_back(entry.second);
    }
    for (ChangeHandler& handler : handlers) {
        if (!handler) continue;
        try {
            handler();
        } catch (...) {
            // A throwing subscriber must not undo a fact that is already on
            // disk, nor starve the subscribers that come after it.
        }
    }
}

uint64_t MemoryStore::Subscribe(ChangeHandler handler) {
    if (!handler) return 0;
    std::lock_guard<std::mutex> lock(subscribersMu_);
    const uint64_t token = nextToken_++;
    subscribers_.emplace_back(token, std::move(handler));
    return token;
}

void MemoryStore::Unsubscribe(uint64_t token) {
    if (token == 0) return;
    std::lock_guard<std::mutex> lock(subscribersMu_);
    subscribers_.erase(
        std::remove_if(subscribers_.begin(), subscribers_.end(),
                       [token](const std::pair<uint64_t, ChangeHandler>& entry) {
                           return entry.first == token;
                       }),
        subscribers_.end());
}

// --- shared store ---------------------------------------------------------

std::shared_ptr<MemoryStore> OpenSharedMemory(const std::wstring& configDir,
                                              std::wstring* error) {
    if (error != nullptr) error->clear();
    const std::wstring key = NormalizeDirectoryKey(configDir);
    if (key.empty()) {
        SetError(error, kMemoryDirEmpty);
        return nullptr;
    }

    // Held across Load so two threads calling at once cannot each create a
    // store (and then write the same facts file independently). The registry
    // only keeps weak_ptr, so a store still dies when its last caller lets go.
    std::lock_guard<std::mutex> lock(SharedRegistryMu());
    auto& registry = SharedRegistry();
    auto existing = registry.find(key);
    if (existing != registry.end()) {
        if (std::shared_ptr<MemoryStore> store = existing->second.lock()) {
            return store;  // loaded once, never reloaded
        }
        registry.erase(existing);  // every caller dropped it
    }

    std::shared_ptr<MemoryStore> store = std::make_shared<MemoryStore>();
    std::wstring loadError;
    if (!store->Load(configDir, &loadError)) {
        // A store that could not read its file is never shared: handing it out
        // would let Engine/UI treat "error" as advisory and write facts over a
        // file they could not see. Report it the way Load() does, register
        // nothing, and let the next call retry from scratch once the file is
        // fixed.
        SetError(error, loadError.empty() ? kFactLoadFailed : loadError.c_str());
        return nullptr;
    }
    registry.emplace(key, store);
    return store;
}

} // namespace assistant
