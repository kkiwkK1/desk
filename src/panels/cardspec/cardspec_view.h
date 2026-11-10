// M2.1 loaded state + paint code for the CARDSPEC card, kept apart from the
// wg::Module wrapper so headless tests can exercise the fixed-file loader, the
// two layouts and the HTTP source states directly.
//
// Loading contract:
//  - exactly one path is ever opened: cardspec.json in the directory of the
//    module's own config file (store().path()). There is no user-selectable
//    path, no source URL and no fallback file. The spec only names a trusted
//    config profile; it can never supply a URL or grant itself network access.
//  - the read is capped at cardspec::kMaxSpecBytes (32 KiB). A larger file is
//    rejected from its metadata without reading the payload, and a failed
//    post-read size re-check fails closed instead of parsing a prefix.
//  - a reparse point (symlink/junction) at the leaf is refused instead of
//    followed, so the card cannot be pointed at an arbitrary target.
//  - a v1 inline document is parsed exactly as before (no network at all).
//    A v2 HTTP document only fetches when LoadOptions.allowNetwork is true
//    *and* cardspec::AuthorizeHttp accepts the profile/exact-URL approval;
//    the initial Load always passes allowNetwork = false, and an empty fetch
//    is a fail-closed error, never implicit I/O.
//  - every failure closes its handle and yields a short Chinese message. The
//    raw payload, the response body, the URL and the absolute path are never
//    part of a message, a detail line or a chip.
//  - nothing is ever created or overwritten: a missing file stays missing.
#pragma once

#include "data/cardspec/cardspec.h"
#include "data/cardspec/http_source.h"
#include "data/cardspec/http.h"

#include "widget/geom.h"
#include "widget/module.h"
#include "widget/skin.h"

#include <cstdint>
#include <string>

namespace cardspec_view {

// Fixed file names. cardspec.ini is the framework's Store file and only holds
// host state (card position etc.); it is never the spec and never written by
// this module. The spec is the sibling cardspec.json, read-only.
inline constexpr const wchar_t* kConfigFileName = L"cardspec.ini";
inline constexpr const wchar_t* kSpecFileName = L"cardspec.json";

// Wide preset: the board hands PaintBody a 568 px wide body.
constexpr int kCardWidth = 568;
constexpr int kPadX = wg::skin::kCardPadX;              // 12
constexpr int kTextWidth = kCardWidth - kPadX * 2;      // 544

// One text line box. 16 px holds the 14 px CJK mask as well as the 5x7 ASCII
// font at up to 1.5 scale, so a painted line can never bleed past its box.
constexpr int kLineH = 16;
constexpr int kTitleBlock = 24;   // spec title line plus its gap
constexpr int kRowPitch = 34;     // rows: label + text; bar: label + bar
constexpr int kBarOffsetY = 20;   // bar top inside a row block
constexpr int kBarH = 6;
// One extra meta line under a successful HTTP card: "AS OF HH:MM". The caption
// is captured by the loader, never computed in Paint, and only contributes
// height while state == Ok (Ready/Blocked/HttpError use the error layout).
constexpr int kAsOfGap = 6;

enum class State {
    Ok,        // spec parsed; title + rows are painted
    Missing,   // no cardspec.json in the config directory (or no bound store)
    TooLarge,  // file bigger than cardspec::kMaxSpecBytes, not read
    Invalid,   // unreadable, empty or rejected source document
    Ready,     // authorized HTTP source; initial Load never fetches
    Blocked,   // HTTP policy missing, invalid or not explicitly approved
    HttpError, // failed fetch/binding; old successful values are not retained
};

struct ViewState {
    State state = State::Missing;
    cardspec::Spec spec;   // meaningful only when state == State::Ok
    std::wstring message;  // Chinese, one line, no raw payload, no full path
    std::wstring detail;   // optional second line (field/limit error text)
    bool isHttp = false;
    cardspec::HttpSpec request; // profile/bindings only; no authority or URL
    std::wstring asOf;     // captured on successful fetch; never computed in Paint
    // Permission epoch this view was produced under. Internal bookkeeping only
    // (never a spec/permission source): the module displays this view only
    // while the epoch is still current, so a revoke that lands after the read
    // returns - even after the host's Commit - cannot show the old result.
    std::uint64_t publicationEpoch = 0;
};

struct LoadOptions {
    bool allowNetwork = false; // false for initial Load, even with prior approval
    cardspec::HttpPolicy policy;
    cardspec::HttpFetcher fetch; // empty => fail closed, never implicit I/O
};

// Fixed spec path from the module's config file path. Empty input (no bound
// store) or a path without a directory returns empty: the loader must never
// fall back to a CWD-relative guess.
std::wstring SpecPathFor(const std::wstring& configPath);

// Reads and parses the fixed file. Never throws, never leaves a handle open,
// never reads more than cardspec::kMaxSpecBytes. The single-argument overload
// is the v1-compatible entry point: no network, no policy, fail-closed fetch.
ViewState LoadSpecFile(const std::wstring& specPath);
ViewState LoadSpecFile(const std::wstring& specPath, const LoadOptions& options);

// Content height only (the host adds 48 px of card chrome and rounds to 8 px).
int BodyHeight(const ViewState& state);

// Paints title/rows or the error card into `body`. Never paints outside that
// rectangle: text is elided with cjk metrics and a line whose box does not fit
// is skipped. Does not touch the canvas clip rectangle (the board owns it).
void DrawBody(const ViewState& state, wg::Canvas& canvas, const wg::Rect& body,
              const wg::Theme& theme);

// Title-bar chip: LIVE (v1) / HTTP (fetched) / READY / BLOCKED / HTTP ERR /
// NO SPEC / TOO BIG / INVALID.
wg::StatusChip Status(const ViewState& state, const wg::Theme& theme);

// A published view whose permission epoch was superseded (revoke or a new
// approval): a Blocked card that keeps the request for the menu but no old
// values, message or as-of caption.
ViewState ExpiredHttpView(const ViewState& stale);

} // namespace cardspec_view
