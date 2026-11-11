#include "remote_json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace openmt
{

static const Json gNull;

const Json& Json::operator[](const std::string& key) const
{
    if (type_ != Object)
        return gNull;
    std::map<std::string, Json>::const_iterator it = obj_.find(key);
    return it == obj_.end() ? gNull : it->second;
}

Json& Json::set(const std::string& key, const Json& value)
{
    if (type_ == Null)
        type_ = Object;
    if (type_ == Object)
        obj_[key] = value;
    return *this;
}

const Json& Json::at(size_t i) const
{
    return (type_ == Array && i < arr_.size()) ? arr_[i] : gNull;
}

Json& Json::push(const Json& value)
{
    if (type_ == Null)
        type_ = Array;
    if (type_ == Array)
        arr_.push_back(value);
    return *this;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Writing                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

static void quote(const std::string& s, std::string& out)
{
    out += '"';
    for (size_t i = 0U; i < s.size(); i++)
    {
        unsigned char c = (unsigned char)s[i];

        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20U)
                {
                    char buf[8];

                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned int)c);
                    out += buf;
                }
                else
                    out += (char)c;         /* UTF-8 bytes go through as they are */
                break;
        }
    }
    out += '"';
}

void Json::dumpTo(std::string& out) const
{
    switch (type_)
    {
        case Null:   out += "null"; break;
        case Bool:   out += bool_ ? "true" : "false"; break;
        case Number:
        {
            char buf[40];

            if (!std::isfinite(num_))
                out += "null";
            else if (num_ == std::floor(num_) && std::fabs(num_) < 9.0e15)
            {
                std::snprintf(buf, sizeof(buf), "%lld", (long long)num_);
                out += buf;
            }
            else
            {
                std::snprintf(buf, sizeof(buf), "%.10g", num_);
                out += buf;
            }
            break;
        }
        case String: quote(str_, out); break;
        case Array:
            out += '[';
            for (size_t i = 0U; i < arr_.size(); i++)
            {
                if (i > 0U)
                    out += ',';
                arr_[i].dumpTo(out);
            }
            out += ']';
            break;
        case Object:
        {
            bool first = true;

            out += '{';
            for (std::map<std::string, Json>::const_iterator it = obj_.begin(); it != obj_.end(); ++it)
            {
                if (!first)
                    out += ',';
                first = false;
                quote(it->first, out);
                out += ':';
                it->second.dumpTo(out);
            }
            out += '}';
            break;
        }
    }
}

std::string Json::dump() const
{
    std::string out;

    dumpTo(out);
    return out;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Parsing                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

namespace
{
    struct Parser
    {
        const std::string& s;
        size_t             pos;
        std::string        err;

        explicit Parser(const std::string& text) : s(text), pos(0U), err() {}

        bool fail(const char* what)
        {
            if (err.empty())
                err = std::string(what) + " at offset " + std::to_string((unsigned long long)pos);
            return false;
        }

        void skip()
        {
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
                pos++;
        }

        bool literal(const char* word)
        {
            size_t n = std::strlen(word);

            if (s.compare(pos, n, word) != 0)
                return fail("bad literal");
            pos += n;
            return true;
        }

        static void utf8(unsigned int cp, std::string& out)
        {
            if (cp < 0x80U)
                out += (char)cp;
            else if (cp < 0x800U)
            {
                out += (char)(0xC0U | (cp >> 6));
                out += (char)(0x80U | (cp & 0x3FU));
            }
            else if (cp < 0x10000U)
            {
                out += (char)(0xE0U | (cp >> 12));
                out += (char)(0x80U | ((cp >> 6) & 0x3FU));
                out += (char)(0x80U | (cp & 0x3FU));
            }
            else
            {
                out += (char)(0xF0U | (cp >> 18));
                out += (char)(0x80U | ((cp >> 12) & 0x3FU));
                out += (char)(0x80U | ((cp >> 6) & 0x3FU));
                out += (char)(0x80U | (cp & 0x3FU));
            }
        }

        bool hex4(unsigned int& v)
        {
            if (pos + 4U > s.size())
                return fail("short \\u escape");
            v = 0U;
            for (int i = 0; i < 4; i++)
            {
                char c = s[pos++];

                v <<= 4;
                if (c >= '0' && c <= '9')
                    v |= (unsigned int)(c - '0');
                else if (c >= 'a' && c <= 'f')
                    v |= (unsigned int)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')
                    v |= (unsigned int)(c - 'A' + 10);
                else
                    return fail("bad \\u escape");
            }
            return true;
        }

        bool str(std::string& out)
        {
            out.clear();
            if (pos >= s.size() || s[pos] != '"')
                return fail("string expected");
            pos++;
            while (pos < s.size())
            {
                unsigned char c = (unsigned char)s[pos++];

                if (c == '"')
                    return true;
                if (c < 0x20U)
                    return fail("control character in string");
                if (c != '\\')
                {
                    out += (char)c;
                    continue;
                }
                if (pos >= s.size())
                    break;
                char e = s[pos++];

                switch (e)
                {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u':
                    {
                        unsigned int cp = 0U;

                        if (!hex4(cp))
                            return false;
                        if (cp >= 0xD800U && cp < 0xDC00U)              /* a surrogate pair */
                        {
                            unsigned int lo = 0U;

                            if (pos + 2U > s.size() || s[pos] != '\\' || s[pos + 1U] != 'u')
                                return fail("lone surrogate");
                            pos += 2U;
                            if (!hex4(lo))
                                return false;
                            if (lo < 0xDC00U || lo > 0xDFFFU)
                                return fail("bad surrogate pair");
                            cp = 0x10000U + ((cp - 0xD800U) << 10) + (lo - 0xDC00U);
                        }
                        else if (cp >= 0xDC00U && cp <= 0xDFFFU)
                            return fail("lone surrogate");
                        utf8(cp, out);
                        break;
                    }
                    default:
                        return fail("bad escape");
                }
            }
            return fail("unterminated string");
        }

        bool value(Json& out, int depth)
        {
            if (depth > 16)
                return fail("nested too deeply");
            skip();
            if (pos >= s.size())
                return fail("value expected");

            char c = s[pos];

            if (c == '{')
            {
                pos++;
                out = Json::object();
                skip();
                if (pos < s.size() && s[pos] == '}')
                {
                    pos++;
                    return true;
                }
                for (;;)
                {
                    std::string key;
                    Json v;

                    skip();
                    if (!str(key))
                        return false;
                    skip();
                    if (pos >= s.size() || s[pos] != ':')
                        return fail("':' expected");
                    pos++;
                    if (!value(v, depth + 1))
                        return false;
                    out.set(key, v);
                    skip();
                    if (pos < s.size() && s[pos] == ',')
                    {
                        pos++;
                        continue;
                    }
                    if (pos < s.size() && s[pos] == '}')
                    {
                        pos++;
                        return true;
                    }
                    return fail("',' or '}' expected");
                }
            }
            if (c == '[')
            {
                pos++;
                out = Json::array();
                skip();
                if (pos < s.size() && s[pos] == ']')
                {
                    pos++;
                    return true;
                }
                for (;;)
                {
                    Json v;

                    if (!value(v, depth + 1))
                        return false;
                    out.push(v);
                    skip();
                    if (pos < s.size() && s[pos] == ',')
                    {
                        pos++;
                        continue;
                    }
                    if (pos < s.size() && s[pos] == ']')
                    {
                        pos++;
                        return true;
                    }
                    return fail("',' or ']' expected");
                }
            }
            if (c == '"')
            {
                std::string t;

                if (!str(t))
                    return false;
                out = Json(t);
                return true;
            }
            if (c == 't')
            {
                if (!literal("true"))
                    return false;
                out = Json(true);
                return true;
            }
            if (c == 'f')
            {
                if (!literal("false"))
                    return false;
                out = Json(false);
                return true;
            }
            if (c == 'n')
            {
                if (!literal("null"))
                    return false;
                out = Json();
                return true;
            }
            if (c == '-' || (c >= '0' && c <= '9'))
            {
                size_t start = pos;

                if (s[pos] == '-')
                    pos++;
                while (pos < s.size() && ((s[pos] >= '0' && s[pos] <= '9') || s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E' ||
                                          s[pos] == '+' || s[pos] == '-'))
                    pos++;

                std::string t = s.substr(start, pos - start);
                char* end = NULL;
                double d = std::strtod(t.c_str(), &end);

                if (t.empty() || end == NULL || *end != '\0' || !std::isfinite(d))
                    return fail("bad number");
                out = Json(d);
                return true;
            }
            return fail("unexpected character");
        }
    };
}

bool Json::parse(const std::string& text, Json& out, std::string& error)
{
    Parser p(text);
    Json v;

    if (!p.value(v, 0))
    {
        error = p.err;
        return false;
    }
    p.skip();
    if (p.pos != text.size())
    {
        p.fail("trailing characters");
        error = p.err;
        return false;
    }
    out = v;
    return true;
}

}
