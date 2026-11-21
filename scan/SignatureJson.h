#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// SignatureJson.h — minimal, dependency-free JSON parser.
//
// Pure C++17: no Qt, no Win32, no exceptions, no third-party libraries.
// Implements the RFC 8259 subset needed by SignatureDB::LoadFromFile:
// objects, arrays, strings (escapes, \uXXXX, surrogate pairs), numbers,
// booleans and null.  Deliberately strict — rejects trailing commas,
// trailing garbage, unescaped control characters, leading zeros, malformed
// numbers and excessive nesting.  Errors carry a message and a byte offset.
//
// Keeping this in scan/ (rather than reaching for QJsonDocument) is what lets
// custom signature loading work in non-GUI / headless builds.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace phoenix {

struct JsonValue
{
    enum class Type { Null, Bool, Number, String, Array, Object };

    using Member = std::pair<std::string, JsonValue>;

    Type                   type    = Type::Null;
    bool                   boolean = false;
    double                 number  = 0.0;
    std::string            string;
    std::vector<JsonValue> array;
    std::vector<Member>    object;

    bool IsObject() const { return type == Type::Object; }
    bool IsArray()  const { return type == Type::Array;  }
    bool IsString() const { return type == Type::String; }
    bool IsNumber() const { return type == Type::Number; }

    // First matching member wins (JSON duplicate keys are unspecified).
    const JsonValue* Find(const std::string& key) const
    {
        if (type != Type::Object) return nullptr;
        for (const Member& kv : object)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }

    std::string AsString(const std::string& def = "") const
    { return type == Type::String ? string : def; }

    double AsNumber(double def = 0.0) const
    { return type == Type::Number ? number : def; }

    int AsInt(int def = 0) const
    { return type == Type::Number ? static_cast<int>(number) : def; }
};

class JsonParser
{
public:
    explicit JsonParser(std::string text) : m_text(std::move(text)) {}

    bool Parse(JsonValue& out)
    {
        SkipWhitespace();
        if (!ParseValue(out, 0)) return false;
        SkipWhitespace();
        if (m_pos != m_text.size()) return Fail("trailing data after JSON value");
        return true;
    }

    const std::string& Error()    const { return m_error;    }
    size_t             ErrorPos() const { return m_errorPos; }

private:
    static constexpr int kMaxDepth = 64;

    bool Fail(const char* msg)
    {
        if (m_error.empty()) {
            m_error    = msg;
            m_errorPos = m_pos;
        }
        return false;
    }

    void SkipWhitespace()
    {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++m_pos;
            else break;
        }
    }

    bool Consume(char expected)
    {
        if (m_pos >= m_text.size() || m_text[m_pos] != expected) return false;
        ++m_pos;
        return true;
    }

    bool ParseLiteral(const char* lit, JsonValue& out,
                      JsonValue::Type type, bool boolean = false)
    {
        const size_t n = std::char_traits<char>::length(lit);
        if (m_text.compare(m_pos, n, lit) != 0) return Fail("invalid literal");
        m_pos     += n;
        out.type   = type;
        out.boolean = boolean;
        return true;
    }

    static int HexDigit(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    }

    bool ReadHex4(uint32_t& cp)
    {
        if (m_pos + 4 > m_text.size()) return Fail("truncated \\u escape");
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            const int d = HexDigit(m_text[m_pos++]);
            if (d < 0) return Fail("invalid \\u escape");
            cp = (cp << 4) | static_cast<uint32_t>(d);
        }
        return true;
    }

    static void AppendUtf8(std::string& out, uint32_t cp)
    {
        if (cp <= 0x7Fu) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FFu) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0xFFFFu) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }

    bool ParseString(std::string& out)
    {
        if (!Consume('"')) return Fail("expected string");
        out.clear();
        while (m_pos < m_text.size()) {
            const unsigned char c = static_cast<unsigned char>(m_text[m_pos]);
            if (c == '"') { ++m_pos; return true; }
            if (c < 0x20) return Fail("unescaped control character in string");
            if (c != '\\') { out.push_back(static_cast<char>(c)); ++m_pos; continue; }

            ++m_pos;  // consume backslash
            if (m_pos >= m_text.size()) return Fail("truncated escape");
            const char esc = m_text[m_pos++];
            switch (esc) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!ReadHex4(cp)) return false;
                    if (cp >= 0xD800u && cp <= 0xDBFFu) {
                        // High surrogate: require a following \u low surrogate.
                        if (m_pos + 2 > m_text.size() ||
                            m_text[m_pos] != '\\' || m_text[m_pos + 1] != 'u')
                            return Fail("high surrogate without low surrogate");
                        m_pos += 2;
                        uint32_t lo = 0;
                        if (!ReadHex4(lo)) return false;
                        if (lo < 0xDC00u || lo > 0xDFFFu)
                            return Fail("invalid low surrogate");
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                        return Fail("unexpected low surrogate");
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return Fail("unknown escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseNumber(JsonValue& out)
    {
        const size_t start = m_pos;
        if (m_pos < m_text.size() && m_text[m_pos] == '-') ++m_pos;
        if (m_pos >= m_text.size()) return Fail("truncated number");
        if (m_text[m_pos] == '0') {
            ++m_pos;
            if (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
                return Fail("leading zero in number");
        } else if (m_text[m_pos] >= '1' && m_text[m_pos] <= '9') {
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
                ++m_pos;
        } else {
            return Fail("invalid number");
        }
        if (m_pos < m_text.size() && m_text[m_pos] == '.') {
            ++m_pos;
            if (m_pos >= m_text.size() || m_text[m_pos] < '0' || m_text[m_pos] > '9')
                return Fail("invalid fraction");
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
                ++m_pos;
        }
        if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E')) {
            ++m_pos;
            if (m_pos < m_text.size() && (m_text[m_pos] == '+' || m_text[m_pos] == '-'))
                ++m_pos;
            if (m_pos >= m_text.size() || m_text[m_pos] < '0' || m_text[m_pos] > '9')
                return Fail("invalid exponent");
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
                ++m_pos;
        }
        const std::string token = m_text.substr(start, m_pos - start);
        out.type   = JsonValue::Type::Number;
        out.number = std::strtod(token.c_str(), nullptr);
        return true;
    }

    bool ParseObject(JsonValue& out, int depth)
    {
        if (depth > kMaxDepth) return Fail("nesting too deep");
        ++m_pos;  // '{'
        out.type = JsonValue::Type::Object;
        SkipWhitespace();
        if (m_pos < m_text.size() && m_text[m_pos] == '}') { ++m_pos; return true; }
        for (;;) {
            SkipWhitespace();
            if (m_pos >= m_text.size() || m_text[m_pos] != '"')
                return Fail("expected object key");
            std::string key;
            if (!ParseString(key)) return false;
            SkipWhitespace();
            if (!Consume(':')) return Fail("expected ':' after object key");
            SkipWhitespace();
            JsonValue value;
            if (!ParseValue(value, depth + 1)) return false;
            out.object.emplace_back(std::move(key), std::move(value));
            SkipWhitespace();
            if (m_pos >= m_text.size()) return Fail("unterminated object");
            const char c = m_text[m_pos++];
            if (c == '}') return true;
            if (c != ',') return Fail("expected ',' or '}' in object");
            // A comma must be followed by another key (no trailing comma).
        }
    }

    bool ParseArray(JsonValue& out, int depth)
    {
        if (depth > kMaxDepth) return Fail("nesting too deep");
        ++m_pos;  // '['
        out.type = JsonValue::Type::Array;
        SkipWhitespace();
        if (m_pos < m_text.size() && m_text[m_pos] == ']') { ++m_pos; return true; }
        for (;;) {
            SkipWhitespace();
            JsonValue value;
            if (!ParseValue(value, depth + 1)) return false;
            out.array.push_back(std::move(value));
            SkipWhitespace();
            if (m_pos >= m_text.size()) return Fail("unterminated array");
            const char c = m_text[m_pos++];
            if (c == ']') return true;
            if (c != ',') return Fail("expected ',' or ']' in array");
            SkipWhitespace();
            if (m_pos < m_text.size() && m_text[m_pos] == ']')
                return Fail("trailing comma in array");
        }
    }

    bool ParseValue(JsonValue& out, int depth)
    {
        if (depth > kMaxDepth) return Fail("nesting too deep");
        if (m_pos >= m_text.size()) return Fail("unexpected end of input");
        switch (m_text[m_pos]) {
            case '{': return ParseObject(out, depth);
            case '[': return ParseArray(out, depth);
            case '"':
                out.type = JsonValue::Type::String;
                return ParseString(out.string);
            case 't': return ParseLiteral("true",  out, JsonValue::Type::Bool,  true);
            case 'f': return ParseLiteral("false", out, JsonValue::Type::Bool,  false);
            case 'n': return ParseLiteral("null",  out, JsonValue::Type::Null);
            default:
                if (m_text[m_pos] == '-' ||
                    (m_text[m_pos] >= '0' && m_text[m_pos] <= '9'))
                    return ParseNumber(out);
                return Fail("unexpected character");
        }
    }

    std::string m_text;
    size_t      m_pos      = 0;
    std::string m_error;
    size_t      m_errorPos = 0;
};

} // namespace phoenix
