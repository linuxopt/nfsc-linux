// SpeedBreaker texpack. GPL-3.0-or-later (see COPYING).
#include "image_io.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace img
{
    namespace
    {
        void PutBe32(std::vector<uint8_t>& v, uint32_t x)
        {
            v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
        }
        uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

        void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
        {
            PutBe32(out, uint32_t(data.size()));
            size_t at = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), data.begin(), data.end());
            PutBe32(out, uint32_t(crc32(0, out.data() + at, uInt(out.size() - at))));
        }

        uint8_t Paeth(int a, int b, int c)
        {
            int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
            return uint8_t(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
        }

        uint16_t To565(const float c[3])
        {
            int r = std::clamp(int(std::lround(c[0] * 31.0f / 255.0f)), 0, 31);
            int g = std::clamp(int(std::lround(c[1] * 63.0f / 255.0f)), 0, 63);
            int b = std::clamp(int(std::lround(c[2] * 31.0f / 255.0f)), 0, 31);
            return uint16_t(r << 11 | g << 5 | b);
        }
        void From565(uint16_t v, int c[3])
        {
            int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            c[0] = (r << 3) | (r >> 2);
            c[1] = (g << 2) | (g >> 4);
            c[2] = (b << 3) | (b >> 2);
        }

        // BC1 colour part of a block: 8 bytes. threeColor: index 3 is transparent.
        void EncodeColor(const uint8_t in[64], bool threeColor, uint8_t* out)
        {
            // Texels that count (transparent ones in three-colour mode don't).
            float pts[16][3];
            int n = 0;
            for (int i = 0; i < 16; i++)
                if (!threeColor || in[i * 4 + 3] >= 128)
                {
                    for (int k = 0; k < 3; k++) pts[n][k] = in[i * 4 + k];
                    n++;
                }
            uint16_t c0 = 0, c1 = 0;
            if (n > 0)
            {
                float mean[3] = {};
                for (int i = 0; i < n; i++) for (int k = 0; k < 3; k++) mean[k] += pts[i][k];
                for (float& m : mean) m /= float(n);
                float cov[6] = {};
                for (int i = 0; i < n; i++)
                {
                    float d[3] = { pts[i][0] - mean[0], pts[i][1] - mean[1], pts[i][2] - mean[2] };
                    cov[0] += d[0] * d[0]; cov[1] += d[0] * d[1]; cov[2] += d[0] * d[2];
                    cov[3] += d[1] * d[1]; cov[4] += d[1] * d[2]; cov[5] += d[2] * d[2];
                }
                float axis[3] = { 0.9f, 1.0f, 0.7f };
                for (int it = 0; it < 8; it++)
                {
                    float a[3] = { cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2],
                                   cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                                   cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2] };
                    float len = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
                    if (len < 1e-6f) break;
                    for (int k = 0; k < 3; k++) axis[k] = a[k] / len;
                }
                float lo = 1e9f, hi = -1e9f;
                for (int i = 0; i < n; i++)
                {
                    float t = (pts[i][0] - mean[0]) * axis[0] + (pts[i][1] - mean[1]) * axis[1] + (pts[i][2] - mean[2]) * axis[2];
                    lo = std::min(lo, t); hi = std::max(hi, t);
                }
                float e0[3], e1[3];
                for (int k = 0; k < 3; k++) { e0[k] = mean[k] + axis[k] * hi; e1[k] = mean[k] + axis[k] * lo; }
                // One least-squares refinement of the endpoints over the chosen weights.
                const float w4[4] = { 0.0f, 1.0f, 1.0f / 3.0f, 2.0f / 3.0f };  // weight of e1 for indices 0..3
                for (int pass = 0; pass < 2; pass++)
                {
                    float aa = 0, bb = 0, ab = 0, ax[3] = {}, bx[3] = {};
                    for (int i = 0; i < n; i++)
                    {
                        float best = 1e30f; int bi = 0;
                        for (int j = 0; j < (threeColor ? 3 : 4); j++)
                        {
                            float w = threeColor ? (j == 2 ? 0.5f : w4[j]) : w4[j];
                            float d = 0;
                            for (int k = 0; k < 3; k++) { float c = e0[k] * (1 - w) + e1[k] * w - pts[i][k]; d += c * c; }
                            if (d < best) { best = d; bi = j; }
                        }
                        float w = threeColor ? (bi == 2 ? 0.5f : w4[bi]) : w4[bi];
                        float a = 1 - w, b = w;
                        aa += a * a; bb += b * b; ab += a * b;
                        for (int k = 0; k < 3; k++) { ax[k] += a * pts[i][k]; bx[k] += b * pts[i][k]; }
                    }
                    float det = aa * bb - ab * ab;
                    if (std::fabs(det) < 1e-4f) break;
                    for (int k = 0; k < 3; k++)
                    {
                        e0[k] = std::clamp((ax[k] * bb - bx[k] * ab) / det, 0.0f, 255.0f);
                        e1[k] = std::clamp((bx[k] * aa - ax[k] * ab) / det, 0.0f, 255.0f);
                    }
                }
                c0 = To565(e0);
                c1 = To565(e1);
            }
            // Four-colour mode needs c0 > c1; three-colour mode c0 <= c1.
            if (threeColor ? c0 > c1 : c0 < c1) std::swap(c0, c1);
            if (!threeColor && c0 == c1)
            {
                if (c1 > 0) c1--; else c0++;
            }
            int p[4][3];
            From565(c0, p[0]);
            From565(c1, p[1]);
            for (int k = 0; k < 3; k++)
            {
                if (threeColor) { p[2][k] = (p[0][k] + p[1][k]) / 2; p[3][k] = 0; }
                else { p[2][k] = (2 * p[0][k] + p[1][k]) / 3; p[3][k] = (p[0][k] + 2 * p[1][k]) / 3; }
            }
            uint32_t idx = 0;
            for (int i = 0; i < 16; i++)
            {
                int bi = 0;
                if (threeColor && in[i * 4 + 3] < 128)
                    bi = 3;
                else
                {
                    int best = 1 << 30;
                    for (int j = 0; j < (threeColor ? 3 : 4); j++)
                    {
                        int d = 0;
                        for (int k = 0; k < 3; k++) { int c = p[j][k] - in[i * 4 + k]; d += c * c; }
                        if (d < best) { best = d; bi = j; }
                    }
                }
                idx |= uint32_t(bi) << (i * 2);
            }
            memcpy(out, &c0, 2);
            memcpy(out + 2, &c1, 2);
            memcpy(out + 4, &idx, 4);
        }

        void EncodeAlpha(const uint8_t in[64], uint8_t* out)
        {
            int lo = 255, hi = 0;
            for (int i = 0; i < 16; i++) { lo = std::min<int>(lo, in[i * 4 + 3]); hi = std::max<int>(hi, in[i * 4 + 3]); }
            out[0] = uint8_t(hi);
            out[1] = uint8_t(lo);
            int pal[8] = { hi, lo };
            for (int j = 1; j < 7; j++) pal[j + 1] = hi == lo ? hi : ((7 - j) * hi + j * lo) / 7;
            uint64_t bits = 0;
            for (int i = 0; i < 16; i++)
            {
                int best = 1 << 30, bi = 0;
                for (int j = 0; j < 8; j++)
                {
                    int d = std::abs(pal[j] - in[i * 4 + 3]);
                    if (d < best) { best = d; bi = j; }
                }
                bits |= uint64_t(bi) << (i * 3);
            }
            for (int k = 0; k < 6; k++) out[2 + k] = uint8_t(bits >> (k * 8));
        }
    }

    bool WritePng(const std::filesystem::path& path, const Rgba& image)
    {
        std::vector<uint8_t> raw;
        raw.reserve(size_t(image.width * 4 + 1) * image.height);
        for (uint32_t y = 0; y < image.height; y++)
        {
            raw.push_back(0);
            raw.insert(raw.end(), image.px.begin() + size_t(y) * image.width * 4, image.px.begin() + size_t(y + 1) * image.width * 4);
        }
        uLongf zlen = compressBound(uLong(raw.size()));
        std::vector<uint8_t> z(zlen);
        if (compress2(z.data(), &zlen, raw.data(), uLong(raw.size()), 6) != Z_OK)
            return false;
        z.resize(zlen);
        std::vector<uint8_t> out = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        std::vector<uint8_t> ihdr;
        PutBe32(ihdr, image.width);
        PutBe32(ihdr, image.height);
        ihdr.insert(ihdr.end(), { 8, 6, 0, 0, 0 });
        Chunk(out, "IHDR", ihdr);
        Chunk(out, "IDAT", z);
        Chunk(out, "IEND", {});
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
        return bool(f);
    }

    bool ReadPng(const std::filesystem::path& path, Rgba& image, std::string& error)
    {
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (file.size() < 8 || memcmp(file.data(), "\x89PNG\r\n\x1a\n", 8) != 0)
        {
            error = "not a PNG";
            return false;
        }
        uint32_t w = 0, h = 0;
        int depth = 0, type = 0, interlace = 0;
        std::vector<uint8_t> idat;
        for (size_t p = 8; p + 12 <= file.size();)
        {
            uint32_t len = Be32(&file[p]);
            if (p + 12 + len > file.size()) break;
            const uint8_t* d = &file[p + 8];
            if (!memcmp(&file[p + 4], "IHDR", 4))
            {
                w = Be32(d); h = Be32(d + 4); depth = d[8]; type = d[9]; interlace = d[12];
            }
            else if (!memcmp(&file[p + 4], "IDAT", 4))
                idat.insert(idat.end(), d, d + len);
            else if (!memcmp(&file[p + 4], "IEND", 4))
                break;
            p += 12 + len;
        }
        int channels = type == 6 ? 4 : type == 2 ? 3 : type == 4 ? 2 : type == 0 ? 1 : 0;
        if (depth != 8 || channels == 0 || interlace != 0 || w == 0 || h == 0)
        {
            error = "a PNG kind this doesn't read (8-bit, not interlaced, not paletted)";
            return false;
        }
        size_t stride = size_t(w) * channels;
        std::vector<uint8_t> raw((stride + 1) * h);
        uLongf rawLen = uLongf(raw.size());
        if (uncompress(raw.data(), &rawLen, idat.data(), uLong(idat.size())) != Z_OK || rawLen != raw.size())
        {
            error = "PNG data doesn't inflate";
            return false;
        }
        std::vector<uint8_t> prev(stride, 0), cur(stride);
        image.width = w;
        image.height = h;
        image.px.assign(size_t(w) * h * 4, 255);
        for (uint32_t y = 0; y < h; y++)
        {
            const uint8_t* row = &raw[y * (stride + 1)];
            uint8_t filter = row[0];
            for (size_t x = 0; x < stride; x++)
            {
                int a = x >= size_t(channels) ? cur[x - channels] : 0, b = prev[x], c = x >= size_t(channels) ? prev[x - channels] : 0;
                uint8_t v = row[1 + x];
                switch (filter)
                {
                case 1: v = uint8_t(v + a); break;
                case 2: v = uint8_t(v + b); break;
                case 3: v = uint8_t(v + (a + b) / 2); break;
                case 4: v = uint8_t(v + Paeth(a, b, c)); break;
                default: break;
                }
                cur[x] = v;
            }
            for (uint32_t x = 0; x < w; x++)
            {
                uint8_t* o = &image.px[(size_t(y) * w + x) * 4];
                const uint8_t* s = &cur[size_t(x) * channels];
                if (channels >= 3) { o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; if (channels == 4) o[3] = s[3]; }
                else { o[0] = o[1] = o[2] = s[0]; if (channels == 2) o[3] = s[1]; }
            }
            std::swap(prev, cur);
        }
        return true;
    }

    void DecodeBlock(int bc, const uint8_t* block, uint8_t out[64])
    {
        const uint8_t* color = bc == 1 ? block : block + 8;
        uint16_t c0, c1;
        memcpy(&c0, color, 2);
        memcpy(&c1, color + 2, 2);
        uint32_t idx;
        memcpy(&idx, color + 4, 4);
        int p[4][4];
        From565(c0, p[0]);
        From565(c1, p[1]);
        p[0][3] = p[1][3] = p[2][3] = p[3][3] = 255;
        for (int k = 0; k < 3; k++)
        {
            if (bc != 1 || c0 > c1) { p[2][k] = (2 * p[0][k] + p[1][k]) / 3; p[3][k] = (p[0][k] + 2 * p[1][k]) / 3; }
            else { p[2][k] = (p[0][k] + p[1][k]) / 2; p[3][k] = 0; }
        }
        if (bc == 1 && c0 <= c1)
            p[3][3] = 0;
        for (int i = 0; i < 16; i++)
            for (int k = 0; k < 4; k++)
                out[i * 4 + k] = uint8_t(p[(idx >> (i * 2)) & 3][k]);
        if (bc == 2)
        {
            for (int i = 0; i < 16; i++)
            {
                int a = (block[i / 2] >> ((i & 1) * 4)) & 15;
                out[i * 4 + 3] = uint8_t(a * 17);
            }
        }
        else if (bc == 3)
        {
            int a0 = block[0], a1 = block[1], pal[8] = { a0, a1 };
            if (a0 > a1) for (int j = 1; j < 7; j++) pal[j + 1] = ((7 - j) * a0 + j * a1) / 7;
            else { for (int j = 1; j < 5; j++) pal[j + 1] = ((5 - j) * a0 + j * a1) / 5; pal[6] = 0; pal[7] = 255; }
            uint64_t bits = 0;
            for (int k = 0; k < 6; k++) bits |= uint64_t(block[2 + k]) << (k * 8);
            for (int i = 0; i < 16; i++)
                out[i * 4 + 3] = uint8_t(pal[(bits >> (i * 3)) & 7]);
        }
    }

    void EncodeBlock(int bc, const uint8_t in[64], bool alpha1, uint8_t* block)
    {
        if (bc == 1)
        {
            bool anyClear = false;
            if (alpha1)
                for (int i = 0; i < 16; i++) anyClear |= in[i * 4 + 3] < 128;
            EncodeColor(in, anyClear, block);
            return;
        }
        EncodeAlpha(in, block);
        EncodeColor(in, false, block + 8);
    }

    Rgba Half(const Rgba& s)
    {
        Rgba d;
        d.width = std::max(1u, s.width / 2);
        d.height = std::max(1u, s.height / 2);
        d.px.resize(size_t(d.width) * d.height * 4);
        for (uint32_t y = 0; y < d.height; y++)
            for (uint32_t x = 0; x < d.width; x++)
                for (int k = 0; k < 4; k++)
                {
                    uint32_t x0 = std::min(x * 2, s.width - 1), x1 = std::min(x * 2 + 1, s.width - 1);
                    uint32_t y0 = std::min(y * 2, s.height - 1), y1 = std::min(y * 2 + 1, s.height - 1);
                    int sum = s.px[(size_t(y0) * s.width + x0) * 4 + k] + s.px[(size_t(y0) * s.width + x1) * 4 + k] +
                              s.px[(size_t(y1) * s.width + x0) * 4 + k] + s.px[(size_t(y1) * s.width + x1) * 4 + k];
                    d.px[(size_t(y) * d.width + x) * 4 + k] = uint8_t((sum + 2) / 4);
                }
        return d;
    }

    Rgba Fit(const Rgba& image, uint32_t maxSide)
    {
        Rgba r = image;
        while (std::max(r.width, r.height) > maxSide && r.width > 1 && r.height > 1)
            r = Half(r);
        return r;
    }

    bool WriteDds(const std::filesystem::path& path, const Rgba& base, int bc, bool alpha1, std::string& error)
    {
        if (base.width % 4 || base.height % 4)
        {
            error = "size not a multiple of 4";
            return false;
        }
        std::vector<Rgba> levels{ base };
        while (levels.back().width > 4 && levels.back().height > 4)
            levels.push_back(Half(levels.back()));
        uint32_t blockBytes = bc == 1 ? 8 : 16;
        std::vector<uint8_t> out(128, 0);
        auto put = [&](size_t at, uint32_t v) { memcpy(&out[at], &v, 4); };
        memcpy(&out[0], "DDS ", 4);
        put(4, 124);
        put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000);  // caps, height, width, pixelformat, mipmapcount, linearsize
        put(12, base.height);
        put(16, base.width);
        put(20, ((base.width + 3) / 4) * ((base.height + 3) / 4) * blockBytes);
        put(28, uint32_t(levels.size()));
        put(76, 32);
        put(80, 0x4);  // fourcc
        memcpy(&out[84], bc == 1 ? "DXT1" : "DXT5", 4);
        put(108, 0x1000 | 0x8 | 0x400000);  // texture, complex, mipmap
        uint8_t texels[64];
        for (const Rgba& l : levels)
            for (uint32_t by = 0; by < (l.height + 3) / 4; by++)
                for (uint32_t bx = 0; bx < (l.width + 3) / 4; bx++)
                {
                    for (int i = 0; i < 16; i++)
                    {
                        uint32_t x = std::min(bx * 4 + i % 4, l.width - 1), y = std::min(by * 4 + i / 4, l.height - 1);
                        memcpy(&texels[i * 4], &l.px[(size_t(y) * l.width + x) * 4], 4);
                    }
                    uint8_t block[16];
                    EncodeBlock(bc, texels, alpha1, block);
                    out.insert(out.end(), block, block + blockBytes);
                }
        std::filesystem::path tmp = path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
            if (!f)
            {
                error = "can't write";
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec)
        {
            error = ec.message();
            return false;
        }
        return true;
    }
}
