// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
#include "settings_file.h"

#include <cstdio>
#include <format>
#include <fstream>
#include <sstream>

namespace launcher
{
    namespace
    {
        std::string Trim(std::string s)
        {
            size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? "" : s.substr(a, b - a + 1);
        }

        bool Range(const std::string& text, double& lo, double& hi)
        {
            size_t at = text.find("from ");
            if (at == std::string::npos)
                return false;
            return sscanf(text.c_str() + at, "from %lf to %lf", &lo, &hi) == 2;
        }

        void ParseType(Option& o, const std::string& type)
        {
            if (type.find("applies at the next launch") != std::string::npos)
                o.nextLaunch = true;
            if (type.rfind("True or false", 0) == 0)
                o.kind = Kind::Bool;
            else if (type.rfind("One of ", 0) == 0)
            {
                o.kind = Kind::Choice;
                for (size_t p = type.find('"'); p != std::string::npos;)
                {
                    size_t q = type.find('"', p + 1);
                    if (q == std::string::npos)
                        break;
                    o.choices.push_back(type.substr(p + 1, q - p - 1));
                    p = type.find('"', q + 1);
                }
                o.alsoNumber = type.find("or a number from") != std::string::npos && Range(type, o.min, o.max);
            }
            else if (type.rfind("A whole number", 0) == 0)
            {
                o.kind = Kind::Int;
                Range(type, o.min, o.max);
            }
            else if (type.rfind("A number", 0) == 0)
            {
                o.kind = Kind::Float;
                Range(type, o.min, o.max);
            }
            else if (type.rfind("A key name", 0) == 0)
                o.kind = Kind::Key;
        }
    }

    bool SettingsFile::Load(const std::filesystem::path& path, std::string& error)
    {
        std::ifstream f(path);
        if (!f)
        {
            error = std::format("can't read {}", path.string());
            return false;
        }
        m_lines.clear();
        m_options.clear();
        for (std::string line; std::getline(f, line);)
            m_lines.push_back(line);
        std::string section;
        std::vector<std::string> comments;
        for (size_t i = 0; i < m_lines.size(); i++)
        {
            std::string t = Trim(m_lines[i]);
            if (t.empty())
            {
                comments.clear();
                continue;
            }
            if (t[0] == '#')
            {
                comments.push_back(Trim(t.substr(1)));
                continue;
            }
            if (t[0] == '[')
            {
                section = t.substr(1, t.find(']') - 1);
                comments.clear();
                continue;
            }
            size_t eq = t.find('=');
            if (eq == std::string::npos)
                continue;
            Option o;
            o.section = section;
            o.key = Trim(t.substr(0, eq));
            o.value = Trim(t.substr(eq + 1));
            o.line = i;
            o.label = o.key;
            if (!comments.empty())
            {
                const std::string& first = comments[0];
                size_t colon = first.find(": ");
                if (colon != std::string::npos)
                {
                    o.label = first.substr(0, colon);
                    o.description = first.substr(colon + 2);
                }
                else
                    o.description = first;
                if (comments.size() > 1)
                    ParseType(o, comments[1]);
            }
            m_options.push_back(std::move(o));
            comments.clear();
        }
        m_path = path;
        m_dirty = false;
        return true;
    }

    bool SettingsFile::Save(std::string& error)
    {
        if (m_path.empty())
        {
            error = "no settings file";
            return false;
        }
        std::filesystem::path tmp = m_path;
        tmp += ".launcher-tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            for (const std::string& line : m_lines)
                f << line << '\n';
            if (!f)
            {
                error = std::format("can't write {}", tmp.string());
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, m_path, ec);
        if (ec)
        {
            error = ec.message();
            return false;
        }
        m_dirty = false;
        return true;
    }

    Option* SettingsFile::Find(const std::string& section, const std::string& key)
    {
        for (Option& o : m_options)
            if (o.section == section && o.key == key)
                return &o;
        return nullptr;
    }

    std::string SettingsFile::Text(const Option& o)
    {
        const std::string& v = o.value;
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            return v.substr(1, v.size() - 2);
        return v;
    }

    void SettingsFile::SetText(Option& o, const std::string& text)
    {
        bool quote = o.kind == Kind::Key || (o.kind == Kind::Choice) || (o.kind == Kind::Other && !o.value.empty() && o.value[0] == '"');
        // A choice that is also a number ("16") stays quoted when it is one of the named choices.
        std::string v = quote ? "\"" + text + "\"" : text;
        if (v == o.value)
            return;
        o.value = v;
        std::string& line = m_lines[o.line];
        size_t eq = line.find('=');
        line = line.substr(0, eq + 1) + " " + v;
        m_dirty = true;
    }

    void SettingsFile::SetBool(Option& o, bool v)
    {
        SetText(o, v ? "true" : "false");
    }

    void SettingsFile::SetNumber(Option& o, double v)
    {
        if (o.kind == Kind::Int || (o.kind == Kind::Choice && o.alsoNumber))
            SetText(o, std::format("{}", int64_t(v)));
        else
        {
            std::string s = std::format("{:.2f}", v);
            while (s.size() > 3 && s.back() == '0') s.pop_back();
            SetText(o, s);
        }
    }

    bool SettingsFile::Set(const std::string& section, const std::string& key, const std::string& text)
    {
        Option* o = Find(section, key);
        if (!o)
            return false;
        if (o->kind == Kind::Bool)
            SetBool(*o, text == "true");
        else
            SetText(*o, text);
        return true;
    }
}
