// SpeedBreaker launcher. GPL-3.0-or-later (see COPYING).
//
// What this computer is (CPU features, GPUs, memory, display, a Steam Deck),
// and the settings and AI texture quality that suit it.
#pragma once
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace launcher
{
    struct Gpu
    {
        uint32_t index = 0;  // the Vulkan loader's order (as the game and Real-ESRGAN number them)
        std::string name;
        int type = 0;        // VkPhysicalDeviceType: 1 integrated, 2 discrete, 3 virtual, 4 CPU
        uint64_t vram = 0;   // device-local memory (bytes)
        uint32_t vendor = 0;
        bool bc = false;     // BC (DXT) textures
    };

    enum class Tier { None, Low, Mid, High, Ultra };

    struct Hardware
    {
        std::string cpu;
        unsigned threads = 0;
        bool x86_64_v3 = false, x86_64_v4 = false;
        uint64_t ram = 0;
        std::vector<Gpu> gpus;
        int best = -1;       // index into gpus: the one the game will draw with
        int displayW = 0, displayH = 0, refresh = 60;
        bool steamDeck = false, laptop = false;
        uint64_t freeDisk = 0;  // in the data folder

        Tier tier = Tier::None;
        std::string Describe() const;
        std::string Fingerprint() const;  // changes when the CPU, GPU or display does
    };

    // Fills everything but the GPUs and display (the launcher's Vulkan and
    // SDL setup adds those), then Classify().
    void DetectSystem(Hardware& hw, const std::string& dataDir);
    void Classify(Hardware& hw);
    const char* TierName(Tier t);

    // The game's settings this tier and display suit: (section, key, value as text).
    std::vector<std::tuple<std::string, std::string, std::string>> RecommendedSettings(const Hardware& hw);

    struct AiPreset
    {
        bool enabled = false;
        std::string model;     // a Real-ESRGAN model name
        int scale = 4;
        int maxSide = 2048;    // largest texture side after upscaling
        std::string label;
    };
    AiPreset RecommendedAi(const Hardware& hw);
    std::vector<AiPreset> AiPresets();
}
