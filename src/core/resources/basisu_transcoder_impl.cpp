// Single compilation unit that compiles the basis_universal transcoder.
// SYSTEM include via CMake prevents compiler warnings from vendor code.

// Required defines before including the transcoder source.
// Note: BASISD_SUPPORT_KTX2 and BASISD_SUPPORT_KTX2_ZSTD must be defined
// before the transcoder source is included; they are checked with #error.
#define BASISD_SUPPORT_KTX2 1
#define BASISD_SUPPORT_KTX2_ZSTD 0
#define BASISD_SUPPORT_KTX2_ZLIB 0

// Suppress warnings in vendor code that triggers under our strict flags.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4100 4127 4189 4244 4324 4456 4458 4459 4505 4702 4706 4996 6262 28182 28183)
#endif

#include "basisu_transcoder.cpp"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace f4::resources {
void initBasisTranscoder() {
    static bool s_initialized = false;
    if (!s_initialized) {
        basist::basisu_transcoder_init();
        s_initialized = true;
    }
}
}
