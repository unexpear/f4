#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "core/math/math.h"
#include "core/memory/serialization.h"

using namespace f4::math;
using namespace f4::memory;

namespace {
struct SamplePod {
    float x;
    std::uint32_t flags;
    double d;
};
static_assert(std::is_trivially_copyable_v<SamplePod>);

constexpr std::uint32_t kChunkIdMesh = 1001;
constexpr std::uint32_t kChunkIdMeta = 1002;
} // namespace

TEST_CASE("host is little-endian on every supported platform") {
    CHECK(hostIsLittleEndian());
}

TEST_CASE("pod values roundtrip through writer and reader") {
    MemoryWriter writer;
    const SamplePod pod{1.5f, 0xDEADBEEFu, -3.25};
    writer.writeValue(pod);

    const std::int32_t signedValue = -42;
    writer.writeValue(signedValue);
    const Vec3 vec(1.0f, -2.0f, 3.0f); // engine math types are trivially copyable
    writer.writeValue(vec);

    MemoryReader reader(writer.bytes());
    SamplePod out{};
    REQUIRE(reader.readValue(out));
    CHECK(out.x == pod.x);
    CHECK(out.flags == pod.flags);
    CHECK(out.d == pod.d);

    std::int32_t signedOut = 0;
    REQUIRE(reader.readValue(signedOut));
    CHECK(signedOut == -42);

    Vec3 vecOut;
    REQUIRE(reader.readValue(vecOut));
    CHECK(vecOut.x == 1.0f);
    CHECK(vecOut.y == -2.0f);
    CHECK(vecOut.z == 3.0f);

    CHECK(reader.atEnd());
}

TEST_CASE("reader bounds checks reject truncated reads") {
    MemoryWriter writer;
    writer.writeValue(std::uint64_t{0x1122334455667788ull});

    SUBCASE("reading past the end fails") {
        MemoryReader reader(writer.bytes());
        std::uint64_t out = 0;
        CHECK(reader.readValue(out));
        CHECK(!reader.readValue(out)); // nothing left
    }

    SUBCASE("partial read fails without consuming") {
        MemoryReader reader(writer.bytes());
        std::byte sink[8] = {};
        CHECK(reader.read(sink, 4)); // half fits
        CHECK(!reader.read(sink, 8)); // only 4 remain
        CHECK(reader.remaining() == 4);
    }

    SUBCASE("skip respects the end") {
        MemoryReader reader(writer.bytes());
        CHECK(!reader.skip(9));
        CHECK(reader.skip(3));
        CHECK(reader.remaining() == 5);
    }
}

TEST_CASE("stream preamble pins explicit little-endian policy") {
    MemoryWriter writer;
    writeStreamPreamble(writer);
    CHECK(writer.bytes().size() == sizeof(kEndianMarker));

    // Correct byte order is accepted.
    MemoryReader good(writer.bytes());
    CHECK(checkStreamPreamble(good));

    // A big-endian stream (marker written most-significant-byte-first) must
    // be REJECTED, not silently interpreted.
    std::byte swapped[4] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    MemoryReader bad(swapped, 4);
    CHECK(!checkStreamPreamble(bad));

    // Truncated stream is rejected too.
    MemoryReader tiny(writer.bytes().data(), 2);
    CHECK(!checkStreamPreamble(tiny));
}

TEST_CASE("chunks frame, validate, and roundtrip payloads") {
    MemoryWriter writer;
    writeStreamPreamble(writer);

    const char text[] = "mesh payload bytes";
    const SamplePod pod{7.0f, 1, 2.5};

    CHECK(writeChunk(writer, kChunkIdMesh, 1, text, std::strlen(text)));
    CHECK(writeChunk(writer, kChunkIdMeta, 2, &pod, sizeof(pod)));

    MemoryReader reader(writer.bytes());
    REQUIRE(checkStreamPreamble(reader));

    ChunkView viewA;
    REQUIRE(readChunkView(reader, viewA));
    CHECK(viewA.id == kChunkIdMesh);
    CHECK(viewA.version == 1);
    CHECK(viewA.payloadSize == std::strlen(text));
    CHECK(std::memcmp(viewA.payload, text, std::strlen(text)) == 0);

    ChunkView viewB;
    REQUIRE(readChunkView(reader, viewB));
    CHECK(viewB.id == kChunkIdMeta);
    CHECK(viewB.version == 2);
    CHECK(viewB.payloadSize == sizeof(pod));
    CHECK(reader.atEnd()); // stream fully consumed by validated chunks
}

TEST_CASE("readChunkInto enforces id, version, and capacity") {
    MemoryWriter writer;
    const std::uint32_t numbers[3] = {10, 20, 30};
    CHECK(writeChunk(writer, 77, 4, numbers, sizeof(numbers)));

    SUBCASE("happy path") {
        MemoryReader reader(writer.bytes());
        std::uint32_t out[3] = {};
        REQUIRE(readChunkInto(reader, 77, 4, out, sizeof(out)));
        CHECK(out[0] == 10);
        CHECK(out[1] == 20);
        CHECK(out[2] == 30);
    }

    SUBCASE("wrong id rejected") {
        MemoryReader reader(writer.bytes());
        std::uint32_t out[3] = {};
        CHECK(!readChunkInto(reader, 78, 4, out, sizeof(out)));
    }

    SUBCASE("wrong version rejected") {
        MemoryReader reader(writer.bytes());
        std::uint32_t out[3] = {};
        CHECK(!readChunkInto(reader, 77, 5, out, sizeof(out)));
    }

    SUBCASE("undersized destination rejected") {
        MemoryReader reader(writer.bytes());
        std::uint32_t out[2] = {}; // needs 12 bytes, has 8
        CHECK(!readChunkInto(reader, 77, 4, out, sizeof(out)));
    }
}

TEST_CASE("truncated chunk payload is rejected before any copy") {
    MemoryWriter writer;
    const std::uint32_t numbers[4] = {1, 2, 3, 4};
    CHECK(writeChunk(writer, 9, 1, numbers, sizeof(numbers)));

    // Corrupt a COPY: pretend the chunk declares one element more than what
    // was written. memcpy-based patching avoids any aliasing tricks.
    const std::size_t total = writer.bytes().size();
    std::byte patched[128];
    REQUIRE(total <= sizeof(patched));
    std::memcpy(patched, writer.bytes().data(), total);
    ChunkHeader header{};
    std::memcpy(&header, patched, sizeof(header));
    header.payloadSize += sizeof(std::uint32_t);
    std::memcpy(patched, &header, sizeof(header));

    MemoryReader reader(patched, total);
    ChunkView view;
    CHECK(!readChunkView(reader, view)); // declared size exceeds remaining
}
