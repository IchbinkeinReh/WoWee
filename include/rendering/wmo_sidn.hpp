#pragma once

/// A WMO material's window light, as the 3.3.5a client works it out.
///
/// A material with MOMT flag 0x10 carries a colour (sidnColor, +0x10) that
/// lights its windows at night. Every frame the client scales it by the
/// light's window level into the material's frameSidnColor (+0x14,
/// 0x007a8520), and each draw (0x007ac6a0, 0x007ac9f0, 0x007a9380) hands the
/// MapObj vertex programs half of that, byte by byte, as c29 - which the lit
/// programs add to the vertex colour (0x007a8940). Nothing here touches a
/// device, so it can be checked on its own.

#include "rendering/day_night.hpp"

#include <cmath>
#include <cstdint>

namespace wowee::rendering::wmo_sidn {

/// The light's window level over the day (0x007f3230 samples the curve at
/// 0xaf4c80 into 0xd38cdc): full at night, out by 07:00, back from 20:30.
inline constexpr daynight::CurveKey kWindowLevel[] = {
    {0.25f, 1.0f}, {0.29166666f, 0.0f}, {0.85416663f, 0.0f}, {0.89583331f, 1.0f}};

inline float windowLevel(float dayFraction) { return daynight::sampleCurve(kWindowLevel, dayFraction); }

/// frameSidnColor (0x007a8520): each colour byte of sidnColor times the
/// level as a byte - round(level x 255 - 0.5) to even, as the FPU rounds
/// (0xadfe48 is 0.5) - over 256; the alpha byte left 0. D3DCOLOR in and out.
inline uint32_t frameSidnColor(uint32_t sidnColor, float level) {
    const float lf = std::nearbyint(level * 255.0f - 0.5f);
    const auto l = static_cast<uint32_t>(lf < 0.0f ? 0.0f : lf);
    const uint32_t b = ((sidnColor & 0xff) * l) >> 8;
    const uint32_t g = (((sidnColor >> 8) & 0xff) * l) >> 8;
    const uint32_t r = (((sidnColor >> 16) & 0xff) * l) >> 8;
    return (r << 16) | (g << 8) | b;
}

/// c29 (0x007ac6a0): `add` - the light's ambient for the WMO of the game
/// object under the cursor, otherwise 0 - plus frameSidnColor where the
/// material has flag 0x10, saturating byte by byte, then each colour byte
/// halved; the alpha byte is the sum's.
inline uint32_t emissive(uint32_t add, uint32_t frameSidn) {
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        uint32_t s = ((add >> shift) & 0xff) + ((frameSidn >> shift) & 0xff);
        if (s > 0xff) s = 0xff;
        if (shift < 24) s >>= 1;
        out |= s << shift;
    }
    return out;
}

}  // namespace wowee::rendering::wmo_sidn
