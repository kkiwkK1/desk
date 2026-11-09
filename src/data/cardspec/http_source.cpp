// M2.1 source schema, RFC 6901 pointers and pure HTTP response binding.
//
// Safety invariants:
//   * no network, file, credential or executable call in this translation unit;
//   * v2 is a closed schema and v1 is still delegated to the unchanged Parse();
//   * pointers are literal RFC 6901 references only: no JSONPath, wildcard,
//     script, regex or model extraction semantics;
//   * diagnostics name a field/limit but never echo document, response or
//     pointer bytes (responses may contain URLs, secrets or user data);
//   * failure leaves the caller's out untouched.
#include "data/cardspec/http_source.h"

#include "data/cardspec/strict_json.h"
#include "data/assistant/json.h"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cardspec {
namespace {

using assistant::JsonValue;

constexpr const wchar_t* kRootPath = L"root";

bool Fail(std::wstring* error, std::wstring message) {
    if (error) *error = std::move(message);
    return false;
}

std::wstring FieldPath(const std::wstring& path, std::string_view key) {
    return path + L"." + assistant::ToWide(key);
}

std::wstring IndexPath(const std::wstring& path, std::size_t index) {
    return path + L"[" + std::to_wstring(index) + L"]";
}

std::wstring AllowedList(std::initializer_list<const char*> allowed) {
    std::wstring out;
    bool first = true;
    for (const char* key : allowed) {
        if (!first) out += L"/";
        first = false;
        out += assistant::ToWide(key);
    }
    return out;
}

bool CheckObjectShape(const JsonValue& value, const std::wstring& path,
                      std::initializer_list<const char*> allowed, std::wstring* error) {
    if (!value.IsObject()) return Fail(error, path + L"：必须是 JSON 对象");
    for (const auto& member : value.Members()) {
        bool known = false;
        for (const char* key : allowed) {
            if (member.first == key) {
                known = true;
                break;
            }
        }
        if (!known) {
            return Fail(error, path + L"：不允许的字段（只允许 " + AllowedList(allowed) + L"）");
        }
    }
    return true;
}

const JsonValue* Field(const JsonValue& object, std::string_view key) {
    if (!object.IsObject()) return nullptr;
    for (const auto& member : object.Members()) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

bool RequireField(const JsonValue& object, const std::wstring& path, const char* key,
                  const JsonValue** out, std::wstring* error) {
    const JsonValue* value = Field(object, key);
    if (!value) return Fail(error, FieldPath(path, key) + L"：缺少必填字段");
    *out = value;
    return true;
}

bool ReadText(const JsonValue& value, const std::wstring& path, std::wstring* out,
              std::wstring* error) {
    if (!value.IsString()) return Fail(error, path + L"：必须是字符串");
    const std::string& utf8 = value.String();
    std::wstring wide = assistant::ToWide(utf8);
    // The document is UTF-8 validated before parsing; a mismatch means the
    // conversion substituted bytes, so reject rather than accept a changed value.
    if (assistant::ToUtf8(wide) != utf8) {
        return Fail(error, path + L"：包含非法 UTF-8 序列");
    }
    *out = std::move(wide);
    return true;
}

bool ReadNumber(const JsonValue& value, const std::wstring& path, double* out,
                std::wstring* error) {
    if (!value.IsNumber()) return Fail(error, path + L"：必须是数字");
    const double number = value.Number();
    if (!std::isfinite(number)) return Fail(error, path + L"：必须是有限数值");
    *out = number;
    return true;
}

bool IsAsciiProfile(std::string_view profile) {
    if (profile.empty() || profile.size() > 32) return false;
    for (const unsigned char c : profile) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// --- RFC 6901 JSON Pointer ---------------------------------------------

bool ValidUtf8RoundTrip(std::string_view text) {
    return assistant::ToUtf8(assistant::ToWide(text)) == std::string(text);
}

bool DecodeToken(std::string_view raw, std::string* out, const std::wstring& path,
                 std::wstring* error) {
    out->clear();
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c != '~') {
            out->push_back(c);
            continue;
        }
        if (i + 1 >= raw.size()) {
            return Fail(error, path + L"：JSON Pointer 的 ~ 转义不完整");
        }
        const char escaped = raw[++i];
        if (escaped == '0') {
            out->push_back('~');
        } else if (escaped == '1') {
            out->push_back('/');
        } else {
            return Fail(error, path + L"：JSON Pointer 只允许 ~0/~1 转义");
        }
    }
    return true;
}

bool SplitPointer(std::string_view pointer, std::vector<std::string>* tokens,
                  const std::wstring& path, std::wstring* error) {
    tokens->clear();
    if (pointer.size() > kMaxPointerBytes) {
        return Fail(error, path + L"：JSON Pointer 超过 " +
                               std::to_wstring(kMaxPointerBytes) + L" 字节上限");
    }
    if (!ValidUtf8RoundTrip(pointer)) {
        return Fail(error, path + L"：JSON Pointer 不是有效 UTF-8");
    }
    if (pointer.empty()) return true;  // empty pointer selects the root.
    if (pointer.front() != '/') {
        return Fail(error, path + L"：JSON Pointer 必须以 / 开头");
    }

    std::size_t start = 1;
    for (;;) {
        if (tokens->size() >= kMaxPointerSegments) {
            return Fail(error, path + L"：JSON Pointer 最多 " +
                                   std::to_wstring(kMaxPointerSegments) + L" 段");
        }
        const std::size_t slash = pointer.find('/', start);
        const std::string_view raw = slash == std::string_view::npos
                                         ? pointer.substr(start)
                                         : pointer.substr(start, slash - start);
        std::string token;
        if (!DecodeToken(raw, &token, path, error)) return false;
        tokens->push_back(std::move(token));
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

bool CheckPointer(std::string_view pointer, const std::wstring& path,
                  std::wstring* error) {
    std::vector<std::string> tokens;
    return SplitPointer(pointer, &tokens, path, error);
}

bool ReadPointer(const JsonValue& value, const std::wstring& path, std::string* out,
                 std::wstring* error) {
    if (!value.IsString()) return Fail(error, path + L"：必须是字符串");
    const std::string& pointer = value.String();
    if (!CheckPointer(pointer, path, error)) return false;
    *out = pointer;
    return true;
}

// Canonical decimal array index: 0, or [1-9][0-9]*, with no sign/leading zero.
bool ParseArrayIndex(std::string_view token, std::size_t* out) {
    if (token.empty()) return false;
    if (token.size() > 1 && token.front() == '0') return false;
    std::size_t value = 0;
    const std::size_t maximum = std::numeric_limits<std::size_t>::max();
    for (const char c : token) {
        if (c < '0' || c > '9') return false;
        const std::size_t digit = static_cast<std::size_t>(c - '0');
        if (value > (maximum - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

bool ResolvePointer(const JsonValue& root, std::string_view pointer,
                    const JsonValue** out, const std::wstring& path,
                    std::wstring* error) {
    std::vector<std::string> tokens;
    if (!SplitPointer(pointer, &tokens, path, error)) return false;

    const JsonValue* current = &root;
    for (const std::string& token : tokens) {
        if (current->IsObject()) {
            const JsonValue* found = nullptr;
            for (const auto& member : current->Members()) {
                if (member.first == token) {
                    found = &member.second;
                    break;
                }
            }
            if (!found) return Fail(error, path + L"：Pointer 未找到对象键");
            current = found;
        } else if (current->IsArray()) {
            std::size_t index = 0;
            if (!ParseArrayIndex(token, &index)) {
                return Fail(error, path + L"：数组下标必须是规范十进制数字");
            }
            const std::vector<JsonValue>& items = current->Items();
            if (index >= items.size()) return Fail(error, path + L"：数组下标越界");
            current = &items[index];
        } else {
            return Fail(error, path + L"：Pointer 穿过了非对象/数组值");
        }
    }
    *out = current;
    return true;
}

// --- v2 schema ----------------------------------------------------------

bool ValidateHttpLabel(const HttpSpec& spec, std::size_t index, std::wstring* error) {
    // Reuse the v1 typed validator so labels share exactly the same UTF-16,
    // control-character and length rules. Placeholders keep the reported row
    // index equal to the real one.
    Spec probe;
    probe.title = L"x";
    probe.layout = Layout::Rows;
    Row placeholder;
    placeholder.label = L"x";
    placeholder.text = L"x";
    for (std::size_t i = 0; i < index; ++i) probe.rows.push_back(placeholder);
    Row target = placeholder;
    target.label = spec.rows[index].label;
    probe.rows.push_back(std::move(target));
    return Validate(probe, error);
}

bool ValidateHttpTitle(const HttpSpec& spec, std::wstring* error) {
    Spec probe;
    probe.title = spec.title;
    probe.layout = Layout::Rows;
    Row placeholder;
    placeholder.label = L"x";
    placeholder.text = L"x";
    probe.rows.push_back(std::move(placeholder));
    return Validate(probe, error);  // reports root.title for title problems.
}

bool BuildHttpSpec(const JsonValue& root, HttpSpec* out, std::wstring* error) {
    if (!CheckObjectShape(root, kRootPath, {"version", "title", "source", "layout", "rows"},
                          error)) {
        return false;
    }
    const JsonValue* version = nullptr;
    const JsonValue* title = nullptr;
    const JsonValue* source = nullptr;
    const JsonValue* layout = nullptr;
    const JsonValue* rows = nullptr;
    if (!RequireField(root, kRootPath, "version", &version, error)) return false;
    if (!RequireField(root, kRootPath, "title", &title, error)) return false;
    if (!RequireField(root, kRootPath, "source", &source, error)) return false;
    if (!RequireField(root, kRootPath, "layout", &layout, error)) return false;
    if (!RequireField(root, kRootPath, "rows", &rows, error)) return false;

    double version_number = 0.0;
    if (!ReadNumber(*version, FieldPath(kRootPath, "version"), &version_number, error)) {
        return false;
    }
    if (version_number != 2.0) return Fail(error, L"root.version：只允许数值 2");

    HttpSpec spec;
    if (!ReadText(*title, FieldPath(kRootPath, "title"), &spec.title, error)) return false;

    if (!layout->IsString()) return Fail(error, L"root.layout：必须是字符串");
    if (layout->String() == "rows") {
        spec.layout = Layout::Rows;
    } else if (layout->String() == "bar") {
        spec.layout = Layout::Bar;
    } else {
        return Fail(error, L"root.layout：只允许 \"rows\" 或 \"bar\"");
    }

    if (!CheckObjectShape(*source, L"root.source", {"type", "profile"}, error)) return false;
    const JsonValue* source_type = nullptr;
    const JsonValue* profile = nullptr;
    if (!RequireField(*source, L"root.source", "type", &source_type, error)) return false;
    if (!source_type->IsString() || source_type->String() != "http") {
        return Fail(error, L"root.source.type：只允许 \"http\"");
    }
    if (!RequireField(*source, L"root.source", "profile", &profile, error)) return false;
    if (!profile->IsString()) {
        return Fail(error, L"root.source.profile：必须是字符串");
    }
    spec.profile = profile->String();
    if (!IsAsciiProfile(spec.profile)) {
        return Fail(error, L"root.source.profile：只允许 1..32 位 ASCII 字母/数字/_/-");
    }

    if (!rows->IsArray()) return Fail(error, L"root.rows：必须是数组");
    const std::vector<JsonValue>& items = rows->Items();
    if (items.empty()) return Fail(error, L"root.rows：至少需要 1 行");
    if (items.size() > kMaxRows) {
        return Fail(error, L"root.rows：最多 " + std::to_wstring(kMaxRows) + L" 行");
    }

    spec.rows.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        const JsonValue& item = items[i];
        const std::wstring row_path = IndexPath(L"root.rows", i);
        HttpRow row;
        if (spec.layout == Layout::Rows) {
            if (!CheckObjectShape(item, row_path, {"label", "text_path"}, error)) {
                return false;
            }
            const JsonValue* label = nullptr;
            const JsonValue* text_path = nullptr;
            if (!RequireField(item, row_path, "label", &label, error)) return false;
            if (!RequireField(item, row_path, "text_path", &text_path, error)) return false;
            if (!ReadText(*label, FieldPath(row_path, "label"), &row.label, error)) {
                return false;
            }
            if (!ReadPointer(*text_path, FieldPath(row_path, "text_path"),
                             &row.textPointer, error)) {
                return false;
            }
        } else {
            if (!CheckObjectShape(item, row_path,
                                  {"label", "value_path", "max", "max_path"}, error)) {
                return false;
            }
            const JsonValue* label = nullptr;
            const JsonValue* value_path = nullptr;
            if (!RequireField(item, row_path, "label", &label, error)) return false;
            if (!RequireField(item, row_path, "value_path", &value_path, error)) return false;
            if (!ReadText(*label, FieldPath(row_path, "label"), &row.label, error)) {
                return false;
            }
            if (!ReadPointer(*value_path, FieldPath(row_path, "value_path"),
                             &row.valuePointer, error)) {
                return false;
            }

            const JsonValue* max = Field(item, "max");
            const JsonValue* max_path = Field(item, "max_path");
            if ((max != nullptr) == (max_path != nullptr)) {
                return Fail(error, row_path + L"：max 与 max_path 必须恰有一个");
            }
            if (max) {
                row.maxFromPointer = false;
                if (!ReadNumber(*max, FieldPath(row_path, "max"), &row.max, error)) {
                    return false;
                }
            } else {
                row.maxFromPointer = true;
                if (!ReadPointer(*max_path, FieldPath(row_path, "max_path"),
                                 &row.maxPointer, error)) {
                    return false;
                }
            }
        }
        spec.rows.push_back(std::move(row));
    }

    if (!ValidateHttpSpec(spec, error)) return false;
    *out = std::move(spec);
    return true;
}

} // namespace

bool ValidateHttpSpec(const HttpSpec& spec, std::wstring* error) {
    if (error) error->clear();
    if (spec.layout != Layout::Rows && spec.layout != Layout::Bar) {
        return Fail(error, L"root.layout：布局枚举无效");
    }
    if (!IsAsciiProfile(spec.profile)) {
        return Fail(error, L"root.source.profile：只允许 1..32 位 ASCII 字母/数字/_/-");
    }
    if (!ValidateHttpTitle(spec, error)) return false;
    if (spec.rows.empty()) return Fail(error, L"root.rows：至少需要 1 行");
    if (spec.rows.size() > kMaxRows) {
        return Fail(error, L"root.rows：最多 " + std::to_wstring(kMaxRows) + L" 行");
    }

    for (std::size_t i = 0; i < spec.rows.size(); ++i) {
        const HttpRow& row = spec.rows[i];
        const std::wstring row_path = IndexPath(L"root.rows", i);
        if (!ValidateHttpLabel(spec, i, error)) return false;
        if (spec.layout == Layout::Rows) {
            if (!CheckPointer(row.textPointer, FieldPath(row_path, "text_path"), error)) {
                return false;
            }
            continue;
        }

        if (!CheckPointer(row.valuePointer, FieldPath(row_path, "value_path"), error)) {
            return false;
        }
        if (row.maxFromPointer) {
            if (!CheckPointer(row.maxPointer, FieldPath(row_path, "max_path"), error)) {
                return false;
            }
        } else {
            if (!std::isfinite(row.max)) {
                return Fail(error, FieldPath(row_path, "max") + L"：必须是有限数值");
            }
            if (!(row.max > 0.0)) {
                return Fail(error, FieldPath(row_path, "max") + L"：必须大于 0");
            }
        }
    }
    return true;
}

bool ParseSourceDocument(std::string_view utf8, SourceDocument* out,
                         std::wstring* error) {
    if (error) error->clear();
    if (!out) return Fail(error, L"ParseSourceDocument：输出对象为空");

    JsonValue root;
    if (!ParseStrictJson(utf8, &root, error)) return false;

    bool version_two = false;
    if (root.IsObject()) {
        const JsonValue* version = Field(root, "version");
        if (version && version->IsNumber() && version->Number() == 2.0) {
            version_two = true;
        }
    }

    SourceDocument document;
    if (version_two) {
        HttpSpec spec;
        if (!BuildHttpSpec(root, &spec, error)) return false;
        document.kind = SourceKind::Http;
        document.httpSpec = std::move(spec);
    } else {
        // All non-v2 documents keep the exact v1 entry point and error matrix.
        Spec spec;
        if (!Parse(utf8, &spec, error)) return false;
        document.kind = SourceKind::Inline;
        document.inlineSpec = std::move(spec);
    }
    *out = std::move(document);
    return true;
}

bool DecodeHttpResponse(const HttpSpec& spec, std::string_view utf8, Spec* out,
                        std::wstring* error) {
    if (error) error->clear();
    if (!out) return Fail(error, L"DecodeHttpResponse：输出对象为空");
    if (!ValidateHttpSpec(spec, error)) return false;

    JsonValue root;
    if (!ParseStrictJson(utf8, &root, error)) return false;

    Spec bound;
    bound.title = spec.title;
    bound.layout = spec.layout;
    bound.rows.reserve(spec.rows.size());

    for (std::size_t i = 0; i < spec.rows.size(); ++i) {
        const HttpRow& http_row = spec.rows[i];
        const std::wstring row_path = IndexPath(L"root.rows", i);
        Row row;
        row.label = http_row.label;

        if (spec.layout == Layout::Rows) {
            const JsonValue* value = nullptr;
            if (!ResolvePointer(root, http_row.textPointer, &value,
                                FieldPath(row_path, "text_path"), error)) {
                return false;
            }
            if (value->IsString()) {
                const std::string& utf8_text = value->String();
                std::wstring wide = assistant::ToWide(utf8_text);
                if (assistant::ToUtf8(wide) != utf8_text) {
                    return Fail(error, FieldPath(row_path, "text") + L"：包含非法 UTF-8 序列");
                }
                row.text = std::move(wide);
            } else if (value->IsNumber()) {
                const double number = value->Number();
                if (!std::isfinite(number)) {
                    return Fail(error,
                                FieldPath(row_path, "text") + L"：必须是有限数值");
                }
                // Compact JSON number, e.g. 10 not 1e+01.
                row.text = assistant::ToWide(JsonValue::MakeNumber(number).Dump());
            } else if (value->IsBool()) {
                row.text = value->Bool() ? L"true" : L"false";
            } else {
                return Fail(error, FieldPath(row_path, "text") +
                                       L"：必须是字符串、有限数值或布尔值");
            }
        } else {
            const JsonValue* value = nullptr;
            if (!ResolvePointer(root, http_row.valuePointer, &value,
                                FieldPath(row_path, "value_path"), error)) {
                return false;
            }
            if (!value->IsNumber()) {
                return Fail(error, FieldPath(row_path, "value") + L"：必须是 JSON 数值");
            }
            const double number = value->Number();
            if (!std::isfinite(number)) {
                return Fail(error, FieldPath(row_path, "value") + L"：必须是有限数值");
            }
            row.value = number;

            if (http_row.maxFromPointer) {
                const JsonValue* max = nullptr;
                if (!ResolvePointer(root, http_row.maxPointer, &max,
                                    FieldPath(row_path, "max_path"), error)) {
                    return false;
                }
                if (!max->IsNumber()) {
                    return Fail(error, FieldPath(row_path, "max") + L"：必须是 JSON 数值");
                }
                if (!std::isfinite(max->Number())) {
                    return Fail(error, FieldPath(row_path, "max") + L"：必须是有限数值");
                }
                row.max = max->Number();
            } else {
                row.max = http_row.max;
            }
        }
        bound.rows.push_back(std::move(row));
    }

    // Reuse v1 typed rules for title/label text, UTF-16, controls, row count,
    // finite ranges and value <= max.
    if (!Validate(bound, error)) return false;
    *out = std::move(bound);
    return true;
}

} // namespace cardspec
