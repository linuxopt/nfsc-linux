// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Texture replacement (texture packs, such as the AI-upscaled pack the
// launcher makes; tools/texpack/). A block-compressed guest texture (DXT1,
// DXT2/3, DXT4/5, DXN) is identified by a hash of its base level's guest
// bytes, exactly as the game's texture pack (TPK) holds them on the disc,
// plus its format and size: Key() below. A pack is a folder of
// <key as 16 hex digits>.dds files (BC1/BC2/BC3/BC5/BC7 or RGBA8, any size,
// with or without mips); when one matches, the renderer samples it instead of
// the guest texture, at its own resolution.
//
//   NFSMW_TEXTURE_PACK=<dir>[:<dir>...]  the packs, the first match wins
//                                        (default: <user data>/texture-pack,
//                                        then each enabled mod's textures/)
//   NFSMW_TEXTURE_REPLACE=0              off
//   NFSMW_TEXTURE_KEYS=<file>            appends each distinct key the game
//                                        loads ("key format width height size")
//
// Only block-compressed formats are replaced: no resolve or render target
// writes them, so a replacement never stands in for a surface the GPU draws.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace texreplace
{
    // The hash a pack names its files by: XXH3-64 of the base level's guest
    // bytes, mixed with the guest format number and size.
    uint64_t Key(const uint8_t* bytes, size_t size, uint32_t format, uint32_t width, uint32_t height);

    // Whether replacement is on and any pack has files (scans once).
    bool Active();

    // Whether a pack has `key`.
    bool Has(uint64_t key);

    // NFSMW_TEXTURE_KEYS: records `key` (once each).
    bool LoggingKeys();
    void LogKey(uint64_t key, uint32_t format, uint32_t width, uint32_t height, uint32_t size);

    struct Level
    {
        uint32_t width, height;
        std::vector<uint8_t> data;
    };
    struct Image
    {
        int vkFormat = 0;  // VkFormat
        uint32_t width = 0, height = 0;
        std::vector<Level> levels;
        size_t bytes = 0;
    };

    // Loads `key`'s DDS (the first pack that has it). False: unreadable or a
    // format this doesn't take (logged once).
    bool Load(uint64_t key, Image& out);

    // For the [perf] line and the log at exit.
    struct Stats
    {
        uint64_t replaced = 0, missed = 0, failed = 0;
        uint64_t bytes = 0;
    };
    Stats& Counters();
    std::string Describe();
}
