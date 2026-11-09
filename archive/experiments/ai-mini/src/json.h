// Minimal JSON for the AI-mini experiment.
//
// The widgets in this repo deliberately avoid third-party code, so a chat API
// payload cannot pull in nlohmann/json (300 KB of header, exceptions, ICU-ish
// strings). This is the smallest thing that can build a request and read a
// DeepSeek/OpenAI-style response: one Value type, a recursive parser with a
// depth limit, and a writer that preserves key order. UTF-8 in, UTF-8 out.
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace jsonlite {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() = default;
    explicit Value(bool v) : type_(Type::Bool), bool_(v) {}
    explicit Value(double v) : type_(Type::Number), num_(v) {}
    explicit Value(int v) : type_(Type::Number), num_(v) {}
    explicit Value(long long v) : type_(Type::Number), num_((double)v) {}
    explicit Value(std::string v) : type_(Type::String), str_(std::move(v)) {}
    explicit Value(const char* v) : type_(Type::String), str_(v ? v : "") {}

    static Value MakeArray();
    static Value MakeObject();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool AsBool(bool fallback = false) const;
    double AsNumber(double fallback = 0.0) const;
    long long AsInt(long long fallback = 0) const;
    const std::string& AsString() const { return str_; }
    std::string StringOr(const std::string& fallback = std::string()) const;

    const Value* Get(const std::string& key) const;
    const Value* At(std::size_t index) const;
    std::size_t size() const;

    // Object: replaces an existing key, otherwise appends (order is kept).
    void Set(const std::string& key, Value value);
    void Push(Value value);

    std::string Dump() const;

private:
    friend bool Parse(const std::string&, Value&, std::string*);

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Value> arr_;
    std::vector<std::pair<std::string, Value>> obj_;
};

// Returns false and fills 'error' (if non-null) on malformed input.
bool Parse(const std::string& text, Value& out, std::string* error = nullptr);

} // namespace jsonlite
