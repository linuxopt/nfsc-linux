// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
#include "icons.h"

#include "../texpack/image_io.h"

#include <image.h>
#include <xdbf_wrapper.h>
#include <xex.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

namespace launcher::icons
{
    namespace
    {
        constexpr int kSizes[] = { 256, 128, 64, 32, 16 };

        fs::path DataHome()
        {
            const char* xdg = std::getenv("XDG_DATA_HOME");
            if (xdg && *xdg == '/')
                return xdg;
            const char* home = std::getenv("HOME");
            return fs::path(home ? home : "/tmp") / ".local/share";
        }
        fs::path EntryPath() { return DataHome() / "applications" / (std::string(kAppId) + ".desktop"); }
        fs::path ThemeIcon(int size) { return DataHome() / std::format("icons/hicolor/{0}x{0}/apps", size) / (std::string(kAppId) + ".png"); }

        // Bilinear, for a title icon smaller than 256 (no AI upscaler).
        img::Rgba Resize(const img::Rgba& src, uint32_t w, uint32_t h)
        {
            img::Rgba out;
            out.width = w;
            out.height = h;
            out.px.resize(size_t(w) * h * 4);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                {
                    float fx = std::max(0.0f, (x + 0.5f) * src.width / w - 0.5f), fy = std::max(0.0f, (y + 0.5f) * src.height / h - 0.5f);
                    uint32_t x0 = std::min(uint32_t(fx), src.width - 1), y0 = std::min(uint32_t(fy), src.height - 1);
                    uint32_t x1 = std::min(x0 + 1, src.width - 1), y1 = std::min(y0 + 1, src.height - 1);
                    float ax = fx - x0, ay = fy - y0;
                    for (int c = 0; c < 4; c++)
                    {
                        auto at = [&](uint32_t px, uint32_t py) { return float(src.px[(size_t(py) * src.width + px) * 4 + c]); };
                        float v = (at(x0, y0) * (1 - ax) + at(x1, y0) * ax) * (1 - ay) + (at(x0, y1) * (1 - ax) + at(x1, y1) * ax) * ay;
                        out.px[(size_t(y) * w + x) * 4 + c] = uint8_t(std::clamp(v + 0.5f, 0.0f, 255.0f));
                    }
                }
            return out;
        }

        // A desktop entry's Exec= argument (the spec's quoting).
        std::string QuoteExec(const std::string& s)
        {
            std::string q = "\"";
            for (char c : s)
            {
                if (c == '"' || c == '`' || c == '$' || c == '\\')
                    q += '\\';
                if (c == '%')
                    q += '%';
                q += c;
            }
            return q + "\"";
        }
    }

    fs::path File(const fs::path& dir, uint32_t id) { return dir / std::format("{:x}.png", id); }
    fs::path AppIcon(const fs::path& dir, int size) { return dir / std::format("{}-{}.png", kAppId, size); }

    bool Extract(const fs::path& xex, const fs::path& dir, std::string& error)
    {
        std::ifstream f(xex, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
        if (data.size() < 0x200 || memcmp(data.data(), "XEX2", 4) != 0)
        {
            error = xex.string() + " isn't an Xbox 360 program";
            return false;
        }
        const auto* res = reinterpret_cast<const be<uint32_t>*>(getOptHeaderPtr(data.data(), XEX_HEADER_RESOURCE_INFO));
        if (!res)
        {
            error = "default.xex has no resources";
            return false;
        }
        Image image = Xex2LoadImage(data.data(), data.size());
        if (!image.data || image.size == 0)
        {
            error = "default.xex didn't load";
            return false;
        }
        // XEX_HEADER_RESOURCE_INFO: its size, then { name[8], address, size } each.
        struct Resource { char name[8]; be<uint32_t> address, size; };
        uint32_t headerSize = res[0];
        const auto* r = reinterpret_cast<const Resource*>(res + 1);
        size_t count = headerSize >= 4 ? (headerSize - 4) / sizeof(Resource) : 0;
        std::error_code ec;
        fs::create_directories(dir, ec);
        size_t written = 0;
        for (size_t i = 0; i < count; i++)
        {
            uint32_t address = r[i].address, size = r[i].size;
            if (address < image.base || uint64_t(address - image.base) + size > image.size)
                continue;
            const uint8_t* p = image.data.get() + (address - image.base);
            XDBFWrapper x(p, size);
            if (!x.pBuffer)
                continue;
            for (uint32_t e = 0; e < x.pHeader->EntryCount; e++)
            {
                const XDBFEntry& en = x.pEntries[e];
                if (en.NamespaceID != XDBF_SPA_NAMESPACE_IMAGE)
                    continue;
                uint64_t id = en.ResourceID;
                const uint8_t* png = x.pContent + uint32_t(en.Offset);
                uint32_t length = en.Length;
                if (png < p || png + length > p + size || length < 8 || memcmp(png, "\x89PNG", 4) != 0)
                    continue;
                std::ofstream out(File(dir, uint32_t(id)), std::ios::binary);
                out.write(reinterpret_cast<const char*>(png), length);
                written += bool(out);
            }
        }
        if (!written)
        {
            error = "default.xex has no icons";
            return false;
        }
        return true;
    }

    bool LoadSquare(const fs::path& png, uint32_t size, std::vector<uint8_t>& rgba, std::string& error)
    {
        img::Rgba im;
        if (!img::ReadPng(png, im, error))
            return false;
        while (im.width >= size * 2 && im.height >= size * 2)
            im = img::Half(im);
        if (im.width != size || im.height != size)
            im = Resize(im, size, size);
        rgba = std::move(im.px);
        return true;
    }

    bool MakeAppIcons(const fs::path& title, const fs::path& dir, std::string& error)
    {
        img::Rgba src;
        if (!img::ReadPng(title, src, error))
            return false;
        if (src.width != 256 || src.height != 256)
            src = src.width > 256 ? img::Fit(src, 256) : Resize(src, 256, 256);
        if (src.width != 256 || src.height != 256)
            src = Resize(src, 256, 256);
        std::error_code ec;
        fs::create_directories(dir, ec);
        img::Rgba level = src;
        for (int size : kSizes)
        {
            while (int(level.width) > size)
                level = img::Half(level);
            if (!img::WritePng(AppIcon(dir, size), level))
            {
                error = "can't write " + AppIcon(dir, size).string();
                return false;
            }
        }
        return true;
    }

    bool InstallMenuEntry(const std::string& appImage, const fs::path& iconDir, std::string& error)
    {
        if (appImage.empty())
        {
            error = "the launcher isn't running from its AppImage";
            return false;
        }
        std::error_code ec;
        for (int size : kSizes)
        {
            fs::path from = AppIcon(iconDir, size);
            if (!fs::exists(from))
                continue;
            fs::create_directories(ThemeIcon(size).parent_path(), ec);
            fs::copy_file(from, ThemeIcon(size), fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                error = ec.message();
                return false;
            }
        }
        fs::create_directories(EntryPath().parent_path(), ec);
        std::ofstream f(EntryPath());
        f << "[Desktop Entry]\n"
             "Type=Application\n"
             "Name=NFSC Linux\n"
             "GenericName=Racing game\n"
             "Comment=Need for Speed: Carbon (Xbox 360), built natively from your own disc\n"
             "Exec=" << QuoteExec(appImage) << "\n"
             "TryExec=" << appImage << "\n"
             "Icon=" << kAppId << "\n"
             "StartupWMClass=" << kAppId << "\n"
             "Categories=Game;RacingGame;\n"
             "Keywords=nfs;carbon;racing;xbox;\n"
             "Terminal=false\n"
             "X-NFSC-Linux=true\n";
        if (!f)
        {
            error = "can't write " + EntryPath().string();
            return false;
        }
        return true;
    }

    bool RemoveMenuEntry(std::string& error)
    {
        std::error_code ec;
        if (MenuEntryInstalled())
            fs::remove(EntryPath(), ec);
        for (int size : kSizes)
            fs::remove(ThemeIcon(size), ec);
        if (ec)
            error = ec.message();
        return !ec;
    }

    bool MenuEntryInstalled() { return !MenuEntryTarget().empty(); }

    std::string MenuEntryTarget()
    {
        std::ifstream f(EntryPath());
        std::string target;
        bool ours = false;
        for (std::string line; std::getline(f, line);)
        {
            if (line == "X-NFSC-Linux=true")
                ours = true;
            if (line.rfind("TryExec=", 0) == 0)
                target = line.substr(8);
        }
        return ours ? target : std::string();
    }
}
