// SpeedBreaker texpack. GPL-3.0-or-later (see COPYING).
//
// Builds a texture pack for the runtime's texture replacement
// (runtime/gpu/texture_replace.h) from the installed game's own files:
//
//   sb-texpack scan <game files> <work dir> [--min N]
//       Every block-compressed texture in the game's texture packs (TPK,
//       plain and JDLZ/HUFF-compressed, in NFS/ZDIR.BIN + ZZDATA*.BIN): its
//       key (the hash the runtime computes from its guest bytes), and its
//       pixels as <work>/src/<key>.png, listed in <work>/manifest.tsv.
//       Normal maps and textures under N texels a side (default 32) are left out.
//   sb-texpack encode <work dir> <upscaled dir> <pack dir> [--max N] [--jobs N]
//       Each <upscaled>/<key>.png (an AI upscaler's output) as <pack>/<key>.dds:
//       BC1 or BC3 as the original, with mips, at most N texels a side.
//
// Progress for the launcher: "progress <done> <total>" lines on stdout.
#include "ea_compress.h"
#include "image_io.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // Same as texreplace::Key (runtime/gpu/texture_replace.cpp).
    uint64_t Key(const uint8_t* bytes, size_t size, uint32_t format, uint32_t width, uint32_t height)
    {
        uint64_t h = XXH3_64bits(bytes, size);
        h ^= (uint64_t(format) << 56) ^ (uint64_t(width) << 28) ^ uint64_t(height);
        h *= 0x9E3779B97F4A7C15ull;
        return h ^ (h >> 31);
    }

    uint32_t U32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
    uint16_t U16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }

    struct Chunk
    {
        uint32_t id, size;
        size_t offset;  // of its header
        int depth;
    };

    void Walk(const uint8_t* b, size_t off, size_t end, int depth, std::vector<Chunk>& out)
    {
        while (off + 8 <= end)
        {
            uint32_t id = U32(b + off), size = U32(b + off + 4);
            if (off + 8 + size > end)
                break;
            out.push_back({ id, size, off, depth });
            if ((id & 0x80000000u) && size)
                Walk(b, off + 8, off + 8 + size, depth + 1, out);
            off += 8 + size;
        }
    }

    // Xenos 2D tiling (as runtime/gpu/renderer.cpp's untile).
    uint32_t Tiled2D(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bppLog2)
    {
        uint32_t outer = (((y >> 5) * (pitch >> 5)) + (x >> 5)) << 6;
        uint32_t inner = (((y >> 1) & 7u) << 3) | (x & 7u);
        uint32_t oi = (outer | inner) << bppLog2;
        uint32_t bank = (y >> 4) & 1u;
        uint32_t pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
        return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) | (((oi >> 4) & 1u) << 5) |
            (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
    }

    struct Tex
    {
        uint64_t key;
        uint32_t gpuFormat, width, height;
        int bc;           // 1 DXT1, 2 DXT3, 3 DXT5
        std::string name, pack;
    };

    bool LooksLikeNormalMap(const std::string& n)
    {
        std::string u = n;
        for (char& c : u) c = char(toupper(uint8_t(c)));
        auto ends = [&](const char* s) { size_t l = strlen(s); return u.size() >= l && u.compare(u.size() - l, l, s) == 0; };
        return u.find("NORMAL") != std::string::npos || ends("_N") || ends("_NM") || ends("_NRM") || u.find("BUMP") != std::string::npos;
    }

    // A texture record (TPK 0x33310004 entry, or a compressed texture's
    // trailer) starting at its hash.
    struct Record
    {
        uint32_t hash, dataOffset, dataSize, baseSize;
        uint16_t width, height;
        uint8_t compression;
        std::string name;
    };

    bool ParseRecord(const uint8_t* q, size_t avail, Record& r)
    {
        if (avail < 40)
            return false;
        r.hash = U32(q);
        r.dataOffset = U32(q + 8);
        r.dataSize = U32(q + 16);
        r.baseSize = U32(q + 24);
        r.width = U16(q + 28);
        r.height = U16(q + 30);
        r.compression = q[34];
        // The name: a length byte, then the characters, near the record's end.
        r.name.clear();
        for (size_t p = 40; p + 2 < std::min<size_t>(avail, 120); p++)
        {
            uint8_t len = q[p];
            if (len >= 2 && len < 40 && p + 1 + len <= avail)
            {
                bool ok = true;
                for (size_t k = 0; k + 1 < len && ok; k++)
                    ok = q[p + 1 + k] >= 0x20 && q[p + 1 + k] < 0x7F;
                if (ok && q[p + len] == 0)
                {
                    r.name.assign(reinterpret_cast<const char*>(q + p + 1), len - 1);
                    break;
                }
            }
        }
        return true;
    }

    int BcFor(uint8_t compression, uint32_t& gpuFormat)
    {
        switch (compression)
        {
        case 0x22: gpuFormat = 18; return 1;  // DXT1
        case 0x24: gpuFormat = 19; return 2;  // DXT3
        case 0x26: gpuFormat = 20; return 3;  // DXT5
        default: return 0;
        }
    }

    struct Scanner
    {
        fs::path work;
        uint32_t minSide = 32;
        std::unordered_set<uint64_t> seen;
        std::vector<Tex> textures;
        size_t skipped = 0, failed = 0, normalMaps = 0;

        // The base level's guest bytes (Xenos-tiled, 8-in-16 swapped) to RGBA.
        img::Rgba Decode(const uint8_t* base, size_t size, uint32_t w, uint32_t h, int bc)
        {
            uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
            uint32_t pitch = (bw + 31) & ~31u;
            uint32_t bppLog2 = bc == 1 ? 3 : 4, blockBytes = 1u << bppLog2;
            img::Rgba out;
            out.width = w;
            out.height = h;
            out.px.assign(size_t(w) * h * 4, 0);
            uint8_t block[16], texels[64];
            for (uint32_t by = 0; by < bh; by++)
                for (uint32_t bx = 0; bx < bw; bx++)
                {
                    uint32_t a = Tiled2D(bx, by, pitch, bppLog2);
                    if (a + blockBytes > size)
                        continue;
                    for (uint32_t k = 0; k < blockBytes; k += 2)
                    {
                        block[k] = base[a + k + 1];
                        block[k + 1] = base[a + k];
                    }
                    img::DecodeBlock(bc, block, texels);
                    for (int i = 0; i < 16; i++)
                    {
                        uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                        if (x < w && y < h)
                            memcpy(&out.px[(size_t(y) * w + x) * 4], &texels[i * 4], 4);
                    }
                }
            return out;
        }

        void Add(const Record& r, const uint8_t* bytes, size_t avail, const std::string& pack)
        {
            uint32_t gpuFormat;
            int bc = BcFor(r.compression, gpuFormat);
            if (!bc || r.width < minSide || r.height < minSide || r.baseSize == 0 || r.baseSize > avail)
            {
                skipped++;
                return;
            }
            if (LooksLikeNormalMap(r.name))
            {
                normalMaps++;
                return;
            }
            uint64_t key = Key(bytes, r.baseSize, gpuFormat, r.width, r.height);
            if (!seen.insert(key).second)
                return;
            img::Rgba rgba = Decode(bytes, r.baseSize, r.width, r.height, bc);
            if (!img::WritePng(work / "src" / std::format("{:016x}.png", key), rgba))
            {
                failed++;
                return;
            }
            textures.push_back({ key, gpuFormat, r.width, r.height, bc, r.name, pack });
        }

        void ScanFile(const std::vector<uint8_t>& b)
        {
            std::vector<Chunk> chunks;
            Walk(b.data(), 0, b.size(), 0, chunks);
            for (size_t i = 0; i < chunks.size(); i++)
            {
                if (chunks[i].id != 0xB3300000u)
                    continue;
                size_t tpkEnd = chunks[i].offset + 8 + chunks[i].size;
                std::map<uint32_t, const Chunk*> m;
                for (size_t j = i + 1; j < chunks.size() && chunks[j].offset < tpkEnd; j++)
                    m.try_emplace(chunks[j].id, &chunks[j]);
                std::string pack = "?";
                if (auto it = m.find(0x33310001u); it != m.end() && it->second->size >= 32)
                {
                    const char* n = reinterpret_cast<const char*>(b.data() + it->second->offset + 12);
                    pack.assign(n, strnlen(n, 28));
                }
                auto hashesIt = m.find(0x33310002u);
                if (hashesIt == m.end())
                    continue;
                std::vector<uint32_t> hashes;
                for (uint32_t k = 0; k + 8 <= hashesIt->second->size; k += 8)
                    hashes.push_back(U32(b.data() + hashesIt->second->offset + 8 + k));

                if (auto info = m.find(0x33310004u), data = m.find(0x33320003u); info != m.end() && data != m.end())
                {
                    // Plain: records, then the pixels after the data chunk's 0x11 padding.
                    const uint8_t* rec = b.data() + info->second->offset + 8;
                    size_t recSize = info->second->size;
                    size_t p = data->second->offset + 8, dEnd = p + data->second->size;
                    while (p < dEnd && b[p] == 0x11) p++;
                    p &= ~size_t(0x7F);
                    size_t pos = 0;
                    for (uint32_t h : hashes)
                    {
                        const uint8_t* found = nullptr;
                        for (size_t q = pos; q + 4 <= recSize; q += 4)
                            if (U32(rec + q) == h) { found = rec + q; pos = q + 4; break; }
                        Record r;
                        if (!found || !ParseRecord(found, recSize - size_t(found - rec), r))
                            continue;
                        size_t at = p + r.dataOffset;
                        if (at >= b.size())
                            continue;
                        Add(r, b.data() + at, b.size() - at, pack);
                    }
                }
                else if (auto entries = m.find(0x33310003u); entries != m.end())
                {
                    // Compressed: each entry a chain of 0x55441122 blocks
                    // (from the TPK's start); unpacked, the pixels then a
                    // record.
                    const uint8_t* e = b.data() + entries->second->offset + 8;
                    size_t tpkStart = chunks[i].offset;
                    for (uint32_t k = 0; k + 24 <= entries->second->size; k += 24)
                    {
                        uint32_t hash = U32(e + k), off = U32(e + k + 4), size = U32(e + k + 8), unpacked = U32(e + k + 12);
                        if (tpkStart + off + size > b.size() || unpacked > (64u << 20))
                        {
                            failed++;
                            continue;
                        }
                        std::vector<uint8_t> blob;
                        std::string error;
                        if (!ea::DecompressBlocks(b.data() + tpkStart + off, size, unpacked, blob, error))
                        {
                            if (failed++ < 5)
                                fprintf(stderr, "texpack: %s texture %08x: %s\n", pack.c_str(), hash, error.c_str());
                            continue;
                        }
                        // The record: its hash, in the trailer.
                        const uint8_t* found = nullptr;
                        size_t trailer = blob.size() > 0xC0 ? blob.size() - 0xC0 : 0;
                        for (size_t q = trailer; q + 4 <= blob.size(); q += 4)
                            if (U32(blob.data() + q) == hash) { found = blob.data() + q; break; }
                        Record r;
                        if (!found || !ParseRecord(found, blob.size() - size_t(found - blob.data()), r))
                        {
                            failed++;
                            continue;
                        }
                        if (r.dataOffset >= blob.size())
                            r.dataOffset = 0;
                        Add(r, blob.data() + r.dataOffset, blob.size() - r.dataOffset, pack);
                    }
                }
            }
        }
    };

    int Scan(const fs::path& game, const fs::path& work, uint32_t minSide)
    {
        fs::path nfs = game / "NFS";
        std::ifstream zdir(nfs / "ZDIR.BIN", std::ios::binary);
        if (!zdir)
        {
            fprintf(stderr, "texpack: no %s\n", (nfs / "ZDIR.BIN").c_str());
            return 1;
        }
        std::vector<uint8_t> dir((std::istreambuf_iterator<char>(zdir)), std::istreambuf_iterator<char>());
        fs::create_directories(work / "src");
        Scanner s;
        s.work = work;
        s.minSide = minSide;
        size_t entries = dir.size() / 24;
        std::map<uint32_t, std::ifstream> archives;
        std::vector<uint8_t> file;
        for (size_t i = 0; i < entries; i++)
        {
            const uint8_t* e = dir.data() + i * 24;
            uint32_t archive = U32(e + 4), sector = U32(e + 8), size = U32(e + 16);
            auto& f = archives[archive];
            if (!f.is_open())
                f.open(nfs / std::format("ZZDATA{}.BIN", archive), std::ios::binary);
            if (!f || size == 0)
                continue;
            file.resize(size);
            f.seekg(std::streamoff(sector) * 0x800);
            f.read(reinterpret_cast<char*>(file.data()), size);
            if (!f)
            {
                f.clear();
                continue;
            }
            // Whole files can be compressed too (one JDLZ/HUFF/RAWW blob, or a
            // 0x55441122 block chain).
            if (file.size() >= 16 && (!memcmp(file.data(), "JDLZ", 4) || !memcmp(file.data(), "HUFF", 4) || !memcmp(file.data(), "RAWW", 4)))
            {
                std::vector<uint8_t> unpacked;
                std::string error;
                if (ea::Decompress(file.data(), file.size(), unpacked, error))
                    file = std::move(unpacked);
            }
            else if (file.size() >= 24 && U32(file.data()) == 0x55441122u)
            {
                // A streaming bundle: sections, each a run of blocks (offsets
                // from the section's start), with padding between them.
                size_t p = 0, sections = 0;
                while (p + 24 <= file.size())
                {
                    if (U32(file.data() + p) != 0x55441122u)
                    {
                        p = (p + 0x80) & ~size_t(0x7F);
                        continue;
                    }
                    size_t runStart = p, total = 0;
                    while (p + 24 <= file.size() && U32(file.data() + p) == 0x55441122u)
                    {
                        uint32_t us = U32(file.data() + p + 4), cs = U32(file.data() + p + 8), uo = U32(file.data() + p + 12);
                        if (cs < 24 || p + cs > file.size()) { p = file.size(); break; }
                        total = std::max<size_t>(total, size_t(uo) + us);
                        p += cs;
                    }
                    std::vector<uint8_t> unpacked;
                    std::string error;
                    if (total && total < (512u << 20) && ea::DecompressBlocks(file.data() + runStart, p - runStart, total, unpacked, error))
                    {
                        s.ScanFile(unpacked);
                        sections++;
                    }
                }
                printf("progress %zu %zu\n", i + 1, entries);
                fflush(stdout);
                continue;
            }
            s.ScanFile(file);
            printf("progress %zu %zu\n", i + 1, entries);
            fflush(stdout);
        }
        std::ofstream manifest(work / "manifest.tsv", std::ios::trunc);
        manifest << "# key\tgpu_format\twidth\theight\tbc\tname\tpack\n";
        for (const Tex& t : s.textures)
            manifest << std::format("{:016x}\t{}\t{}\t{}\t{}\t{}\t{}\n", t.key, t.gpuFormat, t.width, t.height, t.bc, t.name, t.pack);
        fprintf(stderr, "texpack: %zu textures to upscale (%zu normal maps and %zu small or other-format ones left out, %zu unreadable)\n",
            s.textures.size(), s.normalMaps, s.skipped, s.failed);
        printf("done %zu\n", s.textures.size());
        return 0;
    }

    int Encode(const fs::path& work, const fs::path& upscaled, const fs::path& pack, uint32_t maxSide, unsigned jobs)
    {
        std::ifstream manifest(work / "manifest.tsv");
        if (!manifest)
        {
            fprintf(stderr, "texpack: no %s (run scan first)\n", (work / "manifest.tsv").c_str());
            return 1;
        }
        struct Item { std::string key; int bc; uint32_t width, height; };
        std::vector<Item> items;
        for (std::string line; std::getline(manifest, line);)
        {
            if (line.empty() || line[0] == '#')
                continue;
            std::istringstream ss(line);
            Item it;
            uint32_t fmt;
            ss >> it.key >> fmt >> it.width >> it.height >> it.bc;
            if (fs::exists(upscaled / (it.key + ".png")))
                items.push_back(it);
        }
        fs::create_directories(pack);
        std::atomic<size_t> next{ 0 }, done{ 0 }, failed{ 0 };
        std::mutex outMutex;
        auto worker = [&] {
            for (size_t i; (i = next++) < items.size();)
            {
                const Item& it = items[i];
                img::Rgba image;
                std::string error;
                bool ok = img::ReadPng(upscaled / (it.key + ".png"), image, error);
                if (ok)
                {
                    image = img::Fit(image, maxSide);
                    // Whole 4x4 blocks: crop to a multiple of 4 (upscales of power-of-two sizes already are).
                    uint32_t w = image.width & ~3u, h = image.height & ~3u;
                    if (w < 4 || h < 4)
                        ok = false, error = "too small";
                    else if (w != image.width || h != image.height)
                    {
                        img::Rgba cropped;
                        cropped.width = w;
                        cropped.height = h;
                        cropped.px.resize(size_t(w) * h * 4);
                        for (uint32_t y = 0; y < h; y++)
                            memcpy(&cropped.px[size_t(y) * w * 4], &image.px[size_t(y) * image.width * 4], size_t(w) * 4);
                        image = std::move(cropped);
                    }
                }
                if (ok)
                    ok = img::WriteDds(pack / (it.key + ".dds"), image, it.bc == 1 ? 1 : 3, it.bc == 1, error);
                std::lock_guard lock(outMutex);
                if (!ok && failed++ < 5)
                    fprintf(stderr, "texpack: %s: %s\n", it.key.c_str(), error.c_str());
                printf("progress %zu %zu\n", ++done, items.size());
                fflush(stdout);
            }
        };
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < std::max(1u, jobs); t++)
            threads.emplace_back(worker);
        for (auto& t : threads)
            t.join();
        fprintf(stderr, "texpack: %zu textures encoded into %s (%zu failed)\n", items.size() - failed, pack.c_str(), size_t(failed));
        printf("done %zu\n", items.size() - failed);
        return failed == items.size() && !items.empty() ? 1 : 0;
    }
}

int main(int argc, char** argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    auto option = [&](const char* name, uint32_t fallback) {
        for (size_t i = 0; i + 1 < args.size(); i++)
            if (args[i] == name)
            {
                uint32_t v = uint32_t(std::stoul(args[i + 1]));
                args.erase(args.begin() + i, args.begin() + i + 2);
                return v;
            }
        return fallback;
    };
    uint32_t minSide = option("--min", 32), maxSide = option("--max", 4096);
    unsigned jobs = option("--jobs", std::max(1u, std::thread::hardware_concurrency()));
    if (args.size() == 3 && args[0] == "scan")
        return Scan(args[1], args[2], minSide);
    if (args.size() == 4 && args[0] == "encode")
        return Encode(args[1], args[2], args[3], maxSide, jobs);
    fprintf(stderr,
        "usage: sb-texpack scan <game files> <work dir> [--min N]\n"
        "       sb-texpack encode <work dir> <upscaled dir> <pack dir> [--max N] [--jobs N]\n");
    return 2;
}
