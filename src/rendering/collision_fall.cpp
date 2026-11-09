#include "rendering/collision_fall.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wowee::rendering::collision {

namespace {
constexpr float kEpsA = 2.3841858e-07f;
constexpr float kEpsB = 9.536743e-07f;
constexpr float kInvG = 0.051837362f;       // 1/g
constexpr float kHalfG = 9.645553f;         // g/2
constexpr float kTwoG = 38.58221f;          // 2g
constexpr float kTwoOverG = 0.103674725f;   // 2/g

float terminal(bool safe) { return safe ? kSafeFallSpeed : kTerminalSpeed; }

// 0x00988220: the time a fall from rest takes to go `d`.
float timeFromRest(float d, bool safe) {
    const float T = terminal(safe);
    const float tT = kInvG * T;
    const float dT = tT * T * 0.5f;
    if (d >= dT) return tT + (d - dT) / T;
    if (d <= 0.0f) return 0.0f;
    return std::sqrt(d * kTwoOverG);
}

// 0x0075d2d0: a wall, neither floor nor ceiling (both bounds inclusive).
bool isWall(const glm::vec3& n) { return n.z >= -kWalkableNormalZ && n.z <= kWalkableNormalZ; }

// 0x0075d0a0 on a point: within 1/12 yard of the triangle seen along its
// normal.
bool overFacet(const Facet& f, const glm::vec3& p) {
    for (int k = 0; k < 3; ++k) {
        const glm::vec3& a = f.v[k];
        const glm::vec3& b = f.v[(k + 1) % 3];
        const glm::vec3& c = f.v[(k + 2) % 3];
        glm::vec3 side = glm::cross(b - a, f.n);
        const float len2 = glm::dot(side, side);
        if (len2 < kEpsB) return false;
        side /= std::sqrt(len2);
        if (glm::dot(side, c - a) > 0.0f) side = -side;
        if (glm::dot(side, p - a) > 1.0f / 12.0f) return false;
    }
    return true;
}

// 0x0075d340: walkable ground under the feet moved by `disp`.
bool landsOn(const Mover& m, const Facet& f, const glm::vec3& disp) {
    const float limit = m.player ? kWalkableNormalZ : kWalkableNormalZOther;
    return f.n.z > limit && overFacet(f, m.pos + disp);
}

// 0x0075db00: the normal of the facet's edge nearest `p`, turned along the
// facet and toward `dir`.
glm::vec3 edgeNormal(const Facet& f, const glm::vec3& p, const glm::vec3& dir) {
    const glm::vec3* base[3] = {&f.v[0], &f.v[1], &f.v[2]};
    glm::vec3 edges[3] = {f.v[1] - f.v[0], f.v[2] - f.v[1], f.v[0] - f.v[2]};
    float best = std::numeric_limits<float>::max();
    glm::vec3 e(0.0f);
    for (int i = 0; i < 3; ++i) {
        glm::vec3 d = edges[i];
        const float l2 = glm::dot(d, d);
        if (l2 > kEpsA) d /= std::sqrt(l2);
        const glm::vec3 foot = *base[i] + d * glm::dot(p - *base[i], d);
        const float dist2 = glm::dot(p - foot, p - foot);
        if (dist2 < best) {
            best = dist2;
            e = d;
        }
    }
    glm::vec3 out = glm::cross(e, f.n);
    if (glm::dot(out, dir) < 0.0f) out = -out;
    return out;
}

// 0x0075d740: a point of the body's foot (the point, the column's foot
// corners) within 1/720 of the facet's plane.
bool footOnPlane(const Mover& m, const Facet& f) {
    const float hw = m.halfWidth;
    const float k = hw * kPyramidRise;
    const glm::vec3 p = m.pos;
    const glm::vec3 pts[5] = {p,
                              p + glm::vec3(hw, hw, k),
                              p + glm::vec3(-hw, hw, k),
                              p + glm::vec3(hw, -hw, k),
                              p + glm::vec3(-hw, -hw, k)};
    for (const auto& q : pts) {
        if (std::abs(glm::dot(f.n, q) + f.d) < kSkin) return true;
    }
    return false;
}

// 0x0075d680: where three planes meet.
glm::vec3 meet(const Plane& a, const Plane& b, const glm::vec3& fn, float fd) {
    const glm::mat3 rows = glm::transpose(glm::mat3(fn, a.n, b.n));
    return glm::inverse(rows) * glm::vec3(-fd, -a.d, -b.d);
}

// 0x0075d4b0: the facet edge that runs most nearly through `p` along `c`.
glm::vec3 closestEdgeDir(const Facet& f, const glm::vec3& c, const glm::vec3& p) {
    glm::vec3 out(0.0f);
    float best = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i) {
        const glm::vec3& a = f.v[i];
        glm::vec3 e = a - f.v[(i + 1) % 3];
        const float len = glm::length(e);
        if (len < kEpsA) continue;
        e /= len;
        float score;
        if (std::abs(glm::dot(c, e) - 1.0f) >= kEpsB) {
            const glm::vec3 w = glm::cross(e, c);
            const float s = glm::dot(w, a) - glm::dot(w, p);
            score = s * s;
        } else {
            const float t = glm::dot(a - p, c);
            const glm::vec3 q = a - (p + c * t);
            score = glm::dot(q, q);
        }
        if (score < best) {
            best = score;
            out = e;
        }
    }
    return out;
}

// 0x0075e760: the normal to slide off a wall by when two of the body's faces
// touch it along a crease. (Its one-face case tests 0x0075d890, which can
// never be true, so it is left out.)
glm::vec3 creaseNormal(const Mover& m, const Facet& f, const Body& body, const std::vector<int>& contacts) {
    if (contacts.size() == 2) {
        const Plane& p0 = body.p[contacts[0]];
        const Plane& p1 = body.p[contacts[1]];
        glm::vec3 c = glm::cross(p0.n, p1.n);
        c *= 1.0f / std::sqrt(glm::dot(c, c));
        if (std::abs(c.z) >= kEpsB && std::abs(glm::dot(c, f.n)) >= kEpsB &&
            (std::abs(c.z - 1.0f) < kEpsB || !footOnPlane(m, f))) {
            const glm::vec3 point = meet(p0, p1, f.n, f.d);
            const glm::vec3 e = closestEdgeDir(f, c, point);
            glm::vec3 n = glm::cross(c, e);
            const float len = glm::length(n);
            if (std::abs(len) < kEpsA) return -p0.n;
            n /= len;
            return glm::dot(n, p0.n) <= 0.0f ? n : -n;
        }
    }
    return f.n;
}

// 0x0075e9c0: the sideways push that slides the rest of a fall's move off
// what it hit.
glm::vec2 slideVector(const Mover& m, const glm::vec3& dir, float allowed, float requested, const Facet& f,
                      const Body& body, const std::vector<int>& contacts) {
    const glm::vec3 n = !isWall(f.n) ? edgeNormal(f, m.pos + dir * allowed, dir)
                                     : creaseNormal(m, f, body, contacts);
    glm::vec2 h(n.x, n.y);
    const float h2 = glm::dot(h, h);
    if (h2 > kEpsA) h /= std::sqrt(h2);
    float k = (requested - allowed) * glm::dot(-n, dir);
    const float den = n.y * h.y + n.x * h.x;
    if (std::abs(den) >= kEpsA) k /= den;
    return h * (k + kSlidePushOff);
}

// 0x00987050 with the fall begun: how far below where it began the feet are
// to be after `ms`, from where they are.
float fallDrop(const Mover& m, uint32_t ms) {
    const float t = static_cast<float>(ms) * 0.001f;
    const float d = fallDistance(t, m.safeFall, m.fallV0);
    if (!m.falling) return d;
    return (m.pos.z - m.fallStartZ) + d;
}

enum class SweepOutcome { None, Hit, Failed };

struct FallSweepResult {
    SweepOutcome outcome = SweepOutcome::None;
    bool landed = false;
    bool ceiling = false;
    glm::vec2 slide{0.0f};
};

// 0x00760fc0: one sweep of a fall's move; `disp` and `allowed` come back as
// what was made of it.
FallSweepResult fallSweep(Mover& m, float tNow, glm::vec3& disp, float& allowed, Sweeper& sweeper) {
    FallSweepResult r;
    const float L = allowed;
    if (std::abs(L) < kEpsA) return r;
    const glm::vec3 dir = disp * (1.0f / L);
    const SweepResult s = sweeper.run(m, dir, L);
    allowed = s.allowed;
    if (s.facet < 0) return r;
    r.outcome = SweepOutcome::Hit;
    disp = dir * allowed;
    const Facet& f = sweeper.facet(s.facet);
    if (landsOn(m, f, disp)) {
        r.landed = true;
        return r;
    }
    if (dir.z > 0.0f) {
        // Rising into a flat ceiling (the column's top touching, 0x0075de40)
        // within the rise that is left.
        const float left = timeToApex(m) - tNow;
        bool flat = false;
        for (int c : s.contacts) flat |= std::abs(sweeper.body().p[c].n.z - 1.0f) < kEpsB;
        if (left >= 0.0f && allowed <= left * m.fallSpeed && flat) {
            r.ceiling = true;
            return r;
        }
    }
    r.slide = slideVector(m, dir, allowed, L, f, sweeper.body(), s.contacts);
    return r;
}

// 0x0075e040: is the fall still on its way up over this sub-move?
bool ascending(const Mover& m, float tNow, float remain, const glm::vec3& disp, bool ceiling) {
    if (ceiling) return true;
    if (m.fallV0 > 0.0f) return false;
    const float tA = timeToApex(m);
    if (tA > tNow + remain) return true;
    if (tA < tNow) return false;
    const float hd = (tA - tNow) * m.fallSpeed;
    return !(hd * hd <= disp.x * disp.x + disp.y * disp.y);
}

// 0x0075eb00: how long the cut-short move took, from the fall's own curve
// and from how far across the ground it got.
float hitTime(Mover& m, float remain, float tNow, glm::vec3& disp, float& allowed, bool ceiling, float hlen) {
    const bool asc = ascending(m, tNow, remain, disp, ceiling);
    float hT = 0.0f;
    if (hlen > kEpsB) hT = (glm::length(glm::vec2(disp)) / hlen) * remain;
    const float tHit = fallTimeFor(m, (m.fallStartZ - m.pos.z) - disp.z, asc);
    if (tNow >= tHit) {
        disp = glm::vec3(0.0f);
        allowed = 0.0f;
        return 0.0f;
    }
    const float dt = tHit - tNow;
    if (remain < dt) return remain;
    if (hT <= dt) return dt;
    const float r = dt / hT;
    disp.x *= r;
    disp.y *= r;
    allowed = glm::length(disp);
    return dt;
}

// 0x007612b0: the fall's move over `ms`, swept and slid.
FallResult integrate(Mover& m, uint32_t ms, glm::vec3 d, Sweeper& sweeper) {
    FallResult out;
    const float tTot = static_cast<float>(ms) * 0.001f;
    const float t0 = static_cast<float>(m.fallMs) * 0.001f;
    float el = 0.0f;
    int iter = 0;
    bool zeroH = false;
    int pass = 0;
    bool ceiling = false;
    bool landed = false;
    float len = glm::length(d);
    float hlen = glm::length(glm::vec2(d));
    const float hlen0 = hlen;
    const float adz0 = std::abs(d.z);
    const glm::vec2 startXY(m.pos);
    float T = tTot;
    while (std::abs(len) >= kEpsB) {
        const glm::vec3 u = d * (1.0f / len);
        const float tNow = t0 + el;
        float allowed = len;
        const FallSweepResult r = fallSweep(m, tNow, d, allowed, sweeper);
        if (r.outcome == SweepOutcome::Failed) {
            T = tTot;
            break;
        }
        const float used = r.outcome == SweepOutcome::None
                               ? tTot - el
                               : hitTime(m, tTot - el, tNow, d, allowed, r.ceiling, hlen);
        m.pos += d;
        el += used;
        if (r.ceiling) {
            ceiling = true;
            m.fallingFar = true;
            m.fallMs = 0;
            m.fallV0 = 0.0f;
            m.fallStartZ = m.pos.z;
            T = el;
            break;
        }
        if (r.landed) {
            landed = true;
            T = el;
            break;
        }
        if (tTot <= el + 0.0005f) {
            T = tTot;
            break;
        }
        len -= allowed;
        glm::vec2 slide = r.slide;
        if (used <= 0.0005f) {
            if (++iter >= 6) {
                if (zeroH || std::abs(hlen) < kEpsB) {
                    landed = true;
                    T = el;
                    break;
                }
                iter = 0;
                m.fallSpeed = 0.0f;
                zeroH = true;
                d = glm::vec3(0.0f, 0.0f, u.z * len);
                len = std::abs(d.z);
                ++pass;
                continue;
            }
        } else {
            iter = 1;
        }
        d = glm::vec3(u.x * len + slide.x, u.y * len + slide.y, u.z * len);
        if (pass != 0) {
            // Slid further across than the whole move went, and well past
            // its drop: it lands where it is.
            const glm::vec2 q = (glm::vec2(m.pos) - startXY) + glm::vec2(d);
            const float q2 = glm::dot(q, q);
            const float a = hlen0 + kEpsB;
            const float b = adz0 * 1.1866661f;
            if (a * a < q2 && b * b < q2) {
                landed = true;
                T = el;
                break;
            }
        }
        glm::vec2 h(d);
        hlen = glm::length(h);
        if (std::abs(hlen) >= kEpsB) {
            h *= 1.0f / hlen;
            const float sp = glm::dot(h, m.fallDir) * m.fallSpeed;
            m.fallSpeed = sp < 0.0f ? 0.0f : sp;
        } else {
            m.fallSpeed = 0.0f;
        }
        m.fallDir = h;
        len = glm::length(d);
        ++pass;
    }
    out.ms = static_cast<uint32_t>(std::lround(T * 1000.0f));
    if (!ceiling) m.fallMs += out.ms;
    if (landed) land(m);
    out.landed = landed;
    out.ceiling = ceiling;
    if (m.falling) {
        // 0x0075cf80 inline: falling far after half a second, or a ninth of
        // a yard below where a jump began.
        if (!m.fallingFar) {
            if (m.fallV0 == 0.0f ? m.fallMs > 499 : m.pos.z <= m.fallStartZ - 1.0f / 9.0f) m.fallingFar = true;
        }
    }
    return out;
}

}  // namespace

float fallDistance(float t, bool safe, float v0) {
    const float T = terminal(safe);
    const float v = std::min(T, v0);
    if (kGravity * t + v >= T) {
        const float tT = (T - v) * kInvG;
        return (kHalfG * tT + v) * tT + (t - tT) * T;
    }
    return (t * kHalfG + v) * t;
}

float fallTimeFor(const Mover& m, float d, bool asc) {
    const float T = terminal(m.safeFall);
    float v = m.fallV0;
    if (v > T) v = T;
    if (std::abs(v) < kEpsA) return timeFromRest(d, m.safeFall);
    const float disc = v * v + kTwoG * d;
    const float s = disc > 0.0f ? std::sqrt(disc) : 0.0f;
    const float tNeg = (-v - s) * kInvG;
    const float tPos = (s - v) * kInvG;
    const float tTerm = (T - v) * kInvG;
    if (!asc) return tPos <= tTerm ? tPos : tTerm + (d - (v + tTerm * kHalfG) * tTerm) / T;
    return tNeg < 0.0f ? 0.0f : tNeg;
}

float timeToApex(const Mover& m) { return m.falling ? m.fallV0 * -kInvG : 0.0f; }

void startFallAt(Mover& m, float v0) {
    startFall(m);
    m.fallV0 = v0;
}

void land(Mover& m) {
    m.falling = false;
    m.fallingFar = false;
}

FallResult fallStep(Mover& m, uint32_t ms, float hdist, const glm::vec2& dir, Sweeper& sweeper) {
    FallResult out;
    if (ms == 0) return out;
    const float D = fallDrop(m, m.fallMs + ms);
    const glm::vec3 d(dir.x * hdist, dir.y * hdist, -D);
    if (std::abs(glm::length(d)) < kEpsA) {
        m.fallMs += ms;
        out.ms = ms;
        return out;
    }
    return integrate(m, ms, d, sweeper);
}

bool fallsClearOfEdge(Mover& m, const glm::vec2& dir, const glm::vec3& start, float height,
                      Sweeper& sweeper) {
    const uint32_t tms = static_cast<uint32_t>(std::lround(std::sqrt((height + height) * kInvG) * 1000.0f));
    // 0x0075b480 / 0x0075b4f0 keep the fall's state, not the position.
    const Mover saved = m;
    startFallAt(m, 0.0f);
    const float D = fallDrop(m, tms);
    const glm::vec3 d(m.fallDir.x, m.fallDir.y, -D);
    uint32_t done = 0;
    while (tms != 0 && m.falling && done < tms) {
        const uint32_t step = integrate(m, tms - done, d, sweeper).ms;
        if (step == 0) break;
        done += step;
    }
    // Restored: the flags, the fall time and the direction and speed;
    // left as the probe set them: the position, where the fall began and
    // the speed it began with.
    const glm::vec3 at = m.pos;
    const float startZ = m.fallStartZ;
    const float v0 = m.fallV0;
    m = saved;
    m.pos = at;
    m.fallStartZ = startZ;
    m.fallV0 = v0;
    const glm::vec2 off = glm::vec2(at) - glm::vec2(start);
    const float L = glm::length(off);
    return m.halfWidth <= L && glm::dot(dir, off / L) > 0.9848077f;
}

}  // namespace wowee::rendering::collision
