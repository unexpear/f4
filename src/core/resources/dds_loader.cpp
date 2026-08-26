#include "core/resources/dds_loader.h"

#include <cstring>
#include <vector>

#include "core/resources/bc_decode.h"

namespace f4::resources {

namespace {

constexpr std::uint32_t kDdsMagic = 0x20534444u; // "DDS "
constexpr std::uint32_t kFourCCDXT1 = 0x31545844u; // "DXT1"
constexpr std::uint32_t kFourCCDXT3 = 0x33545844u; // "DXT3"
constexpr std::uint32_t kFourCCDXT5 = 0x35545844u; // "DXT5"
constexpr std::uint32_t kFourCCDX10 = 0x30315844u; // "DX10"
constexpr std::uint32_t kFourCCBC4U = 0x55344342u; // "BC4U"
constexpr std::uint32_t kFourCCBC5U = 0x55354342u; // "BC5U"
constexpr std::uint32_t kFourCCATI1 = 0x31495441u; // "ATI1"
constexpr std::uint32_t kFourCCATI2 = 0x32495441u; // "ATI2"

constexpr std::uint32_t kDdpfFourCC = 0x00000004u;

// DXGI_FORMAT values for BC payloads.
constexpr std::uint32_t kDxgiBC1Unorm = 71;
constexpr std::uint32_t kDxgiBC2Unorm = 74;
constexpr std::uint32_t kDxgiBC3Unorm = 77;
constexpr std::uint32_t kDxgiBC4Unorm = 80;
constexpr std::uint32_t kDxgiBC5Unorm = 83;
constexpr std::uint32_t kDxgiBC6HUF16 = 95;
constexpr std::uint32_t kDxgiBC6HSF16 = 96;
constexpr std::uint32_t kDxgiBC7Unorm = 98;

auto readU32(const std::byte* data, std::size_t off) -> std::uint32_t {
    std::uint32_t v;
    std::memcpy(&v, data + off, 4);
    return v;
}

// Bytes per decoded pixel for a BC format's CPU output.
auto pixelStride(BcFormat format) -> std::size_t {
    switch (format) {
    case BcFormat::BC4: return 1;
    case BcFormat::BC5: return 2;
    case BcFormat::BC6H_UF16:
    case BcFormat::BC6H_SF16: return 16; // RGBA32_FLOAT
    default: return 4;
    }
}

// Resize scratch and return its writable data pointer.
auto scratch(std::vector<std::byte>& buf, std::size_t n) -> std::byte* {
    buf.resize(n);
    return buf.data();
}

// Decode a single mip level of `width` x `height` into out (row-major).
auto decodeMip(BcFormat format, const std::byte* src, std::size_t srcLen,
               std::uint32_t width, std::uint32_t height,
               std::vector<std::byte>& out) -> bool {
    const std::size_t blocksWide = std::max<std::size_t>(1, (width + 3) / 4);
    const std::size_t blocksHigh = std::max<std::size_t>(1, (height + 3) / 4);
    const std::size_t blockSize = bcBlockSize(format);
    const std::size_t stride = pixelStride(format);

    const std::size_t required = blocksWide * blocksHigh * blockSize;
    if (srcLen < required) return false;

    out.assign(static_cast<std::size_t>(width) * height * stride, std::byte{0});

    // Scratch for one decoded block.
    std::vector<std::byte> blockPixels;

    for (std::size_t by = 0; by < blocksHigh; ++by) {
        for (std::size_t bx = 0; bx < blocksWide; ++bx) {
            const std::byte* blockSrc = src + (by * blocksWide + bx) * blockSize;

            if (format == BcFormat::BC6H_UF16 || format == BcFormat::BC6H_SF16) {
                float rgb[16 * 3];
                decodeBC6HBlock(blockSrc, rgb, format == BcFormat::BC6H_SF16);
                for (int py = 0; py < 4; ++py) {
                    const std::uint32_t y = static_cast<std::uint32_t>(by * 4 + py);
                    if (y >= height) continue;
                    for (int px = 0; px < 4; ++px) {
                        const std::uint32_t x = static_cast<std::uint32_t>(bx * 4 + px);
                        if (x >= width) continue;
                        const int idx = py * 4 + px;
                        float* dst = reinterpret_cast<float*>(
                            out.data() + (static_cast<std::size_t>(y) * width + x) * stride);
                        dst[0] = rgb[idx * 3 + 0];
                        dst[1] = rgb[idx * 3 + 1];
                        dst[2] = rgb[idx * 3 + 2];
                        dst[3] = 1.0f;
                    }
                }
            } else {
                std::uint8_t* scratchPtr = nullptr;
                switch (format) {
                case BcFormat::BC1: scratchPtr = reinterpret_cast<std::uint8_t*>(scratch(blockPixels, 16 * 4)); decodeBC1Block(blockSrc, scratchPtr); break;
                case BcFormat::BC2: scratchPtr = reinterpret_cast<std::uint8_t*>(scratch(blockPixels, 16 * 4)); decodeBC2Block(blockSrc, scratchPtr); break;
                case BcFormat::BC3: scratchPtr = reinterpret_cast<std::uint8_t*>(scratch(blockPixels, 16 * 4)); decodeBC3Block(blockSrc, scratchPtr); break;
                case BcFormat::BC7: scratchPtr = reinterpret_cast<std::uint8_t*>(scratch(blockPixels, 16 * 4)); decodeBC7Block(blockSrc, scratchPtr); break;
                case BcFormat::BC4: scratchPtr = reinterpret_cast<std::uint8_t*>(scratch(blockPixels, 16 * 1)); decodeBC4Block(blockSrc, scratchPtr); break;
                case BcFormat::BC5: scratchPtr = reinterpret_cast<std::uint8_t*>(scratch(blockPixels, 16 * 2)); decodeBC5Block(blockSrc, scratchPtr); break;
                default: return false;
                }

                for (int py = 0; py < 4; ++py) {
                    const std::uint32_t y = static_cast<std::uint32_t>(by * 4 + py);
                    if (y >= height) continue;
                    for (int px = 0; px < 4; ++px) {
                        const std::uint32_t x = static_cast<std::uint32_t>(bx * 4 + px);
                        if (x >= width) continue;
                        const int idx = py * 4 + px;
                        std::byte* dst = out.data() + (static_cast<std::size_t>(y) * width + x) * stride;
                        std::memcpy(dst, blockPixels.data() + idx * stride, stride);
                    }
                }
            }
        }
    }
    return true;
}

} // anonymous namespace

auto decodedFormat(BcFormat format) -> TextureFormat {
    switch (format) {
    case BcFormat::BC4: return TextureFormat::R8_UNORM;
    case BcFormat::BC5: return TextureFormat::RG8_UNORM;
    case BcFormat::BC6H_UF16:
    case BcFormat::BC6H_SF16: return TextureFormat::RGBA32_FLOAT;
    default: return TextureFormat::RGBA8_UNORM;
    }
}

auto loadDds(std::span<const std::byte> bytes) -> Result<Texture, LoadError> {
    // Minimum: magic(4) + header(124).
    if (bytes.size() < 128) return LoadError::Truncated;
    if (readU32(bytes.data(), 0) != kDdsMagic) return LoadError::BadSignature;

    const std::byte* hdr = bytes.data() + 4;
    const std::uint32_t headerSize = readU32(hdr, 0);
    if (headerSize != 124) return LoadError::Truncated;

    const std::uint32_t height = readU32(hdr, 8);
    const std::uint32_t width = readU32(hdr, 12);
    std::uint32_t mipCount = readU32(hdr, 24);

    // Pixel format struct starts at header offset 72: dwSize(72), dwFlags(76), dwFourCC(80).
    const std::uint32_t pfFlags = readU32(hdr, 76);
    const std::uint32_t fourCC = readU32(hdr, 80);

    if ((pfFlags & kDdpfFourCC) == 0) {
        return LoadError::UnsupportedCompression;
    }

    BcFormat format;
    std::size_t dataOffset = 4 + 124;

    if (fourCC == kFourCCDXT1) format = BcFormat::BC1;
    else if (fourCC == kFourCCDXT3) format = BcFormat::BC2;
    else if (fourCC == kFourCCDXT5) format = BcFormat::BC3;
    else if (fourCC == kFourCCBC4U || fourCC == kFourCCATI1) format = BcFormat::BC4;
    else if (fourCC == kFourCCBC5U || fourCC == kFourCCATI2) format = BcFormat::BC5;
    else if (fourCC == kFourCCDX10) {
        if (bytes.size() < 128 + 20) return LoadError::Truncated;
        const std::uint32_t dxgi = readU32(bytes.data(), 128);
        dataOffset = 4 + 124 + 20;
        switch (dxgi) {
        case kDxgiBC1Unorm: format = BcFormat::BC1; break;
        case kDxgiBC2Unorm: format = BcFormat::BC2; break;
        case kDxgiBC3Unorm: format = BcFormat::BC3; break;
        case kDxgiBC4Unorm: format = BcFormat::BC4; break;
        case kDxgiBC5Unorm: format = BcFormat::BC5; break;
        case kDxgiBC6HUF16: format = BcFormat::BC6H_UF16; break;
        case kDxgiBC6HSF16: format = BcFormat::BC6H_SF16; break;
        case kDxgiBC7Unorm: format = BcFormat::BC7; break;
        default: return LoadError::UnsupportedCompression;
        }
    } else {
        return LoadError::UnsupportedCompression;
    }

    if (width == 0 || height == 0) return LoadError::BadDimensions;
    if (mipCount == 0) mipCount = 1;

    Texture tex;
    tex.format = decodedFormat(format);
    tex.mips.resize(mipCount);

    const std::size_t blockSize = bcBlockSize(format);
    std::size_t srcOff = dataOffset;

    for (std::uint32_t m = 0; m < mipCount; ++m) {
        const std::uint32_t mw = std::max<std::uint32_t>(1u, width >> m);
        const std::uint32_t mh = std::max<std::uint32_t>(1u, height >> m);
        const std::size_t blocksWide = std::max<std::size_t>(1, (mw + 3) / 4);
        const std::size_t blocksHigh = std::max<std::size_t>(1, (mh + 3) / 4);
        const std::size_t mipBytes = blocksWide * blocksHigh * blockSize;

        if (srcOff + mipBytes > bytes.size()) return LoadError::Truncated;

        std::vector<std::byte> decoded;
        if (!decodeMip(format, bytes.data() + srcOff, mipBytes, mw, mh, decoded)) {
            return LoadError::Truncated;
        }

        tex.mips[m].width = mw;
        tex.mips[m].height = mh;
        tex.mips[m].bytes.resize(decoded.size());
        std::memcpy(tex.mips[m].bytes.data(), decoded.data(), decoded.size());

        srcOff += mipBytes;
    }

    return static_cast<Texture&&>(tex);
}

} // namespace f4::resources
