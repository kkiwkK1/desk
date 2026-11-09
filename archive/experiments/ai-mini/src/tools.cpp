#include "tools.h"

#include <windows.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "json.h"
#include "text.h"

namespace tools {
namespace {

std::string Format(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

std::string LocalTime(const std::string&) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    static const char* kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                  "Thursday", "Friday", "Saturday"};

    TIME_ZONE_INFORMATION tz{};
    std::string zone;
    if (GetTimeZoneInformation(&tz) != TIME_ZONE_ID_INVALID) {
        // Keep UTF-8 here: tool output goes to the model, not to the pixel font.
        zone = text::Narrow(tz.StandardName);
    }
    if (zone.empty()) zone = "local";

    return Format("Local time: %04d-%02d-%02d %02d:%02d:%02d %s. Time zone: %s.",
                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                  kDays[t.wDayOfWeek], zone.c_str());
}

struct OsVersion {
    unsigned major = 0;
    unsigned minor = 0;
    unsigned build = 0;
};

OsVersion ReadOsVersion() {
    struct Info {
        ULONG size;
        ULONG major;
        ULONG minor;
        ULONG build;
        ULONG platform;
        WCHAR sp[128];
    } info{};
    info.size = sizeof(info);

    OsVersion out;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        using RtlGetVersionFn = LONG(WINAPI*)(Info*);
        auto fn = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
        if (fn && fn(&info) == 0) {
            out.major = info.major;
            out.minor = info.minor;
            out.build = info.build;
        }
    }
    if (out.build == 0) {
        OSVERSIONINFOW legacy{};
        legacy.dwOSVersionInfoSize = sizeof(legacy);
        if (GetVersionExW(&legacy)) {
            out.major = legacy.dwMajorVersion;
            out.minor = legacy.dwMinorVersion;
            out.build = legacy.dwBuildNumber;
        }
    }
    return out;
}

std::string SystemInfo(const std::string&) {
    wchar_t computer[256]{};
    DWORD computerSize = 256;
    GetComputerNameW(computer, &computerSize);

    wchar_t user[256]{};
    DWORD userSize = 256;
    GetUserNameW(user, &userSize);

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);

    const OsVersion os = ReadOsVersion();
    const char* name = os.build >= 22000 ? "Windows 11"
                       : os.build >= 10240 ? "Windows 10"
                                           : "Windows";
    const char* arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64"
                       : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64"
                                                                                   : "x86";

    const unsigned long long up = GetTickCount64() / 1000;
    const unsigned long long days = up / 86400;
    const unsigned long long hours = (up % 86400) / 3600;
    const unsigned long long mins = (up % 3600) / 60;

    const double totalGb = (double)mem.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    const double freeGb = (double)mem.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);

    const std::string computerText = text::Narrow(computer);
    const std::string userText = text::Narrow(user);

    return Format("Computer: %s (user %s). OS: %s %u.%u build %u, %s. CPU: %u logical cores. "
                  "Memory: %.1f GB free of %.1f GB (%lu%% used). Uptime: %llud %lluh %llum.",
                  computerText.c_str(), userText.c_str(), name, os.major, os.minor, os.build, arch,
                  (unsigned)si.dwNumberOfProcessors, freeGb, totalGb,
                  (unsigned long)mem.dwMemoryLoad, days, hours, mins);
}

std::string DiskFree(const std::string& arguments) {
    std::string wanted;
    jsonlite::Value args;
    if (!arguments.empty() && jsonlite::Parse(arguments, args)) {
        if (const jsonlite::Value* drive = args.Get("drive")) wanted = text::Trim(drive->StringOr());
        else if (args.isString()) wanted = text::Trim(args.AsString());
    }
    char letter = 0;
    if (!wanted.empty()) {
        letter = (char)toupper((unsigned char)wanted[0]);
        if (letter < 'A' || letter > 'Z') letter = 0;
    }

    const DWORD mask = GetLogicalDrives();
    std::string out;
    for (char c = 'A'; c <= 'Z'; ++c) {
        if ((mask & (1u << (c - 'A'))) == 0) continue;
        if (letter && c != letter) continue;

        std::wstring root;
        root.push_back((wchar_t)c);
        root += L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;

        ULARGE_INTEGER available{}, total{}, freeTotal{};
        if (!GetDiskFreeSpaceExW(root.c_str(), &available, &total, &freeTotal)) continue;

        const double totalGb = (double)total.QuadPart / (1024.0 * 1024.0 * 1024.0);
        const double freeGb = (double)available.QuadPart / (1024.0 * 1024.0 * 1024.0);
        if (totalGb < 0.1) continue;

        const int percent = total.QuadPart ? (int)(100.0 * (double)available.QuadPart /
                                                   (double)total.QuadPart)
                                           : 0;
        out += Format("%c: %.1f GB free of %.1f GB (%d%% free). ", c, freeGb, totalGb, percent);
    }

    if (out.empty()) return "No matching fixed drive was found.";
    return text::Trim(out);
}

bool SetClipboardText(const std::wstring& value) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();

    const SIZE_T bytes = (value.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) {
        CloseClipboard();
        return false;
    }
    void* dst = GlobalLock(mem);
    if (!dst) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    std::memcpy(dst, value.c_str(), bytes);
    GlobalUnlock(mem);

    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

std::string CopyText(const std::string& arguments) {
    jsonlite::Value args;
    std::string parseError;
    if (!jsonlite::Parse(arguments, args, &parseError)) {
        return "ERROR: arguments are not valid JSON (" + parseError + ").";
    }

    std::string value;
    if (const jsonlite::Value* text = args.Get("text")) value = text->StringOr();
    else if (args.isString()) value = args.AsString();

    if (value.empty()) return "ERROR: 'text' is required.";
    if (value.size() > 4000) value = text::Truncate(value, 4000);

    const std::wstring wide = text::Widen(value);
    if (!SetClipboardText(wide)) return "ERROR: the clipboard is busy.";
    return "Copied " + std::to_string(wide.size()) + " characters to the clipboard.";
}

constexpr const char* kEmptySchema = R"({"type":"object","properties":{}})";

} // namespace

void Registry::Add(Tool tool) {
    for (Tool& existing : tools_) {
        if (existing.name == tool.name) {
            existing = std::move(tool);
            return;
        }
    }
    tools_.push_back(std::move(tool));
}

std::vector<llm::ToolSchema> Registry::Schemas() const {
    std::vector<llm::ToolSchema> out;
    out.reserve(tools_.size());
    for (const Tool& tool : tools_) {
        out.push_back(llm::ToolSchema{tool.name, tool.description, tool.parameters});
    }
    return out;
}

const Tool* Registry::Find(const std::string& name) const {
    for (const Tool& tool : tools_) {
        if (tool.name == name) return &tool;
    }
    return nullptr;
}

std::string Registry::Invoke(const std::string& name, const std::string& arguments,
                             std::string* error) const {
    if (error) error->clear();
    const Tool* tool = Find(name);
    if (!tool || !tool->run) {
        if (error) *error = "unknown tool";
        std::string available;
        for (const Tool& item : tools_) {
            if (!available.empty()) available += ", ";
            available += item.name;
        }
        return "ERROR: unknown tool '" + name + "'. Available tools: " +
               (available.empty() ? std::string("none") : available) + ".";
    }

    try {
        return tool->run(arguments);
    } catch (const std::exception& e) {
        if (error) *error = "tool threw";
        return "ERROR: tool '" + name + "' failed: " + e.what();
    } catch (...) {
        if (error) *error = "tool threw";
        return "ERROR: tool '" + name + "' failed.";
    }
}

Registry LocalTools() {
    Registry registry;

    registry.Add(Tool{
        "local_time",
        "Current local date, time, weekday and time zone of this computer.",
        kEmptySchema,
        LocalTime});

    registry.Add(Tool{
        "system_info",
        "Computer name, user, Windows version, CPU core count, memory usage and uptime.",
        kEmptySchema,
        SystemInfo});

    registry.Add(Tool{
        "disk_free",
        "Free and total space of fixed drives. Optionally pass a drive letter such as C:.",
        R"({"type":"object","properties":{"drive":{"type":"string","description":"Optional drive letter, for example C:."}}})",
        DiskFree});

    registry.Add(Tool{
        "copy_text",
        "Copy a short piece of text to the Windows clipboard. Use only when the user asks.",
        R"({"type":"object","properties":{"text":{"type":"string","description":"Text to copy, up to 4000 characters."}},"required":["text"]})",
        CopyText});

    return registry;
}

} // namespace tools
