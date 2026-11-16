// SpeedBreaker texpack. GPL-3.0-or-later (see COPYING).
// RGBA8 images: PNG in and out (zlib), DXT decoding, BC1/BC3 encoding, DDS out.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace img
{
    struct Rgba
    {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> px;  // RGBA, rows top to bottom
    };

    bool WritePng(const std::filesystem::path& path, const Rgba& image);
    bool ReadPng(const std::filesystem::path& path, Rgba& image, std::string& error);

    // One 4x4 block to 16 RGBA texels (row-major). bc: 1, 2 (DXT3) or 3 (DXT5).
    void DecodeBlock(int bc, const uint8_t* block, uint8_t out[64]);
    // 16 RGBA texels to one block. bc 1: punch-through alpha when `alpha1`.
    void EncodeBlock(int bc, const uint8_t in[64], bool alpha1, uint8_t* block);

    // Half size, a box filter (for mips).
    Rgba Half(const Rgba& image);
    // Fits the image inside maxSide (keeps its aspect; powers of two stay so).
    Rgba Fit(const Rgba& image, uint32_t maxSide);

    // DDS with a full mip chain. bc 1 or 3.
    bool WriteDds(const std::filesystem::path& path, const Rgba& base, int bc, bool alpha1, std::string& error);
}
