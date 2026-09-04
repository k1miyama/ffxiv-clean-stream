#pragma once

#include <stdint.h>

namespace fcs::host {

enum class StreamResolution : uint32_t {
    Hd720 = 0,
    FullHd1080 = 1,
};

struct StreamResolutionSize {
    uint32_t width;
    uint32_t height;
};

inline constexpr StreamResolution kDefaultStreamResolution =
    StreamResolution::Hd720;

inline constexpr bool IsSupportedStreamResolution(
    StreamResolution resolution) {
    return resolution == StreamResolution::Hd720 ||
           resolution == StreamResolution::FullHd1080;
}

inline constexpr StreamResolutionSize StreamResolutionDimensions(
    StreamResolution resolution) {
    return resolution == StreamResolution::FullHd1080
        ? StreamResolutionSize{1920, 1080}
        : StreamResolutionSize{1280, 720};
}

static_assert(1280u * 9u == 720u * 16u);
static_assert(1920u * 9u == 1080u * 16u);

} // namespace fcs::host
