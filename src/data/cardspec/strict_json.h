// Shared strict JSON scanner for declarative source documents/responses.
#pragma once
#include "data/assistant/json.h"
#include <string>
#include <string_view>

namespace cardspec {
// Same 32KiB/BOM/UTF-8/depth/duplicate-key rules as v1; arbitrary JSON root.
// JSON escaped controls are legal here; selected display text is validated later.
// Errors never echo payload. Success clears error; failure leaves out unchanged.
bool ParseStrictJson(std::string_view utf8, assistant::JsonValue* out,
                     std::wstring* error = nullptr);
} // namespace cardspec
