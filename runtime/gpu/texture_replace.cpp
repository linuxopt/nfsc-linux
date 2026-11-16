// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
// Texture replacement: see texture_replace.h.
#include "texture_replace.h"

#include <user/paths.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <vulkan/vulkan_core.h>

namespace texreplace
{
    namespace
    {
        std::once_flag s_scanOnce;
        bool s_active = false;
        // key -> its file (the first pack that has it)
        std::unordered_map<uint64_t, std::filesystem::path> s_files;
        std::vector<std::filesystem::path> s_packs;
        Stats s_stats;

        std::mutex s_keyMutex;
        std::unordered_set<uint64_t> s_loggedKeys;
        FILE* s_keyFile = nullptr;
        const bool s_logKeys = [] { const char* v = std::getenv("NFSMW_TEXTURE_KEYS"); return v && *v; }();

        bool ParseHex(const std::string& s, uint64_t& out)
        {
            if (s.size() != 16)
                return false;
            uint64_t v = 0;
            for (char c : s)
            {
                v <<= 4;
                if (c >= '0' && c <= '9') v |= uint64_t(c - '0');
                else if (c >= 'a' && c <= 'f') v |= uint64_t(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= uint64_t(c - 'A' + 10);
                else return false;
            }
            out = v;
            return true;
        }

        void AddPack(const std::filesystem::path& dir)
        {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec))
                return;
            size_t before = s_files.size();
            for (auto it = std::filesystem::recursive_directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            {
                const auto& p = it->path();
                if (p.extension() != ".dds" && p.extension() != ".DDS")
                    continue;
                uint64_t key;
                if (ParseHex(p.stem().string(), key))
                    s_files.try_emplace(key, p);
            }
            s_packs.push_back(dir);
            fprintf(stderr, "[textures] pack %s: %zu textures\n", (const char*)dir.u8string().c_str(), s_files.size() - before);
        }

        void Scan()
        {
            const char* off = std::getenv("NFSMW_TEXTURE_REPLACE");
            if (off && off[0] == '0')
            {
                fprintf(stderr, "[textures] replacement off (NFSMW_TEXTURE_REPLACE=0)\n");
                return;
            }
            if (const char* list = std::getenv("NFSMW_TEXTURE_PACK"); list && *list)
            {
                std::string s(list);
                size_t from = 0;
                while (from <= s.size())
                {
                    size_t to = s.find(':', from);
                    if (to == std::string::npos)
                        to = s.size();
                    if (to > from)
                        AddPack(std::filesystem::path(s.substr(from, to - from)));
                    from = to + 1;
                }
            }
            else
            {
                AddPack(GetUserPath() / "texture-pack");
            }
            s_active = !s_files.empty();
            if (!s_active)
                fprintf(stderr, "[textures] no texture pack: the game's own textures\n");
        }

        struct DdsFormat
        {
            VkFormat vk;
            uint32_t blockBytes;  // 0: uncompressed, 4 bytes a texel
        };

        bool FormatFromFourCC(uint32_t fourcc, DdsFormat& out)
        {
            auto cc = [](const char* s) { return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24; };
            if (fourcc == cc("DXT1")) { out = { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8 }; return true; }
            if (fourcc == cc("DXT2") || fourcc == cc("DXT3")) { out = { VK_FORMAT_BC2_UNORM_BLOCK, 16 }; return true; }
            if (fourcc == cc("DXT4") || fourcc == cc("DXT5")) { out = { VK_FORMAT_BC3_UNORM_BLOCK, 16 }; return true; }
            if (fourcc == cc("ATI2") || fourcc == cc("BC5U")) { out = { VK_FORMAT_BC5_UNORM_BLOCK, 16 }; return true; }
            return false;
        }

        bool FormatFromDxgi(uint32_t dxgi, DdsFormat& out)
        {
            switch (dxgi)
            {
            case 28: case 29: out = { VK_FORMAT_R8G8B8A8_UNORM, 0 }; return true;   // R8G8B8A8_UNORM(_SRGB: read as UNORM, as the game's)
            case 71: case 72: out = { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8 }; return true;
            case 74: case 75: out = { VK_FORMAT_BC2_UNORM_BLOCK, 16 }; return true;
            case 77: case 78: out = { VK_FORMAT_BC3_UNORM_BLOCK, 16 }; return true;
            case 83: out = { VK_FORMAT_BC5_UNORM_BLOCK, 16 }; return true;
            case 98: case 99: out = { VK_FORMAT_BC7_UNORM_BLOCK, 16 }; return true;
            default: return false;
            }
        }
    }

    uint64_t Key(const uint8_t* bytes, size_t size, uint32_t format, uint32_t width, uint32_t height)
    {
        uint64_t h = XXH3_64bits(bytes, size);
        h ^= (uint64_t(format) << 56) ^ (uint64_t(width) << 28) ^ uint64_t(height);
        h *= 0x9E3779B97F4A7C15ull;
        return h ^ (h >> 31);
    }

    bool Active()
    {
        std::call_once(s_scanOnce, Scan);
        return s_active;
    }

    bool Has(uint64_t key)
    {
        return Active() && s_files.contains(key);
    }

    bool LoggingKeys()
    {
        return s_logKeys;
    }

    void LogKey(uint64_t key, uint32_t format, uint32_t width, uint32_t height, uint32_t size)
    {
        std::lock_guard lock(s_keyMutex);
        if (!s_loggedKeys.insert(key).second)
            return;
        if (!s_keyFile)
        {
            s_keyFile = fopen(std::getenv("NFSMW_TEXTURE_KEYS"), "a");
            if (!s_keyFile)
                return;
        }
        fprintf(s_keyFile, "%016llx %u %u %u %u\n", (unsigned long long)key, format, width, height, size);
        fflush(s_keyFile);
    }

    bool Load(uint64_t key, Image& out)
    {
        auto it = s_files.find(key);
        if (it == s_files.end())
            return false;
        auto fail = [&](const char* why) {
            s_stats.failed++;
            fprintf(stderr, "[textures] %s: %s: the game's own texture\n", (const char*)it->second.u8string().c_str(), why);
            return false;
        };
        std::ifstream f(it->second, std::ios::binary | std::ios::ate);
        if (!f)
            return fail("can't open");
        size_t fileSize = size_t(f.tellg());
        if (fileSize < 128)
            return fail("too short for a DDS file");
        std::vector<uint8_t> file(fileSize);
        f.seekg(0);
        f.read(reinterpret_cast<char*>(file.data()), std::streamsize(fileSize));
        auto u32 = [&](size_t at) { uint32_t v; memcpy(&v, file.data() + at, 4); return v; };
        if (memcmp(file.data(), "DDS ", 4) != 0 || u32(4) != 124)
            return fail("not a DDS file");
        uint32_t height = u32(12), width = u32(16), mips = std::max(1u, u32(28));
        uint32_t pfFlags = u32(80), fourcc = u32(84);
        size_t dataStart = 128;
        DdsFormat fmt{};
        bool known = false;
        if ((pfFlags & 4) && fourcc == 0x30315844)  // "DX10"
        {
            if (fileSize < 148)
                return fail("DX10 header cut short");
            known = FormatFromDxgi(u32(128), fmt);
            dataStart = 148;
        }
        else if (pfFlags & 4)
            known = FormatFromFourCC(fourcc, fmt);
        else if ((pfFlags & 0x40) && u32(88) == 32 && u32(92) == 0xFF && u32(96) == 0xFF00 && u32(100) == 0xFF0000)
        {
            fmt = { VK_FORMAT_R8G8B8A8_UNORM, 0 };
            known = true;
        }
        if (!known)
            return fail("a pixel format this doesn't take (BC1, BC2, BC3, BC5, BC7 or RGBA8)");
        if (width == 0 || height == 0 || width > 16384 || height > 16384)
            return fail("an impossible size");
        out = {};
        out.vkFormat = int(fmt.vk);
        out.width = width;
        out.height = height;
        size_t at = dataStart;
        for (uint32_t level = 0; level < mips; level++)
        {
            uint32_t w = std::max(width >> level, 1u), h = std::max(height >> level, 1u);
            size_t bytes = fmt.blockBytes ? size_t((w + 3) / 4) * ((h + 3) / 4) * fmt.blockBytes : size_t(w) * h * 4;
            if (at + bytes > fileSize)
            {
                if (level == 0)
                    return fail("its pixels are cut short");
                break;  // the mips it has
            }
            Level l{ w, h, std::vector<uint8_t>(file.begin() + at, file.begin() + at + bytes) };
            out.levels.push_back(std::move(l));
            out.bytes += bytes;
            at += bytes;
            if (w == 1 && h == 1)
                break;
        }
        s_stats.replaced++;
        s_stats.bytes += out.bytes;
        return true;
    }

    Stats& Counters()
    {
        return s_stats;
    }

    std::string Describe()
    {
        return std::format("{} replaced ({} MB), {} failed, from {} textures in {} packs",
            s_stats.replaced, s_stats.bytes >> 20, s_stats.failed, s_files.size(), s_packs.size());
    }
}
