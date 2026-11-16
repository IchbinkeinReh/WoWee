#pragma once

// The ranges the client's camera CVars are registered with (0x005fd630 to
// 0x005fd8c0, attached where each is registered in 0x005f0000's camera init).
// A value outside its CVar's range is refused - the CVar keeps the value it
// had - and the console is told "Value out of range (%f - %f)". Both ends are
// allowed. The bounds are the client's single-precision constants, and the
// value is read as the client reads a CVar number.

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace wowee::addons {

struct CVarRange {
    float min;
    float max;
};

/// The range for a lower-cased CVar name, or none for a CVar without one.
inline std::optional<CVarRange> cameraCVarRange(std::string_view lower) {
    // The pitch limits, +-1.553343 radians, in degrees (0x00ad1b50,
    // 0x00ad1b4c times 57.29578): 89.
    constexpr float kPitch = 1.5533430576324463f * 57.29578f;
    struct Row { std::string_view name; CVarRange range; };
    static constexpr Row kRows[] = {
        {"cameraview", {0.0f, 7.0f}},                                  // 0x005fd630
        {"cameradistancemax", {0.0f, 50.0f}},                          // 0x005fd680
        {"camerasurfacepitch", {-kPitch, kPitch}},                     // 0x005fd6d0
        {"camerasubmergepitch", {-kPitch, kPitch}},
        {"camerasurfacefinalpitch", {-kPitch, kPitch}},
        {"camerasubmergefinalpitch", {-kPitch, kPitch}},
        {"camerapitchsmoothmin", {-kPitch, kPitch}},
        {"camerapitchsmoothmax", {-kPitch, kPitch}},
        {"cameraterraintilttimemin", {0.001f, 300.0f}},                // 0x005fd750
        {"cameraterraintilttimemax", {0.001f, 300.0f}},
        {"camerasmoothtimemin", {0.001f, 300.0f}},
        {"camerasmoothtimemax", {0.001f, 300.0f}},
        {"camerayawsmoothmin", {0.0f, 360.0f}},                        // 0x005fd7b0
        {"camerayawsmoothmax", {0.0f, 360.0f}},
        {"camerapitchmovespeed", {0.1f, 360.0f}},                      // 0x005fd800
        {"camerayawmovespeed", {0.1f, 360.0f}},
        {"camerafovsmoothspeed", {0.1f, 360.0f}},
        {"cameragroundsmoothspeed", {0.1f, 360.0f}},
        {"camerapitchsmoothspeed", {0.1f, 360.0f}},
        {"cameratargetsmoothspeed", {0.1f, 360.0f}},
        {"camerayawsmoothspeed", {0.1f, 360.0f}},
        {"cameradistancemovespeed", {1.0f / 360.0f, 50.0f}},           // 0x005fd860
        {"camerabobbingsmoothspeed", {1.0f / 360.0f, 50.0f}},
        {"cameradistancesmoothspeed", {1.0f / 360.0f, 50.0f}},
        {"cameraheightsmoothspeed", {1.0f / 360.0f, 50.0f}},
        {"cameraflyingmountheightsmoothspeed", {1.0f / 360.0f, 50.0f}},
        {"camerasmoothstyle", {0.0f, 5.0f}},                           // 0x005fd8c0
        {"camerasmoothtrackingstyle", {0.0f, 5.0f}},
    };
    for (const Row& row : kRows) {
        if (row.name == lower) return row.range;
    }
    return std::nullopt;
}

/// Whether `value` may be written to that CVar: true for one without a range.
inline bool cameraCVarAccepts(std::string_view lower, const std::string& value) {
    const auto range = cameraCVarRange(lower);
    if (!range) return true;
    const float v = static_cast<float>(std::atof(value.c_str()));
    return v >= range->min && v <= range->max;
}

}  // namespace wowee::addons
