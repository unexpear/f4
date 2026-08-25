#pragma once

#include <cstdint>

#include "core/containers/array.h"
#include "core/containers/string.h"

namespace f4::resources {

using containers::Array;
using containers::StringView;

// Binary file plumbing deferred from M2 (plan §M2 notes).
//
// v1 limitation: paths are treated as narrow platform strings; ASCII test and
// asset paths are the supported case. UTF-8 path conversion arrives with the
// platform layer if a real need appears.

enum class FileIoError : std::uint8_t {
    None,
    NotFound,
    OpenFailed,
    ReadFailed,
    WriteFailed,
};

// On success out is replaced with the file contents; on ANY failure out is
// left empty so callers never see stale bytes.
[[nodiscard]] auto readBinaryFile(StringView path, Array<std::byte>& out) -> FileIoError;

// Writes the full buffer atomically-enough for tool use: opens, writes,
// flushes, closes. Partial writes report WriteFailed.
[[nodiscard]] auto writeBinaryFile(StringView path, const void* data,
                                   std::size_t bytes) -> FileIoError;

} // namespace f4::resources