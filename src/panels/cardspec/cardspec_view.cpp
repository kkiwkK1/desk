#include "panels/cardspec/cardspec_view.h"

#include "widget/cjk.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cwchar>
#include <string>
#include <utility>

namespace cardspec_view {

using namespace wg;

namespace {

// Closes the fixed spec file on every early return; a failed load must not
// leave a handle behind.
class FileHandle {
public:
    explicit FileHandle(HANDLE handle) : handle_(handle) {}
    ~FileHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    HANDLE get() const { return handle_; }
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_;
};

std::wstring TooLargeMessage() {
    return L"cardspec.json 超过 " + std::to_wstring(cardspec::kMaxSpecBytes / 1024) +
           L" KiB 上限，已拒绝读取";
}

std::wstring ErrorWithCode(const wchar_t* what, DWORD code) {
    return std::wstring(what) + L"（错误码 " + std::to_wstring((unsigned long long)code) + L"）";
}

ViewState ErrorState(State state, std::wstring message, std::wstring detail = {}) {
    ViewState out;
    out.state = state;
    out.message = std::move(message);
    out.detail = std::move(detail);
    return out;
}

// An HTTP source state. The request (profile + row bindings) is always kept so
// the menu can offer approve/revoke; no URL or authority is stored in it.
ViewState HttpState(cardspec::HttpSpec request, State state, std::wstring message,
                    std::wstring detail = {}) {
    ViewState out;
    out.state = state;
    out.isHttp = true;
    out.request = std::move(request);
    out.message = std::move(message);
    out.detail = std::move(detail);
    return out;
}

// Captured once on a successful fetch, never recomputed while painting.
std::wstring NowAsOf() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buf[16];
    swprintf(buf, 16, L"%02u:%02u", (unsigned)now.wHour, (unsigned)now.wMinute);
    return buf;
}

// Blocked is "no permission", not "broken spec": the message says what is
// missing, never which URL the user configured.
ViewState BlockedState(cardspec::HttpSpec request, cardspec::HttpFailure failure) {
    switch (failure) {
    case cardspec::HttpFailure::NotConfigured:
        return HttpState(std::move(request), State::Blocked, L"HTTP 数据源未配置端点",
                         L"在配置中设置 http.profile 与 http.url 后重启加载");
    case cardspec::HttpFailure::InvalidUrl:
        return HttpState(std::move(request), State::Blocked,
                         L"http.url 不是允许的公开 HTTPS 端点",
                         L"只支持 https 443、公开 DNS 主机，无鉴权、无端口");
    case cardspec::HttpFailure::NotApproved:
    default:
        return HttpState(std::move(request), State::Blocked, L"HTTP 数据源未获批准",
                         L"在卡片菜单批准当前 HTTPS 端点");
    }
}

// Error-card body: heading line + message + optional detail + hint, 20 px apart.
int ErrorBlockHeight(const ViewState& state) {
    const int lines = 1 + (state.detail.empty() ? 0 : 1);
    return kLineH + 20 + lines * 20;
}

std::wstring ErrorHeading(const ViewState& state) {
    switch (state.state) {
    case State::Ready: return L"HTTP 待刷新";
    case State::Blocked: return L"HTTP 未授权";
    case State::HttpError: return L"HTTP 失败";
    case State::Missing:
    case State::TooLarge:
    case State::Invalid:
    case State::Ok:
    default: return L"规格不可用";
    }
}

std::wstring ErrorHint(const ViewState& state) {
    switch (state.state) {
    case State::Ready: return L"在卡片菜单 Reload 获取一次数据";
    case State::Blocked: return L"在卡片菜单批准当前 HTTPS 端点";
    case State::HttpError: return L"检查端点与 spec 绑定后 Reload 重试";
    case State::Missing:
    case State::TooLarge:
    case State::Invalid:
    case State::Ok:
    default: return L"修正文件后刷新即可恢复";
    }
}

Color ErrorColor(const ViewState& state, const Theme& theme) {
    switch (state.state) {
    case State::Missing:
    case State::Ready:
    case State::Blocked: return theme.warn;
    default: return theme.bad;
    }
}

// Truncation measured with cjk metrics; Canvas::TextWidth counts every glyph as
// 6*scale px and would let Chinese text run past the card. The ellipsis is
// measured at the real scale: when the room is narrower than "...", only as
// many dots as actually fit are returned (empty when none does), so a tiny
// body can never receive a marker wider than itself. Never splits a surrogate
// pair off the end.
std::wstring FitTo(std::wstring text, int maxWidth, float scale) {
    if (maxWidth <= 0) return std::wstring();
    if ((int)cjk::TextWidth(text, scale) <= maxWidth) return text;

    const std::wstring dots = L"...";
    // TextWidth is the painted advance minus one pixel; +1 keeps ink inside.
    if ((int)cjk::TextWidth(dots, scale) + 1 > maxWidth) {
        std::wstring fit;
        for (const wchar_t c : dots) {
            std::wstring next = fit + c;
            if ((int)cjk::TextWidth(next, scale) + 1 > maxWidth) break;
            fit = std::move(next);
        }
        return fit;
    }

    while (!text.empty() && (int)cjk::TextWidth(text + dots, scale) > maxWidth) {
        text.pop_back();
    }
    while (!text.empty() && text.back() >= 0xD800 && text.back() <= 0xDBFF) {
        text.pop_back();
    }
    return text + dots;
}

std::wstring FormatNumber(double value) {
    if (!std::isfinite(value)) return L"0";  // Parse rejects these; paint stays safe
    wchar_t buf[64];
    if (value == std::floor(value) && std::fabs(value) < 1e15) {
        swprintf(buf, 64, L"%.0f", value);
    } else {
        swprintf(buf, 64, L"%.3g", value);
    }
    return buf;
}

// Reads the one fixed file. Returns false and fills *error (the same fail-closed
// states v1 published) when the file is missing, too large or unreadable.
bool ReadSpecBytes(const std::wstring& specPath, std::string* bytes, ViewState* error) {
    if (specPath.empty()) {
        *error = ErrorState(State::Missing, L"未绑定配置目录，无法读取 cardspec.json");
        return false;
    }

    // A directory at the leaf is reported specifically: CreateFileW without
    // FILE_FLAG_BACKUP_SEMANTICS refuses it with a generic access error, so the
    // attribute is read first. Reparse leaves are left to the handle check
    // below (opened with FILE_FLAG_OPEN_REPARSE_POINT) so a link is always
    // reported as a refused link, never followed.
    const DWORD leaf = GetFileAttributesW(specPath.c_str());
    if (leaf != INVALID_FILE_ATTRIBUTES && (leaf & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (leaf & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        *error = ErrorState(State::Invalid, L"cardspec.json 是目录，不是文件");
        return false;
    }

    // FILE_FLAG_OPEN_REPARSE_POINT opens the link itself, so a symlink/junction
    // leaf is seen and rejected instead of silently followed to another target.
    FileHandle file(CreateFileW(specPath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr));
    if (!file.valid()) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            *error = ErrorState(State::Missing, L"未找到配置目录内的 cardspec.json");
        } else {
            *error = ErrorState(State::Invalid,
                                ErrorWithCode(L"无法打开 cardspec.json", code));
        }
        return false;
    }

    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info)) {
        *error = ErrorState(State::Invalid,
                            ErrorWithCode(L"无法读取 cardspec.json 的信息", GetLastError()));
        return false;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        *error = ErrorState(State::Invalid, L"拒绝读取链接形式的 cardspec.json",
                            L"规格文件不能是符号链接或 reparse point");
        return false;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        *error = ErrorState(State::Invalid, L"cardspec.json 是目录，不是文件");
        return false;
    }

    const ULONGLONG size = ((ULONGLONG)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    if (size > (ULONGLONG)cardspec::kMaxSpecBytes) {
        *error = ErrorState(State::TooLarge, TooLargeMessage());
        return false;
    }

    bytes->clear();
    bytes->reserve((std::size_t)size);
    while (bytes->size() < cardspec::kMaxSpecBytes) {
        char buf[4096];
        const std::size_t want =
            std::min<std::size_t>(sizeof(buf), cardspec::kMaxSpecBytes - bytes->size());
        DWORD got = 0;
        if (!ReadFile(file.get(), buf, (DWORD)want, &got, nullptr)) {
            *error = ErrorState(State::Invalid,
                                ErrorWithCode(L"读取 cardspec.json 失败", GetLastError()));
            return false;
        }
        if (got == 0) break;
        bytes->append(buf, got);
    }
    // Fail closed: if the cap cannot be re-checked, publish nothing rather than
    // parsing a prefix of a file whose size is unknown. The message names the
    // error only - no path and no payload.
    LARGE_INTEGER after{};
    if (!GetFileSizeEx(file.get(), &after)) {
        *error = ErrorState(State::Invalid,
                            ErrorWithCode(L"无法复核 cardspec.json 大小", GetLastError()));
        return false;
    }
    if (after.QuadPart > (LONGLONG)cardspec::kMaxSpecBytes) {
        *error = ErrorState(State::TooLarge, TooLargeMessage());
        return false;
    }
    if (bytes->empty()) {
        *error = ErrorState(State::Invalid, L"cardspec.json 为空文件");
        return false;
    }
    return true;
}

// HTTP source. Authorization is checked before anything else, so an
// unapproved document never reaches the fetcher; the initial Load passes
// allowNetwork = false and therefore never leaves the machine.
ViewState LoadHttp(cardspec::HttpSpec request, const LoadOptions& options) {
    cardspec::HttpEndpoint endpoint;
    cardspec::HttpFailure failure = cardspec::HttpFailure::None;
    if (!cardspec::AuthorizeHttp(request.profile, options.policy, &endpoint, &failure)) {
        return BlockedState(std::move(request), failure);
    }
    if (!options.allowNetwork) {
        return HttpState(std::move(request), State::Ready, L"端点已批准，尚未取数",
                         L"在卡片菜单 Reload 获取一次数据");
    }
    if (!options.fetch) {
        // No injected transport means no network, ever.
        return HttpState(std::move(request), State::HttpError,
                         L"未配置网络依赖，已拒绝出网",
                         L"抓取入口缺失（fail closed）");
    }

    // One GET, no retry, no cache: the caller (worker) owns the epoch check for
    // a revoke that lands while this result is in flight.
    const cardspec::HttpResult result = options.fetch(request.profile, options.policy);
    if (!result.ok()) {
        std::wstring message;
        if (result.failure != cardspec::HttpFailure::None) {
            message = cardspec::HttpFailureText(result.failure);
        } else {
            message = L"HTTP 状态码 " + std::to_wstring(result.status);
        }
        return HttpState(std::move(request), State::HttpError, std::move(message));
    }
    if (result.body.size() > cardspec::kMaxHttpBodyBytes) {
        return HttpState(std::move(request), State::HttpError,
                         L"响应超过 32 KiB 上限，已拒绝");
    }

    cardspec::Spec bound;
    std::wstring decodeError;
    if (!cardspec::DecodeHttpResponse(request, result.body, &bound, &decodeError)) {
        // The binding error names a field/limit only; the body is never shown.
        return HttpState(std::move(request), State::HttpError,
                         L"响应不符合声明的字段绑定", decodeError);
    }

    ViewState out;
    out.state = State::Ok;
    out.isHttp = true;
    out.request = std::move(request);
    out.spec = std::move(bound);
    out.asOf = NowAsOf();
    return out;
}

} // namespace

std::wstring SpecPathFor(const std::wstring& configPath) {
    if (configPath.empty()) return std::wstring();
    const std::size_t slash = configPath.find_last_of(L"\\/");
    // No directory component means "no fixed config directory": refusing the
    // path is safer than reading cardspec.json from the process CWD.
    if (slash == std::wstring::npos) return std::wstring();
    return configPath.substr(0, slash + 1) + kSpecFileName;
}

ViewState LoadSpecFile(const std::wstring& specPath) {
    return LoadSpecFile(specPath, LoadOptions{});
}

ViewState LoadSpecFile(const std::wstring& specPath, const LoadOptions& options) {
    std::string bytes;
    ViewState error;
    if (!ReadSpecBytes(specPath, &bytes, &error)) return error;

    cardspec::SourceDocument document;
    std::wstring parseError;
    if (!cardspec::ParseSourceDocument(bytes, &document, &parseError)) {
        // v1 and v2 share one closed schema; the detail is field/limit text.
        return ErrorState(State::Invalid, L"cardspec.json 不符合 v1/v2 规格", parseError);
    }

    if (document.kind == cardspec::SourceKind::Inline) {
        ViewState out;
        out.state = State::Ok;
        out.spec = std::move(document.inlineSpec);
        return out;
    }
    return LoadHttp(std::move(document.httpSpec), options);
}

int BodyHeight(const ViewState& state) {
    if (state.state != State::Ok) return ErrorBlockHeight(state);
    const std::size_t rows = std::min<std::size_t>(state.spec.rows.size(), cardspec::kMaxRows);
    int height = kTitleBlock + (int)rows * kRowPitch;
    if (state.isHttp && !state.asOf.empty()) height += kAsOfGap + kLineH;
    return height;
}

void DrawBody(const ViewState& state, wg::Canvas& canvas, const wg::Rect& body,
              const wg::Theme& theme) {
    if (body.w <= 0 || body.h <= 0) return;
    const int left = body.x + kPadX;
    const int innerW = body.w - kPadX * 2;
    if (innerW <= 0) return;
    const int right = left + innerW;
    const int bottom = body.bottom();

    // A line is painted only when its whole box fits the body, so a body the
    // host shrank can never bleed ink outside it. `maxW` is additionally
    // clamped to the remaining inner width, and FitTo elides with CJK metrics.
    const auto paintAt = [&](const std::wstring& text, int x, int top, int maxW,
                             Color color, float scale) {
        if (maxW <= 0 || x < left || x >= right) return;
        if (top < body.y || top + kLineH > bottom) return;
        const std::wstring fit = FitTo(text, std::min(maxW, right - x), scale);
        if (fit.empty()) return;
        cjk::TextTop(canvas, fit, (float)x, (float)top, color, scale);
    };

    if (state.state != State::Ok) {
        paintAt(ErrorHeading(state), left, body.y, innerW, ErrorColor(state, theme),
                kTextScale);
        int top = body.y + 20;
        paintAt(state.message, left, top, innerW, theme.text, skin::kMetaScale);
        top += 20;
        if (!state.detail.empty()) {
            paintAt(state.detail, left, top, innerW, theme.faint, skin::kMetaScale);
            top += 20;
        }
        paintAt(ErrorHint(state), left, top, innerW, theme.faint, skin::kMetaScale);
        return;
    }

    // The spec's own title is card content. The host title stays CARDSPEC.
    paintAt(state.spec.title, left, body.y, innerW, theme.text, skin::kTitleScale);

    const std::size_t count =
        std::min<std::size_t>(state.spec.rows.size(), cardspec::kMaxRows);
    for (std::size_t i = 0; i < count; ++i) {
        const cardspec::Row& row = state.spec.rows[i];
        const int top = body.y + kTitleBlock + (int)i * kRowPitch;

        if (state.spec.layout == cardspec::Layout::Bar) {
            const std::wstring value =
                FormatNumber(row.value) + L" / " + FormatNumber(row.max);
            const int valueW = (int)cjk::TextWidth(value, kTextScale);
            const int labelW = std::max(0, innerW - valueW - 12);
            paintAt(row.label, left, top, labelW, theme.label, kTextScale);
            paintAt(value, right - valueW, top, valueW, theme.text, kTextScale);

            // Strict value/max; 0/max paints the empty track, and a defensive
            // guard keeps a hand-built state from dividing by zero.
            double frac = 0.0;
            if (std::isfinite(row.value) && std::isfinite(row.max) && row.max > 0.0) {
                frac = row.value / row.max;
            }
            if (!(frac >= 0.0)) frac = 0.0;  // NaN guard
            if (frac > 1.0) frac = 1.0;
            const int barTop = top + kBarOffsetY;
            if (barTop < body.y || barTop + kBarH > bottom) continue;
            canvas.Bar(left, barTop, innerW, kBarH, (float)frac, theme.ok, theme.track);
        } else {
            paintAt(row.label, left, top, innerW, theme.label, skin::kMetaScale);
            paintAt(row.text, left, top + 18, innerW, theme.text, kTextScale);
        }
    }

    // HTTP success adds one meta line under the rows; the value was captured
    // when the fetch landed, so the card never invents a time while painting.
    if (state.isHttp && !state.asOf.empty()) {
        const int top = body.y + kTitleBlock + (int)count * kRowPitch + kAsOfGap;
        paintAt(L"AS OF " + state.asOf, left, top, innerW, theme.faint,
                skin::kMetaScale);
    }
}

wg::StatusChip Status(const ViewState& state, const wg::Theme& theme) {
    switch (state.state) {
    case State::Ok:
        return state.isHttp ? wg::StatusChip{L"HTTP", theme.ok}
                            : wg::StatusChip{L"LIVE", theme.ok};
    case State::Ready: return wg::StatusChip{L"READY", theme.warn};
    case State::Blocked: return wg::StatusChip{L"BLOCKED", theme.warn};
    case State::HttpError: return wg::StatusChip{L"HTTP ERR", theme.bad};
    case State::Missing: return wg::StatusChip{L"NO SPEC", theme.warn};
    case State::TooLarge: return wg::StatusChip{L"TOO BIG", theme.bad};
    case State::Invalid: return wg::StatusChip{L"INVALID", theme.bad};
    }
    return wg::StatusChip{L"INVALID", theme.bad};
}

ViewState ExpiredHttpView(const ViewState& stale) {
    // No rows, no as-of caption: only the fact that the permission changed.
    ViewState out;
    out.state = State::Blocked;
    out.isHttp = true;
    out.request = stale.request;
    out.publicationEpoch = stale.publicationEpoch;
    out.message = L"网络权限已变更，当前显示已失效";
    out.detail = L"如仍需数据请重新批准并刷新";
    return out;
}

} // namespace cardspec_view
