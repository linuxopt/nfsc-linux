// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
#include "hardware.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <thread>
#include <tuple>

namespace launcher
{
    namespace
    {
        std::string ReadLine(const std::string& path)
        {
            std::ifstream f(path);
            std::string s;
            std::getline(f, s);
            return s;
        }
    }

    void DetectSystem(Hardware& hw, const std::string& dataDir)
    {
        std::ifstream cpuinfo("/proc/cpuinfo");
        std::string flags;
        for (std::string line; std::getline(cpuinfo, line);)
        {
            if (hw.cpu.empty() && line.rfind("model name", 0) == 0)
                hw.cpu = line.substr(line.find(':') + 2);
            if (flags.empty() && line.rfind("flags", 0) == 0)
                flags = " " + line.substr(line.find(':') + 1) + " ";
        }
        auto has = [&](const char* f) { return flags.find(std::string(" ") + f + " ") != std::string::npos; };
        hw.x86_64_v3 = has("avx2") && has("fma") && has("bmi2") && has("movbe");
        hw.x86_64_v4 = hw.x86_64_v3 && has("avx512f") && has("avx512bw") && has("avx512cd") && has("avx512dq") && has("avx512vl");
        hw.threads = std::max(1u, std::thread::hardware_concurrency());
        std::ifstream meminfo("/proc/meminfo");
        for (std::string line; std::getline(meminfo, line);)
            if (line.rfind("MemTotal:", 0) == 0)
            {
                hw.ram = std::stoull(line.substr(9)) * 1024;
                break;
            }
        std::string product = ReadLine("/sys/class/dmi/id/product_name"), board = ReadLine("/sys/class/dmi/id/board_name");
        hw.steamDeck = product == "Jupiter" || product == "Galileo" || board == "Jupiter" || board == "Galileo";
        std::string chassis = ReadLine("/sys/class/dmi/id/chassis_type");
        hw.laptop = chassis == "8" || chassis == "9" || chassis == "10" || chassis == "14" || chassis == "31" || chassis == "32";
        std::error_code ec;
        std::filesystem::path probe = dataDir;
        while (!probe.empty() && !std::filesystem::exists(probe, ec))
            probe = probe.parent_path();
        if (!probe.empty())
            hw.freeDisk = std::filesystem::space(probe, ec).available;
    }

    void Classify(Hardware& hw)
    {
        hw.best = -1;
        for (size_t i = 0; i < hw.gpus.size(); i++)
        {
            auto rank = [](const Gpu& g) { return g.type == 2 ? 0 : g.type == 1 ? 1 : g.type == 3 ? 2 : g.type == 4 ? 4 : 3; };
            if (hw.best < 0 || rank(hw.gpus[i]) < rank(hw.gpus[hw.best]) ||
                (rank(hw.gpus[i]) == rank(hw.gpus[hw.best]) && hw.gpus[i].vram > hw.gpus[hw.best].vram))
                hw.best = int(i);
        }
        if (hw.best < 0 || hw.gpus[hw.best].type == 4)
        {
            hw.tier = Tier::None;
            return;
        }
        const Gpu& g = hw.gpus[hw.best];
        double gb = double(g.vram) / double(1ull << 30);
        if (hw.steamDeck || g.type != 2)
            hw.tier = Tier::Low;  // integrated graphics share the CPU's memory and power
        else if (gb >= 14)
            hw.tier = Tier::Ultra;
        else if (gb >= 9)
            hw.tier = Tier::High;
        else if (gb >= 5)
            hw.tier = Tier::Mid;
        else
            hw.tier = Tier::Low;
    }

    const char* TierName(Tier t)
    {
        switch (t)
        {
        case Tier::Ultra: return "Ultra";
        case Tier::High: return "High";
        case Tier::Mid: return "Medium";
        case Tier::Low: return "Low";
        default: return "Unsupported";
        }
    }

    std::string Hardware::Describe() const
    {
        std::string s = std::format("{} ({} threads, {}), {:.0f} GB RAM", cpu, threads,
            x86_64_v4 ? "AVX-512: x86-64-v4 build" : x86_64_v3 ? "AVX2: x86-64-v3 build" : "no AVX2: unsupported", double(ram) / double(1ull << 30));
        if (best >= 0)
            s += std::format("\n{} ({}, {:.1f} GB)", gpus[best].name, gpus[best].type == 2 ? "dedicated" : "integrated",
                double(gpus[best].vram) / double(1ull << 30));
        if (displayW)
            s += std::format("\nDisplay {}x{} at {} Hz{}", displayW, displayH, refresh, steamDeck ? " (Steam Deck)" : "");
        return s;
    }

    std::string Hardware::Fingerprint() const
    {
        return std::format("{}|{}|{}x{}", cpu, best >= 0 ? gpus[best].name : "none", displayW, displayH);
    }

    std::vector<std::tuple<std::string, std::string, std::string>> RecommendedSettings(const Hardware& hw)
    {
        std::vector<std::tuple<std::string, std::string, std::string>> s;
        auto set = [&](const char* sec, const char* key, std::string v) { s.emplace_back(sec, key, std::move(v)); };
        // Internal resolution: the step nearest the display's height over
        // 720, as far as the GPU can go.
        double want = hw.displayH > 0 ? double(hw.displayH) / 720.0 : 1.0;
        double cap = hw.tier == Tier::Ultra ? 3.0 : hw.tier == Tier::High ? 2.0 : hw.tier == Tier::Mid ? 1.5 : 1.0;
        double r = std::min(want, cap);
        std::string res = hw.steamDeck ? "auto" : r >= 2.9 ? "3x" : r >= 1.9 ? "2x" : r >= 1.4 ? "1.5x" : "1x";
        // A step past the display's own height is supersampling: worth it on the fastest GPUs only.
        if (hw.tier == Tier::Ultra && want < 2.9 && want >= 1.9)
            res = "3x";
        set("video", "internal_resolution", res);
        // (Shadow Resolution: NFS: Carbon's build keeps the 360's 1x; the game ignores the setting.)
        set("video", "anisotropic_filtering", hw.tier >= Tier::Mid ? "16x" : "4x");
        set("video", "anti_aliasing", "true");
        set("video", "mipmaps", "true");
        // Upscaling: FSR 1 where the picture is enlarged a lot.
        double renderH = 720.0 * (res == "3x" ? 3 : res == "2x" ? 2 : res == "1.5x" ? 1.5 : 1);
        set("video", "scaling", hw.displayH > renderH * 1.3 ? "fsr1" : "bicubic");
        set("video", "sharpening", hw.displayH > renderH * 1.3 ? "0.4" : "0.25");
        set("video", "fullscreen", "true");
        set("video", "vsync", "true");
        set("video", "frame_rate", hw.tier == Tier::Low && !hw.steamDeck ? "auto" : "60");
        set("video", "smooth_motion", hw.refresh >= 90 && hw.tier >= Tier::High && !hw.steamDeck ? "true" : "false");
        set("advanced", "async_shaders", "true");
        return s;
    }

    std::vector<AiPreset> AiPresets()
    {
        return {
            { true, "realesrgan-x4plus", 4, 2048, "Ultra: Real-ESRGAN x4plus, 4x, up to 2048 px" },
            { true, "realesrgan-x4plus", 4, 1024, "High: Real-ESRGAN x4plus, up to 1024 px" },
            { true, "realesr-animevideov3", 2, 1024, "Fast: AnimeVideo v3, 2x, up to 1024 px" },
            { false, "", 1, 0, "Off: the game's own textures" },
        };
    }

    AiPreset RecommendedAi(const Hardware& hw)
    {
        auto presets = AiPresets();
        switch (hw.tier)
        {
        case Tier::Ultra:
        case Tier::High: return presets[0];
        case Tier::Mid: return presets[1];
        case Tier::Low: return hw.best >= 0 && hw.gpus[hw.best].vram >= (2ull << 30) ? presets[2] : presets[3];
        default: return presets[3];
        }
    }
}
