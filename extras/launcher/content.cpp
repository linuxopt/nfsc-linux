// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
#include "content.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <format>
#include <functional>
#include <fstream>
#include <map>
#include <sstream>

#include <sys/wait.h>
#include <unistd.h>

namespace launcher
{
    namespace
    {
        // XCONTENT_DATA (big-endian, as the guest wrote it): DeviceID,
        // dwContentType, szDisplayName[128] (UTF-16), szFileName[42].
        constexpr size_t kXContentSize = 4 + 4 + 256 + 42;

        uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
        uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
        uint32_t Le24(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }
        uint32_t Be24(const uint8_t* p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }

        std::string Utf16BeToUtf8(const uint8_t* p, size_t chars)
        {
            std::string out;
            for (size_t i = 0; i < chars; i++)
            {
                uint32_t c = Be16(p + i * 2);
                if (c == 0)
                    break;
                if (c < 0x80) out.push_back(char(c));
                else if (c < 0x800) { out.push_back(char(0xC0 | (c >> 6))); out.push_back(char(0x80 | (c & 0x3F))); }
                else { out.push_back(char(0xE0 | (c >> 12))); out.push_back(char(0x80 | ((c >> 6) & 0x3F))); out.push_back(char(0x80 | (c & 0x3F))); }
            }
            return out;
        }

        std::vector<uint8_t> MakeXContent(uint32_t type, const std::string& fileName, const std::string& displayName)
        {
            std::vector<uint8_t> d(kXContentSize, 0);
            d[3] = 1;  // DeviceID 1: the hard drive
            d[7] = uint8_t(type);
            for (size_t i = 0; i < displayName.size() && i < 127; i++)
                d[8 + i * 2 + 1] = uint8_t(displayName[i]);
            memcpy(&d[264], fileName.data(), std::min<size_t>(fileName.size(), 41));
            return d;
        }

        bool ReadXContent(const fs::path& p, std::string& displayName, uint32_t& type)
        {
            std::ifstream f(p, std::ios::binary);
            uint8_t d[kXContentSize];
            if (!f.read(reinterpret_cast<char*>(d), sizeof(d)))
                return false;
            type = Be32(d + 4);
            displayName = Utf16BeToUtf8(d + 8, 128);
            return true;
        }

        std::string Now()
        {
            std::time_t t = std::time(nullptr);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", std::localtime(&t));
            return buf;
        }

        std::string SafeName(std::string s)
        {
            for (char& c : s)
                if (c == '/' || c == '\\' || c == ':' || c == '\0' || (c == '.' && &c == &s[0]))
                    c = '_';
            if (s.size() > 41)
                s.resize(41);
            return s.empty() ? "content" : s;
        }

        bool Copy(const fs::path& from, const fs::path& to, std::string& error)
        {
            std::error_code ec;
            fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec)
                error = std::format("copying {}: {}", from.string(), ec.message());
            return !ec;
        }

        int RunTool(const std::vector<std::string>& argv)
        {
            pid_t pid = fork();
            if (pid == 0)
            {
                std::vector<char*> args;
                for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
                args.push_back(nullptr);
                execvp(args[0], args.data());
                _exit(127);
            }
            int status = 0;
            waitpid(pid, &status, 0);
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }

        std::map<std::string, std::string> ReadIni(const fs::path& p)
        {
            std::map<std::string, std::string> m;
            std::ifstream f(p);
            for (std::string line; std::getline(f, line);)
            {
                size_t eq = line.find('=');
                if (line.empty() || line[0] == '#' || line[0] == ';' || eq == std::string::npos)
                    continue;
                auto trim = [](std::string s) {
                    size_t a = s.find_first_not_of(" \t\r\""), b = s.find_last_not_of(" \t\r\"");
                    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
                };
                m[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
            }
            return m;
        }

        // ---- STFS (Xbox 360 CON/LIVE/PIRS packages) ----
        struct Stfs
        {
            std::vector<uint8_t> d;
            uint32_t firstHashTable = 0;
            int sex = 0;  // 0 female (read-only: one hash table per level), 1 male
            uint32_t blockSeparation = 0;

            uint32_t BackingDataBlock(uint32_t block) const
            {
                uint32_t r = (((block + 0xAA) / 0xAA) << sex) + block;
                if (block < 0xAA) return r;
                if (block < 0x70E4) return r + (((block + 0x70E4) / 0x70E4) << sex);
                return (1u << sex) + (r + (((block + 0x70E4) / 0x70E4) << sex));
            }
            size_t BlockOffset(uint32_t block) const { return size_t(BackingDataBlock(block)) * 0x1000 + firstHashTable; }

            uint32_t Level0HashBlock(uint32_t block) const
            {
                if (block < 0xAA) return 0;
                uint32_t step = sex ? 0xAC : 0xAB;
                uint32_t n = (block / 0xAA) * step;
                n += ((block / 0x70E4) + 1) << sex;
                if (block / 0x70E4 == 0) return n;
                return n + (1u << sex);
            }
            // The next block in a chain, from the level-0 hash table (the
            // first of a male package's pair, unless separation says else).
            bool NextBlock(uint32_t block, uint32_t& next) const
            {
                size_t at = size_t(Level0HashBlock(block)) * 0x1000 + firstHashTable + (block % 0xAA) * 0x18;
                if (sex == 1 && (blockSeparation & 2))
                    at += 0x1000;
                if (at + 0x18 > d.size()) return false;
                next = Be24(&d[at + 0x15]);
                return true;
            }
            bool ReadChain(uint32_t start, uint32_t blocks, bool consecutive, uint64_t size, std::vector<uint8_t>& out) const
            {
                out.clear();
                uint32_t b = start;
                for (uint32_t i = 0; i < blocks && out.size() < size; i++)
                {
                    size_t off = BlockOffset(b);
                    if (off + 0x1000 > d.size() && off + (size - out.size()) > d.size())
                        return false;
                    size_t n = std::min<size_t>(0x1000, size - out.size());
                    out.insert(out.end(), d.begin() + off, d.begin() + off + n);
                    if (consecutive) b++;
                    else if (!NextBlock(b, b)) return false;
                }
                return out.size() == size;
            }
        };
    }

    uint64_t FolderBytes(const fs::path& p)
    {
        uint64_t n = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec))
                n += it->file_size(ec);
        return n;
    }

    std::string FormatBytes(uint64_t b)
    {
        if (b >= (1ull << 30)) return std::format("{:.1f} GB", double(b) / double(1ull << 30));
        if (b >= (1ull << 20)) return std::format("{:.1f} MB", double(b) / double(1ull << 20));
        if (b >= 1024) return std::format("{:.0f} KB", double(b) / 1024.0);
        return std::format("{} B", b);
    }

    // ---- saves ----

    std::vector<SaveEntry> ListSaves(const fs::path& data)
    {
        std::vector<SaveEntry> out;
        std::error_code ec;
        fs::path dir = data / "save";
        for (const auto& e : fs::directory_iterator(dir, ec))
        {
            if (!e.is_directory(ec))
                continue;
            SaveEntry s;
            s.name = e.path().filename().string();
            uint32_t type;
            if (!ReadXContent(dir / (s.name + ".xcontent"), s.displayName, type) || s.displayName.empty())
                s.displayName = s.name;
            s.bytes = FolderBytes(e.path());
            s.modified = fs::last_write_time(e.path(), ec);
            for (auto it = fs::recursive_directory_iterator(e.path(), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec))
                {
                    s.files.push_back(it->path());
                    s.modified = std::max(s.modified, it->last_write_time(ec));
                }
            out.push_back(std::move(s));
        }
        std::sort(out.begin(), out.end(), [](const SaveEntry& a, const SaveEntry& b) { return a.modified > b.modified; });
        return out;
    }

    bool BackupSaves(const fs::path& data, fs::path& made, std::string& error)
    {
        fs::path from = data / "save";
        if (!fs::exists(from))
        {
            error = "there are no saves yet";
            return false;
        }
        made = data / "save-backups" / Now();
        std::error_code ec;
        fs::create_directories(made, ec);
        return Copy(from, made, error);
    }

    std::vector<fs::path> ListSaveBackups(const fs::path& data)
    {
        std::vector<fs::path> out;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(data / "save-backups", ec))
            if (e.is_directory(ec))
                out.push_back(e.path());
        std::sort(out.rbegin(), out.rend());
        return out;
    }

    bool RestoreSaves(const fs::path& data, const fs::path& backup, std::string& error)
    {
        fs::path made;
        std::error_code ec;
        if (fs::exists(data / "save") && !BackupSaves(data, made, error))
            return false;
        fs::path tmp = data / "save.restoring";
        fs::remove_all(tmp, ec);
        if (!Copy(backup, tmp, error))
            return false;
        fs::remove_all(data / "save", ec);
        fs::rename(tmp, data / "save", ec);
        if (ec)
            error = ec.message();
        return !ec;
    }

    bool ExportSave(const fs::path& data, const SaveEntry& save, const fs::path& toFolder, std::string& error)
    {
        fs::path dest = toFolder / std::format("NFSC-save-{}-{}", SafeName(save.displayName), Now());
        std::error_code ec;
        fs::create_directories(dest / save.name, ec);
        if (!Copy(data / "save" / save.name, dest / save.name, error))
            return false;
        fs::copy_file(data / "save" / (save.name + ".xcontent"), dest / (save.name + ".xcontent"), fs::copy_options::overwrite_existing, ec);
        return true;
    }

    bool ImportSave(const fs::path& data, const fs::path& from, std::string& error)
    {
        std::error_code ec;
        fs::create_directories(data / "save", ec);
        size_t imported = 0;
        for (const auto& e : fs::directory_iterator(from, ec))
        {
            if (e.path().extension() != ".xcontent")
                continue;
            std::string name = e.path().stem().string();
            if (!fs::is_directory(from / name))
                continue;
            fs::path made;
            if (fs::exists(data / "save" / name) && !BackupSaves(data, made, error))
                return false;
            fs::remove_all(data / "save" / name, ec);
            if (!Copy(from / name, data / "save" / name, error))
                return false;
            fs::copy_file(e.path(), data / "save" / (name + ".xcontent"), fs::copy_options::overwrite_existing, ec);
            imported++;
        }
        if (!imported)
            error = "that folder has no save (a <name> folder with a <name>.xcontent beside it, as Export makes)";
        return imported > 0;
    }

    bool DeleteSave(const fs::path& data, const SaveEntry& save, std::string& error)
    {
        fs::path made;
        if (!BackupSaves(data, made, error))
            return false;
        std::error_code ec;
        fs::remove_all(data / "save" / save.name, ec);
        fs::remove(data / "save" / (save.name + ".xcontent"), ec);
        return true;
    }

    // ---- mods ----

    std::vector<Mod> ListMods(const fs::path& data)
    {
        std::vector<Mod> out;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(data / "mods", ec))
        {
            if (!e.is_directory(ec))
                continue;
            Mod m;
            m.folder = e.path();
            m.id = e.path().filename().string();
            auto ini = ReadIni(e.path() / "mod.ini");
            m.name = ini.contains("name") ? ini["name"] : m.id;
            m.description = ini["description"];
            m.author = ini["author"];
            m.version = ini["version"];
            m.enabled = ini["enabled"] == "true" || ini["enabled"] == "1";
            m.order = ini.contains("order") ? std::atoi(ini["order"].c_str()) : 100;
            for (auto it = fs::recursive_directory_iterator(e.path() / "textures", ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                if (it->path().extension() == ".dds") m.textures++;
            ec.clear();
            for (auto it = fs::recursive_directory_iterator(e.path() / "files", ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec)) m.files++;
            ec.clear();
            out.push_back(std::move(m));
        }
        std::sort(out.begin(), out.end(), [](const Mod& a, const Mod& b) { return a.order != b.order ? a.order < b.order : a.name < b.name; });
        return out;
    }

    bool SaveModState(const Mod& m, std::string& error)
    {
        std::ofstream f(m.folder / "mod.ini", std::ios::trunc);
        f << "# SpeedBreaker: Carbon mod (the launcher's Mods tab writes enabled and order)\n";
        f << "name = " << m.name << "\n";
        if (!m.description.empty()) f << "description = " << m.description << "\n";
        if (!m.author.empty()) f << "author = " << m.author << "\n";
        if (!m.version.empty()) f << "version = " << m.version << "\n";
        f << "enabled = " << (m.enabled ? "true" : "false") << "\n";
        f << "order = " << m.order << "\n";
        if (!f)
            error = "can't write mod.ini";
        return bool(f);
    }

    bool AddMod(const fs::path& data, const fs::path& from, std::string& error)
    {
        std::error_code ec;
        std::string name = from.filename().string();
        for (const char* ext : { ".zip", ".7z", ".tar.gz", ".tar.xz", ".tar.zst", ".tgz", ".rar" })
            if (name.size() > strlen(ext) && name.compare(name.size() - strlen(ext), strlen(ext), ext) == 0)
                name.resize(name.size() - strlen(ext));
        fs::path dest = data / "mods" / SafeName(name);
        if (fs::exists(dest))
        {
            error = std::format("a mod named {} is already there", dest.filename().string());
            return false;
        }
        fs::create_directories(dest, ec);
        if (fs::is_directory(from))
        {
            if (!Copy(from, dest, error))
                return false;
        }
        else
        {
            int r = RunTool({ "bsdtar", "-xf", from.string(), "-C", dest.string() });
            if (r != 0 && from.extension() == ".zip")
                r = RunTool({ "unzip", "-q", "-o", from.string(), "-d", dest.string() });
            if (r != 0)
            {
                fs::remove_all(dest, ec);
                error = "can't unpack it (needs bsdtar or unzip on this system); unpack it and add the folder instead";
                return false;
            }
        }
        // An archive holding one folder: that folder is the mod.
        std::vector<fs::path> top;
        for (const auto& e : fs::directory_iterator(dest, ec))
            top.push_back(e.path());
        if (top.size() == 1 && fs::is_directory(top[0]) && !fs::exists(dest / "mod.ini") && !fs::exists(dest / "textures") && !fs::exists(dest / "files"))
        {
            fs::path inner = dest.string() + ".inner";
            fs::rename(top[0], inner, ec);
            fs::remove_all(dest, ec);
            fs::rename(inner, dest, ec);
        }
        // Loose DDS files named by key, without a textures/ folder: a texture mod.
        if (!fs::exists(dest / "textures") && !fs::exists(dest / "files"))
        {
            bool dds = false;
            for (const auto& e : fs::directory_iterator(dest, ec))
                dds |= e.path().extension() == ".dds";
            if (dds)
            {
                fs::create_directories(dest / "textures", ec);
                for (const auto& e : fs::directory_iterator(dest, ec))
                    if (e.path().extension() == ".dds")
                        fs::rename(e.path(), dest / "textures" / e.path().filename(), ec);
            }
        }
        Mod m;
        auto ini = ReadIni(dest / "mod.ini");
        m.folder = dest;
        m.name = ini.contains("name") ? ini["name"] : dest.filename().string();
        m.description = ini["description"];
        m.author = ini["author"];
        m.version = ini["version"];
        m.enabled = true;
        m.order = 100;
        return SaveModState(m, error);
    }

    bool RemoveMod(const Mod& m, std::string& error)
    {
        std::error_code ec;
        fs::remove_all(m.folder, ec);
        if (ec)
            error = ec.message();
        return !ec;
    }

    std::vector<fs::path> ModTextureFolders(const std::vector<Mod>& mods)
    {
        std::vector<fs::path> out;
        for (const Mod& m : mods)
            if (m.enabled && m.textures)
                out.push_back(m.folder / "textures");
        return out;
    }

    bool BuildOverlay(const fs::path& game, const fs::path& cache, const std::vector<Mod>& mods, fs::path& overlay, std::string& error)
    {
        overlay.clear();
        std::vector<const Mod*> fileMods;
        for (const Mod& m : mods)
            if (m.enabled && m.files)
                fileMods.push_back(&m);
        if (fileMods.empty())
            return true;
        std::error_code ec;
        fs::path dir = cache / "mod-overlay";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        // The game's files as links, then each mod's (later mods win).
        auto link = [&](const fs::path& root, const fs::path& file) {
            fs::path rel = fs::relative(file, root, ec);
            fs::path to = dir / rel;
            fs::create_directories(to.parent_path(), ec);
            fs::remove(to, ec);
            fs::create_symlink(fs::absolute(file), to, ec);
        };
        for (auto it = fs::recursive_directory_iterator(game, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec))
                link(game, it->path());
        // The install marker too (dot files are regular files: already linked).
        for (const Mod* m : fileMods)
        {
            fs::path root = m->folder / "files";
            for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec))
                {
                    // Match the game's own name ignoring case (NFS/ZZDATA0.BIN vs nfs/zzdata0.bin).
                    fs::path rel = fs::relative(it->path(), root, ec);
                    fs::path probe = game;
                    fs::path matched;
                    for (const auto& part : rel)
                    {
                        fs::path found;
                        for (const auto& e : fs::directory_iterator(probe, ec))
                        {
                            std::string a = e.path().filename().string(), b = part.string();
                            if (a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return tolower(x) == tolower(y); }))
                                found = e.path().filename();
                        }
                        ec.clear();
                        matched /= found.empty() ? part : found;
                        probe /= found.empty() ? part : found;
                    }
                    fs::path to = dir / matched;
                    fs::create_directories(to.parent_path(), ec);
                    fs::remove(to, ec);
                    fs::create_symlink(fs::absolute(it->path()), to, ec);
                }
        }
        if (ec)
        {
            error = ec.message();
            return false;
        }
        overlay = dir;
        return true;
    }

    // ---- DLC ----

    std::vector<DlcEntry> ListDlc(const fs::path& data)
    {
        std::vector<DlcEntry> out;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(data / "dlc", ec))
        {
            if (!e.is_directory(ec))
                continue;
            DlcEntry d;
            d.name = e.path().filename().string();
            uint32_t type;
            if (!ReadXContent(data / "dlc" / (d.name + ".xcontent"), d.displayName, type) || d.displayName.empty())
                d.displayName = d.name;
            d.bytes = FolderBytes(e.path());
            for (auto it = fs::recursive_directory_iterator(e.path(), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec)) d.files++;
            out.push_back(std::move(d));
        }
        return out;
    }

    bool AddDlc(const fs::path& data, const fs::path& from, std::string& summary, std::string& error)
    {
        std::error_code ec;
        fs::create_directories(data / "dlc", ec);
        if (fs::is_directory(from))
        {
            std::string name = SafeName(from.filename().string());
            if (!Copy(from, data / "dlc" / name, error))
                return false;
            auto x = MakeXContent(2, name, from.filename().string());
            std::ofstream(data / "dlc" / (name + ".xcontent"), std::ios::binary).write(reinterpret_cast<const char*>(x.data()), std::streamsize(x.size()));
            summary = std::format("{}: copied as downloadable content", name);
            return true;
        }
        Stfs s;
        {
            std::ifstream f(from, std::ios::binary);
            s.d.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        if (s.d.size() < 0xC000 || (memcmp(s.d.data(), "CON ", 4) && memcmp(s.d.data(), "LIVE", 4) && memcmp(s.d.data(), "PIRS", 4)))
        {
            error = "not an Xbox 360 content package (CON, LIVE or PIRS)";
            return false;
        }
        uint32_t headerSize = Be32(&s.d[0x340]);
        uint32_t contentType = Be32(&s.d[0x344]);
        uint32_t titleId = Be32(&s.d[0x360]);
        if (s.d[0x3A9] != 0)  // descriptor type 1: SVOD, not STFS
        {
            error = "an SVOD package (a game or video), not downloadable content";
            return false;
        }
        s.firstHashTable = (headerSize + 0xFFF) & ~0xFFFu;
        s.blockSeparation = s.d[0x37B];
        s.sex = (s.blockSeparation & 1) ? 0 : 1;
        uint32_t tableBlocks = uint32_t(s.d[0x37C]) | uint32_t(s.d[0x37D]) << 8;
        uint32_t tableStart = Le24(&s.d[0x37E]);
        std::string display = Utf16BeToUtf8(&s.d[0x411], 128);
        if (titleId != 0x454107EC)
            summary = std::format("note: title {:08X} isn't NFS: Carbon's (454107EC); ", titleId);
        std::vector<uint8_t> table;
        if (!s.ReadChain(tableStart, tableBlocks, false, uint64_t(tableBlocks) * 0x1000, table) &&
            !s.ReadChain(tableStart, tableBlocks, true, uint64_t(tableBlocks) * 0x1000, table))
        {
            error = "its file table can't be read";
            return false;
        }
        struct Entry { std::string name; bool dir, consecutive; uint32_t blocks, start; uint16_t parent; uint32_t size; };
        std::vector<Entry> entries;
        for (size_t p = 0; p + 0x40 <= table.size(); p += 0x40)
        {
            const uint8_t* e = &table[p];
            uint8_t flags = e[0x28];
            if ((flags & 0x3F) == 0)
                break;
            entries.push_back({ std::string(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 0x28)),
                (flags & 0x80) != 0, (flags & 0x40) != 0, Le24(e + 0x29), Le24(e + 0x2F), Be16(e + 0x32), Be32(e + 0x34) });
        }
        std::string name = SafeName(from.filename().string());
        fs::path dest = data / "dlc" / name;
        fs::remove_all(dest, ec);
        fs::create_directories(dest, ec);
        std::function<fs::path(uint16_t)> pathOf = [&](uint16_t parent) -> fs::path {
            if (parent == 0xFFFF || parent >= entries.size())
                return dest;
            return pathOf(entries[parent].parent) / entries[parent].name;
        };
        size_t files = 0;
        for (const Entry& e : entries)
        {
            fs::path p = pathOf(e.parent) / e.name;
            if (e.dir)
            {
                fs::create_directories(p, ec);
                continue;
            }
            std::vector<uint8_t> bytes;
            if (!s.ReadChain(e.start, e.blocks, e.consecutive, e.size, bytes))
            {
                fs::remove_all(dest, ec);
                error = std::format("{} can't be read from the package", e.name);
                return false;
            }
            fs::create_directories(p.parent_path(), ec);
            std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            files++;
        }
        auto x = MakeXContent(2, name, display.empty() ? name : display);
        std::ofstream(data / "dlc" / (name + ".xcontent"), std::ios::binary).write(reinterpret_cast<const char*>(x.data()), std::streamsize(x.size()));
        summary += std::format("\"{}\" ({} files{})", display.empty() ? name : display, files,
            contentType == 2 ? "" : std::format(", content type {:08X}", contentType));
        return true;
    }

    bool RemoveDlc(const fs::path& data, const DlcEntry& d, std::string& error)
    {
        std::error_code ec;
        fs::remove_all(data / "dlc" / d.name, ec);
        fs::remove(data / "dlc" / (d.name + ".xcontent"), ec);
        if (ec)
            error = ec.message();
        return !ec;
    }
}
