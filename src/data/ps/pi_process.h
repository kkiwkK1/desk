#pragma once

#include <windows.h>

#include <string_view>

namespace ps {

// This identifies the installed Pi CLI process, not its current TUI mode.
// The application defaults to fullscreen, but settings may select regular
// mode; users of regular mode must select terminal history scrolling instead.
// Command lines are inspected locally and are never logged or persisted.
bool IsPiCliCommand(std::wstring_view imagePath, std::wstring_view commandLine);

// Inspect only descendants of this terminal's shell. Returns false on access
// errors or if a process cannot be unambiguously identified. Call on demand,
// not from an idle timer; an active Pi can exit between any two wheel events.
bool HasPiCliDescendant(DWORD shellPid);

} // namespace ps
