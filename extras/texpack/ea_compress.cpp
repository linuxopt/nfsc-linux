// SpeedBreaker texpack. GPL-3.0-or-later (see COPYING).
//
// The HUFF decoder below is a C++ port of libs/ea-compress/src/huff/ from
// https://github.com/47PADO47/nfs-mw (MIT OR Apache-2.0; taken here under
// the MIT license; its spec is that repository's docs/formats/huff.md). The
// JDLZ decoder follows the same repository's jdlz.rs. Copyright (c) 2026 the
// nfs-mw contributors; see THIRD_PARTY_NOTICES.md and
// scripts/appimage/licenses/ea-compress-MIT.txt.
#include "ea_compress.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace ea
{
    namespace
    {
        uint32_t U32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

        bool Jdlz(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error)
        {
            if (size < 16 || data[4] != 0x02 || data[5] != 0x10)
            {
                error = "JDLZ: bad header";
                return false;
            }
            size_t outLen = U32(data + 8), end = std::min<size_t>(U32(data + 12), size);
            out.clear();
            out.reserve(outLen);
            size_t ip = 16;
            uint32_t flags1 = 1, flags2 = 1;
            while (ip < end && out.size() < outLen)
            {
                if (flags1 == 1)
                {
                    if (ip >= end) break;
                    flags1 = data[ip++] | 0x100;
                }
                if (flags2 == 1)
                {
                    if (ip >= end) break;
                    flags2 = data[ip++] | 0x100;
                }
                if (flags1 & 1)
                {
                    if (ip + 2 > end)
                    {
                        error = "JDLZ: back-reference past the end";
                        return false;
                    }
                    size_t b0 = data[ip], b1 = data[ip + 1];
                    ip += 2;
                    size_t length, distance;
                    if (flags2 & 1)
                    {
                        length = (((b0 & 0xF0) << 4) | b1) + 3;
                        distance = (b0 & 0x0F) + 1;
                    }
                    else
                    {
                        length = (b0 & 0x1F) + 3;
                        distance = (((b0 & 0xE0) << 3) | b1) + 17;
                    }
                    flags2 >>= 1;
                    if (distance > out.size())
                    {
                        error = "JDLZ: back-reference before the start";
                        return false;
                    }
                    length = std::min(length, outLen - out.size());
                    size_t start = out.size() - distance;
                    for (size_t i = 0; i < length; i++)
                        out.push_back(out[start + i]);
                }
                else
                {
                    out.push_back(data[ip++]);
                }
                flags1 >>= 1;
            }
            if (out.size() != outLen)
            {
                error = std::format("JDLZ: {} of {} bytes", out.size(), outLen);
                return false;
            }
            return true;
        }

        // ---- HUFF (see the file comment) ----

        struct BitReader
        {
            const uint8_t* input;
            size_t size;
            size_t next = 0;
            uint64_t buf = 0;
            uint32_t avail = 0;
            bool overrun = false;

            uint32_t Peek(uint32_t n)
            {
                if (avail < n)
                    while (avail <= 56)
                    {
                        uint8_t byte = next < size ? input[next] : 0;
                        buf |= uint64_t(byte) << (56 - avail);
                        next++;
                        avail += 8;
                    }
                return uint32_t(buf >> (64 - n));
            }
            void Skip(uint32_t n)
            {
                buf <<= n;
                avail -= n;
                if (BitPos() > size * 8)
                    overrun = true;
            }
            uint32_t Bits(uint32_t n)
            {
                uint32_t v = Peek(n);
                Skip(n);
                return v;
            }
            // z zero bits, a one bit, then z + 2 value bits v: v + 2^(z+2) - 4.
            bool Number(uint32_t& out)
            {
                uint32_t window = Peek(32);
                uint32_t zeros = window ? uint32_t(__builtin_clz(window)) : 32;
                if (zeros > 15)
                    return false;
                Skip(zeros + 1);
                uint32_t width = zeros + 2;
                out = Bits(width) + (1u << width) - 4;
                return !overrun;
            }
            size_t BitPos() const { return next * 8 - avail; }
        };

        constexpr uint32_t kMaxCodeLen = 16, kFastBits = 10;

        struct Code
        {
            std::vector<uint8_t> symbols;
            uint32_t first[kMaxCodeLen + 1] = {}, count[kMaxCodeLen + 1] = {}, index[kMaxCodeLen + 1] = {};
            uint32_t maxLen = 0;
            std::vector<uint16_t> fast = std::vector<uint16_t>(1u << kFastBits, 0);

            bool Read(BitReader& bits, std::string& error)
            {
                uint32_t next = 0, total = 0, len = 0;
                while (next != (1u << len))
                {
                    len++;
                    if (len > kMaxCodeLen)
                    {
                        error = "HUFF: code lengths do not fill the code space";
                        return false;
                    }
                    uint32_t n;
                    if (!bits.Number(n))
                    {
                        error = "HUFF: bad code length";
                        return false;
                    }
                    next <<= 1;
                    first[len] = next;
                    count[len] = n;
                    index[len] = total;
                    next += n;
                    total += n;
                    if (next > (1u << len) || total > 256)
                    {
                        error = "HUFF: too many codes";
                        return false;
                    }
                }
                maxLen = len;
                bool used[256] = {};
                uint8_t value = 0xFF;
                for (uint32_t i = 0; i < total; i++)
                {
                    uint32_t skip;
                    if (!bits.Number(skip))
                    {
                        error = "HUFF: bad symbol";
                        return false;
                    }
                    skip %= (256 - i);
                    for (;;)
                    {
                        value = uint8_t(value + 1);
                        if (!used[value])
                        {
                            if (skip == 0)
                                break;
                            skip--;
                        }
                    }
                    used[value] = true;
                    symbols.push_back(value);
                }
                for (uint32_t l = 1; l <= std::min(maxLen, kFastBits); l++)
                {
                    uint32_t shift = kFastBits - l;
                    for (uint32_t i = 0; i < count[l]; i++)
                    {
                        uint32_t c = first[l] + i;
                        uint16_t entry = uint16_t(symbols[index[l] + i] | (l << 8));
                        std::fill(fast.begin() + (c << shift), fast.begin() + ((c + 1) << shift), entry);
                    }
                }
                return true;
            }

            bool Decode(BitReader& bits, uint8_t& out) const
            {
                uint16_t entry = fast[bits.Peek(kFastBits)];
                if (entry)
                {
                    bits.Skip(entry >> 8);
                    out = uint8_t(entry);
                    return !bits.overrun;
                }
                uint32_t window = bits.Peek(kMaxCodeLen);
                for (uint32_t l = kFastBits + 1; l <= maxLen; l++)
                {
                    uint32_t offset = (window >> (kMaxCodeLen - l)) - first[l];
                    if (offset < count[l])
                    {
                        bits.Skip(l);
                        out = symbols[index[l] + offset];
                        return !bits.overrun;
                    }
                }
                return false;
            }
        };

        bool Huff(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error)
        {
            if (size < 16 || data[4] != 0x01 || data[5] != 0x10)
            {
                error = "HUFF: bad header";
                return false;
            }
            size_t expected = U32(data + 8);
            size_t end = std::min<size_t>(16 + size_t(U32(data + 12)), size);
            BitReader bits{ data + 16, end - 16 };
            uint32_t id = bits.Bits(16);
            int filter;
            switch (id & ~0x8100u)
            {
            case 0x30FB: filter = 0; break;
            case 0x32FB: filter = 1; break;
            case 0x34FB: filter = 2; break;
            default: error = std::format("HUFF: stream type {:04X}", id); return false;
            }
            uint32_t width = (id & 0x8000) ? 32 : 24;
            if (id & 0x0100)
                bits.Bits(width);
            size_t streamSize = bits.Bits(width);
            if (streamSize != expected)
            {
                error = "HUFF: stream size doesn't match the header";
                return false;
            }
            uint8_t clue = uint8_t(bits.Bits(8));
            Code code;
            if (!code.Read(bits, error))
                return false;
            out.clear();
            out.reserve(expected);
            for (;;)
            {
                uint8_t byte;
                if (!code.Decode(bits, byte))
                {
                    error = "HUFF: invalid code";
                    return false;
                }
                if (byte != clue)
                {
                    if (out.size() == expected)
                    {
                        error = "HUFF: output past the size";
                        return false;
                    }
                    out.push_back(byte);
                    continue;
                }
                uint32_t run;
                if (!bits.Number(run))
                {
                    error = "HUFF: bad run";
                    return false;
                }
                if (run > 0)
                {
                    if (out.empty() || run > expected - out.size())
                    {
                        error = "HUFF: bad run";
                        return false;
                    }
                    out.resize(out.size() + run, out.back());
                }
                else if (bits.Bits(1) == 1)
                    break;
                else
                {
                    if (out.size() == expected)
                    {
                        error = "HUFF: output past the size";
                        return false;
                    }
                    out.push_back(uint8_t(bits.Bits(8)));
                }
                if (bits.overrun)
                {
                    error = "HUFF: stream ended early";
                    return false;
                }
            }
            if (out.size() != expected)
            {
                error = "HUFF: stream ended before the size";
                return false;
            }
            if (filter == 1)
            {
                uint8_t sum = 0;
                for (uint8_t& b : out) b = sum = uint8_t(sum + b);
            }
            else if (filter == 2)
            {
                uint8_t delta = 0, sum = 0;
                for (uint8_t& b : out)
                {
                    delta = uint8_t(delta + b);
                    b = sum = uint8_t(sum + delta);
                }
            }
            return true;
        }
    }

    bool Decompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error)
    {
        if (size < 16)
        {
            error = "blob too short";
            return false;
        }
        if (memcmp(data, "JDLZ", 4) == 0)
            return Jdlz(data, size, out, error);
        if (memcmp(data, "HUFF", 4) == 0)
            return Huff(data, size, out, error);
        if (memcmp(data, "RAWW", 4) == 0)
        {
            size_t n = U32(data + 8);
            if (16 + n > size)
            {
                error = "RAWW: cut short";
                return false;
            }
            out.assign(data + 16, data + 16 + n);
            return true;
        }
        error = std::format("unknown compression {:.4}", reinterpret_cast<const char*>(data));
        return false;
    }

    bool DecompressBlocks(const uint8_t* data, size_t size, size_t outSize, std::vector<uint8_t>& out, std::string& error)
    {
        out.assign(outSize, 0);
        size_t p = 0, written = 0;
        std::vector<uint8_t> block;
        while (p + 24 <= size && written < outSize)
        {
            if (U32(data + p) != 0x55441122)
                break;  // padding after the last block
            uint32_t uSize = U32(data + p + 4), cSize = U32(data + p + 8), uOffset = U32(data + p + 12);
            if (cSize < 24 || p + cSize > size)
            {
                error = "block cut short";
                return false;
            }
            if (!Decompress(data + p + 24, cSize - 24, block, error))
                return false;
            if (block.size() != uSize)
            {
                error = "block size mismatch";
                return false;
            }
            // uOffset: where the block's bytes go. (The chain is rotated: the
            // block for offset 0 comes last.)
            if (size_t(uOffset) + uSize > outSize)
            {
                error = std::format("block offset {:#x} size {:#x} outside {:#x}", uOffset, uSize, outSize);
                return false;
            }
            memcpy(out.data() + uOffset, block.data(), uSize);
            written += uSize;
            p += cSize;
        }
        if (written != outSize)
        {
            error = std::format("blocks gave {} of {} bytes", written, outSize);
            return false;
        }
        return true;
    }
}
