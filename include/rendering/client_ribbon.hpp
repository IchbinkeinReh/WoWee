#pragma once

/// The client's ribbon emitter (CRibbonEmitter, 0x0097f510 to 0x00980b70):
/// a strip of edges laid down behind a bone as it moves, each edge a pair of
/// points across the bone's own Y axis, aged out after the emitter's
/// lifetime and textured along the strip by its age.
///
/// Pure arithmetic: M2Renderer feeds it the bone's matrix and the tracks each
/// frame (as 0x00828a00 does) and draws the strip it keeps.

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wowee::rendering::client_ribbon {

/// One point of the strip (CGxVertexPCT: position, colour, texture).
struct Vertex {
    glm::vec3 position{0.0f};
    uint32_t color = 0xFFFFFFFFu;  ///< ARGB
    glm::vec2 uv{0.0f};
};

/// A ribbon's material as the client keeps and applies it. 0x00832ea0 turns
/// each M2 material the emitter names into these bits - lit (not 0x1),
/// fogged (not 0x2), depth test (not 0x8), depth write (not 0x10), culled
/// (not 0x4) - and its blend through the table at 0x00a45570, the same
/// entries as the M2 batches' 0x00a453b0 row; 0x00980b70 sets each before
/// it draws the strip with that material's texture.
struct MaterialState {
    /// The M2 blend the pipeline is built for: 0 opaque, 1 alpha key, 2
    /// alpha, 3 no-alpha add, 4 add, 5 mod, 6 mod2x. 7 has no entry in the
    /// table (it reads past it) and is drawn as alpha, as the M2 batches are.
    uint16_t blend = 2;
    bool lit = true;
    bool fogged = true;
    bool depthTest = true;
    bool depthWrite = true;
    bool cull = true;
    /// The alpha test's reference, from the Gx blend (0x00873ee0, table
    /// 0x00ad8b7c): none for opaque and no-alpha add, 224/255 for alpha
    /// key, 1/255 for the rest.
    float alphaRef = 1.0f / 255.0f;
};

[[nodiscard]] inline MaterialState materialState(uint16_t m2Flags, uint16_t m2Blend) {
    MaterialState m;
    m.blend = m2Blend <= 6 ? m2Blend : 2;
    m.lit = (m2Flags & 0x01u) == 0;
    m.fogged = (m2Flags & 0x02u) == 0;
    m.cull = (m2Flags & 0x04u) == 0;
    m.depthTest = (m2Flags & 0x08u) == 0;
    m.depthWrite = (m2Flags & 0x10u) == 0;
    switch (m.blend) {
        case 0:
        case 3: m.alphaRef = 0.0f; break;            // Gx 0 and Gx 10
        case 1: m.alphaRef = 224.0f / 255.0f; break; // Gx 1
        default: m.alphaRef = 1.0f / 255.0f; break;  // Gx 2 to 5
    }
    return m;
}

class Emitter {
public:
    /// 0x009808a0: the rate rounds up to a whole number of edges a second,
    /// the lifetime is at least a quarter of a second, and there is room for
    /// a lifetime's edges and two more. The texture's grid cell is the
    /// emitter's rows and columns; the colour starts white, both heights at
    /// 10, the gravity at 0.
    void init(float edgesPerSecond, float edgeLifetime, uint32_t rows, uint32_t cols) {
        rate_ = std::ceil(edgesPerSecond);
        lifetime_ = std::max(edgeLifetime, 0.25f);
        const float room = std::ceil(lifetime_ * rate_) + 2.0f;
        capacity_ = room > 2.0f && room < 1e6f ? static_cast<uint32_t>(std::lround(room)) : 2u;
        edges_.assign(capacity_, Edge{});
        head_ = tail_ = 0;
        accum_ = 0.0f;
        haveTransform_ = false;
        updated_ = false;
        uPerSecond_ = 1.0f / lifetime_;
        cols_ = std::max(cols, 1u);
        cellW_ = 1.0f / static_cast<float>(cols_);
        cellH_ = 1.0f / static_cast<float>(std::max(rows, 1u));
        color_ = 0xFFFFFFFFu;
        texSlot_ = 0;
        setCell();
        visible_ = true;
        above_ = below_ = 10.0f;
        gravity_ = 0.0f;
    }

    /// 0x0097fb60: the colour, keeping the alpha.
    void setColor(const glm::vec3& rgb) {
        color_ = (color_ & 0xFF000000u) | (byte(rgb.r) << 16) | (byte(rgb.g) << 8) | byte(rgb.b);
    }
    /// 0x0097fba0.
    void setAlpha(float a) { color_ = (color_ & 0x00FFFFFFu) | (byte(a) << 24); }
    void setAbove(float h) { above_ = h; }   ///< 0x0097f610
    void setBelow(float h) { below_ = h; }   ///< 0x0097f620
    void setGravity(float g) { gravity_ = g; }  ///< 0x0097f630
    /// 0x0097f5f0: which cell of the texture's grid, row by row.
    void setTexSlot(uint32_t slot) {
        if (slot == texSlot_) return;
        texSlot_ = slot;
        setCell();
    }
    /// 0x0097f570: hidden, it forgets where it was, so it starts again
    /// rather than drawing one edge across the gap.
    void setVisible(bool visible) {
        visible_ = visible;
        if (!visible_) haveTransform_ = false;
    }

    /// 0x0097f940: the bone's world matrix this frame (columns: its X, Y and
    /// Z axes and its position). Kept with last frame's.
    void setTransform(const glm::mat4& world) {
        if (!visible_) return;
        const glm::vec3 pos(world[3]);
        const glm::vec3 y(world[1]);
        const glm::vec3 z(world[2]);
        if (!haveTransform_) {
            prev_ = {pos, y, z};
            accum_ = 0.0f;
            haveTransform_ = true;
        } else {
            prev_ = cur_;
        }
        cur_ = {pos, y, z};
    }

    /// 0x00980090. `hidden` is the visibility track off.
    void update(float dt, bool hidden) {
        if (!updated_ && rate_ > 0.0f) dt = 1.0f / rate_ + 0.0001f;
        dt = std::clamp(dt, 0.0f, lifetime_);
        // The old edges that outlive the lifetime this step.
        while (tail_ != head_ && edges_[tail_].age + dt > lifetime_) tail_ = next(tail_);

        if (!hidden && visible_ && haveTransform_) {
            const float f = dt * rate_ + accum_;
            computeEnds();  // 0x0097f700
            if (f >= 1.0f) {
                const int n = static_cast<int>(std::lround(std::floor(f - 1.0f))) + 1;
                float c = 1.0f;
                for (int k = 0; k < n; ++k) {
                    // Spread along the way from last frame's place to this
                    // one's, each as old as the part of the step since.
                    const float t = (c - accum_) * (1.0f / (f - accum_));
                    edges_[head_].bottom.color = edges_[head_].top.color = color_;
                    emit(-(t * dt), t, true);
                    c += 1.0f;
                }
            }
            accum_ = f - std::floor(f);
            // The tip, where the bone is now; not yet an edge of its own.
            emit(0.0f, 1.0f, false);
            Edge& tip = edges_[head_];
            tip.bottom.uv = {u0_, v0_};
            tip.top.uv = {u0_, v1_};
            tip.bottom.color = tip.top.color = color_;
        }

        for (uint32_t i = tail_; i != head_; i = next(i)) {
            Edge& e = edges_[i];
            // Gravity as age squared: what it adds over this step.
            const float g = (e.age * 2.0f + dt) * gravity_ * dt;
            e.bottom.position.z += g;
            e.top.position.z += g;
            e.age += dt;
            // Across the cell by age: its start at birth, its end at the
            // lifetime.
            const float u = e.age * cellW_ * uPerSecond_ + u0_;
            e.bottom.uv = {u, v0_};
            e.top.uv = {u, v1_};
        }
        updated_ = true;
    }

    /// 0x00980b70: the strip, oldest edge first, the tip last; nothing while
    /// the emitter holds no edge. Two points an edge, bottom then top.
    void appendStrip(std::vector<Vertex>& out) const {
        if (tail_ == head_) return;
        for (uint32_t i = tail_;; i = next(i)) {
            out.push_back(edges_[i].bottom);
            out.push_back(edges_[i].top);
            if (i == head_) break;
        }
    }

    [[nodiscard]] uint32_t capacity() const { return capacity_; }
    /// Edges laid down and still alive, the tip not counted.
    [[nodiscard]] uint32_t edgeCount() const {
        return head_ >= tail_ ? head_ - tail_ : capacity_ - tail_ + head_;
    }
    [[nodiscard]] float rate() const { return rate_; }
    [[nodiscard]] float lifetime() const { return lifetime_; }
    [[nodiscard]] uint32_t color() const { return color_; }

private:
    struct Frame {
        glm::vec3 pos{0.0f}, y{0.0f}, z{0.0f};
    };
    struct Edge {
        Vertex bottom, top;
        float age = 0.0f;
    };

    /// A colour channel as the client stores it: v x 255 + 0.5, truncated,
    /// its low byte (the 0xc00 rounding mode around fistp).
    static uint32_t byte(float v) {
        return static_cast<uint32_t>(static_cast<int>(v * 255.0f + 0.5f)) & 0xFFu;
    }
    [[nodiscard]] uint32_t next(uint32_t i) const { return i + 1 >= capacity_ ? 0 : i + 1; }

    /// 0x0097f510.
    void setCell() {
        const uint32_t row = texSlot_ / cols_;
        const uint32_t col = texSlot_ % cols_;
        u0_ = static_cast<float>(col) * cellW_;
        v0_ = static_cast<float>(row) * cellH_;
        v1_ = v0_ + cellH_;
    }

    /// 0x0097f700: both ends of last frame's edge and this frame's, below
    /// and above along the bone's Y axis, and each frame's Z axis as long as
    /// the way between them, which bends the strip between the two.
    void computeEnds() {
        const float d = glm::length(prev_.pos - cur_.pos);
        b0_ = prev_.pos - prev_.y * below_;
        b1_ = cur_.pos - cur_.y * below_;
        t0_ = prev_.pos + prev_.y * above_;
        t1_ = cur_.pos + cur_.y * above_;
        z0_ = prev_.z * d;
        z1_ = cur_.z * d;
    }

    /// 0x0097fef0: the edge `t` of the way along, and its age; `advance`
    /// makes it an edge rather than the tip.
    void emit(float age, float t, bool advance) {
        const float s = 1.0f - t;
        Edge& e = edges_[head_];
        e.bottom.position = (b0_ + z0_ * t) * s + (b1_ - z1_ * s) * t;
        e.top.position = (t0_ + z0_ * t) * s + (t1_ - z1_ * s) * t;
        e.age = age;
        if (advance) head_ = next(head_);
    }

    std::vector<Edge> edges_;
    uint32_t capacity_ = 2, head_ = 0, tail_ = 0;
    float rate_ = 0.0f, lifetime_ = 0.25f, accum_ = 0.0f;
    float uPerSecond_ = 4.0f, cellW_ = 1.0f, cellH_ = 1.0f, u0_ = 0.0f, v0_ = 0.0f, v1_ = 1.0f;
    uint32_t cols_ = 1, texSlot_ = 0;
    uint32_t color_ = 0xFFFFFFFFu;
    float above_ = 10.0f, below_ = 10.0f, gravity_ = 0.0f;
    bool visible_ = true, haveTransform_ = false, updated_ = false;
    Frame prev_, cur_;
    glm::vec3 b0_{0.0f}, b1_{0.0f}, t0_{0.0f}, t1_{0.0f}, z0_{0.0f}, z1_{0.0f};
};

}  // namespace wowee::rendering::client_ribbon
