// M2.0 declarative card data. No I/O or executable capabilities.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cardspec {

inline constexpr std::size_t kMaxSpecBytes = 32 * 1024;
inline constexpr std::size_t kMaxRows = 8;

enum class Layout { Rows, Bar };

struct Row {
    std::wstring label;
    std::wstring text;  // Rows only.
    double value = 0.0; // Bar only.
    double max = 1.0;   // Bar only.
};

struct Spec {
    std::wstring title;
    Layout layout = Layout::Rows;
    std::vector<Row> rows;
};

// Success clears *error; failure describes a field/limit without input bytes.
bool Validate(const Spec& spec, std::wstring* error = nullptr);

// Strict v1 inline-only schema; rejects unknown/duplicate keys and invalid UTF-8.
// Failure leaves *out untouched. A UTF-8 BOM at the start is accepted.
bool Parse(std::string_view utf8, Spec* out, std::wstring* error = nullptr);

// Deterministic UTF-8 JSON. Invalid typed input returns empty and fills *error.
std::string Serialize(const Spec& spec, std::wstring* error = nullptr);

} // namespace cardspec
