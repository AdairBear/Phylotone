#include "phylo/assistant/Json.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace phylo::json
{

const Value* Value::get(const std::string& key) const
{
    if (type != Type::Object)
        return nullptr;
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key)
            return &values[i];
    return nullptr;
}

namespace
{

class Parser
{
public:
    explicit Parser(const std::string& t) : text(t) {}

    bool parseDocument(Value& out, std::string& error)
    {
        if (!parseValue(out, 0))
        {
            error = err;
            return false;
        }
        skipSpace();
        if (pos != text.size())
        {
            error = "trailing characters after JSON value";
            return false;
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 64;

    const std::string& text;
    std::size_t pos = 0;
    std::string err;

    bool fail(const char* msg)
    {
        if (err.empty())
            err = msg;
        return false;
    }

    void skipSpace()
    {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
            ++pos;
    }

    bool literal(const char* word)
    {
        const std::size_t n = std::char_traits<char>::length(word);
        if (text.compare(pos, n, word) == 0)
        {
            pos += n;
            return true;
        }
        return false;
    }

    bool parseValue(Value& out, int depth)
    {
        if (depth > kMaxDepth)
            return fail("JSON nested too deeply");
        skipSpace();
        if (pos >= text.size())
            return fail("unexpected end of JSON");

        const char c = text[pos];
        if (c == '{')
            return parseObject(out, depth);
        if (c == '[')
            return parseArray(out, depth);
        if (c == '"')
        {
            out.type = Value::Type::String;
            return parseString(out.str);
        }
        if (literal("true"))
        {
            out.type = Value::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (literal("false"))
        {
            out.type = Value::Type::Bool;
            out.boolean = false;
            return true;
        }
        if (literal("null"))
        {
            out.type = Value::Type::Null;
            return true;
        }
        return parseNumber(out);
    }

    bool parseObject(Value& out, int depth)
    {
        out.type = Value::Type::Object;
        ++pos; // {
        skipSpace();
        if (pos < text.size() && text[pos] == '}')
        {
            ++pos;
            return true;
        }
        while (true)
        {
            skipSpace();
            std::string key;
            if (pos >= text.size() || text[pos] != '"' || !parseString(key))
                return fail("expected object key");
            skipSpace();
            if (pos >= text.size() || text[pos] != ':')
                return fail("expected ':' after key");
            ++pos;
            Value v;
            if (!parseValue(v, depth + 1))
                return false;
            out.keys.push_back(std::move(key));
            out.values.push_back(std::move(v));
            skipSpace();
            if (pos < text.size() && text[pos] == ',')
            {
                ++pos;
                continue;
            }
            if (pos < text.size() && text[pos] == '}')
            {
                ++pos;
                return true;
            }
            return fail("expected ',' or '}' in object");
        }
    }

    bool parseArray(Value& out, int depth)
    {
        out.type = Value::Type::Array;
        ++pos; // [
        skipSpace();
        if (pos < text.size() && text[pos] == ']')
        {
            ++pos;
            return true;
        }
        while (true)
        {
            Value v;
            if (!parseValue(v, depth + 1))
                return false;
            out.items.push_back(std::move(v));
            skipSpace();
            if (pos < text.size() && text[pos] == ',')
            {
                ++pos;
                continue;
            }
            if (pos < text.size() && text[pos] == ']')
            {
                ++pos;
                return true;
            }
            return fail("expected ',' or ']' in array");
        }
    }

    static void appendUtf8(std::string& s, unsigned cp)
    {
        if (cp < 0x80)
            s.push_back(static_cast<char>(cp));
        else if (cp < 0x800)
        {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseHex4(unsigned& cp)
    {
        if (pos + 4 > text.size())
            return false;
        cp = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char h = text[pos + static_cast<std::size_t>(i)];
            cp <<= 4;
            if (h >= '0' && h <= '9')
                cp |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f')
                cp |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F')
                cp |= static_cast<unsigned>(h - 'A' + 10);
            else
                return false;
        }
        pos += 4;
        return true;
    }

    bool parseString(std::string& out)
    {
        ++pos; // opening quote
        while (pos < text.size())
        {
            const char c = text[pos++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return fail("control character in string");
            if (c != '\\')
            {
                out.push_back(c);
                continue;
            }
            if (pos >= text.size())
                break;
            const char e = text[pos++];
            switch (e)
            {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
            {
                unsigned cp = 0;
                if (!parseHex4(cp))
                    return fail("bad \\u escape");
                if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                    unsigned lo = 0;
                    if (pos + 2 <= text.size() && text[pos] == '\\' && text[pos + 1] == 'u')
                    {
                        pos += 2;
                        if (!parseHex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
                            return fail("bad surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    else
                        return fail("lone surrogate");
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                return fail("bad escape in string");
            }
        }
        return fail("unterminated string");
    }

    bool parseNumber(Value& out)
    {
        const std::size_t start = pos;
        if (pos < text.size() && text[pos] == '-')
            ++pos;
        if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos])))
            return fail("invalid value");
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
            ++pos;
        if (pos < text.size() && text[pos] == '.')
        {
            ++pos;
            if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos])))
                return fail("invalid number");
            while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
                ++pos;
        }
        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E'))
        {
            ++pos;
            if (pos < text.size() && (text[pos] == '+' || text[pos] == '-'))
                ++pos;
            if (pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[pos])))
                return fail("invalid number");
            while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos])))
                ++pos;
        }
        const double v = std::strtod(text.substr(start, pos - start).c_str(), nullptr);
        if (!std::isfinite(v))
            return fail("number out of range");
        out.type = Value::Type::Number;
        out.number = v;
        return true;
    }
};

} // namespace

bool parse(const std::string& text, Value& out, std::string& error)
{
    out = Value{};
    Parser p(text);
    return p.parseDocument(out, error);
}

std::string quote(const std::string& s)
{
    std::string out = "\"";
    for (const char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[(c >> 4) & 0xF]);
                out.push_back(hex[c & 0xF]);
            }
            else
            {
                out.push_back(c);
            }
        }
    }
    out += "\"";
    return out;
}

} // namespace phylo::json
