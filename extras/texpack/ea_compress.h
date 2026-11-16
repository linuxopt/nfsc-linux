// SpeedBreaker texpack. GPL-3.0-or-later (see COPYING).
//
// EA Black Box compression, as NFS: Carbon's Xbox 360 texture packs use it:
//   - "JDLZ" (LZ77 with two flag streams),
//   - "HUFF" (EA's compression library stream 0x30FB, Huffman with run
//     length; ported from 47PADO47/nfs-mw's libs/ea-compress (MIT OR Apache-2.0, used under MIT),
//     written there from its own spec, docs/formats/huff.md),
//   - "RAWW" (stored),
// and the 0x55441122 block chain a compressed texture pack entry is made of.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ea
{
    // One JDLZ, HUFF or RAWW blob (its 16-byte header first). False (with
    // `error`) on anything malformed.
    bool Decompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error);

    // A compressed texture's blocks: 24-byte headers (0x55441122, unpacked
    // size, stored size with its header, unpacked offset, stored offset, 0),
    // each followed by a blob. Writes each block at its unpacked offset.
    bool DecompressBlocks(const uint8_t* data, size_t size, size_t outSize, std::vector<uint8_t>& out, std::string& error);
}
