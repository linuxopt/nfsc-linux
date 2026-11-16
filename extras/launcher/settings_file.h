// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
//
// The game's settings.toml, read and written as the game writes it: each
// option with the comments above it ("# Label: what it does", then its type:
// "True or false.", "One of "a", "b".", "A whole number from 0 to 100.",
// "A key name such as ..."). The launcher builds its menus from those, so
// they follow the game's own options. Values change in place; every comment
// and unknown line stays as it was.
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace launcher
{
    enum class Kind { Bool, Choice, Int, Float, Key, Other };

    struct Option
    {
        std::string section, key;
        std::string label, description;
        Kind kind = Kind::Other;
        std::vector<std::string> choices;  // Choice
        bool alsoNumber = false;           // Choice: "or a number from A to B"
        double min = 0, max = 0;
        bool nextLaunch = false;           // "applies at the next launch"
        std::string value;                 // as written: true, 0.3, "auto"
        size_t line = 0;                   // the value's line in `lines`
    };

    class SettingsFile
    {
    public:
        bool Load(const std::filesystem::path& path, std::string& error);
        bool Save(std::string& error);
        bool Loaded() const { return !m_path.empty(); }
        bool Dirty() const { return m_dirty; }
        const std::filesystem::path& Path() const { return m_path; }

        std::vector<Option>& Options() { return m_options; }
        Option* Find(const std::string& section, const std::string& key);

        // The value without quotes ("auto", not "\"auto\"").
        static std::string Text(const Option& o);
        void SetText(Option& o, const std::string& text);  // quotes as the kind needs
        void SetBool(Option& o, bool v);
        void SetNumber(Option& o, double v);
        bool Set(const std::string& section, const std::string& key, const std::string& text);

    private:
        std::filesystem::path m_path;
        std::vector<std::string> m_lines;
        std::vector<Option> m_options;
        bool m_dirty = false;
    };
}
