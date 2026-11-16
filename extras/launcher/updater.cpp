// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
#include "updater.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <unistd.h>
#include <variant>

namespace launcher::update
{
    namespace
    {
        // A minimal JSON reader: enough for GitHub's answer, strict about syntax.
        struct Value;
        using Object = std::map<std::string, std::shared_ptr<Value>>;
        using Array = std::vector<std::shared_ptr<Value>>;
        struct Value
        {
            std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v;
        };

        struct Parser
        {
            const std::string& s;
            size_t i = 0;
            std::string error;
            int depth = 0;

            void Ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++; }
            bool Fail(const char* why) { if (error.empty()) error = std::string(why) + " at byte " + std::to_string(i); return false; }

            static void Utf8(std::string& out, uint32_t c)
            {
                if (c < 0x80) out += char(c);
                else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
                else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
                else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
            }
            bool Hex4(uint32_t& c)
            {
                if (i + 4 > s.size()) return Fail("cut-short escape");
                c = 0;
                for (int k = 0; k < 4; k++)
                {
                    char h = s[i++];
                    c <<= 4;
                    if (h >= '0' && h <= '9') c |= uint32_t(h - '0');
                    else if (h >= 'a' && h <= 'f') c |= uint32_t(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') c |= uint32_t(h - 'A' + 10);
                    else return Fail("bad escape");
                }
                return true;
            }
            bool String(std::string& out)
            {
                if (i >= s.size() || s[i] != '"') return Fail("expected a string");
                i++;
                while (i < s.size() && s[i] != '"')
                {
                    char c = s[i++];
                    if (uint8_t(c) < 0x20) return Fail("control character in a string");
                    if (c != '\\') { out += c; continue; }
                    if (i >= s.size()) return Fail("cut-short escape");
                    char e = s[i++];
                    switch (e)
                    {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                    {
                        uint32_t c1;
                        if (!Hex4(c1)) return false;
                        if (c1 >= 0xD800 && c1 < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u')
                        {
                            i += 2;
                            uint32_t c2;
                            if (!Hex4(c2)) return false;
                            c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                        }
                        Utf8(out, c1);
                        break;
                    }
                    default: return Fail("bad escape");
                    }
                }
                if (i >= s.size()) return Fail("unterminated string");
                i++;
                return true;
            }
            bool Parse(Value& out)
            {
                if (++depth > 64) return Fail("too deep");
                Ws();
                if (i >= s.size()) return Fail("unexpected end");
                char c = s[i];
                bool ok = true;
                if (c == '{')
                {
                    i++;
                    Object o;
                    Ws();
                    if (i < s.size() && s[i] == '}') i++;
                    else
                        for (;;)
                        {
                            Ws();
                            std::string key;
                            if (!String(key)) return false;
                            Ws();
                            if (i >= s.size() || s[i] != ':') return Fail("expected ':'");
                            i++;
                            auto v = std::make_shared<Value>();
                            if (!Parse(*v)) return false;
                            o[key] = v;
                            Ws();
                            if (i < s.size() && s[i] == ',') { i++; continue; }
                            if (i < s.size() && s[i] == '}') { i++; break; }
                            return Fail("expected ',' or '}'");
                        }
                    out.v = std::move(o);
                }
                else if (c == '[')
                {
                    i++;
                    Array a;
                    Ws();
                    if (i < s.size() && s[i] == ']') i++;
                    else
                        for (;;)
                        {
                            auto v = std::make_shared<Value>();
                            if (!Parse(*v)) return false;
                            a.push_back(v);
                            Ws();
                            if (i < s.size() && s[i] == ',') { i++; continue; }
                            if (i < s.size() && s[i] == ']') { i++; break; }
                            return Fail("expected ',' or ']'");
                        }
                    out.v = std::move(a);
                }
                else if (c == '"')
                {
                    std::string str;
                    ok = String(str);
                    out.v = std::move(str);
                }
                else if (s.compare(i, 4, "true") == 0) { i += 4; out.v = true; }
                else if (s.compare(i, 5, "false") == 0) { i += 5; out.v = false; }
                else if (s.compare(i, 4, "null") == 0) { i += 4; out.v = nullptr; }
                else if (c == '-' || (c >= '0' && c <= '9'))
                {
                    const char* begin = s.c_str() + i;
                    char* end = nullptr;
                    double d = std::strtod(begin, &end);
                    if (end == begin) return Fail("bad number");
                    i += size_t(end - begin);
                    out.v = d;
                }
                else
                    return Fail("unexpected character");
                depth--;
                return ok;
            }
        };

        const Value* Member(const Object& o, const char* key)
        {
            auto it = o.find(key);
            return it == o.end() ? nullptr : it->second.get();
        }
        std::string Str(const Object& o, const char* key)
        {
            const Value* v = Member(o, key);
            return v && std::holds_alternative<std::string>(v->v) ? std::get<std::string>(v->v) : std::string();
        }
        bool Bool(const Object& o, const char* key)
        {
            const Value* v = Member(o, key);
            return v && std::holds_alternative<bool>(v->v) && std::get<bool>(v->v);
        }

        struct Version
        {
            uint64_t n[3] = {};
            std::string pre;
        };
        bool ParseVersion(std::string t, Version& v)
        {
            if (!t.empty() && (t[0] == 'v' || t[0] == 'V'))
                t.erase(0, 1);
            if (size_t plus = t.find('+'); plus != std::string::npos)
                t.resize(plus);
            if (size_t dash = t.find('-'); dash != std::string::npos)
            {
                v.pre = t.substr(dash + 1);
                t.resize(dash);
                if (v.pre.empty())
                    return false;
            }
            size_t at = 0;
            for (int k = 0; k < 3; k++)
            {
                size_t end = t.find('.', at);
                if (k < 2 && end == std::string::npos)
                    return false;
                std::string part = t.substr(at, (k < 2 ? end : t.size()) - at);
                if (part.empty() || part.size() > 9 || part.find_first_not_of("0123456789") != std::string::npos)
                    return false;
                v.n[k] = std::stoull(part);
                at = end + 1;
            }
            return true;
        }
    }

    const Asset* Release::Find(const std::string& suffix) const
    {
        for (const Asset& a : assets)
            if (a.name.size() >= suffix.size() && a.name.compare(a.name.size() - suffix.size(), suffix.size(), suffix) == 0)
                return &a;
        return nullptr;
    }

    bool ParseRelease(const std::string& json, Release& out, std::string& error)
    {
        Parser p{ json };
        Value root;
        if (!p.Parse(root))
        {
            error = "GitHub's answer isn't JSON (" + p.error + ")";
            return false;
        }
        p.Ws();
        if (p.i != json.size() || !std::holds_alternative<Object>(root.v))
        {
            error = "GitHub's answer isn't a release";
            return false;
        }
        const Object& o = std::get<Object>(root.v);
        out = {};
        out.tag = Str(o, "tag_name");
        out.name = Str(o, "name");
        out.url = Str(o, "html_url");
        out.body = Str(o, "body");
        out.draft = Bool(o, "draft");
        out.prerelease = Bool(o, "prerelease");
        if (out.tag.empty())
        {
            std::string message = Str(o, "message");
            error = message.empty() ? "GitHub's answer has no release" : "GitHub says: " + message;
            return false;
        }
        if (const Value* assets = Member(o, "assets"); assets && std::holds_alternative<Array>(assets->v))
            for (const auto& a : std::get<Array>(assets->v))
            {
                if (!std::holds_alternative<Object>(a->v))
                    continue;
                const Object& ao = std::get<Object>(a->v);
                Asset asset{ Str(ao, "name"), Str(ao, "browser_download_url"), 0 };
                if (const Value* size = Member(ao, "size"); size && std::holds_alternative<double>(size->v))
                    asset.size = uint64_t(std::get<double>(size->v));
                if (!asset.name.empty() && asset.url.rfind("https://", 0) == 0)
                    out.assets.push_back(std::move(asset));
            }
        return true;
    }

    bool Valid(const std::string& version)
    {
        Version v;
        return ParseVersion(version, v);
    }

    int Compare(const std::string& a, const std::string& b)
    {
        Version x, y;
        ParseVersion(a, x);
        ParseVersion(b, y);
        for (int k = 0; k < 3; k++)
            if (x.n[k] != y.n[k])
                return x.n[k] < y.n[k] ? -1 : 1;
        if (x.pre == y.pre) return 0;
        if (x.pre.empty()) return 1;   // a release after its pre-releases
        if (y.pre.empty()) return -1;
        return x.pre < y.pre ? -1 : 1;
    }

    std::string FindCurl()
    {
        const char* path = std::getenv("PATH");
        std::string dirs = path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
        dirs += ":/usr/bin:/bin";
        size_t at = 0;
        while (at <= dirs.size())
        {
            size_t end = dirs.find(':', at);
            if (end == std::string::npos) end = dirs.size();
            std::string candidate = dirs.substr(at, end - at) + "/curl";
            if (end > at && access(candidate.c_str(), X_OK) == 0)
                return candidate;
            at = end + 1;
        }
        return {};
    }
}
