#include "json.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace assistant {
namespace {

constexpr int kMaxDepth = 64;

const std::string& EmptyString() {
    static const std::string kEmpty;
    return kEmpty;
}

const JsonValue& NullValue() {
    static const JsonValue kNull;
    return kNull;
}

const std::vector<JsonValue>& EmptyItems() {
    static const std::vector<JsonValue> kEmpty;
    return kEmpty;
}

const std::vector<std::pair<std::string, JsonValue>>& EmptyMembers() {
    static const std::vector<std::pair<std::string, JsonValue>> kEmpty;
    return kEmpty;
}

void AppendCodepoint(std::string& out, unsigned codepoint) {
    if (codepoint <= 0x7Fu) {
        out.push_back((char)codepoint);
    } else if (codepoint <= 0x7FFu) {
        out.push_back((char)(0xC0u | (codepoint >> 6)));
        out.push_back((char)(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint <= 0xFFFFu) {
        out.push_back((char)(0xE0u | (codepoint >> 12)));
        out.push_back((char)(0x80u | ((codepoint >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (codepoint & 0x3Fu)));
    } else {
        out.push_back((char)(0xF0u | (codepoint >> 18)));
        out.push_back((char)(0x80u | ((codepoint >> 12) & 0x3Fu)));
        out.push_back((char)(0x80u | ((codepoint >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (codepoint & 0x3Fu)));
    }
}

std::string EscapeString(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size() + 2);
    out.push_back('"');
    for (const unsigned char c : utf8) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", (unsigned)c);
                    out += buffer;
                } else {
                    out.push_back((char)c);
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

// Shortest decimal text that reads back to the exact same double; JSON has no
// inf/nan, so those degrade to 0 instead of producing invalid output.
std::string FormatNumber(double value) {
    if (!std::isfinite(value)) return "0";
    char buffer[48];
    // Prefer the plain integer form ("10", not "1e+01") for counts and ids.
    if (value == std::floor(value) && std::fabs(value) < 1e15) {
        std::snprintf(buffer, sizeof(buffer), "%.0f", value);
        if (std::strcmp(buffer, "-0") == 0) return "0";
        return buffer;
    }
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        if (std::strtod(buffer, nullptr) == value) break;
    }
    if (std::strcmp(buffer, "-0") == 0) return "0";
    return buffer;
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool ParseValue(JsonValue& out) {
        SkipWhitespace();
        return ParseValueAt(out, 0);
    }

    bool Finish() {
        SkipWhitespace();
        if (pos_ != text_.size()) return Fail("trailing data");
        return true;
    }

    const std::string& error() const { return error_; }

private:
    bool Fail(const char* what) {
        if (error_.empty()) {
            error_ = std::string(what) + " at byte " + std::to_string(pos_);
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

    bool ParseValueAt(JsonValue& out, int depth) {
        if (depth > kMaxDepth) return Fail("nesting too deep");
        if (pos_ >= text_.size()) return Fail("unexpected end");

        switch (text_[pos_]) {
            case '{': return ParseObject(out, depth);
            case '[': return ParseArray(out, depth);
            case '"': {
                std::string text;
                if (!ParseString(text)) return false;
                out = JsonValue::MakeString(std::move(text));
                return true;
            }
            case 't':
                if (!ConsumeLiteral("true")) return false;
                out = JsonValue::MakeBool(true);
                return true;
            case 'f':
                if (!ConsumeLiteral("false")) return false;
                out = JsonValue::MakeBool(false);
                return true;
            case 'n':
                if (!ConsumeLiteral("null")) return false;
                out = JsonValue::MakeNull();
                return true;
            default:
                return ParseNumber(out);
        }
    }

    bool ParseObject(JsonValue& out, int depth) {
        ++pos_;  // '{'
        JsonValue object = JsonValue::MakeObject();
        SkipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            out = std::move(object);
            return true;
        }
        for (;;) {
            SkipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != '"') return Fail("expected key");
            std::string key;
            if (!ParseString(key)) return false;

            SkipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != ':') return Fail("expected ':'");
            ++pos_;

            JsonValue member;
            if (!ParseValueAt(member, depth + 1)) return false;
            object.Set(std::move(key), std::move(member));

            SkipWhitespace();
            if (pos_ >= text_.size()) return Fail("unterminated object");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                out = std::move(object);
                return true;
            }
            return Fail("expected ',' or '}'");
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
            if (!ParseValueAt(item, depth + 1)) return false;
            array.Push(std::move(item));

            SkipWhitespace();
            if (pos_ >= text_.size()) return Fail("unterminated array");
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                out = std::move(array);
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseString(std::string& out) {
        if (pos_ >= text_.size() || text_[pos_] != '"') return Fail("expected string");
        ++pos_;
        out.clear();

        while (pos_ < text_.size()) {
            const unsigned char c = (unsigned char)text_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= text_.size()) return Fail("unterminated escape");
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
                            // High surrogate: pair it with the following \uXXXX.
                            if (pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
                                text_[pos_ + 1] == 'u') {
                                pos_ += 2;
                                unsigned low = 0;
                                if (!ParseHex4(low)) return false;
                                if (low >= 0xDC00u && low <= 0xDFFFu) {
                                    codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) +
                                                (low - 0xDC00u);
                                } else {
                                    AppendCodepoint(out, 0xFFFDu);  // lone high surrogate
                                    codepoint = low;
                                }
                            } else {
                                codepoint = 0xFFFDu;
                            }
                        } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
                            codepoint = 0xFFFDu;  // lone low surrogate
                        }
                        AppendCodepoint(out, codepoint);
                        break;
                    }
                    default:
                        return Fail("bad escape");
                }
                continue;
            }
            if (c < 0x20u) return Fail("raw control character in string");
            out.push_back((char)c);
        }
        return Fail("unterminated string");
    }

    bool ParseHex4(unsigned& out) {
        if (pos_ + 4 > text_.size()) return Fail("short \\u escape");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_ + (std::size_t)i];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= (unsigned)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= (unsigned)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= (unsigned)(c - 'A' + 10);
            } else {
                return Fail("bad \\u escape");
            }
        }
        pos_ += 4;
        out = value;
        return true;
    }

    bool ParseNumber(JsonValue& out) {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;

        if (pos_ >= text_.size()) return Fail("bad number");
        if (text_[pos_] == '0') {
            ++pos_;
        } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        } else {
            return Fail("bad number");
        }

        if (pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
                return Fail("bad fraction");
            }
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }

        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
                return Fail("bad exponent");
            }
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }

        const std::string text(text_.substr(start, pos_ - start));
        out = JsonValue::MakeNumber(std::strtod(text.c_str(), nullptr));
        return true;
    }

    bool ConsumeLiteral(const char* literal) {
        const std::size_t length = std::strlen(literal);
        if (text_.compare(pos_, length, literal) != 0) return Fail("bad literal");
        pos_ += length;
        return true;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::string error_;
};

} // namespace

// --- JsonValue -----------------------------------------------------------

JsonValue::JsonValue() = default;
JsonValue::~JsonValue() = default;
JsonValue::JsonValue(const JsonValue&) = default;
JsonValue::JsonValue(JsonValue&&) noexcept = default;
JsonValue& JsonValue::operator=(const JsonValue&) = default;
JsonValue& JsonValue::operator=(JsonValue&&) noexcept = default;

JsonValue JsonValue::MakeNull() { return JsonValue(); }

JsonValue JsonValue::MakeBool(bool value) {
    JsonValue json;
    json.type_ = Type::Bool;
    json.bool_ = value;
    return json;
}

JsonValue JsonValue::MakeNumber(double value) {
    JsonValue json;
    json.type_ = Type::Number;
    json.number_ = value;
    return json;
}

JsonValue JsonValue::MakeInt(long long value) {
    return MakeNumber((double)value);
}

JsonValue JsonValue::MakeString(std::string utf8) {
    JsonValue json;
    json.type_ = Type::String;
    json.string_ = std::move(utf8);
    return json;
}

JsonValue JsonValue::MakeArray() {
    JsonValue json;
    json.type_ = Type::Array;
    return json;
}

JsonValue JsonValue::MakeObject() {
    JsonValue json;
    json.type_ = Type::Object;
    return json;
}

bool JsonValue::Bool(bool fallback) const {
    return type_ == Type::Bool ? bool_ : fallback;
}

double JsonValue::Number(double fallback) const {
    return type_ == Type::Number ? number_ : fallback;
}

long long JsonValue::Integer(long long fallback) const {
    if (type_ != Type::Number) return fallback;
    if (!std::isfinite(number_)) return fallback;
    return (long long)std::llround(number_);
}

const std::string& JsonValue::String() const {
    return type_ == Type::String ? string_ : EmptyString();
}

std::size_t JsonValue::Size() const {
    if (type_ == Type::Array) return items_.size();
    if (type_ == Type::Object) return members_.size();
    return 0;
}

const std::vector<JsonValue>& JsonValue::Items() const {
    return type_ == Type::Array ? items_ : EmptyItems();
}

const std::vector<std::pair<std::string, JsonValue>>& JsonValue::Members() const {
    return type_ == Type::Object ? members_ : EmptyMembers();
}

const JsonValue& JsonValue::operator[](std::size_t index) const {
    if (type_ == Type::Array && index < items_.size()) return items_[index];
    return NullValue();
}

const JsonValue& JsonValue::operator[](std::string_view key) const {
    if (type_ == Type::Object) {
        for (const auto& member : members_) {
            if (member.first == key) return member.second;
        }
    }
    return NullValue();
}

bool JsonValue::Has(std::string_view key) const {
    if (type_ != Type::Object) return false;
    for (const auto& member : members_) {
        if (member.first == key) return true;
    }
    return false;
}

JsonValue& JsonValue::Set(std::string key, JsonValue value) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        bool_ = false;
        number_ = 0.0;
        string_.clear();
        items_.clear();
        members_.clear();
    }
    for (auto& member : members_) {
        if (member.first == key) {
            member.second = std::move(value);
            return *this;
        }
    }
    members_.emplace_back(std::move(key), std::move(value));
    return *this;
}

JsonValue& JsonValue::Push(JsonValue value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        bool_ = false;
        number_ = 0.0;
        string_.clear();
        items_.clear();
        members_.clear();
    }
    items_.push_back(std::move(value));
    return *this;
}

std::string JsonValue::Dump() const {
    switch (type_) {
        case Type::Null:
            return "null";
        case Type::Bool:
            return bool_ ? "true" : "false";
        case Type::Number:
            return FormatNumber(number_);
        case Type::String:
            return EscapeString(string_);
        case Type::Array: {
            std::string out = "[";
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (i != 0) out.push_back(',');
                out += items_[i].Dump();
            }
            out.push_back(']');
            return out;
        }
        case Type::Object: {
            std::string out = "{";
            for (std::size_t i = 0; i < members_.size(); ++i) {
                if (i != 0) out.push_back(',');
                out += EscapeString(members_[i].first);
                out.push_back(':');
                out += members_[i].second.Dump();
            }
            out.push_back('}');
            return out;
        }
    }
    return "null";
}

// --- Parsing and UTF conversion -----------------------------------------

bool ParseJson(std::string_view utf8, JsonValue* out, std::string* error) {
    if (!out) return false;
    Parser parser(utf8);
    JsonValue value;
    if (!parser.ParseValue(value) || !parser.Finish()) {
        if (error) *error = parser.error();
        return false;
    }
    *out = std::move(value);
    return true;
}

std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    if (text.size() > (std::size_t)INT32_MAX) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return {};
    std::string out((std::size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), size, nullptr,
                        nullptr);
    return out;
}

std::wstring ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    if (utf8.size() > (std::size_t)INT32_MAX) return {};
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out((std::size_t)size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), size);
    return out;
}

} // namespace assistant
