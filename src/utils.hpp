#pragma once
#include <cstdint>

namespace utils {

/**
 * Unpacks a 0xRRGGBBAA packed color into a float[4] array.
 * Resulting array is in RGBA order, each component normalized to [0, 1].
 */
inline void colorToRgba(uint32_t packed, float out[4]) {
    out[0] = ((packed >> 24) & 0xFF) / 255.0f; // R
    out[1] = ((packed >> 16) & 0xFF) / 255.0f; // G
    out[2] = ((packed >> 8)  & 0xFF) / 255.0f; // B
    out[3] = (packed       & 0xFF) / 255.0f; // A
}

} // namespace utils
