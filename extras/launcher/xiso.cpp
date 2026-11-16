// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
#include "xiso.h"

#include <cstdint>
#include <cstring>
#include <format>
#include <fstream>
#include <vector>

namespace launcher
{
    namespace
    {
        constexpr uint64_t kSector = 2048;
        constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";

        uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
        uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

        int CompareNoCase(const std::string& a, const std::string& b)
        {
            for (size_t i = 0; i < a.size() && i < b.size(); i++)
            {
                int x = toupper(uint8_t(a[i])), y = toupper(uint8_t(b[i]));
                if (x != y) return x < y ? -1 : 1;
            }
            return a.size() == b.size() ? 0 : a.size() < b.size() ? -1 : 1;
        }
    }

    bool ExtractFromIso(const std::filesystem::path& iso, const std::string& name, const std::filesystem::path& dest, std::string& error)
    {
        std::ifstream f(iso, std::ios::binary);
        if (!f)
        {
            error = "can't open " + iso.string();
            return false;
        }
        // The game partition: at the start of an extracted XISO, or past the
        // video partition of a full disc image (XGD2, XGD3, XGD1).
        uint64_t base = UINT64_MAX;
        for (uint64_t offset : { 0ull, 0xFD90000ull, 0x2080000ull, 0x18300000ull })
        {
            char magic[20];
            f.clear();
            f.seekg(std::streamoff(offset + 32 * kSector));
            if (f.read(magic, 20) && memcmp(magic, kMagic, 20) == 0)
            {
                base = offset;
                break;
            }
        }
        if (base == UINT64_MAX)
        {
            error = "not an Xbox 360 disc image (no XDVDFS volume)";
            return false;
        }
        uint8_t header[28];
        f.seekg(std::streamoff(base + 32 * kSector));
        f.read(reinterpret_cast<char*>(header), sizeof(header));
        uint32_t rootSector = Le32(header + 20), rootSize = Le32(header + 24);
        if (rootSize == 0 || rootSize > (64u << 20))
        {
            error = "the disc image's root folder is unreadable";
            return false;
        }
        std::vector<uint8_t> dir(rootSize);
        f.seekg(std::streamoff(base + uint64_t(rootSector) * kSector));
        if (!f.read(reinterpret_cast<char*>(dir.data()), rootSize))
        {
            error = "the disc image is cut short";
            return false;
        }
        // The folder is a binary tree; offsets in 4-byte units.
        uint32_t at = 0;
        for (int guard = 0; guard < 4096; guard++)
        {
            if (at + 14 > dir.size())
                break;
            const uint8_t* e = &dir[at];
            uint16_t left = Le16(e), right = Le16(e + 2);
            uint32_t sector = Le32(e + 4), size = Le32(e + 8);
            uint8_t len = e[13];
            if (at + 14 + len > dir.size())
                break;
            std::string entry(reinterpret_cast<const char*>(e + 14), len);
            int c = CompareNoCase(name, entry);
            if (c == 0)
            {
                std::ofstream o(dest, std::ios::binary | std::ios::trunc);
                f.seekg(std::streamoff(base + uint64_t(sector) * kSector));
                std::vector<char> buf(1 << 20);
                for (uint64_t left2 = size; left2 > 0;)
                {
                    size_t n = size_t(std::min<uint64_t>(left2, buf.size()));
                    if (!f.read(buf.data(), std::streamsize(n)))
                    {
                        error = "the disc image is cut short";
                        return false;
                    }
                    o.write(buf.data(), std::streamsize(n));
                    left2 -= n;
                }
                if (!o)
                {
                    error = "can't write " + dest.string();
                    return false;
                }
                return true;
            }
            uint16_t next = c < 0 ? left : right;
            if (next == 0 || next == 0xFFFF)
                break;
            at = uint32_t(next) * 4;
        }
        error = std::format("{} isn't on this disc image: is it NFS: Carbon (Xbox 360)?", name);
        return false;
    }
}
