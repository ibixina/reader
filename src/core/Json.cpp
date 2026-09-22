#include "core/Json.h"
#include <cctype>
#include <iomanip>
#include <sstream>

namespace reader::json {

namespace {
struct Parser {
    const char* p;
    const char* end;
    [[noreturn]] void fail(const std::string& m) { throw ParseError("JSON: " + m); }
    void ws() {
        while (p != end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    char peek() {
        if (p == end) fail("unexpected end");
        return *p;
    }
    void expect(char c) {
        if (p == end || *p != c) fail(std::string("expected '") + c + "'");
        ++p;
    }
    bool consume(const char* word) {
        const char* q = p;
        for (const char* w = word; *w; ++w, ++q)
            if (q == end || *q != *w) return false;
        p = q;
        return true;
    }
    Value value() {
        ws();
        char c = peek();
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') return Value(string());
        if (c == 't') {
            if (!consume("true")) fail("bad literal");
            return Value(true);
        }
        if (c == 'f') {
            if (!consume("false")) fail("bad literal");
            return Value(false);
        }
        if (c == 'n') {
            if (!consume("null")) fail("bad literal");
            return {};
        }
        return Value(number());
    }
    Value object() {
        expect('{');
        Object o;
        ws();
        if (peek() == '}') { ++p; return Value(std::move(o)); }
        for (;;) {
            ws();
            if (peek() != '"') fail("expected key");
            std::string k = string();
            ws();
            expect(':');
            o.emplace(std::move(k), value());
            ws();
            char c = peek();
            if (c == ',') { ++p; continue; }
            if (c == '}') { ++p; break; }
            fail("expected ',' or '}'");
        }
        return Value(std::move(o));
    }
    Value array() {
        expect('[');
        Array a;
        ws();
        if (peek() == ']') { ++p; return Value(std::move(a)); }
        for (;;) {
            a.push_back(value());
            ws();
            char c = peek();
            if (c == ',') { ++p; continue; }
            if (c == ']') { ++p; break; }
            fail("expected ',' or ']'");
        }
        return Value(std::move(a));
    }
    std::string string() {
        expect('"');
        std::string out;
        const auto readHex = [&]() {
            if (end - p < 4) fail("bad \\u");
            unsigned cp = 0;
            for (int i = 0; i < 4; ++i) {
                const char h = *p++;
                cp <<= 4;
                if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                else fail("bad hex");
            }
            return cp;
        };
        const auto appendUtf8 = [&](unsigned cp) {
            if (cp < 0x80) out += static_cast<char>(cp);
            else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        };
        while (p != end && *p != '"') {
            char c = *p++;
            if (c == '\\') {
                if (p == end) fail("bad escape");
                char e = *p++;
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        unsigned cp = readHex();
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (end - p < 6 || p[0] != '\\' || p[1] != 'u')
                                fail("high surrogate without low surrogate");
                            p += 2;
                            const unsigned low = readHex();
                            if (low < 0xDC00 || low > 0xDFFF)
                                fail("invalid low surrogate");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            fail("unpaired low surrogate");
                        }
                        appendUtf8(cp);
                        break;
                    }
                    default: fail("bad escape");
                }
            } else {
                if (static_cast<unsigned char>(c) < 0x20) fail("unescaped control character");
                out += c;
            }
        }
        expect('"');
        return out;
    }
    double number() {
        const char* start = p;
        if (p != end && *p == '-') ++p;

        const char* integerStart = p;
        while (p != end && std::isdigit(static_cast<unsigned char>(*p))) ++p;
        if (p == integerStart) fail("expected number");
        if (*integerStart == '0' && p - integerStart > 1)
            fail("leading zero in number");

        if (p != end && *p == '.') {
            ++p;
            const char* fractionStart = p;
            while (p != end && std::isdigit(static_cast<unsigned char>(*p))) ++p;
            if (p == fractionStart) fail("expected digits after decimal point");
        }

        if (p != end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p != end && (*p == '+' || *p == '-')) ++p;
            const char* exponentStart = p;
            while (p != end && std::isdigit(static_cast<unsigned char>(*p))) ++p;
            if (p == exponentStart) fail("expected exponent digits");
        }

        try {
            std::size_t consumed = 0;
            const std::string token(start, p);
            const double value = std::stod(token, &consumed);
            if (consumed != token.size()) fail("invalid number");
            return value;
        } catch (const std::invalid_argument&) {
            fail("invalid number");
        } catch (const std::out_of_range&) {
            fail("number out of range");
        }
    }
};

void writeTo(std::ostringstream& os, const Value& v) {
    if (v.isNull()) { os << "null"; return; }
    if (v.isBool()) { os << (v.asBool() ? "true" : "false"); return; }
    if (v.isNumber()) {
        os << v.asNumber();
        return;
    }
    if (v.isString()) {
        os << '"';
        for (char c : v.asString()) {
            switch (c) {
                case '"': os << "\\\""; break;
                case '\\': os << "\\\\"; break;
                case '\b': os << "\\b"; break;
                case '\f': os << "\\f"; break;
                case '\n': os << "\\n"; break;
                case '\r': os << "\\r"; break;
                case '\t': os << "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        os << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<unsigned>(static_cast<unsigned char>(c)) << std::dec
                           << std::setfill(' ');
                    } else {
                        os << c;
                    }
            }
        }
        os << '"';
        return;
    }
    if (v.isArray()) {
        os << '[';
        bool first = true;
        for (const auto& e : v.asArray()) {
            if (!first) os << ',';
            first = false;
            writeTo(os, e);
        }
        os << ']';
        return;
    }
    os << '{';
    bool first = true;
    for (const auto& [k, e] : v.asObject()) {
        if (!first) os << ',';
        first = false;
        writeTo(os, Value(k));
        os << ':';
        writeTo(os, e);
    }
    os << '}';
}
} // namespace

Value parse(const std::string& text) {
    Parser p{text.data(), text.data() + text.size()};
    Value v = p.value();
    p.ws();
    if (p.p != p.end) throw ParseError("JSON: trailing content");
    return v;
}

std::string serialize(const Value& v) {
    std::ostringstream os;
    writeTo(os, v);
    return os.str();
}

namespace {
void writePretty(std::ostringstream& os, const Value& v, int indent) {
    std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
    std::string child(static_cast<std::size_t>(indent + 1) * 2, ' ');
    if (v.isArray()) {
        const auto& a = v.asArray();
        if (a.empty()) {
            os << "[]";
            return;
        }
        os << "[\n";
        for (std::size_t i = 0; i < a.size(); ++i) {
            os << child;
            writePretty(os, a[i], indent + 1);
            if (i + 1 < a.size()) os << ',';
            os << '\n';
        }
        os << pad << ']';
        return;
    }
    if (v.isObject()) {
        const auto& o = v.asObject();
        if (o.empty()) {
            os << "{}";
            return;
        }
        os << "{\n";
        std::size_t i = 0;
        for (const auto& [k, e] : o) {
            os << child;
            writeTo(os, Value(k));
            os << ": ";
            writePretty(os, e, indent + 1);
            if (++i < o.size()) os << ',';
            os << '\n';
        }
        os << pad << '}';
        return;
    }
    writeTo(os, v);
}
} // namespace

std::string pretty(const Value& v, int indent) {
    std::ostringstream os;
    writePretty(os, v, indent);
    return os.str();
}

} // namespace reader::json
