// A widget/module-owned global hotkey. IDs must stay >= 100 for widgets and
// 1..99 for module-local actions (the board allocates the global id).
#pragma once

#include <windows.h>

namespace wg {

struct Hotkey {
    int id = 0;
    UINT mods = MOD_CONTROL | MOD_ALT;
    UINT vk = 0;
};

} // namespace wg
