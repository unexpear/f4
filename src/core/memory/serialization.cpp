#include "core/memory/serialization.h"

namespace f4::memory {

void writeStreamPreamble(MemoryWriter& writer) {
    writer.writeValue(kEndianMarker);
}

auto checkStreamPreamble(MemoryReader& reader) -> bool {
    if (!hostIsLittleEndian()) {
        return false;
    }
    std::uint32_t marker = 0;
    if (!reader.readValue(marker)) {
        return false;
    }
    return marker == kEndianMarker;
}

bool writeChunk(MemoryWriter& writer, std::uint32_t id, std::uint32_t version,
                const void* payload, std::size_t payloadBytes) {
    F4_ASSERT(payloadBytes == 0 || payload != nullptr, "null payload with non-zero size");
    F4_ASSERT(hostIsLittleEndian(), "serialization requires a little-endian host");

    const ChunkHeader header{id, version,
                             static_cast<std::uint32_t>(payloadBytes)};
    writer.writeValue(header);
    writer.write(payload, payloadBytes);
    return true;
}

auto readChunkView(MemoryReader& reader, ChunkView& out) -> bool {
    ChunkHeader header;
    if (!reader.readValue(header)) {
        return false;
    }
    // Declared size must fit in the remaining stream - this is the truncation
    // and corruption guard.
    if (header.payloadSize > reader.remaining()) {
        return false;
    }
    out.id = header.id;
    out.version = header.version;
    out.payloadSize = header.payloadSize;
    out.payload = reader.current();
    return reader.skip(header.payloadSize);
}

auto readChunkInto(MemoryReader& reader, std::uint32_t expectedId, std::uint32_t expectedVersion,
                   void* destination, std::size_t destinationCapacity) -> bool {
    ChunkView view;
    if (!readChunkView(reader, view)) {
        return false;
    }
    if (view.id != expectedId || view.version != expectedVersion) {
        return false;
    }
    if (view.payloadSize > destinationCapacity) {
        return false;
    }
    if (view.payloadSize > 0 && destination != nullptr) {
        std::memcpy(destination, view.payload, view.payloadSize);
    }
    return true;
}

} // namespace f4::memory