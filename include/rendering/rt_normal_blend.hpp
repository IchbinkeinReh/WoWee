#pragma once

// Where the traced lamps stop lighting a surface by its own normal and light
// it by its face's instead (rtNormalAgreement, rt_gbuffer.glsli): all of its
// own while it lies within `keep` degrees of the face, none of it past `face`
// degrees, a smoothstep between. The Ray Tracing page's two "Normal blend"
// sliders. One pair of cosines feeds both places the rule is applied - a
// room's baked colour in the G-buffer (wmo.frag, through the per-frame
// block) and every other surface's lamp light (rt_trace, through the pass
// block) - from Renderer, so the two cannot disagree.

#include <algorithm>
#include <cmath>

namespace wowee::rendering {

inline constexpr int kNormalBlendKeepDefault = 15;
inline constexpr int kNormalBlendFaceDefault = 35;
inline constexpr int kNormalBlendMaxDegrees = 90;

struct NormalBlendCosines {
    float nearCos;  ///< cos(keep): at or above, the normal's own
    float farCos;   ///< cos(face): at or below, the face's
};

/// The two angles as the cosines the shaders compare with. The face angle is
/// held to at least the keep angle (whichever slider moved last, the rule
/// stays one blend), and the cosines are taken to four decimals - which at
/// the defaults is exactly the 0.9659 and 0.8192 the rule was written with.
/// Equal angles become a step at that angle: the far cosine is kept just
/// under the near one, so the smoothstep has a width.
[[nodiscard]] inline NormalBlendCosines normalBlendCosines(int keepDegrees, int faceDegrees) {
    const int keep = std::clamp(keepDegrees, 0, kNormalBlendMaxDegrees);
    const int face = std::clamp(std::max(faceDegrees, keep), 0, kNormalBlendMaxDegrees);
    constexpr double kRad = 3.14159265358979323846 / 180.0;
    const auto rounded = [](double c) { return static_cast<float>(std::round(c * 10000.0) / 10000.0); };
    NormalBlendCosines out{rounded(std::cos(keep * kRad)), rounded(std::cos(face * kRad))};
    if (out.farCos > out.nearCos - 1e-4f) out.farCos = out.nearCos - 1e-4f;
    return out;
}

}  // namespace wowee::rendering
