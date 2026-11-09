#pragma once

#include <memory>

namespace wg { class Module; }

// A component factory: the host only needs this declaration and the framework.
std::unique_ptr<wg::Module> CreateClockPanel();
