#include "core/resources/file_io.h"

#include <cstdio>

namespace f4::resources {

namespace {
auto openFile(StringView path, const char* mode) -> std::FILE* {
#if defined(_MSC_VER)
    std::FILE* file = nullptr;
    if (fopen_s(&file, path.data(), mode) != 0) {
        return nullptr;
    }
    return file;
#else
    return std::fopen(path.data(), mode);
#endif
}
} // anonymous namespace

auto readBinaryFile(StringView path, Array<std::byte>& out) -> FileIoError {
    out.clear();

    std::FILE* file = openFile(path, "rb");
    if (file == nullptr) {
        return FileIoError::NotFound;
    }

    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return FileIoError::ReadFailed;
    }
    const long end = std::ftell(file);
    if (end < 0) {
        std::fclose(file);
        return FileIoError::ReadFailed;
    }
    if (std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return FileIoError::ReadFailed;
    }

    out.resize(static_cast<std::size_t>(end));
    if (!out.isEmpty() &&
        std::fread(out.data(), 1, out.size(), file) != out.size()) {
        std::fclose(file);
        out.clear();
        return FileIoError::ReadFailed;
    }

    std::fclose(file);
    return FileIoError::None;
}

auto writeBinaryFile(StringView path, const void* data, std::size_t bytes) -> FileIoError {
    std::FILE* file = openFile(path, "wb");
    if (file == nullptr) {
        return bytes == 0 ? FileIoError::None : FileIoError::OpenFailed;
    }

    if (bytes > 0 && data != nullptr && std::fwrite(data, 1, bytes, file) != bytes) {
        std::fclose(file);
        return FileIoError::WriteFailed;
    }

    std::fclose(file);
    return FileIoError::None;
}

} // namespace f4::resources