// The one data struct the widget paints. Streaming text lives in the widget
// itself (guarded by a mutex), because WidgetT only commits a finished frame.
#pragma once

#include <string>

struct MiniData {
    std::wstring question;
    std::wstring answer;
    std::wstring status = L"ASK ME";
    std::wstring meta;
    std::wstring tools;
    bool ok = false;
};
