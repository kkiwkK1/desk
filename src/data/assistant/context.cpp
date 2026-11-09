// DSH context + prompt assembly (M1.1) - see context.h for the contract.
//
// Only the plain-JSON DSH projection cache is read: workspace.json (project
// titles/paths/updatedAt) and session_projcache/sessions/*.json (the last
// non-empty turnOutline response). Nothing under ~/.dsh/sessions is touched, so
// no zstd decoding is needed. Parsing is a private scanner over the known DSH
// shapes; it stays inside this translation unit on purpose.
#include "data/assistant/context.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace assistant {
namespace {

constexpr std::size_t kMaxWorkspaceBytes = 8u << 20;
constexpr std::size_t kMaxSessionBytes = 8u << 20;
constexpr int kMaxSessionFiles = 12;   // newest cache files inspected per Build
constexpr std::size_t kFactTextChars = 240;
constexpr std::size_t kResponseChars = 600;
constexpr std::size_t kEpisodeQuestionChars = 80;
constexpr std::size_t kEpisodeAnswerChars = 180;
constexpr int kEpisodeContext = 3;

// --- text helpers ---------------------------------------------------------

std::wstring Utf8ToWide(const std::string& bytes) {
    if (bytes.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                   static_cast<int>(bytes.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    if (size <= 0) {
        codePage = CP_ACP;
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

std::wstring Trimmed(const std::wstring& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && IsSpace(text[begin])) ++begin;
    while (end > begin && IsSpace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::wstring ExeDir() {
    wchar_t path[32768]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, 32768);
    if (n == 0) return L".";
    std::wstring text(path, n);
    const std::size_t slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : text.substr(0, slash);
}

void SkipSpace(const std::wstring& text, std::size_t* pos) {
    while (*pos < text.size() && IsSpace(text[*pos])) ++*pos;
}

bool ParseJsonString(const std::wstring& text, std::size_t* pos, std::wstring* out) {
    std::size_t i = *pos;
    if (i >= text.size() || text[i] != L'"') return false;
    ++i;
    std::wstring result;
    while (i < text.size()) {
        const wchar_t c = text[i++];
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

bool SkipValue(const std::wstring& text, std::size_t* pos) {
    std::size_t i = *pos;
    SkipSpace(text, &i);
    if (i >= text.size()) return false;
    const wchar_t c = text[i];
    if (c == L'"') {
        std::wstring ignored;
        const bool ok = ParseJsonString(text, &i, &ignored);
        *pos = i;
        return ok;
    }
    if (c == L'{' || c == L'[') {
        const wchar_t close = c == L'{' ? L'}' : L']';
        ++i;
        SkipSpace(text, &i);
        if (i < text.size() && text[i] == close) {
            ++i;
            *pos = i;
            return true;
        }
        for (;;) {
            SkipSpace(text, &i);
            if (i >= text.size()) return false;
            if (c == L'{') {
                std::wstring key;
                if (!ParseJsonString(text, &i, &key)) return false;
                SkipSpace(text, &i);
                if (i >= text.size() || text[i] != L':') return false;
                ++i;
            }
            if (!SkipValue(text, &i)) return false;
            SkipSpace(text, &i);
            if (i >= text.size()) return false;
            if (text[i] == L',') {
                ++i;
                continue;
            }
            if (text[i] == close) {
                ++i;
                *pos = i;
                return true;
            }
            return false;
        }
    }
    // number / true / false / null
    while (i < text.size() && !IsSpace(text[i]) && text[i] != L',' && text[i] != L'}' &&
           text[i] != L']') {
        ++i;
    }
    *pos = i;
    return true;
}

// Position of the value for a direct member of the object starting at objOpen
// (objOpen must point at '{'). Returns false when the key is absent.
bool FindMember(const std::wstring& text, std::size_t objOpen, const wchar_t* key,
                std::size_t* valuePos) {
    if (objOpen >= text.size() || text[objOpen] != L'{') return false;
    std::size_t i = objOpen + 1;
    for (;;) {
        SkipSpace(text, &i);
        if (i >= text.size() || text[i] == L'}') return false;
        if (text[i] != L'"') return false;
        std::wstring member;
        if (!ParseJsonString(text, &i, &member)) return false;
        SkipSpace(text, &i);
        if (i >= text.size() || text[i] != L':') return false;
        ++i;
        SkipSpace(text, &i);
        if (member == key) {
            *valuePos = i;
            return true;
        }
        if (!SkipValue(text, &i)) return false;
        SkipSpace(text, &i);
        if (i >= text.size()) return false;
        if (text[i] == L',') {
            ++i;
            continue;
        }
        return false;
    }
}

bool GetString(const std::wstring& text, std::size_t valuePos, std::wstring* out) {
    if (valuePos >= text.size() || text[valuePos] != L'"') return false;
    std::size_t i = valuePos;
    return ParseJsonString(text, &i, out);
}

bool GetObject(const std::wstring& text, std::size_t valuePos, std::size_t* objOpen) {
    if (valuePos >= text.size() || text[valuePos] != L'{') return false;
    *objOpen = valuePos;
    return true;
}

bool GetArray(const std::wstring& text, std::size_t valuePos, std::size_t* arrayOpen) {
    if (valuePos >= text.size() || text[valuePos] != L'[') return false;
    *arrayOpen = valuePos;
    return true;
}

// Direct members of an object, in file order.
bool ObjectMembers(const std::wstring& text, std::size_t objOpen,
                   std::vector<std::pair<std::wstring, std::size_t>>* out) {
    out->clear();
    if (objOpen >= text.size() || text[objOpen] != L'{') return false;
    std::size_t i = objOpen + 1;
    for (;;) {
        SkipSpace(text, &i);
        if (i >= text.size()) return false;
        if (text[i] == L'}') return true;
        if (text[i] != L'"') return false;
        std::wstring member;
        if (!ParseJsonString(text, &i, &member)) return false;
        SkipSpace(text, &i);
        if (i >= text.size() || text[i] != L':') return false;
        ++i;
        SkipSpace(text, &i);
        out->emplace_back(std::move(member), i);
        if (!SkipValue(text, &i)) return false;
        SkipSpace(text, &i);
        if (i >= text.size()) return false;
        if (text[i] == L',') {
            ++i;
            continue;
        }
        if (text[i] == L'}') return true;
        return false;
    }
}

bool ArrayElements(const std::wstring& text, std::size_t arrayOpen,
                   std::vector<std::size_t>* out) {
    out->clear();
    if (arrayOpen >= text.size() || text[arrayOpen] != L'[') return false;
    std::size_t i = arrayOpen + 1;
    for (;;) {
        SkipSpace(text, &i);
        if (i >= text.size()) return false;
        if (text[i] == L']') return true;
        out->push_back(i);
        if (!SkipValue(text, &i)) return false;
        SkipSpace(text, &i);
        if (i >= text.size()) return false;
        if (text[i] == L',') {
            ++i;
            continue;
        }
        if (text[i] == L']') return true;
        return false;
    }
}

// Root object of a parsed file (skips a UTF-8 BOM and leading whitespace).
std::size_t RootObject(const std::wstring& text) {
    std::size_t i = 0;
    if (!text.empty() && text[0] == 0xFEFF) ++i;
    SkipSpace(text, &i);
    return i < text.size() && text[i] == L'{' ? i : std::wstring::npos;
}

// --- time -----------------------------------------------------------------

long long DaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2 ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153u * (month + (month > 2 ? -3 : 9)) + 2u) / 5u + day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return static_cast<long long>(era) * 146097LL + static_cast<long long>(doe) - 719468LL;
}

void CivilFromDays(long long z, int* year, unsigned* month, unsigned* day) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const long long y0 = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    const unsigned dd = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned mm = mp < 10u ? mp + 3u : mp - 9u;
    *year = static_cast<int>(y0 + (mm <= 2u ? 1 : 0));
    *month = mm;
    *day = dd;
}

bool ParseIsoMs(const std::wstring& text, long long* out) {
    // Accepts 2026-11-06T17:15:00.751Z (and the same without millis).
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, millis = 0;
    std::size_t i = 0;
    auto digits = [&](int* value, int count) {
        if (i + static_cast<std::size_t>(count) > text.size()) return false;
        int result = 0;
        for (int k = 0; k < count; ++k) {
            const wchar_t c = text[i++];
            if (c < L'0' || c > L'9') return false;
            result = result * 10 + (c - L'0');
        }
        *value = result;
        return true;
    };
    if (!digits(&year, 4) || i >= text.size() || text[i] != L'-') return false;
    ++i;
    if (!digits(&month, 2) || i >= text.size() || text[i] != L'-') return false;
    ++i;
    if (!digits(&day, 2)) return false;
    if (i < text.size() && (text[i] == L'T' || text[i] == L't')) {
        ++i;
        if (!digits(&hour, 2) || i >= text.size() || text[i] != L':') return false;
        ++i;
        if (!digits(&minute, 2) || i >= text.size() || text[i] != L':') return false;
        ++i;
        if (!digits(&second, 2)) return false;
        if (i < text.size() && text[i] == L'.') {
            ++i;
            int scale = 100;
            while (i < text.size() && text[i] >= L'0' && text[i] <= L'9') {
                millis += (text[i] - L'0') * scale;
                scale /= 10;
                ++i;
            }
        }
    }
    if (month < 1 || month > 12 || day < 1 || day > 31) return false;
    const long long days = DaysFromCivil(year, static_cast<unsigned>(month),
                                         static_cast<unsigned>(day));
    const long long seconds = days * 86400LL + hour * 3600LL + minute * 60LL + second;
    *out = seconds * 1000LL + millis;
    return true;
}

std::wstring PadNumber(long long value, int width) {
    std::wstring digits = std::to_wstring(value < 0 ? -value : value);
    while (static_cast<int>(digits.size()) < width) digits.insert(digits.begin(), L'0');
    return digits;
}

std::wstring FormatIsoMinute(long long ms) {
    const long long days = ms / 86400000LL;
    const long long rest = ms % 86400000LL;
    int year = 0;
    unsigned month = 0, day = 0;
    CivilFromDays(days, &year, &month, &day);
    return PadNumber(year, 4) + L"-" + PadNumber(month, 2) + L"-" +
           PadNumber(day, 2) + L" " + PadNumber(rest / 3600000LL, 2) + L":" +
           PadNumber((rest / 60000LL) % 60, 2) + L" UTC";
}

std::wstring FormatIsoSecond(long long ms) {
    const long long days = ms / 86400000LL;
    const long long rest = ms % 86400000LL;
    int year = 0;
    unsigned month = 0, day = 0;
    CivilFromDays(days, &year, &month, &day);
    return PadNumber(year, 4) + L"-" + PadNumber(month, 2) + L"-" + PadNumber(day, 2) +
           L"T" + PadNumber(rest / 3600000LL, 2) + L":" +
           PadNumber((rest / 60000LL) % 60, 2) + L":" +
           PadNumber((rest / 1000LL) % 60, 2) + L"Z";
}

// --- file helpers ---------------------------------------------------------

bool ReadFileBytes(const std::wstring& path, std::size_t maxBytes, std::string* out) {
    out->clear();
    HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    char buffer[65536];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(handle, buffer, sizeof(buffer), &got, nullptr)) {
            CloseHandle(handle);
            return false;
        }
        if (got == 0) break;
        if (out->size() + got > maxBytes) {
            const std::size_t keep = maxBytes - out->size();
            out->append(buffer, keep);
            break;
        }
        out->append(buffer, got);
    }
    CloseHandle(handle);
    return true;
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    std::wstring partial;
    for (std::size_t i = 0; i < path.size(); ++i) {
        partial += path[i];
        const bool separator = path[i] == L'\\' || path[i] == L'/';
        if (!separator && i + 1 != path.size()) continue;
        std::wstring current = partial;
        while (!current.empty() && (current.back() == L'\\' || current.back() == L'/')) {
            current.pop_back();
        }
        if (current.empty()) continue;
        if (current.size() == 2 && current[1] == L':') continue;
        if (!CreateDirectoryW(current.c_str(), nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) return false;
        }
    }
    return true;
}

bool AppendLineUtf8(const std::wstring& path, const std::wstring& line) {
    const std::string bytes = WideToUtf8(line);
    HANDLE handle = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()),
                              &wrote, nullptr);
    CloseHandle(handle);
    return ok && wrote == bytes.size();
}

std::wstring JoinIds(const std::vector<std::wstring>& ids) {
    std::wstring out;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) out += L",";
        out += ids[i];
    }
    return out;
}

struct SessionFile {
    std::wstring path;
    std::wstring id;
    long long updatedMs = 0;
};

long long FileTimeToMs(const FILETIME& ft) {
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<long long>(u.QuadPart / 10000ULL - 11644473600000LL);
}

bool ParseSessionJson(const std::wstring& text, const std::wstring& id,
                      long long updatedMs, DshSession* session) {
    const std::size_t root = RootObject(text);
    if (root == std::wstring::npos) return false;
    session->id = id;
    session->updatedMs = updatedMs;

    std::size_t scope = root;  // "record" object when present
    std::size_t value = 0;
    if (FindMember(text, root, L"record", &value)) {
        std::size_t object = 0;
        if (GetObject(text, value, &object)) scope = object;
    }

    std::size_t identity = 0;
    if (FindMember(text, scope, L"identity", &identity)) {
        std::size_t object = 0;
        if (GetObject(text, identity, &object) &&
            FindMember(text, object, L"cwd", &value)) {
            GetString(text, value, &session->cwd);
        }
    }

    std::size_t rows = 0;
    if (!FindMember(text, scope, L"rows", &rows)) return false;
    std::size_t rowsObject = 0;
    if (!GetObject(text, rows, &rowsObject)) return false;

    if (FindMember(text, rowsObject, L"title", &value)) {
        std::size_t titleRow = 0;
        if (GetObject(text, value, &titleRow) &&
            FindMember(text, titleRow, L"val", &value)) {
            GetString(text, value, &session->title);
        }
    }

    std::size_t outline = 0;
    if (!FindMember(text, rowsObject, L"turnOutline", &outline)) return false;
    std::size_t outlineObject = 0;
    if (!GetObject(text, outline, &outlineObject)) return false;
    std::size_t outlineVal = 0;
    if (!FindMember(text, outlineObject, L"val", &outlineVal)) return false;
    std::size_t valObject = 0;
    if (!GetObject(text, outlineVal, &valObject)) return false;
    std::size_t turns = 0;
    if (!FindMember(text, valObject, L"turns", &turns)) return false;
    std::size_t turnsArray = 0;
    if (!GetArray(text, turns, &turnsArray)) return false;

    std::vector<std::size_t> elements;
    if (!ArrayElements(text, turnsArray, &elements)) return false;
    for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
        std::size_t turnObject = 0;
        if (!GetObject(text, *it, &turnObject)) continue;
        std::size_t response = 0;
        if (!FindMember(text, turnObject, L"response", &response)) continue;
        std::wstring textResponse;
        if (!GetString(text, response, &textResponse)) continue;
        if (textResponse.empty()) continue;
        session->lastResponse = std::move(textResponse);
        return true;
    }
    return false;
}

} // namespace

std::wstring DefaultDshRoot() {
    wchar_t buffer[32768];
    // An isolated test instance must never read the real user's DSH data: only
    // a non-empty DESK_TEST_INSTANCE switches the root, and only an explicit
    // DESK_TEST_DSH_ROOT picks a fixture. Without it the root does not exist,
    // so the builder sees an empty DSH instead of the live profile.
    DWORD length = GetEnvironmentVariableW(L"DESK_TEST_INSTANCE", buffer,
                                           static_cast<DWORD>(std::size(buffer)));
    if (length > 0 && length < std::size(buffer)) {
        length = GetEnvironmentVariableW(L"DESK_TEST_DSH_ROOT", buffer,
                                         static_cast<DWORD>(std::size(buffer)));
        if (length > 0 && length < std::size(buffer)) {
            return std::wstring(buffer, length);
        }
        return ExeDir() + L"\\test-dsh";
    }
    length = GetEnvironmentVariableW(L"USERPROFILE", buffer,
                                           static_cast<DWORD>(std::size(buffer)));
    if (length > 0 && length < std::size(buffer)) {
        return std::wstring(buffer, length) + L"\\.dsh";
    }
    length = GetEnvironmentVariableW(L"HOME", buffer,
                                     static_cast<DWORD>(std::size(buffer)));
    if (length > 0 && length < std::size(buffer)) {
        return std::wstring(buffer, length) + L"\\.dsh";
    }
    return L"";
}

std::vector<DshProject> ReadDshProjects(const std::wstring& dshRoot, int limit) {
    std::vector<DshProject> projects;
    if (dshRoot.empty() || limit <= 0) return projects;

    std::string bytes;
    if (!ReadFileBytes(dshRoot + L"\\storages\\workspace.json", kMaxWorkspaceBytes,
                       &bytes)) {
        return projects;
    }
    const std::wstring text = Utf8ToWide(bytes);
    const std::size_t root = RootObject(text);
    if (root == std::wstring::npos) return projects;

    std::size_t tables = 0;
    if (!FindMember(text, root, L"tables", &tables)) return projects;
    std::size_t tablesObject = 0;
    if (!GetObject(text, tables, &tablesObject)) return projects;
    std::size_t workspaces = 0;
    if (!FindMember(text, tablesObject, L"workspaces", &workspaces)) return projects;
    std::size_t workspacesObject = 0;
    if (!GetObject(text, workspaces, &workspacesObject)) return projects;

    std::vector<std::pair<std::wstring, std::size_t>> workspacesMembers;
    if (!ObjectMembers(text, workspacesObject, &workspacesMembers)) return projects;
    for (const auto& member : workspacesMembers) {
        std::size_t object = 0;
        if (!GetObject(text, member.second, &object)) continue;

        DshProject project;
        project.id = member.first;
        std::size_t value = 0;
        if (FindMember(text, object, L"title", &value)) GetString(text, value, &project.title);
        if (FindMember(text, object, L"path", &value)) GetString(text, value, &project.path);
        if (FindMember(text, object, L"updatedAt", &value)) {
            std::wstring iso;
            if (GetString(text, value, &iso)) ParseIsoMs(iso, &project.updatedMs);
        }
        if (project.title.empty() && project.path.empty()) continue;
        projects.push_back(std::move(project));
    }

    std::sort(projects.begin(), projects.end(),
              [](const DshProject& a, const DshProject& b) {
                  return a.updatedMs > b.updatedMs;
              });
    if (static_cast<int>(projects.size()) > limit) projects.resize(limit);
    return projects;
}

std::vector<DshSession> ReadDshSessions(const std::wstring& dshRoot, int limit) {
    std::vector<DshSession> sessions;
    if (dshRoot.empty() || limit <= 0) return sessions;

    const std::wstring directory =
        dshRoot + L"\\storages\\session_projcache\\sessions";
    std::vector<SessionFile> files;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW((directory + L"\\*.json").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return sessions;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        SessionFile file;
        file.path = directory + L"\\" + data.cFileName;
        std::wstring name = data.cFileName;
        if (name.size() > 5 && name.compare(name.size() - 5, 5, L".json") == 0) {
            name.resize(name.size() - 5);
        }
        file.id = name;
        file.updatedMs = FileTimeToMs(data.ftLastWriteTime);
        files.push_back(std::move(file));
    } while (FindNextFileW(search, &data));
    FindClose(search);

    std::sort(files.begin(), files.end(),
              [](const SessionFile& a, const SessionFile& b) {
                  return a.updatedMs > b.updatedMs;
              });
    if (static_cast<int>(files.size()) > kMaxSessionFiles) files.resize(kMaxSessionFiles);

    for (const SessionFile& file : files) {
        if (static_cast<int>(sessions.size()) >= limit) break;
        std::string bytes;
        if (!ReadFileBytes(file.path, kMaxSessionBytes, &bytes)) continue;
        const std::wstring text = Utf8ToWide(bytes);
        DshSession session;
        if (!ParseSessionJson(text, file.id, file.updatedMs, &session)) continue;
        if (session.lastResponse.empty()) continue;
        sessions.push_back(std::move(session));
    }
    sessions.resize(static_cast<std::size_t>(
        std::min<int>(static_cast<int>(sessions.size()), limit)));
    return sessions;
}

// --- ContextBuilder -------------------------------------------------------

ContextBuilder::ContextBuilder(std::wstring configDir, std::wstring dshRoot,
                               const MemoryStore& memory)
    : configDir_(std::move(configDir)),
      dshRoot_(dshRoot.empty() ? DefaultDshRoot() : std::move(dshRoot)),
      memory_(memory) {
    while (!configDir_.empty() &&
           (configDir_.back() == L'\\' || configDir_.back() == L'/')) {
        configDir_.pop_back();
    }
    sendLogPath_ = configDir_ + L"\\assistant-send.log";
}

bool ContextBuilder::AppendSendLog(long long nowMs, const ContextBundle& bundle,
                                   int promptChars, int budgetTokens) const {
    if (configDir_.empty()) return false;
    if (!EnsureDirectory(configDir_)) return false;
    std::wstring line = FormatIsoSecond(nowMs);
    line += L" budget=";
    line += std::to_wstring(budgetTokens);
    line += L" chars=";
    line += std::to_wstring(promptChars);
    line += L" sources=";
    line += JoinIds(bundle.sourceIds);
    line += L"\n";
    return AppendLineUtf8(sendLogPath_, line);
}

ContextBundle ContextBuilder::Build(long long nowMs, int budgetTokens,
                                    std::wstring_view question) {
    const int tokens = budgetTokens > 0 ? budgetTokens : 6000;
    const std::size_t charBudget = static_cast<std::size_t>(tokens) * 2;

    ContextBundle bundle;
    std::wstring prompt;
    prompt += L"你是 Desktop Desk 的桌面助理（Windows 桌面板）。用中文简洁回答。\n";
    prompt += L"当前时间(UTC)：" + FormatIsoMinute(nowMs) + L"\n";

    const std::wstring footer =
        L"\n引用规则：结论必须引用下方真实 id（记忆事实 [[mem:<id>]]，DSH 项目/会话/"
        L"近期问答 [[src:<id>]]）；没有可引用来源时不要编造来源。\n";
    const std::size_t reserve = footer.size() + 64;

    const std::vector<Fact> facts = memory_.Query(question, 6, nowMs);
    if (!facts.empty()) {
        std::wstring section = L"\n## 记忆事实\n";
        bool any = false;
        for (const Fact& fact : facts) {
            if (MemoryStore::IsSensitive(fact.text) ||
                MemoryStore::IsSensitive(fact.sourceRef)) {
                continue;
            }
            std::wstring text = fact.text;
            if (text.size() > kFactTextChars) {
                text.resize(kFactTextChars);
                text += L"…";
            }
            std::wstring line = L"- [" + fact.id + L"|" + fact.kind + L"] " + text;
            if (!fact.sourceRef.empty()) line += L"（来源：" + fact.sourceRef + L"）";
            line += L"\n";
            if (prompt.size() + section.size() + line.size() + reserve > charBudget) break;
            section += line;
            any = true;
            bundle.sourceIds.push_back(fact.id);
        }
        if (any) prompt += section;
    }

    // --- recent episodes ---------------------------------------------------
    // Episodes are re-injected inside the same budget, but fail closed: an
    // episode may only come back while every memory id it cited still exists,
    // is still active, and has not been edited after the answer. Otherwise a
    // forgotten/edited fact would quietly return through the old answer.
    const std::vector<Fact> allFacts = memory_.AllFacts();
    std::unordered_map<std::wstring, const Fact*> activeFacts;
    std::vector<std::wstring> deletedTexts;
    for (const Fact& fact : allFacts) {
        if (fact.status == L"active") {
            activeFacts[fact.id] = &fact;
        } else if (fact.text.size() >= 4) {
            deletedTexts.push_back(fact.text);
        }
    }
    const std::vector<Episode> episodes = memory_.RecentEpisodes(kEpisodeContext);
    if (!episodes.empty()) {
        std::wstring section = L"\n## 近期问答（同一预算内，可引用）\n";
        bool any = false;
        for (auto it = episodes.rbegin(); it != episodes.rend(); ++it) {
            const Episode& episode = *it;
            if (MemoryStore::IsSensitive(episode.question) ||
                MemoryStore::IsSensitive(episode.answer)) {
                continue;
            }
            bool usable = true;
            std::size_t pos = 0;
            while (usable && pos < episode.sources.size()) {
                const std::size_t comma = episode.sources.find(L',', pos);
                const std::wstring id = Trimmed(episode.sources.substr(
                    pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos));
                if (!id.empty()) {
                    const auto found = activeFacts.find(id);
                    if (found == activeFacts.end()) {
                        usable = false;  // deleted, cleared or unknown id
                    } else if (found->second->updatedMs > episode.createdMs) {
                        usable = false;  // fact edited after this answer
                    }
                }
                if (comma == std::wstring::npos) break;
                pos = comma + 1;
            }
            for (const std::wstring& text : deletedTexts) {
                if (!usable) break;
                if (episode.question.find(text) != std::wstring::npos ||
                    episode.answer.find(text) != std::wstring::npos) {
                    usable = false;
                }
            }
            if (!usable) continue;
            std::wstring questionText = episode.question;
            if (questionText.size() > kEpisodeQuestionChars) {
                questionText.resize(kEpisodeQuestionChars);
                questionText += L"…";
            }
            std::wstring answerText = episode.answer;
            if (answerText.size() > kEpisodeAnswerChars) {
                answerText.resize(kEpisodeAnswerChars);
                answerText += L"…";
            }
            std::wstring line = L"- [" + episode.id + L"] 问：" + questionText +
                                L" 答：" + answerText + L"\n";
            if (prompt.size() + section.size() + line.size() + reserve > charBudget) break;
            section += line;
            any = true;
            bundle.sourceIds.push_back(episode.id);
        }
        if (any) prompt += section;
    }

    const std::vector<DshSession> sessions = ReadDshSessions(dshRoot_, 4);
    if (!sessions.empty()) {
        std::wstring section = L"\n## 最近 DSH 会话结论\n";
        bool any = false;
        for (const DshSession& session : sessions) {
            if (MemoryStore::IsSensitive(session.lastResponse) ||
                MemoryStore::IsSensitive(session.title) ||
                MemoryStore::IsSensitive(session.cwd)) {
                continue;
            }
            std::wstring response = session.lastResponse;
            if (response.size() > kResponseChars) {
                response.resize(kResponseChars);
                response += L"…";
            }
            const std::wstring label = session.title.empty() ? session.id : session.title;
            std::wstring line =
                L"- [dsh-session-" + session.id + L"] " + label + L"：" + response + L"\n";
            if (prompt.size() + section.size() + line.size() + reserve > charBudget) break;
            section += line;
            any = true;
            bundle.sourceIds.push_back(L"dsh-session-" + session.id);
        }
        if (any) prompt += section;
    }

    const std::vector<DshProject> projects = ReadDshProjects(dshRoot_, 3);
    if (!projects.empty()) {
        std::wstring section = L"\n## DSH 活跃项目\n";
        bool any = false;
        for (const DshProject& project : projects) {
            if (MemoryStore::IsSensitive(project.path) ||
                MemoryStore::IsSensitive(project.title)) {
                continue;
            }
            std::wstring line = L"- [dsh-project-" + project.id + L"] " +
                                (project.title.empty() ? project.path : project.title);
            if (!project.path.empty() && !project.title.empty()) line += L" · " + project.path;
            if (project.updatedMs > 0) {
                line += L"（更新于 " + FormatIsoMinute(project.updatedMs) + L"）";
            }
            line += L"\n";
            if (prompt.size() + section.size() + line.size() + reserve > charBudget) break;
            section += line;
            any = true;
            bundle.sourceIds.push_back(L"dsh-project-" + project.id);
        }
        if (any) prompt += section;
    }

    if (prompt.size() + footer.size() <= charBudget) {
        prompt += footer;
    } else if (prompt.size() < charBudget) {
        prompt += footer.substr(0, charBudget - prompt.size());
    }
    if (prompt.size() > charBudget) prompt.resize(charBudget);
    bundle.prompt = std::move(prompt);
    AppendSendLog(nowMs, bundle, static_cast<int>(bundle.prompt.size()), tokens);
    return bundle;
}

} // namespace assistant
