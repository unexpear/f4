#include "core/resources/stb_image_loader.h"

#include <cstring>

#include <stb_image.h>

namespace f4::resources {

auto loadStbImage(std::span<const std::byte> bytes) -> Result<Texture, LoadError> {
    if (bytes.empty()) {
        return LoadError::Truncated;
    }

    int w = 0;
    int h = 0;
    int channels = 0;

    // Request 4 channels (RGBA) regardless of source format.
    unsigned char* pixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.data()),
        static_cast<int>(bytes.size()),
        &w, &h, &channels, 4);

    if (pixels == nullptr) {
        // stb_image failed — map its failure to our error codes.
        return LoadError::Truncated;
    }
    if (w <= 0 || h <= 0) {
        stbi_image_free(pixels);
        return LoadError::BadDimensions;
    }

    const auto width = static_cast<std::uint32_t>(w);
    const auto height = static_cast<std::uint32_t>(h);
    const std::size_t dataSize = static_cast<std::size_t>(width) * height * 4;

    Texture tex;
    tex.format = TextureFormat::RGBA8_UNORM;
    tex.mips.resize(1);
    tex.mips[0].width = width;
    tex.mips[0].height = height;
    tex.mips[0].bytes.resize(dataSize);
    std::memcpy(tex.mips[0].bytes.data(), pixels, dataSize);

    stbi_image_free(pixels);
    return static_cast<Texture&&>(tex);
}

auto loadStbImageHdr(std::span<const std::byte> bytes) -> Result<Texture, LoadError> {
    if (bytes.empty()) {
        return LoadError::Truncated;
    }

    int w = 0;
    int h = 0;
    int channels = 0;

    float* pixels = stbi_loadf_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.data()),
        static_cast<int>(bytes.size()),
        &w, &h, &channels, 4);

    if (pixels == nullptr) {
        return LoadError::Truncated;
    }
    if (w <= 0 || h <= 0) {
        stbi_image_free(pixels);
        return LoadError::BadDimensions;
    }

    const auto width = static_cast<std::uint32_t>(w);
    const auto height = static_cast<std::uint32_t>(h);
    const std::size_t dataSize = static_cast<std::size_t>(width) * height * 4;

    Texture tex;
    tex.format = TextureFormat::RGBA32_FLOAT;
    tex.mips.resize(1);
    tex.mips[0].width = width;
    tex.mips[0].height = height;
    tex.mips[0].bytes.resize(dataSize * sizeof(float));
    std::memcpy(tex.mips[0].bytes.data(), pixels, tex.mips[0].bytes.size());

    stbi_image_free(pixels);
    return static_cast<Texture&&>(tex);
}

} // namespace f4::resources
