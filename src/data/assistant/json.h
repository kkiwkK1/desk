// Minimal JSON for the assistant runtime (M1.0).
//
// pi speaks JSONL on stdin/stdout and the memory/context files are JSON lines,
// so the assistant needs one small, dependency-free JSON value type:
//   * strict, depth-limited recursive-descent parser (no third-party library);
//   * compact deterministic serializer (object members keep insertion order);
//   * UTF-8 <-> UTF-16 helpers - JSON stays UTF-8 on the wire, the UI is wide.
//
// Not a general-purpose number type: numbers are IEEE doubles plus an integer
// accessor; RPC ids and token counters fit comfortably.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace assistant {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue();  // null
    ~JsonValue();
    JsonValue(const JsonValue&);
    JsonValue(JsonValue&&) noexcept;
    JsonValue& operator=(const JsonValue&);
    JsonValue& operator=(JsonValue&&) noexcept;

    static JsonValue MakeNull();
    static JsonValue MakeBool(bool value);
    static JsonValue MakeNumber(double value);
    static JsonValue MakeInt(long long value);
    static JsonValue MakeString(std::string utf8);
    static JsonValue MakeArray();
    static JsonValue MakeObject();

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    bool Bool(bool fallback = false) const;
    double Number(double fallback = 0.0) const;
    long long Integer(long long fallback = 0) const;  // rounds numbers
    // Empty for non-strings (never throws).
    const std::string& String() const;

    // Arrays: item count. Objects: member count. Other types: 0.
    std::size_t Size() const;
    // Empty for non-arrays / non-objects.
    const std::vector<JsonValue>& Items() const;
    const std::vector<std::pair<std::string, JsonValue>>& Members() const;

    // Missing array index / object key returns a shared null value.
    const JsonValue& operator[](std::size_t index) const;
    const JsonValue& operator[](std::string_view key) const;
    bool Has(std::string_view key) const;

    // Builders. Set()/Push() convert the value to object/array if needed and
    // return *this so calls can be chained.
    JsonValue& Set(std::string key, JsonValue value);
    JsonValue& Push(JsonValue value);

    // Compact UTF-8 JSON text (no trailing newline).
    std::string Dump() const;

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<JsonValue> items_;
    std::vector<std::pair<std::string, JsonValue>> members_;
};

// Parses exactly one UTF-8 JSON value; trailing whitespace is allowed.
// On failure returns false, leaves *out untouched and fills *error (if given)
// with a short ASCII description including the byte offset. *error never
// contains document payload bytes.
bool ParseJson(std::string_view utf8, JsonValue* out, std::string* error = nullptr);

// UTF-8 <-> UTF-16 (invalid input is replaced, never throws).
std::string ToUtf8(std::wstring_view text);
std::wstring ToWide(std::string_view utf8);

} // namespace assistant
