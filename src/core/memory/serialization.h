#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "core/assert.h"
#include "core/containers/array.h"

namespace f4::memory {

using containers::Array;

// Byte-stream serialization for POD data and framed chunks.
//
// Contracts (see plan §M2):
//  - BYTE IMAGE: values are copied verbatim via memcpy - the stream is a flat
//    byte array with NO alignment requirements. Note the image includes any
//    internal padding bytes of the host representation.
//  - TRIVIALLY COPYABLE: enforced at compile time on every value path.
//  - ENDIAN POLICY: explicitly little-endian. Every stream starts with an
//    endian marker written in native order; readers verify it against
//    std::endian::native and REJECT mismatched streams rather than silently
//    byte-swapping. Cross-endian support is deliberately deferred until a
//    platform needs it.
//  - CHUNK FRAMING: {u32 id, u32 version, u32 payloadSize} little-endian
//    header per chunk. Readers validate declared size against both remaining
//    stream bytes and the caller's output capacity before copying anything.
inline constexpr std::uint32_t kEndianMarker = 0x01020304u;

[[nodiscard]] constexpr auto hostIsLittleEndian() -> bool {
    return std::endian::native == std::endian::little;
}

class MemoryWriter {
public:
    void write(const void* source, std::size_t bytes) {
        const auto* first = static_cast<const std::byte*>(source);
        const std::size_t start = buffer_.size();
        buffer_.resize(start + bytes);
        if (bytes > 0) {
            std::memcpy(buffer_.data() + start, first, bytes);
        }
    }

    template <typename T>
    void writeValue(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>,
                      "serialization paths require trivially copyable types");
        write(&value, sizeof(T));
    }

    [[nodiscard]] auto bytes() const -> const Array<std::byte>& { return buffer_; }
    void reset() { buffer_.clear(); }

private:
    Array<std::byte> buffer_;
};

class MemoryReader {
public:
    MemoryReader(const std::byte* data, std::size_t size) : data_(data), size_(size) {}

    explicit MemoryReader(const Array<std::byte>& buffer)
        : data_(buffer.data()), size_(buffer.size()) {}

    [[nodiscard]] auto read(void* destination, std::size_t bytes) -> bool {
        if (bytes > remaining()) {
            return false;
        }
        if (bytes > 0) {
            std::memcpy(destination, data_ + cursor_, bytes);
        }
        cursor_ += bytes;
        return true;
    }

    template <typename T>
    [[nodiscard]] auto readValue(T& out) -> bool {
        static_assert(std::is_trivially_copyable_v<T>,
                      "serialization paths require trivially copyable types");
        return read(&out, sizeof(T));
    }

    // Skips forward without copying; fails if it would run past the end.
    [[nodiscard]] auto skip(std::size_t bytes) -> bool {
        if (bytes > remaining()) {
            return false;
        }
        cursor_ += bytes;
        return true;
    }

    [[nodiscard]] auto remaining() const -> std::size_t { return size_ - cursor_; }
    [[nodiscard]] auto position() const -> std::size_t { return cursor_; }
    [[nodiscard]] auto totalSize() const -> std::size_t { return size_; }
    [[nodiscard]] auto atEnd() const -> bool { return cursor_ == size_; }

    [[nodiscard]] auto current() const -> const std::byte* { return data_ + cursor_; }

private:
    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t cursor_ = 0;
};

struct ChunkHeader {
    std::uint32_t id = 0;
    std::uint32_t version = 0;
    std::uint32_t payloadSize = 0;
};

constexpr std::size_t kChunkHeaderBytes = sizeof(ChunkHeader);

void writeStreamPreamble(MemoryWriter& writer);
[[nodiscard]] bool checkStreamPreamble(MemoryReader& reader);
bool writeChunk(MemoryWriter& writer, std::uint32_t id, std::uint32_t version,
                const void* payload, std::size_t payloadBytes);
struct ChunkView {
    std::uint32_t id = 0;
    std::uint32_t version = 0;
    std::uint32_t payloadSize = 0;
    const std::byte* payload = nullptr;
};
[[nodiscard]] bool readChunkView(MemoryReader& reader, ChunkView& out);
[[nodiscard]] bool readChunkInto(MemoryReader& reader, std::uint32_t expectedId,
                                 std::uint32_t expectedVersion, void* destination,
                                 std::size_t destinationCapacity);

} // namespace f4::memory