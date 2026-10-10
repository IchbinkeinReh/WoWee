#pragma once

// The client's particle step and its spline emitter's curve, as pure logic.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace wowee::rendering::m2_particle {

/// What an emitter hands every particle's step (CParticleEmitter2, read from
/// the file by 0x00832ea0): gravity +0xb4 (file 0x84 track), drag +0x168
/// (file 0x174), wind +0x16c..0x174 (file 0x1a0) and the age it blows
/// until +0x178 (file 0x1ac).
struct StepParams {
    float gravity = 0.0f;
    float drag = 0.0f;
    glm::vec3 wind{0.0f};
    float windTime = 0.0f;
};

/// One particle's move over dt (0x00979bb0). `age` is its age before the
/// step. Wind first, while the particle is younger than the wind time; then
/// the move with gravity's half-square term and gravity on the velocity; then
/// drag takes min(dt * drag, 1) of the velocity away.
inline void step(glm::vec3& position, glm::vec3& velocity, float age, float dt,
                 const StepParams& p) {
    if (age < p.windTime) {
        velocity += p.wind * dt;
    }
    position += velocity * dt;
    position.z -= p.gravity * dt * dt * 0.5f;
    velocity.z -= p.gravity * dt;
    if (p.drag != 0.0f) {
        const float k = std::min(dt * p.drag, 1.0f);
        velocity -= velocity * k;
    }
}

/// zSource (+0xbc, file track 0xF0), as its setter 0x00978da0 stores it:
/// anything under a thousandth is zero, and zero means "not used".
inline float zSourceValue(float v) {
    return std::fabs(v) < 0.001f ? 0.0f : v;
}

/// The direction a plane or spline emitter gives a particle born at `pos`
/// when zSource is set (0x009815c0, 0x00981d40): away from the point zSource
/// above the emitter's origin.
inline glm::vec3 zSourceDirection(const glm::vec3& pos, float zSource) {
    const glm::vec3 d(pos.x, pos.y, pos.z - zSource);
    const float len = std::sqrt(glm::dot(d, d));
    return len > 0.0f ? d / len : glm::vec3(0.0f, 0.0f, 1.0f);
}

/// The same for a sphere emitter (0x00981950), which only normalises a
/// vector longer than its epsilon.
inline glm::vec3 sphereZSourceDirection(const glm::vec3& pos, float zSource) {
    const glm::vec3 d(pos.x, pos.y, pos.z - zSource);
    const float lenSq = glm::dot(d, d);
    return lenSq > 2.3841858e-07f ? d / std::sqrt(lenSq) : d;
}

/// The spline emitter's curve (vtable 0x00aa2e20): cubic Bezier segments of
/// four points sharing their ends, walked by arc length.
class BezierSpline {
public:
    /// 0x004c4d50: n points make n / 3 segments and 3 * segments + 1 points.
    void setPoints(const std::vector<glm::vec3>& in) {
        const size_t segs = in.size() / 3;
        pts_.clear();
        segLen_.clear();
        total_ = 0.0f;
        if (segs == 0) return;
        const size_t n = std::min(in.size(), segs * 3 + 1);
        pts_.assign(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(n));
        while (pts_.size() < segs * 3 + 1) pts_.push_back(pts_.back());
        // 0x004c3830 measures only when there are more than three points.
        if (pts_.size() > 3) {
            for (size_t s = 0; s < segs; ++s) {
                segLen_.push_back(segmentLength(s * 3));
                total_ += segLen_.back();
            }
        }
    }

    bool empty() const { return pts_.empty(); }
    size_t segmentCount() const { return pts_.empty() ? 0 : (pts_.size() - 1) / 3; }
    float totalLength() const { return total_; }

    /// 0x004c3870 with mode 1: the first point at t <= 0, the last at t >= 1,
    /// otherwise the segment the arc length t * total falls in (0x004c3bd0)
    /// at the fraction of it that is left (0x004c39d0).
    glm::vec3 position(float t) const {
        if (pts_.empty()) return glm::vec3(0.0f);
        if (!(t > 0.0f)) return pts_.front();
        if (t >= 1.0f) return pts_.back();
        size_t seg = 0;
        float local = 0.0f;
        locate(t, seg, local);
        return bezier(seg * 3, local);
    }

    /// 0x004c3920 with mode 1 (0x004c4970 -> 0x004c3e70): the derivative at
    /// t clamped to 0..1.
    glm::vec3 tangent(float t) const {
        if (pts_.empty()) return glm::vec3(0.0f, 0.0f, 1.0f);
        t = std::clamp(t, 0.0f, 1.0f);
        size_t seg = 0;
        float local = 0.0f;
        locate(t, seg, local);
        return derivative(seg * 3, local);
    }

private:
    glm::vec3 bezier(size_t i, float t) const {
        // The basis at 0x00ac3778.
        const float u = 1.0f - t;
        return pts_[i] * (u * u * u) + pts_[i + 1] * (3.0f * t * u * u) +
               pts_[i + 2] * (3.0f * t * t * u) + pts_[i + 3] * (t * t * t);
    }
    glm::vec3 derivative(size_t i, float t) const {
        // The basis at 0x00b4a308.
        return pts_[i] * ((-3.0f * t + 6.0f) * t - 3.0f) +
               pts_[i + 1] * ((9.0f * t - 12.0f) * t + 3.0f) +
               pts_[i + 2] * ((-9.0f * t + 6.0f) * t) +
               pts_[i + 3] * (3.0f * t * t);
    }
    /// 0x004c3b10: twenty chords of a twentieth.
    float segmentLength(size_t i) const {
        float len = 0.0f;
        glm::vec3 prev = bezier(i, 0.0f);
        float t = 0.05f;
        for (int k = 0; k < 20; ++k) {
            const glm::vec3 cur = bezier(i, t);
            len += glm::length(cur - prev);
            prev = cur;
            t += 0.05f;
        }
        return len;
    }
    /// 0x004c3bd0: with fewer than two segments, t itself.
    void locate(float t, size_t& seg, float& local) const {
        const size_t segs = segmentCount();
        seg = 0;
        if (segs < 2 || segLen_.size() < segs) {
            local = t;
            return;
        }
        const float target = total_ * t;
        float acc = 0.0f;
        do {
            const float next = segLen_[seg] + acc;
            if (!(next <= target)) break;
            ++seg;
            acc = next;
        } while (seg < segs - 1);
        local = segLen_[seg] > 0.0f ? (target - acc) / segLen_[seg] : 0.0f;
    }

    std::vector<glm::vec3> pts_;
    std::vector<float> segLen_;
    float total_ = 0.0f;
};

/// A spline emitter's direction when zSource is unset (0x00981d40): up, turned
/// about the curve's tangent by `angle`; straight up when the vertical range
/// is zero.
inline glm::vec3 splineDirection(const glm::vec3& tangent, float angle) {
    const float len = glm::length(tangent);
    if (!(len > 0.0f)) return glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::quat q = glm::angleAxis(angle, tangent / len);
    return q * glm::vec3(0.0f, 0.0f, 1.0f);
}

}  // namespace wowee::rendering::m2_particle
