#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace reader::json {

struct Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value {
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
    Storage data = nullptr;

    Value() = default;
    Value(std::nullptr_t) : data(nullptr) {}
    Value(bool b) : data(b) {}
    Value(int i) : data(static_cast<double>(i)) {}
    Value(double d) : data(d) {}
    Value(const char* s) : data(std::string(s)) {}
    Value(std::string s) : data(std::move(s)) {}
    Value(Array a) : data(std::move(a)) {}
    Value(Object o) : data(std::move(o)) {}

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(data); }
    bool isBool() const { return std::holds_alternative<bool>(data); }
    bool isNumber() const { return std::holds_alternative<double>(data); }
    bool isString() const { return std::holds_alternative<std::string>(data); }
    bool isArray() const { return std::holds_alternative<Array>(data); }
    bool isObject() const { return std::holds_alternative<Object>(data); }

    bool asBool(bool d = false) const {
        return isBool() ? std::get<bool>(data) : d;
    }
    double asNumber(double d = 0) const {
        return isNumber() ? std::get<double>(data) : d;
    }
    const std::string& asString(const std::string& d = empty()) const {
        return isString() ? std::get<std::string>(data) : d;
    }
    const Array& asArray(const Array& d = emptyArr()) const {
        return isArray() ? std::get<Array>(data) : d;
    }
    const Object& asObject(const Object& d = emptyObj()) const {
        return isObject() ? std::get<Object>(data) : d;
    }
    Array& asArray() { return std::get<Array>(data); }
    Object& asObject() { return std::get<Object>(data); }

    const Value& at(const std::string& key) const {
        static const Value null;
        if (!isObject()) return null;
        auto it = std::get<Object>(data).find(key);
        return it == std::get<Object>(data).end() ? null : it->second;
    }
    Value& operator[](const std::string& key) {
        if (!isObject()) data = Object{};
        return std::get<Object>(data)[key];
    }

private:
    static const std::string& empty() {
        static const std::string s;
        return s;
    }
    static const Array& emptyArr() {
        static const Array a;
        return a;
    }
    static const Object& emptyObj() {
        static const Object o;
        return o;
    }
};

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

Value parse(const std::string& text);
std::string serialize(const Value& v);
// Human-readable indented form for the Ingest Raw tab.
std::string pretty(const Value& v, int indent = 0);

} // namespace reader::json
