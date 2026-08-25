#pragma once

#include <cstdint>

#include "core/containers/array.h"
#include "core/memory/serialization.h"

namespace f4::resources {

using containers::Array;

// .f4asset container: endian preamble + magic + format version, then M2
// chunk framing. Loaders write normalized records here; the ladder rungs all
// consume this instead of inventing formats.

inline constexpr std::uint32_t kAssetMagic = 0x53413446u; // "F4AS" little-endian
inline constexpr std::uint32_t kAssetFormatVersion = 1u;

class AssetWriter {
public:
    explicit AssetWriter(memory::MemoryWriter& writer) : writer_(writer) {}

    // Writes preamble + magic + format version. Call once, before addChunk.
    void begin() {
        memory::writeStreamPreamble(writer_);
        writer_.writeValue(kAssetMagic);
        writer_.writeValue(kAssetFormatVersion);
        begun_ = true;
    }

    bool addChunk(std::uint32_t id, std::uint32_t version, const void* payload,
                  std::size_t payloadBytes) {
        if (!begun_) {
            return false;
        }
        return memory::writeChunk(writer_, id, version, payload, payloadBytes);
    }

private:
    memory::MemoryWriter& writer_;
    bool begun_ = false;
};

class AssetReader {
public:
    AssetReader(const std::byte* data, std::size_t size)
        : reader_(data, size), state_(State::Empty) {}

    explicit AssetReader(const Array<std::byte>& buffer)
        : reader_(buffer), state_(State::Empty) {}

    // Validates preamble + magic + format version. Must be called before next().
    [[nodiscard]] bool begin() {
        if (state_ != State::Empty || !memory::checkStreamPreamble(reader_)) {
            state_ = State::Error;
            return false;
        }
        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        if (!reader_.readValue(magic) || magic != kAssetMagic || !reader_.readValue(version) ||
            version != kAssetFormatVersion) {
            state_ = State::Error;
            return false;
        }
        state_ = State::Begun;
        return true;
    }

    // Advances to the next chunk. Returns false at clean end-of-stream OR on
    // error - check error() afterwards. A trailing-garbage stream (bytes left
    // that do not form a full valid header) is an ERROR, not a clean end.
    [[nodiscard]] bool next(memory::ChunkView& out) {
        if (state_ != State::Begun) {
            return false;
        }
        if (reader_.atEnd()) {
            state_ = State::Done;
            return false;
        }
        if (!memory::readChunkView(reader_, out)) {
            state_ = State::Error;
            return false;
        }
        return true;
    }

    [[nodiscard]] auto error() const -> bool { return state_ == State::Error; }
    [[nodiscard]] auto cleanlyFinished() const -> bool { return state_ == State::Done; }

private:
    enum class State : std::uint8_t { Empty, Begun, Done, Error };

    memory::MemoryReader reader_;
    State state_ = State::Empty;
};

} // namespace f4::resources