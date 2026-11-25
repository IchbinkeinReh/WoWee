#pragma once

#include <cmath>
#include <limits>

namespace wowee::core {

/// A number to be printed with so many decimals, as the client's C runtime rounds it: it goes by
/// the decimal digits, so that what is exactly half way goes up (Visual C++ 2005's __fptostr,
/// 0x00414ac5). The system's rounds such to the even digit: 12.25 was 12.2. What is half way is
/// handed on as the next number away from 0; the same is done in extern/lua-5.1.5/src/lobject.c.
[[nodiscard]] inline double halfUp(double v, int decimals) {
    if (decimals < 0 || decimals > 50) return v;
    // Half way is an odd number of that many halvings and no other: ten to the power of it is
    // two to it and five to it, and only the two of it can be short.
    const double halves = std::ldexp(std::fabs(v), decimals + 1);
    if (!(halves < 9007199254740992.0) || halves != std::floor(halves) || std::fmod(halves, 2.0) != 1.0) return v;
    return std::nextafter(v, v < 0.0 ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity());
}

}  // namespace wowee::core
