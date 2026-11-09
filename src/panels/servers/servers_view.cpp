#include "servers_view.h"

#include "widget/size.h"
#include "widget/skin.h"

#include <algorithm>
#include <string>
#include <vector>

namespace servers_view {

using namespace wg;

namespace {

constexpr int kDetailsHeight = 40;

// The details block costs 6px of gap plus 40px of two meta lines; the lowest
// line is painted at y+25 and is 7px tall, so 40px holds it and the divider.
//
// How many rows fit: the Standard preset is 340px wide and 112..640px tall
// (outer). With the 48px of card chrome that leaves 592px of body, i.e. 18 rows
// collapsed and 17 expanded (the details block takes the rest). The data source
// caps hosts at server::kMaxHosts == 8, so a real card is at most
// 8*32 + 6 + 40 = 302px of body, 350px outer - far inside the preset.
//
// The cap below is the real boundary of the preset and exists so that a future
// data source cannot make the card taller than the framework allows. The board
// validates the height and clips painting to the body rect, so overlong data
// would be silently cut (or the layout refused) rather than reported. Instead
// the card paints only the rows that fit and Status() appends "+N" for the
// hidden ones; BodyHeight() uses the same count, so the two never disagree.
constexpr int kStandardMaxHeight = 640;   // SizePresetRule(Standard).maxHeight
constexpr int kMaxRowsCollapsed =
    (kStandardMaxHeight - wg::kCardChromeHeight) / skin::kRowPitch;   // 18
constexpr int kMaxRowsExpanded = (kStandardMaxHeight - wg::kCardChromeHeight -
                                  skin::kDetailsGap - kDetailsHeight) /
                                 skin::kRowPitch;   // 17
static_assert(kMaxRowsExpanded >= (int)server::kMaxHosts,
              "the configured host limit must fit the expanded Standard card");
static_assert(wg::kCardChromeHeight + (int)server::kMaxHosts * skin::kRowPitch +
                      skin::kDetailsGap + kDetailsHeight <=
                  kStandardMaxHeight,
              "8 hosts expanded must stay inside the Standard preset");

int Capacity(bool expanded) {
    return expanded ? kMaxRowsExpanded : kMaxRowsCollapsed;
}

// Rows actually drawn/painted. A server dataset always has at least one row so
// the empty state ("NO HOSTS" / "SSH HOSTS...") keeps the card's slot.
int Rows(const server::ServerData& data, bool expanded) {
    return std::min((int)std::max<size_t>(1, data.rows.size()), Capacity(expanded));
}

int HiddenRows(const server::ServerData& data, bool expanded) {
    return (int)std::max<size_t>(1, data.rows.size()) - Rows(data, expanded);
}

std::wstring DisplayPath(std::wstring path) {
    for (wchar_t& c : path) {
        if (c == L'\\') c = L'/';
    }
    // The card is 340px wide: keep the last three components instead of
    // truncating the tail of an absolute path.
    size_t end = path.size();
    for (int i = 0; i < 3 && end > 0; ++i) {
        const size_t slash = path.find_last_of(L'/', end - 1);
        if (slash == std::wstring::npos) break;
        end = slash;
        if (i == 2) return path.substr(end + 1);
    }
    return path;
}

std::wstring CoresLine(const server::ServerData& data) {
    std::wstring text = L"CORES";
    if (data.hosts.empty()) return text + L"  --";
    for (const server::HostStatus& host : data.hosts) {
        text += L"  ";
        text += host.name;
        text += L" ";
        text += host.cores > 0 ? std::to_wstring(host.cores) : std::wstring(L"-");
    }
    return text;
}

} // namespace

int BodyHeight(const server::ServerData& data, bool expanded) {
    return skin::RowsHeight(Rows(data, expanded)) +
           (expanded ? skin::kDetailsGap + kDetailsHeight : 0);
}

void DrawBody(const server::ServerData& data, wg::Canvas& canvas, const wg::Rect& body,
              const wg::Theme& theme, bool expanded) {
    const int rowCount = Rows(data, expanded);
    if ((int)data.rows.size() <= rowCount) {
        skin::Rows(canvas, body, data.rows, theme);
    } else {
        // Overlong dataset: paint only the rows that fit instead of past the
        // body rect (Status() reports the hidden remainder as "+N").
        const std::vector<wg::Row> visible(data.rows.begin(),
                                           data.rows.begin() + rowCount);
        skin::Rows(canvas, body, visible, theme);
    }
    if (!expanded) return;

    const int y = body.y + skin::RowsHeight(rowCount) + skin::kDetailsGap;
    const int left = body.x + skin::kCardPadX;
    const int right = body.right() - skin::kCardPadX;
    skin::Divider(canvas, left, right, y, theme);

    canvas.Text(skin::FitText(canvas, CoresLine(data), right - left, 1.0f), (float)left,
                (float)(y + 9), theme.faint, 1.0f);

    const std::wstring updated = data.updated.empty() ? std::wstring() : L"UPDATED " + data.updated;
    const int reserve = updated.empty() ? 0 : (int)canvas.TextWidth(updated, 1.0f) + 12;
    const int maxLeft = std::max(0, right - left - reserve);

    std::wstring ssh = data.sshFound ? L"SSH  " + DisplayPath(data.sshPath)
                                     : std::wstring(L"SSH CLIENT NOT FOUND");
    ssh = skin::FitText(canvas, ssh, maxLeft, 1.0f);
    canvas.Text(ssh, (float)left, (float)(y + 25), data.sshFound ? theme.faint : theme.bad,
                1.0f);
    if (!updated.empty()) {
        canvas.TextRight(updated, right, (float)(y + 25), theme.faint, 1.0f);
    }
}

wg::StatusChip Status(const server::ServerData& data, const wg::Theme& theme,
                      bool expanded) {
    // Rows that do not fit are reported in the title bar, not painted outside
    // the body. With server::kMaxHosts (8) this is always zero; it only becomes
    // visible if a future data source returns more rows than the Standard card
    // can show. The board caches the status between refreshes, so a toggle of
    // the details block may keep an older count for one refresh cycle.
    const int hidden = std::max(0, HiddenRows(data, expanded));
    const auto withHidden = [hidden](std::wstring text) {
        return hidden > 0 ? text + L" +" + std::to_wstring(hidden) : text;
    };

    if (data.hosts.empty()) return wg::StatusChip{L"NO HOSTS", theme.warn};
    if (!data.sshFound) return wg::StatusChip{withHidden(L"NO SSH"), theme.bad};

    int up = 0;
    for (const server::HostStatus& host : data.hosts) {
        if (host.ok) ++up;
    }
    wchar_t text[32];
    swprintf(text, 32, L"%d/%d UP", up, (int)data.hosts.size());
    const wg::Color color = up == (int)data.hosts.size() ? theme.ok
                            : up > 0 ? theme.warn
                                     : theme.bad;
    return wg::StatusChip{withHidden(text), color};
}

} // namespace servers_view
