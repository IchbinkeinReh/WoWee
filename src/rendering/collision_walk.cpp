#include "rendering/collision_walk.hpp"
#include "rendering/collision_fall.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wowee::rendering::collision {

namespace {
constexpr float kTiny = 2.3841858e-07f;   // 2^-22, the client's "nothing" for a length
constexpr float kTinyDist = 9.536743e-07f;  // 2^-20

glm::vec2 xy(const glm::vec3& v) { return {v.x, v.y}; }
}  // namespace

SweepResult Sweeper::run(const Mover& m, const glm::vec3& dir, float dist) {
    body_ = Body(m.pos, m.halfWidth, m.height);
    glm::vec3 lo, hi;
    sweepBox(body_, dir, dist, lo, hi);
    facets_.clear();
    if (gather_) gather_(lo, hi, facets_);
    return sweep(body_, dir, dist, facets_);
}

float stepHeightOf(const Mover& m) { return m.player ? m.stepHeight : 2.0f; }

float stepBudget(const Mover& m) {
    float left = stepHeightOf(m);
    if (m.stepping) left -= m.pos.z - m.stepBaseZ;
    return std::max(left, 0.0f);
}

void startFall(Mover& m) {
    // 0x00988370(0): from rest, here; stepping (in 0xf91fffff) cleared.
    m.falling = true;
    m.stepping = false;
    m.fallStartZ = m.pos.z;
    m.fallMs = 0;
    m.fallV0 = 0.0f;
}

bool contactPush(const Body& body, const std::vector<int>& contacts, glm::vec3& out) {
    const size_t count = contacts.size();
    if (count == 0 || count > 4) return false;
    for (int c : contacts) {
        if (std::abs(body.p[c].n.z - (-kPyramidNormalZ)) >= kTinyDist) return false;
    }
    const auto n = [&](size_t i) { return body.p[contacts[i]].n; };
    switch (count) {
        case 1:
            out = n(0);
            break;
        case 2:
            // Two neighbouring faces: their sum, which 0.638556 brings to
            // unit length.
            out = (n(0) + n(1)) * 0.638556f;
            break;
        case 3: {
            // Of three, two face each other along one axis; the third is
            // the one alone on the other.
            int alongX[3], alongY[3];
            int nx = 0, ny = 0;
            for (size_t i = 0; i < 3; ++i) {
                if (std::abs(n(i).y) < kTinyDist) alongX[nx++] = static_cast<int>(i);
                else if (std::abs(n(i).x) < kTinyDist) alongY[ny++] = static_cast<int>(i);
            }
            const int pick = (nx == 1) ? alongX[0] : (ny == 1 ? alongY[0] : 0);
            out = n(static_cast<size_t>(pick));
            break;
        }
        default:
            out = glm::vec3(0.0f);
            break;
    }
    return true;
}

glm::vec3 liftOnto(const Mover& m, const glm::vec3& dir, float& remaining, glm::vec3 n, bool usePush,
                   const glm::vec3& push) {
    // A face too steep to stand on, met by the pyramid's faces only: the
    // pyramid's push stands in for it.
    if (usePush && n.z < kWalkableNormalZ && glm::dot(push, push) >= kTiny) n = -push;
    float rise = -glm::dot(n, dir) * remaining;
    if (std::abs(n.z) < kTiny) {
        rise = rise >= 0.0f ? std::numeric_limits<float>::max() : std::numeric_limits<float>::lowest();
    } else {
        rise /= n.z;
    }
    float climb;
    if (!m.stepping || rise >= 0.0f) {
        const float budget = stepBudget(m);
        if (rise <= budget) {
            const float step = stepHeightOf(m);
            climb = rise;
            if (rise < -step) {
                remaining = -(remaining / rise) * step;
                climb = -step;
            }
        } else {
            remaining = (remaining / rise) * budget;
            climb = budget;
        }
    } else {
        remaining = 0.0f;
        climb = stepBudget(m);
    }
    return {0.0f, 0.0f, climb};
}

glm::vec3 slideAlong(const glm::vec3& dir, float remaining, float horizontalLeft, const glm::vec3& n) {
    if (n.z < 0.0f && -n.z < kWalkableNormalZ) return glm::vec3(0.0f);
    glm::vec2 nh = xy(n);
    const float len2 = glm::dot(nh, nh);
    if (len2 > kTiny) nh /= std::sqrt(len2);
    float push = -glm::dot(n, dir) * remaining;
    const float across = glm::dot(xy(n), nh);
    if (std::abs(across) >= kTiny) push /= across;
    glm::vec2 slide = nh * (push + kSlidePushOff);
    const glm::vec2 total = xy(dir) * remaining + slide;
    const float total2 = glm::dot(total, total);
    if (horizontalLeft * horizontalLeft < total2) {
        const float scale = horizontalLeft / std::sqrt(total2);
        slide = total * scale - xy(dir) * remaining;
    }
    return {slide.x, slide.y, 0.0f};
}

namespace {

// 0x0075d0a0: the feet within 1/12 yard of the triangle, seen along its
// normal.
bool feetOver(const Facet& f, const glm::vec3& feet) {
    for (int k = 0; k < 3; ++k) {
        const glm::vec3& a = f.v[k];
        const glm::vec3& b = f.v[(k + 1) % 3];
        const glm::vec3& c = f.v[(k + 2) % 3];
        glm::vec3 side = glm::cross(b - a, f.n);
        const float len2 = glm::dot(side, side);
        if (len2 < kTinyDist) return false;
        side /= std::sqrt(len2);
        if (glm::dot(side, c - a) > 0.0f) side = -side;
        if (glm::dot(side, feet - a) > 1.0f / 12.0f) return false;
    }
    return true;
}

// 0x0075cfe0: a corner of the body's top within 1/720 of the facet's plane.
bool headTouches(const Mover& m, const Facet& f) {
    const float top = m.pos.z + m.height;
    const float r = m.halfWidth;
    const glm::vec3 corners[4] = {{m.pos.x - r, m.pos.y - r, top},
                                  {m.pos.x + r, m.pos.y - r, top},
                                  {m.pos.x + r, m.pos.y + r, top},
                                  {m.pos.x - r, m.pos.y + r, top}};
    for (const auto& c : corners) {
        if (std::abs(glm::dot(f.n, c) + f.d) < kSkin) return true;
    }
    return false;
}

// 0x0075d1c0: whether a facet met is ground to go on along (true), and
// whether meeting it overhead while stepping makes the unit fall.
bool classify(Mover& m, const Facet& f, bool* fallNow) {
    *fallNow = false;
    const float limit = m.player ? kWalkableNormalZ : kWalkableNormalZOther;
    if (limit < f.n.z) {
        if (feetOver(f, m.pos)) m.stepping = false;
        return true;
    }
    if (!(f.n.z > 0.0f) && headTouches(m, f)) {
        if (m.stepping) {
            *fallNow = true;
            m.stepping = false;
        }
        return -f.n.z >= kWalkableNormalZ;
    }
    return m.stepping;
}

}  // namespace

bool tryStepUp(Mover& m, const glm::vec2& dir, const glm::vec3& n, const Facet& met, Sweeper& sweeper) {
    const glm::vec3 start = m.pos;
    const float across = std::max(m.halfWidth + kSkin, stepHeightOf(m) * kStepAcross);
    glm::vec2 way = dir;
    // A face leaning back (but too steep to walk): step straight into it,
    // if that meets the same face.
    if (n.z < kWalkableNormalZ && n.z > 0.0f) {
        const glm::vec2 into = -glm::normalize(xy(n));
        const SweepResult probe = sweeper.run(m, glm::vec3(into, 0.0f), across);
        way = into;
        if (probe.facet < 0 || sweeper.facet(probe.facet).n != met.n) way = dir;
    }
    // Up by what is left of the step height.
    const SweepResult up = sweeper.run(m, glm::vec3(0.0f, 0.0f, 1.0f), stepBudget(m));
    float rise = up.allowed;
    if (std::abs(rise + (m.stepping ? m.pos.z - m.stepBaseZ : 0.0f)) < kTiny) {
        m.stepping = false;
        return true;
    }
    m.pos.z += rise;
    // Across.
    SweepResult side = sweeper.run(m, glm::vec3(way, 0.0f), across);
    float went = side.allowed;
    m.pos += glm::vec3(way * went, 0.0f);
    if (std::abs(went - across) >= kTinyDist && side.facet >= 0) {
        const Facet ahead = sweeper.facet(side.facet);
        bool fallNow = false;
        if (classify(m, ahead, &fallNow)) {
            // Walkable ahead: lift onto it the rest of the way across.
            float left = across - went;
            glm::vec3 push(0.0f);
            const bool usePush = contactPush(sweeper.body(), side.contacts, push);
            const glm::vec3 lift = liftOnto(m, glm::vec3(way, 0.0f), left, ahead.n, usePush, push);
            const glm::vec3 move = lift + glm::vec3(way * left, 0.0f);
            const float len = glm::length(move);
            if (len != 0.0f) {
                const SweepResult more = sweeper.run(m, move / len, len);
                const glm::vec3 moved = move / len * more.allowed;
                m.pos += moved;
                went += glm::length(xy(moved));
                rise += lift.z;
            }
        }
    }
    if (side.facet < 0 || m.halfWidth < went) {
        // Down again by as much as it went up.
        const SweepResult down = sweeper.run(m, glm::vec3(0.0f, 0.0f, -1.0f), rise);
        m.pos.z -= down.allowed;
        const bool landsOnWalkable = down.facet >= 0 && !tooSteep(sweeper.facet(down.facet).n);
        // Nothing under it, walkable ground, or a fall from there that
        // carries the unit clear past the edge (0x007619c0): a step.
        if (down.facet < 0 || landsOnWalkable ||
            fallsClearOfEdge(m, way, start, rise - down.allowed, sweeper)) {
            m.pos = start;
            if (!m.stepping) m.stepBaseZ = start.z;
            m.stepping = true;
            return true;
        }
    }
    m.stepping = false;
    m.pos = start;
    return true;
}

WalkResult walk(Mover& m, const glm::vec2& dirIn, float dist, uint32_t ms, Sweeper& sweeper) {
    WalkResult out;
    if (std::abs(dist) < kTinyDist) return out;
    const float total = static_cast<float>(ms);
    glm::vec2 groundDir = dirIn;            // local_38
    glm::vec3 dir(dirIn, 0.0f);             // local_30
    float remaining = dist;                 // local_8
    float horizontalLeft = dist;            // local_3c
    float timeLeft = total;                 // local_18
    float timeUsed = 0.0f;                  // local_40
    float climbLeft = dist / m.halfWidth;   // local_64
    int shortSweeps = 0;                    // local_60
    bool stop = false;                      // local_1c
    bool fallNow = false;                   // local_5c
    int ground = -1;

    SweepResult s = sweeper.run(m, dir, remaining);
    for (;;) {
        glm::vec3 move = dir * s.allowed;
        if (move.z <= climbLeft) {
            climbLeft -= move.z;
        } else {
            const float k = climbLeft / move.z;
            stop = true;
            move *= k;
            s.allowed *= k;
            climbLeft = 0.0f;
        }
        m.pos += move;

        if (s.facet < 0) {
            // Nothing in the way for the rest of the time.
            timeUsed += timeLeft;
            const float left = total - timeUsed;
            if (left < 1.0f) {
                // Keep to the ground: down by the pyramid's slope over the move.
                const SweepResult down = sweeper.run(m, glm::vec3(0.0f, 0.0f, -1.0f), dist * kStepDownPerMove);
                m.pos.z -= down.allowed;
                if (down.facet < 0) {
                    if (!m.stepping) {
                        startFall(m);
                        out.fell = true;
                        return out;
                    }
                } else {
                    const glm::vec3& n = sweeper.facet(down.facet).n;
                    if (!tooSteep(n)) {
                        m.stepping = false;
                    } else if (!m.stepping || glm::dot(xy(n), groundDir) > -kFacingEpsilon) {
                        startFall(m);
                        out.fell = true;
                        return out;
                    }
                    ground = down.facet;
                    out.groundNormal = n;
                }
                break;
            }
            // Time left over: go on along the original direction.
            groundDir = dirIn;
            remaining = (left / total) * dist;
            dir = glm::vec3(dirIn, 0.0f);
            timeLeft = left;
            s = sweeper.run(m, dir, remaining);
            continue;
        }

        // A facet in the way.
        const Facet hit = sweeper.facet(s.facet);
        ground = s.facet;
        if (tooSteep(hit.n)) {
            out.blocked = true;
            out.blocking = hit;
        }
        out.groundNormal = hit.n;
        const float spent = (s.allowed / remaining) * timeLeft;
        remaining -= s.allowed;
        horizontalLeft -= glm::length(glm::vec2(move));
        if (spent < 1.0f) {
            if (++shortSweeps > 5) {
                stop = true;
                break;
            }
        } else {
            shortSweeps = 1;
        }
        timeUsed += spent;
        timeLeft -= spent;
        if (total - timeUsed < 1.0f) {
            if (std::abs(total - timeUsed) >= kTinyDist) stop = true;
            break;
        }

        glm::vec3 turn(0.0f);
        glm::vec3 push(0.0f);
        bool usePush = false;
        if (classify(m, hit, &fallNow)) {
            usePush = contactPush(sweeper.body(), s.contacts, push);
            const float before = remaining;
            turn = liftOnto(m, dir, remaining, hit.n, usePush, push);
            timeLeft = (remaining / before) * timeLeft;
        } else {
            if (fallNow) break;
            stop = true;
            usePush = contactPush(sweeper.body(), s.contacts, push);
            const bool wasStepping = m.stepping;
            if (!tryStepUp(m, groundDir, hit.n, hit, sweeper)) return out;
            if (m.stepping) {
                const float before = remaining;
                turn = liftOnto(m, dir, remaining, hit.n, usePush, push);
                timeLeft = (remaining / before) * timeLeft;
            } else if (wasStepping) {
                startFall(m);
                out.fell = true;
                return out;
            } else {
                turn = slideAlong(dir, remaining, horizontalLeft, hit.n);
            }
        }
        const glm::vec3 next = dir * remaining + turn;
        remaining = glm::length(next);
        if (remaining < 0.001f) {
            stop = true;
            break;
        }
        dir = next / remaining;
        glm::vec2 g = xy(next);
        const float gl = glm::length(g);
        if (gl >= kTiny) groundDir = g / gl;
        if (horizontalLeft - gl > kTinyDist) stop = true;
        horizontalLeft = gl;
        // Stepping: no higher than the step height over where it began.
        if (m.stepping && next.z > kTinyDist) {
            const float top = m.pos.z + next.z;
            const float ceiling = stepHeightOf(m) + m.stepBaseZ;
            if (ceiling < top) {
                const float allowed = ceiling - m.pos.z;
                remaining = (allowed / next.z) * remaining;
                if (std::abs(allowed - next.z) >= kTinyDist) stop = true;
                if (remaining < 0.001f) {
                    startFall(m);
                    out.fell = true;
                    return out;
                }
            }
        }
        s = sweeper.run(m, dir, remaining);
    }

    if (fallNow) {
        startFall(m);
        out.fell = true;
        return out;
    }
    if (ground < 0 && !m.stepping) {
        startFall(m);
        out.fell = true;
        return out;
    }
    out.stopped = stop;
    out.groundFacet = ground;
    return out;
}

}  // namespace wowee::rendering::collision
