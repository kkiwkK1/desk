// M2.0 CardSpec core: strict, inline-only declarative card data.
//
// Safety invariants:
//   * no file, network, exec or model calls - this translation unit only
//     transforms an in-memory UTF-8 document into a typed Spec and back;
//   * the v1 schema is closed: every object layer rejects unknown and
//     duplicate keys, so a future field can never be silently accepted;
//   * UTF-8 is validated byte-exactly (no ToWide substitution) before any
//     conversion, and text fields are checked for C0/DEL and unpaired UTF-16
//     surrogates;
//   * error text names the field path and limit only - it never repeats the
//     document payload (which may contain URLs, paths or secrets).
#include "data/cardspec/cardspec.h"

#include "data/cardspec/strict_json.h"
#include "data/assistant/json.h"

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cardspec {
namespace {

using assistant::JsonValue;

// Same ceiling as the assistant JSON parser; deep documents fail cleanly
// instead of exhausting the stack.
constexpr int kMaxJsonDepth = 64;

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

// --- Strict UTF-8 / UTF-16 checks ---------------------------------------

// Rejects overlong forms, encoded surrogates, truncation and >U+10FFFF.
bool IsValidUtf8(std::string_view text, std::size_t* bad_offset) {
    std::size_t i = 0;
    const auto fail = [bad_offset](std::size_t at) {
        if (bad_offset) *bad_offset = at;
        return false;
    };
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80u) {
            ++i;
            continue;
        }
        std::size_t extra = 0;
        unsigned codepoint = 0;
        if ((lead & 0xE0u) == 0xC0u) {
            extra = 1;
            codepoint = lead & 0x1Fu;
        } else if ((lead & 0xF0u) == 0xE0u) {
            extra = 2;
            codepoint = lead & 0x0Fu;
        } else if ((lead & 0xF8u) == 0xF0u) {
            extra = 3;
            codepoint = lead & 0x07u;
        } else {
            return fail(i);
        }
        if (i + extra >= text.size()) return fail(i);
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char continuation = static_cast<unsigned char>(text[i + k]);
            if ((continuation & 0xC0u) != 0x80u) return fail(i);
            codepoint = (codepoint << 6) | (continuation & 0x3Fu);
        }
        static constexpr unsigned kMinCodepoint[4] = {0u, 0x80u, 0x800u, 0x10000u};
        if (codepoint < kMinCodepoint[extra]) return fail(i);  // overlong
        if (codepoint > 0x10FFFFu) return fail(i);
        if (codepoint >= 0xD800u && codepoint <= 0xDFFFu) return fail(i);
        i += extra + 1;
    }
    return true;
}

void AppendUtf8(std::string& out, unsigned codepoint) {
    if (codepoint <= 0x7Fu) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
}

bool IsWellFormedUtf16(std::wstring_view text) {
#if WCHAR_MAX <= 0xFFFF
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned unit = static_cast<unsigned>(text[i]);
        if (unit >= 0xD800u && unit <= 0xDBFFu) {
            if (i + 1 >= text.size()) return false;
            const unsigned low = static_cast<unsigned>(text[i + 1]);
            if (low < 0xDC00u || low > 0xDFFFu) return false;
            i += 2;
        } else if (unit >= 0xDC00u && unit <= 0xDFFFu) {
            return false;
        } else {
            ++i;
        }
    }
    return true;
#else
    for (const wchar_t ch : text) {
        const unsigned long codepoint = static_cast<unsigned long>(ch);
        if (codepoint > 0x10FFFFul) return false;
        if (codepoint >= 0xD800ul && codepoint <= 0xDFFFul) return false;
    }
    return true;
#endif
}

std::size_t Utf16Units(std::wstring_view text) {
#if WCHAR_MAX <= 0xFFFF
    return text.size();
#else
    std::size_t units = 0;
    for (const wchar_t ch : text) units += static_cast<unsigned long>(ch) > 0xFFFFul ? 2 : 1;
    return units;
#endif
}

// --- Strict JSON reader -------------------------------------------------

// assistant::ParseJson collapses duplicate members through JsonValue::Set and
// does not validate UTF-8, so the schema layer needs its own scanner. It
// produces the same assistant::JsonValue tree and reports ASCII-safe byte
// offsets instead of echoing document bytes.
class StrictJsonParser {
public:
    explicit StrictJsonParser(std::string_view text) : text_(text) {}

    bool ParseDocument(JsonValue& out) {
        SkipWhitespace();
        JsonValue value;
        if (!ParseValue(value, 0)) return false;
        SkipWhitespace();
        if (pos_ != text_.size()) return SyntaxError(L"文档结尾存在多余内容");
        out = std::move(value);
        return true;
    }

    const std::wstring& error() const { return error_; }

private:
    bool SyntaxError(std::wstring_view what) {
        if (error_.empty()) {
            error_ = L"JSON 语法错误（字节 " + std::to_wstring(pos_) + L"）：" +
                     std::wstring(what);
        }
        return false;
    }

    void SkipWhitespace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++pos_;
        }
    }

    bool ParseValue(JsonValue& out, int depth) {
        if (depth > kMaxJsonDepth) return SyntaxError(L"JSON 嵌套层数超过 64");
        if (pos_ >= text_.size()) return SyntaxError(L"JSON 意外结束");
        switch (text_[pos_]) {
            case '{': return ParseObject(out, depth);
            case '[': return ParseArray(out, depth);
            case '"': {
                std::string text;
                if (!ParseString(text)) return false;
                out = JsonValue::MakeString(std::move(text));
                return true;
            }
            case 't': return ParseLiteral("true", JsonValue::MakeBool(true), out);
            case 'f': return ParseLiteral("false", JsonValue::MakeBool(false), out);
            case 'n': return ParseLiteral("null", JsonValue::MakeNull(), out);
            default: return ParseNumber(out);
        }
    }

    bool ParseObject(JsonValue& out, int depth) {
        ++pos_;  // '{'
        JsonValue object = JsonValue::MakeObject();
        std::vector<std::string> keys;
        SkipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            out = std::move(object);
            return true;
        }
        for (;;) {
            SkipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return SyntaxError(L"JSON 对象缺少键");
            }
            std::string key;
            if (!ParseString(key)) return false;
            for (const std::string& seen : keys) {
                if (seen == key) return SyntaxError(L"JSON 对象存在重复键");
            }
            keys.push_back(key);

            SkipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return SyntaxError(L"JSON 对象缺少冒号");
            }
            ++pos_;

            JsonValue member;
            if (!ParseValue(member, depth + 1)) return false;
            object.Set(std::move(key), std::move(member));

            SkipWhitespace();
            if (pos_ >= text_.size()) return SyntaxError(L"JSON 对象未结束");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                out = std::move(object);
                return true;
            }
            return SyntaxError(L"JSON 对象缺少逗号或右括号");
        }
    }

    bool ParseArray(JsonValue& out, int depth) {
        ++pos_;  // '['
        JsonValue array = JsonValue::MakeArray();
        SkipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            out = std::move(array);
            return true;
        }
        for (;;) {
            JsonValue item;
            if (!ParseValue(item, depth + 1)) return false;
            array.Push(std::move(item));

            SkipWhitespace();
            if (pos_ >= text_.size()) return SyntaxError(L"JSON 数组未结束");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                out = std::move(array);
                return true;
            }
            return SyntaxError(L"JSON 数组缺少逗号或右括号");
        }
    }

    bool ParseString(std::string& out) {
        if (pos_ >= text_.size() || text_[pos_] != '"') {
            return SyntaxError(L"JSON 缺少字符串");
        }
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= text_.size()) return SyntaxError(L"JSON 转义不完整");
                const char escape = text_[pos_++];
                switch (escape) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned codepoint = 0;
                        if (!ParseHex4(codepoint)) return false;
                        if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
                            if (pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
                                text_[pos_ + 1] == 'u') {
                                pos_ += 2;
                                unsigned low = 0;
                                if (!ParseHex4(low)) return false;
                                if (low < 0xDC00u || low > 0xDFFFu) {
                                    return SyntaxError(L"JSON 转义含非法 UTF-16 代理对");
                                }
                                codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) +
                                            (low - 0xDC00u);
                            } else {
                                return SyntaxError(L"JSON 转义含孤立 UTF-16 高代理项");
                            }
                        } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
                            return SyntaxError(L"JSON 转义含孤立 UTF-16 低代理项");
                        }
                        AppendUtf8(out, codepoint);
                        break;
                    }
                    default: return SyntaxError(L"JSON 转义字符无效");
                }
                continue;
            }
            if (c < 0x20u) return SyntaxError(L"JSON 字符串含未转义控制字符");
            out.push_back(static_cast<char>(c));
        }
        return SyntaxError(L"JSON 字符串未结束");
    }

    bool ParseHex4(unsigned& out) {
        if (pos_ + 4 > text_.size()) return SyntaxError(L"JSON \\u 转义不完整");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_ + static_cast<std::size_t>(i)];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                return SyntaxError(L"JSON \\u 转义无效");
            }
        }
        pos_ += 4;
        out = value;
        return true;
    }

    bool ParseNumber(JsonValue& out) {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        if (pos_ >= text_.size()) return SyntaxError(L"JSON 数字格式无效");
        if (text_[pos_] == '0') {
            ++pos_;
        } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        } else {
            return SyntaxError(L"JSON 数字格式无效");
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
                return SyntaxError(L"JSON 小数格式无效");
            }
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
                return SyntaxError(L"JSON 指数格式无效");
            }
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        const std::string literal(text_.substr(start, pos_ - start));
        out = JsonValue::MakeNumber(std::strtod(literal.c_str(), nullptr));
        return true;
    }

    bool ParseLiteral(std::string_view literal, JsonValue value, JsonValue& out) {
        if (text_.compare(pos_, literal.size(), literal) != 0) {
            return SyntaxError(L"JSON 字面量无效");
        }
        pos_ += literal.size();
        out = std::move(value);
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::wstring error_;
};

// --- Field helpers ------------------------------------------------------

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

bool CheckTypedText(std::wstring_view text, const std::wstring& path, std::size_t min_units,
                    std::size_t max_units, std::wstring* error) {
    if (!IsWellFormedUtf16(text)) return Fail(error, path + L"：包含非法 UTF-16 代理项");
    for (const wchar_t ch : text) {
        const unsigned code = static_cast<unsigned>(ch);
        if (code < 0x20u || code == 0x7Fu) {
            return Fail(error, path + L"：禁止控制字符或 DEL");
        }
    }
    const std::size_t units = Utf16Units(text);
    if (units < min_units || units > max_units) {
        return Fail(error, path + L"：长度须为 " + std::to_wstring(min_units) + L".." +
                                std::to_wstring(max_units) + L" 个 UTF-16 码元");
    }
    return true;
}

bool ReadText(const JsonValue& value, const std::wstring& path, std::size_t min_units,
              std::size_t max_units, std::wstring* out, std::wstring* error) {
    if (!value.IsString()) return Fail(error, path + L"：必须是字符串");
    const std::string& utf8 = value.String();
    std::wstring wide = assistant::ToWide(utf8);
    // The document was UTF-8 validated before parsing, so a mismatch means the
    // conversion silently substituted something: reject instead of accepting.
    if (assistant::ToUtf8(wide) != utf8) return Fail(error, path + L"：包含非法 UTF-8 序列");
    if (!CheckTypedText(wide, path, min_units, max_units, error)) return false;
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

bool ReadRow(const JsonValue& item, const std::wstring& row_path, Layout layout, Row* out,
             std::wstring* error) {
    if (layout == Layout::Rows) {
        if (!CheckObjectShape(item, row_path, {"label", "text"}, error)) return false;
        const JsonValue* label = nullptr;
        const JsonValue* text = nullptr;
        if (!RequireField(item, row_path, "label", &label, error)) return false;
        if (!RequireField(item, row_path, "text", &text, error)) return false;
        if (!ReadText(*label, FieldPath(row_path, "label"), 1, 32, &out->label, error)) {
            return false;
        }
        return ReadText(*text, FieldPath(row_path, "text"), 1, 96, &out->text, error);
    }

    if (!CheckObjectShape(item, row_path, {"label", "value", "max"}, error)) return false;
    const JsonValue* label = nullptr;
    const JsonValue* value = nullptr;
    const JsonValue* max = nullptr;
    if (!RequireField(item, row_path, "label", &label, error)) return false;
    if (!RequireField(item, row_path, "value", &value, error)) return false;
    if (!RequireField(item, row_path, "max", &max, error)) return false;
    if (!ReadText(*label, FieldPath(row_path, "label"), 1, 32, &out->label, error)) {
        return false;
    }
    if (!ReadNumber(*value, FieldPath(row_path, "value"), &out->value, error)) return false;
    if (!ReadNumber(*max, FieldPath(row_path, "max"), &out->max, error)) return false;
    if (!(out->max > 0.0)) return Fail(error, FieldPath(row_path, "max") + L"：必须大于 0");
    if (out->value < 0.0 || out->value > out->max) {
        return Fail(error, FieldPath(row_path, "value") + L"：须在 0..max 范围内");
    }
    return true;
}

bool BuildSpec(const JsonValue& root, Spec* out, std::wstring* error) {
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
    if (version_number != 1.0) return Fail(error, L"root.version：只允许数值 1");

    Layout parsed_layout = Layout::Rows;
    if (!layout->IsString()) return Fail(error, L"root.layout：必须是字符串");
    if (layout->String() == "rows") {
        parsed_layout = Layout::Rows;
    } else if (layout->String() == "bar") {
        parsed_layout = Layout::Bar;
    } else {
        return Fail(error, L"root.layout：只允许 \"rows\" 或 \"bar\"");
    }

    if (!CheckObjectShape(*source, L"root.source", {"type"}, error)) return false;
    const JsonValue* source_type = nullptr;
    if (!RequireField(*source, L"root.source", "type", &source_type, error)) return false;
    if (!source_type->IsString() || source_type->String() != "inline") {
        return Fail(error, L"root.source.type：只允许 \"inline\"");
    }

    Spec spec;
    if (!ReadText(*title, FieldPath(kRootPath, "title"), 1, 64, &spec.title, error)) {
        return false;
    }

    if (!rows->IsArray()) return Fail(error, L"root.rows：必须是数组");
    const std::vector<JsonValue>& items = rows->Items();
    if (items.empty()) return Fail(error, L"root.rows：至少需要 1 行");
    if (items.size() > kMaxRows) {
        return Fail(error, L"root.rows：最多 " + std::to_wstring(kMaxRows) + L" 行");
    }
    spec.layout = parsed_layout;
    spec.rows.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        Row row;
        if (!ReadRow(items[i], IndexPath(L"root.rows", i), parsed_layout, &row, error)) {
            return false;
        }
        spec.rows.push_back(std::move(row));
    }

    *out = std::move(spec);
    return true;
}

// Shared by Parse()/ParseStrictJson(): size/BOM/UTF-8/depth/duplicate-key
// rules are identical, only the schema layer differs.
bool ParseStrictRoot(std::string_view utf8, JsonValue* out, std::wstring* error,
                     const wchar_t* api_name) {
    if (!out) return Fail(error, std::wstring(api_name) + L"：输出对象为空");
    if (utf8.size() > kMaxSpecBytes) {
        return Fail(error, L"文档大小超过 " + std::to_wstring(kMaxSpecBytes) + L" 字节上限");
    }
    if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEFu &&
        static_cast<unsigned char>(utf8[1]) == 0xBBu &&
        static_cast<unsigned char>(utf8[2]) == 0xBFu) {
        utf8.remove_prefix(3);  // BOM is accepted but still counts towards kMaxSpecBytes.
    }
    if (utf8.empty()) return Fail(error, L"JSON 语法错误（字节 0）：文档为空");
    std::size_t bad_offset = 0;
    if (!IsValidUtf8(utf8, &bad_offset)) {
        return Fail(error, L"文档不是有效的 UTF-8（字节 " + std::to_wstring(bad_offset) + L"）");
    }

    StrictJsonParser parser(utf8);
    JsonValue root;
    if (!parser.ParseDocument(root)) return Fail(error, parser.error());
    *out = std::move(root);
    return true;
}

} // namespace

bool ParseStrictJson(std::string_view utf8, assistant::JsonValue* out,
                     std::wstring* error) {
    if (error) error->clear();
    return ParseStrictRoot(utf8, out, error, L"ParseStrictJson");
}

bool Validate(const Spec& spec, std::wstring* error) {
    if (error) error->clear();
    if (spec.layout != Layout::Rows && spec.layout != Layout::Bar) {
        return Fail(error, L"root.layout：布局枚举无效");
    }
    if (!CheckTypedText(spec.title, FieldPath(kRootPath, "title"), 1, 64, error)) return false;
    if (spec.rows.empty()) return Fail(error, L"root.rows：至少需要 1 行");
    if (spec.rows.size() > kMaxRows) {
        return Fail(error, L"root.rows：最多 " + std::to_wstring(kMaxRows) + L" 行");
    }
    for (std::size_t i = 0; i < spec.rows.size(); ++i) {
        const Row& row = spec.rows[i];
        const std::wstring row_path = IndexPath(L"root.rows", i);
        if (!CheckTypedText(row.label, FieldPath(row_path, "label"), 1, 32, error)) {
            return false;
        }
        if (spec.layout == Layout::Rows) {
            if (!CheckTypedText(row.text, FieldPath(row_path, "text"), 1, 96, error)) {
                return false;
            }
            continue;
        }
        if (!std::isfinite(row.value)) {
            return Fail(error, FieldPath(row_path, "value") + L"：必须是有限数值");
        }
        if (!std::isfinite(row.max)) {
            return Fail(error, FieldPath(row_path, "max") + L"：必须是有限数值");
        }
        if (!(row.max > 0.0)) return Fail(error, FieldPath(row_path, "max") + L"：必须大于 0");
        if (row.value < 0.0 || row.value > row.max) {
            return Fail(error, FieldPath(row_path, "value") + L"：须在 0..max 范围内");
        }
    }
    return true;
}

bool Parse(std::string_view utf8, Spec* out, std::wstring* error) {
    if (error) error->clear();
    if (!out) return Fail(error, L"Parse：输出对象为空");
    JsonValue root;
    if (!ParseStrictRoot(utf8, &root, error, L"Parse")) return false;

    Spec spec;
    if (!BuildSpec(root, &spec, error)) return false;
    *out = std::move(spec);
    return true;
}

std::string Serialize(const Spec& spec, std::wstring* error) {
    if (!Validate(spec, error)) return {};

    JsonValue root = JsonValue::MakeObject();
    root.Set("version", JsonValue::MakeInt(1));
    root.Set("title", JsonValue::MakeString(assistant::ToUtf8(spec.title)));

    JsonValue source = JsonValue::MakeObject();
    source.Set("type", JsonValue::MakeString("inline"));
    root.Set("source", std::move(source));

    root.Set("layout", JsonValue::MakeString(spec.layout == Layout::Rows ? "rows" : "bar"));

    JsonValue rows = JsonValue::MakeArray();
    for (const Row& row : spec.rows) {
        JsonValue item = JsonValue::MakeObject();
        item.Set("label", JsonValue::MakeString(assistant::ToUtf8(row.label)));
        if (spec.layout == Layout::Rows) {
            item.Set("text", JsonValue::MakeString(assistant::ToUtf8(row.text)));
        } else {
            item.Set("value", JsonValue::MakeNumber(row.value));
            item.Set("max", JsonValue::MakeNumber(row.max));
        }
        rows.Push(std::move(item));
    }
    root.Set("rows", std::move(rows));
    return root.Dump();
}

} // namespace cardspec
