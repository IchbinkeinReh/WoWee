#pragma once
// The client's M2 point lights outside the ray traced lighting (CM2Lighting).
//
// Every enabled point light of a model the client animated this frame is
// linked into a 64 x 64 grid of 20-yard cells, wrapping (0x00834c70: cell
// floor(x * 0.05) & 63, floor(y * 0.05) & 63; enabled and disabled by
// 0x008356f0 from 0x00828a00, which turns a light on while its visibility
// track holds and its model was stepped this frame). A model gathers the
// cells round its centre (0x0081e400 with the sphere 0x00831af0 builds,
// radius 0: the cells from floor(x / 20 - 0.5) to floor(x / 20 + 0.5)) and
// keeps the four lights nearest by distance squared (0x00834f60). It draws
// with light 0 the combined directional light and lights 1-3 point lights
// (0x008353d0), taken from the far end of that sorted list, so with four
// gathered the nearest of them is the one left out - as the client does.
//
// A point light's diffuse is its diffuse colour track times its intensity
// track times the model's alpha (0x0082f0f0: the light entry's +0x98 / +0xa4
// of the light entry); its ambient is not used for a point light. The
// attenuation is never taken from the file's attenuation tracks: every M2
// light keeps the constant, linear and quadratic terms its constructor gives
// it (0x00834a40: 0, 0.7, 0.03), which the vertex programs apply as
// 1 / (c + l d + q d^2) times max(N.L, 0), summed with the directional light
// and the ambient and the whole clamped to 1 (Diffuse_T1's point-light
// variant).
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wowee::rendering::m2_vertex_lights {

inline constexpr float kCellScale = 0.05f;  // 0x00af58a0, 0x00af59d4
inline constexpr int kGridSize = 64;
inline constexpr float kAttenConstant = 0.0f;
inline constexpr float kAttenLinear = 0.7f;
inline constexpr float kAttenQuadratic = 0.03f;
inline constexpr int kGathered = 4;   // 0x00834f60's slots
inline constexpr int kDrawn = 3;      // lights 1-3 of 0x008353d0

struct Light {
    glm::vec3 position{0.0f};
    glm::vec3 diffuse{0.0f};
};

/// What the vertex programs take of one light at distance d.
inline float attenuation(float d) {
    const float den = kAttenConstant + kAttenLinear * d + kAttenQuadratic * d * d;
    return den > 1e-6f ? 1.0f / den : 0.0f;
}

inline int cellOf(float v) {
    return static_cast<int>(std::floor(v * kCellScale)) & (kGridSize - 1);
}

/// This frame's lights, linked into their cells.
class Grid {
public:
    void clear() {
        for (auto& c : cells_) c.clear();
        lights_.clear();
    }
    void add(const Light& l) {
        const uint32_t i = static_cast<uint32_t>(lights_.size());
        lights_.push_back(l);
        cells_[static_cast<size_t>(cellOf(l.position.y) * kGridSize + cellOf(l.position.x))].push_back(i);
    }
    [[nodiscard]] bool empty() const { return lights_.empty(); }
    [[nodiscard]] const std::vector<Light>& lights() const { return lights_; }

    /// The lights a model at `centre` draws with, as 0x0081e400, 0x00834f60
    /// and 0x008353d0 pick them: at most three, `count` of them set.
    struct Picked {
        std::array<Light, kDrawn> lights{};
        int count = 0;
    };
    [[nodiscard]] Picked pick(const glm::vec3& centre) const {
        Picked out;
        if (lights_.empty()) return out;
        // The four nearest by distance squared, ascending (0x00834f60).
        std::array<uint32_t, kGathered> idx{};
        std::array<float, kGathered> dist{};
        int n = 0;
        auto offer = [&](uint32_t li) {
            const glm::vec3 d = lights_[li].position - centre;
            const float d2 = glm::dot(d, d);
            int k = n;
            if (n == kGathered) {
                if (d2 >= dist[kGathered - 1]) return;
                k = kGathered - 1;
            }
            while (k > 0 && !(dist[k - 1] < d2)) {
                idx[k] = idx[k - 1];
                dist[k] = dist[k - 1];
                --k;
            }
            idx[k] = li;
            dist[k] = d2;
            if (n < kGathered) ++n;
        };
        // The cells covering the centre's +-10 yards (radius 0), x inner,
        // y outer, wrapping as the grid does (0x0081e400).
        const int x0 = static_cast<int>(std::floor(centre.x * kCellScale - 0.5f)) & (kGridSize - 1);
        const int x1 = static_cast<int>(std::floor(centre.x * kCellScale + 0.5f)) & (kGridSize - 1);
        const int y0 = static_cast<int>(std::floor(centre.y * kCellScale - 0.5f)) & (kGridSize - 1);
        const int y1 = static_cast<int>(std::floor(centre.y * kCellScale + 0.5f)) & (kGridSize - 1);
        for (int y = y0;; y = (y + 1) & (kGridSize - 1)) {
            for (int x = x0;; x = (x + 1) & (kGridSize - 1)) {
                for (uint32_t li : cells_[static_cast<size_t>(y * kGridSize + x)]) offer(li);
                if (x == x1) break;
            }
            if (y == y1) break;
        }
        // Lights 1..3 from the far end of the sorted list (0x008353d0).
        for (int k = n - 1; k >= 0 && out.count < kDrawn; --k) out.lights[static_cast<size_t>(out.count++)] = lights_[idx[k]];
        return out;
    }

private:
    std::array<std::vector<uint32_t>, kGridSize * kGridSize> cells_{};
    std::vector<Light> lights_;
};

/// The per-vertex light of up to three point lights at a vertex `p` with
/// normal `n` (unit), as the vertex programs sum it.
inline glm::vec3 vertexLight(const Grid::Picked& picked, const glm::vec3& p, const glm::vec3& n) {
    glm::vec3 sum(0.0f);
    for (int i = 0; i < picked.count; ++i) {
        const Light& l = picked.lights[static_cast<size_t>(i)];
        const glm::vec3 toL = l.position - p;
        const float d = glm::length(toL);
        if (d <= 1e-6f) continue;
        sum += l.diffuse * (std::max(glm::dot(n, toL / d), 0.0f) * attenuation(d));
    }
    return sum;
}

}  // namespace wowee::rendering::m2_vertex_lights
