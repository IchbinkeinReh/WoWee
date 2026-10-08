#pragma once

/// A weapon's swing trail (SWING, 0x007e4c30 to 0x007e4ff0): a spell kit's
/// CharProc 8 starts one on each weapon in the unit's hands, and each frame
/// the weapon model lays the blade's bottom and top ($WTB, $WTT) down into a
/// ring of 128 points, drawn as an untextured strip that fades out.
///
/// Pure arithmetic: SpellVisualSystem finds the blade and hands the strip on.

#include "rendering/client_ribbon.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace wowee::rendering::swing_trail {

/// The blade's events on a weapon model, as the client reads their ids.
inline constexpr uint32_t kEventBladeBottom = 0x42545724u;  ///< "$WTB"
inline constexpr uint32_t kEventBladeTop = 0x54545724u;     ///< "$WTT"

/// 0x007265c0 case 8: the colour, ParamZero as 0xRRGGBB under ParamThree's
/// alpha, and ParamTwo the milliseconds it lays points for - each chopped.
struct Start {
    uint32_t colour = 0;
    uint32_t durationMs = 0;
};
inline Start startFor(float paramZero, float paramTwo, float paramThree) {
    const auto chop = [](float v) { return static_cast<uint32_t>(static_cast<int32_t>(v)); };
    return Start{.colour = (chop(paramThree) << 24) | chop(paramZero), .durationMs = chop(paramTwo)};
}

/// 0x007e4ce0: untextured, unfogged, unlit, both faces, alpha blended,
/// tested and not written.
inline client_ribbon::MaterialState material() {
    client_ribbon::MaterialState m;
    m.blend = 2;
    m.lit = false;
    m.fogged = false;
    m.cull = false;
    m.depthTest = true;
    m.depthWrite = false;
    m.alphaRef = 1.0f / 255.0f;
    return m;
}

class Trail {
public:
    /// 0x007e4ff0: started, or started again and emptied.
    void start(const Start& s, uint32_t nowMs) {
        head_ = tail_ = 0;
        colour_ = s.colour;
        durationMs_ = s.durationMs;
        startMs_ = lastMs_ = nowMs;
    }

    /// 0x007e4ce0: a frame with the blade's top and bottom where they are
    /// now; `out` the strip to draw (empty for none). False once it has
    /// faded away, when the client lets it go (0x007e4f50).
    bool step(uint32_t nowMs, const glm::vec3& top, const glm::vec3& bottom,
              std::vector<client_ribbon::Vertex>& out) {
        out.clear();
        const uint32_t elapsed = nowMs - startMs_;
        if (elapsed < durationMs_) {
            push(bottom);
            push(top);
        }
        const uint32_t alpha = colour_ >> 24;
        const int32_t sinceLast = static_cast<int32_t>(nowMs - lastMs_);
        lastMs_ = nowMs;
        // 0x00407930 chops; the step is a byte, at least 1.
        auto step = static_cast<uint8_t>(
            static_cast<int32_t>(static_cast<float>(sinceLast) * 0.0033333334f * static_cast<float>(alpha)));
        if (step == 0) step = 1;
        const uint32_t pairs = alpha / step;
        if (pairs < 2) return false;
        if (tail_ < head_ - static_cast<int32_t>(2 * pairs)) tail_ = head_ - static_cast<int32_t>(2 * pairs);
        const int32_t count = head_ - tail_;
        if (count == 0) return false;
        if (count > 2) {
            // 0x007e4b70: oldest first, a pair's alpha a step less than the
            // last's.
            uint32_t c = colour_;
            for (int32_t i = tail_; i < head_; ++i) {
                if ((i & 1) == 0) c = (c & 0x00FFFFFFu) | (((c >> 24) - step) & 0xFFu) << 24;
                client_ribbon::Vertex v;
                v.position = points_[static_cast<uint32_t>(i) & 0x7Fu];
                v.color = c;
                out.push_back(v);
            }
            // Past half its time the trail's own alpha goes down a step.
            if (static_cast<int32_t>(durationMs_ >> 1) < static_cast<int32_t>(elapsed))
                colour_ = (colour_ & 0x00FFFFFFu) | (((colour_ >> 24) - step) & 0xFFu) << 24;
        }
        return true;
    }

    uint32_t colour() const { return colour_; }

private:
    void push(const glm::vec3& p) {
        const int32_t at = head_++;
        if (tail_ < at - 0x7F) tail_ = at - 0x7F;
        points_[static_cast<uint32_t>(at) & 0x7Fu] = p;
    }

    std::array<glm::vec3, 128> points_{};
    int32_t head_ = 0;
    int32_t tail_ = 0;
    uint32_t colour_ = 0;
    uint32_t durationMs_ = 0;
    uint32_t startMs_ = 0;
    uint32_t lastMs_ = 0;
};

}  // namespace wowee::rendering::swing_trail
