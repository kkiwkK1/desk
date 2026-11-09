#pragma once

#include "output.h"

#include <string>

namespace srv {

void PrintGeneralHelp(Output& out);
void PrintCommandHelp(const std::wstring& command, Output& out);

} // namespace srv
