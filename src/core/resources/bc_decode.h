#pragma once

#include <cstddef>
#include <cstdint>

namespace f4::resources {

// Per-block BC decoders. Each decodes one 4x4 block into `out`.
// These are pure, allocation-free, and reused by DDS (M5.5) and KTX2 (M5.6).

// BC1 (DXT1): 8-byte block -> 16 RGBA8 pixels (16*4 bytes).
void decodeBC1Block(const std::byte* block, std::uint8_t* rgba);
// BC2 (DXT3): 16-byte block -> 16 RGBA8 pixels.
void decodeBC2Block(const std::byte* block, std::uint8_t* rgba);
// BC3 (DXT5): 16-byte block -> 16 RGBA8 pixels.
void decodeBC3Block(const std::byte* block, std::uint8_t* rgba);
// BC4: 8-byte block -> 16 single-channel values (16 bytes).
void decodeBC4Block(const std::byte* block, std::uint8_t* r);
// BC5: 16-byte block -> 16 two-channel values (16*2 bytes).
void decodeBC5Block(const std::byte* block, std::uint8_t* rg);
// BC6H: 16-byte block -> 16 RGB float triples (16*3 floats).
void decodeBC6HBlock(const std::byte* block, float* rgb, bool isSigned);
// BC7: 16-byte block -> 16 RGBA8 pixels (16*4 bytes).
void decodeBC7Block(const std::byte* block, std::uint8_t* rgba);

} // namespace f4::resources
