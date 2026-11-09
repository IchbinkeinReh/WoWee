#include "rendering/collision_sweep.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wowee::rendering::collision {

Body::Body(const glm::vec3& feetPos, float halfWidth, float height) : r(halfWidth), h(height) {
    place(feetPos);
}

void Body::place(const glm::vec3& feetPos) {
    // 0x0075ca80: the point at the feet, the column's foot 1.849399 r above
    // it, its top at h.
    feet = feetPos;
    const float zb = kPyramidRise * r;
    const glm::vec2 corner[4] = {{-r, -r}, {-r, r}, {r, r}, {r, -r}};
    v[0] = feet;
    for (int i = 0; i < 4; ++i) {
        v[1 + i] = feet + glm::vec3(corner[i], zb);
        v[5 + i] = feet + glm::vec3(corner[i], h);
    }
    // 0x0075c8f0: the planes, each facing out.
    const glm::vec3 n[9] = {{-1, 0, 0},
                            {1, 0, 0},
                            {0, 1, 0},
                            {0, -1, 0},
                            {0, 0, 1},
                            {-kPyramidNormalXY, 0, -kPyramidNormalZ},
                            {kPyramidNormalXY, 0, -kPyramidNormalZ},
                            {0, kPyramidNormalXY, -kPyramidNormalZ},
                            {0, -kPyramidNormalXY, -kPyramidNormalZ}};
    for (int i = 0; i < 9; ++i) {
        p[i].n = n[i];
        p[i].d = -glm::dot(n[i], v[kBodyFaces[i][0]]);
    }
}

glm::vec3 Body::boxMin() const { return feet + glm::vec3(-r, -r, 0.0f); }
glm::vec3 Body::boxMax() const { return feet + glm::vec3(r, r, h); }

void sweepBox(const Body& body, const glm::vec3& dir, float dist, glm::vec3& outMin,
              glm::vec3& outMax) {
    const glm::vec3 m = dir * std::max(dist, kMinLookAhead);
    outMin = glm::min(body.boxMin(), body.boxMin() + m) - glm::vec3(kGatherPad);
    outMax = glm::max(body.boxMax(), body.boxMax() + m) + glm::vec3(kGatherPad);
}

void clipFacet(std::vector<glm::vec3>& poly, const Plane& plane, std::vector<glm::vec3>& scratch) {
    if (poly.size() < 3) {
        poly.clear();
        return;
    }
    // s > 0 is the side kept.
    float sMin = std::numeric_limits<float>::max();
    float sMax = std::numeric_limits<float>::lowest();
    for (const auto& v : poly) {
        const float s = -plane.at(v);
        sMin = std::min(sMin, s);
        sMax = std::max(sMax, s);
    }
    if (sMin > -kSkin) return;
    if (sMax < kSkin) {
        poly.clear();
        return;
    }
    scratch.clear();
    const size_t count = poly.size();
    glm::vec3 prev = poly[count - 1];
    float sPrev = -plane.at(prev);
    for (size_t i = 0; i < count; ++i) {
        const glm::vec3& cur = poly[i];
        const float sCur = -plane.at(cur);
        if (sPrev >= 0.0f && sCur >= 0.0f) {
            scratch.push_back(cur);
        } else if (sPrev >= 0.0f) {
            // Leaving the kept side: the crossing, unless the vertex before
            // it was as good as on the plane.
            if (sPrev > kSkin) scratch.push_back(prev + (cur - prev) * (sPrev / (sPrev - sCur)));
        } else if (sCur >= 0.0f) {
            if (sCur > kSkin) scratch.push_back(prev + (cur - prev) * (sPrev / (sPrev - sCur)));
            scratch.push_back(cur);
        }
        prev = cur;
        sPrev = sCur;
    }
    if (scratch.size() < 3) {
        poly.clear();
        return;
    }
    poly.swap(scratch);
}

namespace {
// How far along `dir` the plane goes before it reaches `v`; where the plane
// hardly moves along `dir` at all (2^-22), the client takes the distance
// itself.
float reach(const Plane& plane, float speed, const glm::vec3& v) {
    const float s = plane.at(v);
    return std::abs(speed) >= kTiny22 ? s / speed : s;
}
}  // namespace

bool solveFace(const Body& body, int face, const glm::vec3& dir, std::vector<glm::vec3>& poly,
               float* best, std::vector<glm::vec3>& scratch) {
    if (poly.empty()) return false;
    const Plane& plane = body.p[face];
    const float speed = glm::dot(plane.n, dir);
    // The least reach, never below nothing.
    float t = *best;
    bool allBehind = true;
    for (const auto& v : poly) {
        const float r = reach(plane, speed, v);
        if (r < t) t = std::max(r, 0.0f);
        if (r > -kTiny20) allBehind = false;
    }
    // Every vertex behind the face: what is there is inside the body
    // already or beside it. Cut it to the body as it stands; nothing left
    // is a miss, and what is left more than 1/36 behind is passed through.
    // Otherwise it touches now.
    if (allBehind) {
        for (int k = 0; k < 9; ++k) {
            if (k == face) continue;
            clipFacet(poly, body.p[k], scratch);
            if (poly.empty()) return false;
        }
        bool deep = true;
        for (const auto& v : poly) {
            if (reach(plane, speed, v) > -kMinLookAhead) {
                deep = false;
                break;
            }
        }
        if (deep) return false;
    }
    if (*best <= t) return false;
    *best = t;
    return true;
}

SweepResult sweep(const Body& body, const glm::vec3& dir, float dist,
                  const std::vector<Facet>& facets, bool pyramidOnly) {
    SweepResult result;
    if (std::abs(dist) < kTiny20) return result;
    const glm::vec3 m = dir * std::max(dist, kMinLookAhead);

    float best = dist;
    std::vector<glm::vec3> poly, scratch;
    Plane sides[4];
    // The pyramid's faces first, then the column's (0x0075cd70, 0x0075be80,
    // 0x0075bc50).
    static constexpr int kOrder[9] = {5, 6, 7, 8, 0, 1, 2, 3, 4};
    for (int face : kOrder) {
        if (pyramidOnly && face < 5) continue;
        const Plane& fp = body.p[face];
        if (glm::dot(fp.n, m) <= 0.0f) continue;
        const int* fv = kBodyFaces[face];
        const int corners = fv[3] < 0 ? 3 : 4;
        glm::vec3 centre(0.0f);
        for (int k = 0; k < corners; ++k) centre += body.v[fv[k]];
        centre /= static_cast<float>(corners);
        // The sides of the face's path: through each edge, along the motion,
        // facing away from the face.
        bool flat = false;
        for (int k = 0; k < corners; ++k) {
            const glm::vec3& a = body.v[fv[k]];
            const glm::vec3& b = body.v[fv[(k + 1) % corners]];
            glm::vec3 n = glm::cross(b - a, m);
            const float len2 = glm::dot(n, n);
            if (len2 < kTiny20) {
                flat = true;
                break;
            }
            n /= std::sqrt(len2);
            if (glm::dot(n, centre - a) > 0.0f) n = -n;
            sides[k].n = n;
            sides[k].d = -glm::dot(n, a);
        }
        if (flat) continue;

        // 0x0075c5a0: the nearest facet in the face's path, no further
        // than the move; a tie goes to the later one.
        float faceBest = dist;
        int faceFacet = -1;
        for (size_t fi = 0; fi < facets.size(); ++fi) {
            const Facet& f = facets[fi];
            if (!facesMotion(f, dir)) continue;
            poly.assign(f.v, f.v + 3);
            for (int k = 0; k < corners && !poly.empty(); ++k) clipFacet(poly, sides[k], scratch);
            if (poly.empty()) continue;
            float t = std::numeric_limits<float>::max();
            if (!solveFace(body, face, dir, poly, &t, scratch)) continue;
            if (t <= faceBest) {
                faceBest = t;
                faceFacet = static_cast<int>(fi);
            }
        }
        if (faceFacet < 0) continue;
        // The facet the sweep reports is the last face's that found one,
        // nearest or not, as the client's is.
        result.facet = faceFacet;
        result.hit = true;
        // The body's faces that touch within 1/720 of the nearest are the
        // contacts; a nearer one starts the list again and goes first.
        if (faceBest < best - kSkin) {
            result.contacts.clear();
            result.contacts.push_back(face);
        } else if (faceBest < best + kSkin) {
            result.contacts.push_back(face);
        }
        if (faceBest < best) {
            best = faceBest;
            if (result.contacts.size() > 1) std::swap(result.contacts.front(), result.contacts.back());
        }
    }
    result.allowed = best >= kSkin ? best : 0.0f;
    return result;
}

}  // namespace wowee::rendering::collision
