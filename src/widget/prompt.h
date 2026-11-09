// Modal single-line input drawn with the same pixel font. Used by widgets that
// need a secret (API key, token) without building their own dialog.
#pragma once

#include <string>

namespace wg {

struct PromptSpec {
    std::wstring label;                              // e.g. L"API KEY"
    std::wstring hint = L"ENTER SAVE   ESC CANCEL";
    std::wstring initial;                            // pre-filled / editable text
    bool mask = false;                               // show *** instead of the text
    bool saved = false;                              // draw a green "SAVED" tag
    int  width = 360;
};

// Blocks on a nested message loop. Returns true only when the user confirmed a
// non-empty value; 'out' is trimmed.
bool PromptText(const PromptSpec& spec, std::wstring& out);

} // namespace wg
