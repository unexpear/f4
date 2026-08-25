#include "core/resources/exr_loader.h"

#include <cstring>
#include <vector>
#include <zlib.h>

namespace f4::resources {

namespace {

constexpr std::uint8_t EXR_MAGIC[4] = {0x76, 0x2f, 0x31, 0x01};

enum class ExrCompression : std::uint8_t {
    None = 0,
    RLE = 1,
    ZIP = 2,
    PIZ = 3,
    ZIPS = 4,
    B44 = 5,
    B44A = 6,
    DWAA = 7,
    DWAB = 8,
};

enum class ExrChannelType : std::uint8_t {
    Half = 0,   // 'H'
    Float = 1,  // 'F'
    UInt = 2,   // 'U'
};

struct ExrChannel {
    char name[256] = {};
    ExrChannelType type = ExrChannelType::Half;
};

struct ExrHeader {
    ExrCompression compression = ExrCompression::None;
    int32_t xMin = 0, yMin = 0, xMax = 0, yMax = 0;
    ExrChannel channels[16] = {};
    std::int32_t channelCount = 0;
};

// IEEE 754 half-float → float conversion.
auto halfToFloat(std::uint16_t h) -> float {
    const std::uint32_t sign = (static_cast<std::uint32_t>(h) >> 15u) & 1u;
    const std::uint32_t exponent = (static_cast<std::uint32_t>(h) >> 10u) & 0x1Fu;
    const std::uint32_t mantissa = static_cast<std::uint32_t>(h) & 0x3FFu;

    std::uint32_t f = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            f = sign << 31u;
        } else {
            // Denorm → normalize.
            std::uint32_t m = mantissa;
            std::uint32_t e = 1;
            while ((m & 0x400u) == 0) { m <<= 1u; ++e; }
            f = (sign << 31u) | ((127u - 15u + 1u - e) << 23u) | ((m & 0x3FFu) << 13u);
        }
    } else if (exponent == 31) {
        // Inf / NaN.
        f = (sign << 31u) | 0x7F800000u | (mantissa << 13u);
    } else {
        f = (sign << 31u) | ((exponent + 127u - 15u) << 23u) | (mantissa << 13u);
    }
    float result;
    std::memcpy(&result, &f, sizeof(result));
    return result;
}

// Read a little-endian int32 from data at offset.
auto readI32(const std::byte* data, std::size_t off) -> std::int32_t {
    std::int32_t v;
    std::memcpy(&v, data + off, 4);
    return v;
}

// Read a little-endian uint32 from data at offset.
auto readU32(const std::byte* data, std::size_t off) -> std::uint32_t {
    std::uint32_t v;
    std::memcpy(&v, data + off, 4);
    return v;
}

auto parseHeader(const std::byte* data, std::size_t size, ExrHeader& hdr) -> bool {
    if (size < 8) return false;
    if (std::memcmp(data, EXR_MAGIC, 4) != 0) return false;

    const std::uint32_t version = readU32(data, 4);
    // Bit 9 = tiled, bit 10 = deep. Reject both.
    if ((version & 0x200) != 0 || (version & 0x400) != 0) return false;

    std::size_t off = 8;

    // Parse attributes until we hit a zero byte (empty name).
    while (off + 1 < size) {
        if (data[off] == std::byte{0}) { ++off; break; }

        // Name: null-terminated string.
        const std::size_t nameStart = off;
        while (off < size && data[off] != std::byte{0}) ++off;
        if (off >= size) return false;
        const std::size_t nameLen = off - nameStart;
        ++off; // skip null terminator

        // Type: null-terminated string.
        const std::size_t typeStart = off;
        while (off < size && data[off] != std::byte{0}) ++off;
        if (off >= size) return false;
        const std::size_t typeLen = off - typeStart;
        ++off; // skip null terminator

        // Data size: uint32.
        if (off + 4 > size) return false;
        const std::uint32_t dataLen = readU32(data, off);
        off += 4;

        if (off + dataLen > size) return false;

        // Process known attributes.
        char name[256] = {};
        char type[256] = {};
        if (nameLen < sizeof(name)) std::memcpy(name, data + nameStart, nameLen);
        if (typeLen < sizeof(type)) std::memcpy(type, data + typeStart, typeLen);

        if (std::strcmp(name, "compression") == 0 && dataLen >= 1) {
            hdr.compression = static_cast<ExrCompression>(data[off]);
        } else if (std::strcmp(name, "dataWindow") == 0 && dataLen >= 16) {
            hdr.xMin = readI32(data, off);
            hdr.yMin = readI32(data, off + 4);
            hdr.xMax = readI32(data, off + 8);
            hdr.yMax = readI32(data, off + 12);
        } else if (std::strcmp(name, "channels") == 0) {
            // ChannelList: repeated (name, pixelType int32, pLinear, reserved[3])
            // until empty name.
            std::size_t chOff = off;
            std::int32_t chIdx = 0;
            while (chOff < off + dataLen && chIdx < 16) {
                if (data[chOff] == std::byte{0}) break;
                // Channel name: null-terminated.
                std::size_t nStart = chOff;
                while (chOff < off + dataLen && data[chOff] != std::byte{0}) ++chOff;
                if (chOff >= off + dataLen) break;
                std::size_t nLen = chOff - nStart;
                ++chOff; // skip null
                if (chOff + 8 > off + dataLen) break;
                // pixelType: int32 LE. 0=UINT, 1=HALF, 2=FLOAT.
                const std::int32_t pt = readI32(data, chOff);
                chOff += 4;
                chOff += 4; // skip pLinear (1) + reserved (3)
                if (nLen < sizeof(hdr.channels[0].name)) {
                    std::memcpy(hdr.channels[chIdx].name, data + nStart, nLen);
                    hdr.channels[chIdx].name[nLen] = '\0';
                }
                switch (pt) {
                case 0: hdr.channels[chIdx].type = ExrChannelType::UInt; break;
                case 1: hdr.channels[chIdx].type = ExrChannelType::Half; break;
                case 2: hdr.channels[chIdx].type = ExrChannelType::Float; break;
                default: break;
                }
                ++chIdx;
            }
            hdr.channelCount = chIdx;
        }

        off += dataLen;
    }

    return hdr.channelCount > 0;
}

// Find channel index by name (e.g., "R", "G", "B", "A").
auto findChannel(const ExrHeader& hdr, const char* name) -> std::int32_t {
    for (std::int32_t i = 0; i < hdr.channelCount; ++i) {
        if (std::strcmp(hdr.channels[i].name, name) == 0) return i;
    }
    return -1;
}

// Bytes per pixel for a channel type.
auto channelBytes(ExrChannelType ct) -> std::size_t {
    switch (ct) {
    case ExrChannelType::Half:  return 2;
    case ExrChannelType::Float: return 4;
    case ExrChannelType::UInt:  return 4;
    }
    return 0;
}

// Decompress a single ZIP-compressed scanline (deflate).
auto decompressZip(const std::byte* src, std::size_t srcLen,
                   std::byte* dst, std::size_t dstLen) -> bool {
    z_stream stream = {};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(src));
    stream.avail_in = static_cast<uInt>(srcLen);
    stream.next_out = reinterpret_cast<Bytef*>(dst);
    stream.avail_out = static_cast<uInt>(dstLen);

    // auto=false gives raw deflate (EXR uses raw deflate, not zlib-wrapped).
    int ret = inflateInit(&stream);
    if (ret != Z_OK) return false;

    ret = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    return (ret == Z_STREAM_END);
}

// Read a single scanline from the EXR data, decompressing if needed.
// Returns a pointer into scanlineBuf which contains the raw interleaved channel data.
auto readScanline(const std::byte* data, std::size_t size, std::size_t& off,
                  ExrCompression compression, std::int32_t expectedY,
                  std::vector<std::byte>& scanlineBuf,
                  std::vector<std::byte>& decompressBuf) -> bool {
    if (off + 8 > size) return false;
    const std::int32_t y = readI32(data, off);
    const std::int32_t dataSize = readI32(data, off + 4);
    off += 8;

    if (y != expectedY) return false;
    if (off + static_cast<std::size_t>(dataSize) > size) return false;

    if (compression == ExrCompression::None) {
        scanlineBuf.resize(static_cast<std::size_t>(dataSize));
        std::memcpy(scanlineBuf.data(), data + off, static_cast<std::size_t>(dataSize));
    } else if (compression == ExrCompression::ZIP) {
        if (!decompressZip(data + off, static_cast<std::size_t>(dataSize),
                           decompressBuf.data(), decompressBuf.size())) {
            return false;
        }
        scanlineBuf = decompressBuf;
    } else {
        return false; // Unsupported compression.
    }

    off += static_cast<std::size_t>(dataSize);
    return true;
}

} // anonymous namespace

auto loadExr(std::span<const std::byte> bytes) -> Result<Texture, LoadError> {
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), EXR_MAGIC, 4) != 0) {
        return LoadError::BadSignature;
    }
    if (bytes.size() < 8) return LoadError::Truncated;

    ExrHeader hdr{};
    if (!parseHeader(bytes.data(), bytes.size(), hdr)) {
        return LoadError::Truncated;
    }

    // Must support uncompressed or ZIP.
    if (hdr.compression != ExrCompression::None &&
        hdr.compression != ExrCompression::ZIP) {
        return LoadError::UnsupportedCompression;
    }

    const std::int32_t width = hdr.xMax - hdr.xMin + 1;
    const std::int32_t height = hdr.yMax - hdr.yMin + 1;
    if (width <= 0 || height <= 0) return LoadError::BadDimensions;

    // Find RGBA channels. If less than 4, we still fill missing ones with defaults.
    const std::int32_t idxR = findChannel(hdr, "R");
    const std::int32_t idxG = findChannel(hdr, "G");
    const std::int32_t idxB = findChannel(hdr, "B");
    const std::int32_t idxA = findChannel(hdr, "A");

    // Calculate per-scanline byte count for the interleaved channel data.
    std::size_t scanlineBytes = 0;
    for (std::int32_t i = 0; i < hdr.channelCount; ++i) {
        scanlineBytes += channelBytes(hdr.channels[i].type);
    }
    scanlineBytes *= static_cast<std::size_t>(width);

    std::vector<std::byte> scanlineBuf;
    std::vector<std::byte> decompressBuf(scanlineBytes);

    Texture tex;
    tex.format = TextureFormat::RGBA32_FLOAT;
    tex.mips.resize(1);
    tex.mips[0].width = static_cast<std::uint32_t>(width);
    tex.mips[0].height = static_cast<std::uint32_t>(height);
    tex.mips[0].bytes.resize(static_cast<std::size_t>(width) * height * 16);

    std::size_t fileOff = 8;
    // Skip past header to find where scanlines start. We re-read from beginning
    // and skip attributes.
    {
        // Re-parse to advance past the header properly.
        fileOff = 8;
        while (fileOff + 1 < bytes.size()) {
            if (bytes[fileOff] == std::byte{0}) { ++fileOff; break; }
            while (fileOff < bytes.size() && bytes[fileOff] != std::byte{0}) ++fileOff;
            ++fileOff; // name null
            while (fileOff < bytes.size() && bytes[fileOff] != std::byte{0}) ++fileOff;
            ++fileOff; // type null
            if (fileOff + 4 > bytes.size()) return LoadError::Truncated;
            const std::uint32_t dLen = readU32(bytes.data(), fileOff);
            fileOff += 4 + dLen;
        }
    }

    const auto fWidth = static_cast<std::size_t>(width);

    for (std::int32_t y = hdr.yMin; y <= hdr.yMax; ++y) {
        if (!readScanline(bytes.data(), bytes.size(), fileOff,
                          hdr.compression, y, scanlineBuf, decompressBuf)) {
            return LoadError::Truncated;
        }

        const std::size_t row = static_cast<std::size_t>(y - hdr.yMin);
        float* out = reinterpret_cast<float*>(
            tex.mips[0].bytes.data() + row * fWidth * 16);

        // De-interleave channels for this scanline.
        // For each pixel, read channel values in channel-list order.
        std::size_t srcOff = 0;
        for (std::size_t px = 0; px < fWidth; ++px) {
            float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (std::int32_t ch = 0; ch < hdr.channelCount; ++ch) {
                float val = 0.0f;
                if (hdr.channels[ch].type == ExrChannelType::Half) {
                    std::uint16_t h;
                    std::memcpy(&h, scanlineBuf.data() + srcOff, 2);
                    val = halfToFloat(h);
                    srcOff += 2;
                } else if (hdr.channels[ch].type == ExrChannelType::Float) {
                    std::memcpy(&val, scanlineBuf.data() + srcOff, 4);
                    srcOff += 4;
                } else {
                    srcOff += 4;
                }

                if (ch == idxR) rgba[0] = val;
                else if (ch == idxG) rgba[1] = val;
                else if (ch == idxB) rgba[2] = val;
                else if (ch == idxA) rgba[3] = val;
            }
            std::memcpy(out + px * 4, rgba, 16);
        }
    }

    return static_cast<Texture&&>(tex);
}

} // namespace f4::resources
