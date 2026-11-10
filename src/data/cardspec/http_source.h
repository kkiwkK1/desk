// M2.1 source schema and pure response bindings; no network or file I/O.
#pragma once
#include "data/cardspec/cardspec.h"

namespace cardspec {

inline constexpr std::size_t kMaxPointerBytes = 256;
inline constexpr std::size_t kMaxPointerSegments = 8;

enum class SourceKind { Inline, Http };

struct HttpRow {
    std::wstring label;
    std::string textPointer;  // Rows: RFC 6901 pointer; empty selects root.
    std::string valuePointer; // Bar: JSON number only.
    bool maxFromPointer = false;
    std::string maxPointer;
    double max = 1.0;
};

struct HttpSpec {
    std::wstring title;
    Layout layout = Layout::Rows;
    std::string profile; // ASCII [A-Za-z0-9_-], 1..32; never a URL/permission.
    std::vector<HttpRow> rows;
};

struct SourceDocument {
    SourceKind kind = SourceKind::Inline;
    Spec inlineSpec;
    HttpSpec httpSpec;
};

// v1 is delegated unchanged to Parse(). v2 is http-only, closed schema.
// All failures preserve out and never echo document/response/path payload.
bool ParseSourceDocument(std::string_view utf8, SourceDocument* out,
                         std::wstring* error = nullptr);
bool ValidateHttpSpec(const HttpSpec& spec, std::wstring* error = nullptr);
// RFC 6901 only: decoded literal object keys or canonical array indices.
// Rows accepts string/finite-number/bool for display; null/objects/arrays fail.
// Bar requires finite JSON numbers and applies v1 bounds after binding.
bool DecodeHttpResponse(const HttpSpec& spec, std::string_view utf8, Spec* out,
                        std::wstring* error = nullptr);

} // namespace cardspec
