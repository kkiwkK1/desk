#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace jsonlite {
namespace {

constexpr int kMaxDepth = 64;

void AppendUtf8(std::string& out, unsigned cp) {
    if (cp <= 0x7F) {
        out.push_back((char)cp);
    } else if (cp <= 0x7FF) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool Document(Value& out) {
        SkipWs();
        if (!ParseValue(out, 0)) return false;
        SkipWs();
        if (i_ != s_.size()) return Fail("trailing data");
        return true;
    }

    const std::string& error() const { return error_; }

private:
    bool Fail(const char* what) {
        if (error_.empty()) {
            error_ = std::string(what) + " at byte " + std::to_string(i_);
        }
        return false;
    }

    void SkipWs() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++i_;
            else break;
        }
    }

    bool ParseValue(Value& out, int depth) {
        if (depth > kMaxDepth) return Fail("nesting too deep");
        if (i_ >= s_.size()) return Fail("unexpected end");

        switch (s_[i_]) {
        case '{': return ParseObject(out, depth);
        case '[': return ParseArray(out, depth);
        case '"': {
            std::string text;
            if (!ParseString(text)) return false;
            out = Value(std::move(text));
            return true;
        }
        case 't':
            if (s_.compare(i_, 4, "true") == 0) { i_ += 4; out = Value(true); return true; }
            return Fail("bad literal");
        case 'f':
            if (s_.compare(i_, 5, "false") == 0) { i_ += 5; out = Value(false); return true; }
            return Fail("bad literal");
        case 'n':
            if (s_.compare(i_, 4, "null") == 0) { i_ += 4; out = Value(); return true; }
            return Fail("bad literal");
        default:
            return ParseNumber(out);
        }
    }

    bool ParseNumber(Value& out) {
        const char* begin = s_.c_str() + i_;
        char* end = nullptr;
        const double v = std::strtod(begin, &end);
        if (end == begin) return Fail("expected value");
        // strtod accepts "nan"/"inf"/"+1"; JSON does not.
        if (!std::isfinite(v) || *begin == '+' || *begin == '.') return Fail("bad number");
        i_ += (std::size_t)(end - begin);
        out = Value(v);
        return true;
    }

    bool ParseString(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return Fail("expected string");
        ++i_;
        while (i_ < s_.size()) {
            const unsigned char c = (unsigned char)s_[i_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (i_ >= s_.size()) return Fail("bad escape");
                const char e = s_[i_++];
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!ParseHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (i_ + 1 >= s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u') {
                            return Fail("lone surrogate");
                        }
                        i_ += 2;
                        unsigned low = 0;
                        if (!ParseHex4(low)) return false;
                        if (low < 0xDC00 || low > 0xDFFF) return Fail("bad surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return Fail("lone surrogate");
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return Fail("bad escape");
                }
            } else if (c < 0x20) {
                return Fail("control character in string");
            } else {
                out.push_back((char)c);
            }
        }
        return Fail("unterminated string");
    }

    bool ParseHex4(unsigned& cp) {
        if (i_ + 4 > s_.size()) return Fail("short \\u escape");
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const int d = HexDigit(s_[i_ + k]);
            if (d < 0) return Fail("bad \\u escape");
            cp = (cp << 4) | (unsigned)d;
        }
        i_ += 4;
        return true;
    }

    bool ParseArray(Value& out, int depth) {
        ++i_;  // '['
        out = Value::MakeArray();
        SkipWs();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
        for (;;) {
            Value item;
            if (!ParseValue(item, depth + 1)) return false;
            out.Push(std::move(item));
            SkipWs();
            if (i_ >= s_.size()) return Fail("unterminated array");
            if (s_[i_] == ',') { ++i_; SkipWs(); continue; }
            if (s_[i_] == ']') { ++i_; return true; }
            return Fail("expected , or ]");
        }
    }

    bool ParseObject(Value& out, int depth) {
        ++i_;  // '{'
        out = Value::MakeObject();
        SkipWs();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
        for (;;) {
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (i_ >= s_.size() || s_[i_] != ':') return Fail("expected :");
            ++i_;
            SkipWs();
            Value value;
            if (!ParseValue(value, depth + 1)) return false;
            out.Set(key, std::move(value));
            SkipWs();
            if (i_ >= s_.size()) return Fail("unterminated object");
            if (s_[i_] == ',') { ++i_; SkipWs(); continue; }
            if (s_[i_] == '}') { ++i_; return true; }
            return Fail("expected , or }");
        }
    }

    const std::string& s_;
    std::size_t i_ = 0;
    std::string error_;
};

void WriteString(const std::string& s, std::string& out) {
    out.push_back('"');
    for (const unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                out += buf;
            } else {
                out.push_back((char)c);
            }
        }
    }
    out.push_back('"');
}

} // namespace

Value Value::MakeArray() {
    Value v;
    v.type_ = Type::Array;
    return v;
}

Value Value::MakeObject() {
    Value v;
    v.type_ = Type::Object;
    return v;
}

bool Value::AsBool(bool fallback) const { return type_ == Type::Bool ? bool_ : fallback; }
double Value::AsNumber(double fallback) const { return type_ == Type::Number ? num_ : fallback; }
long long Value::AsInt(long long fallback) const {
    return type_ == Type::Number ? (long long)num_ : fallback;
}

std::string Value::StringOr(const std::string& fallback) const {
    return type_ == Type::String ? str_ : fallback;
}

const Value* Value::Get(const std::string& key) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& kv : obj_) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

const Value* Value::At(std::size_t index) const {
    if (type_ != Type::Array || index >= arr_.size()) return nullptr;
    return &arr_[index];
}

std::size_t Value::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    if (type_ == Type::String) return str_.size();
    return 0;
}

void Value::Set(const std::string& key, Value value) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        arr_.clear();
    }
    for (auto& kv : obj_) {
        if (kv.first == key) {
            kv.second = std::move(value);
            return;
        }
    }
    obj_.emplace_back(key, std::move(value));
}

void Value::Push(Value value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        obj_.clear();
    }
    arr_.push_back(std::move(value));
}

std::string Value::Dump() const {
    switch (type_) {
    case Type::Null: return "null";
    case Type::Bool: return bool_ ? "true" : "false";
    case Type::Number: {
        if (!std::isfinite(num_)) return "null";
        if (num_ == std::floor(num_) && std::fabs(num_) < 1e15) {
            return std::to_string((long long)num_);
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.17g", num_);
        return buf;
    }
    case Type::String: {
        std::string out;
        WriteString(str_, out);
        return out;
    }
    case Type::Array: {
        std::string out = "[";
        for (std::size_t i = 0; i < arr_.size(); ++i) {
            if (i) out.push_back(',');
            out += arr_[i].Dump();
        }
        out.push_back(']');
        return out;
    }
    case Type::Object: {
        std::string out = "{";
        for (std::size_t i = 0; i < obj_.size(); ++i) {
            if (i) out.push_back(',');
            WriteString(obj_[i].first, out);
            out.push_back(':');
            out += obj_[i].second.Dump();
        }
        out.push_back('}');
        return out;
    }
    }
    return "null";
}

bool Parse(const std::string& text, Value& out, std::string* error) {
    Parser parser(text);
    if (!parser.Document(out)) {
        if (error) *error = parser.error();
        return false;
    }
    return true;
}

} // namespace jsonlite
