#include "game/quest_poi_blobs.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace wowee::game::quest_blobs {

Settings& settings() {
    static Settings s;
    return s;
}

std::vector<std::pair<int32_t, int32_t>> smoothArea(const std::vector<std::pair<int32_t, int32_t>>& in,
                                                    int samples) {
    const size_t n = in.size();
    if (n < 2) return in;
    std::vector<std::array<float, 2>> cp;
    cp.reserve(n + 3);
    cp.push_back({static_cast<float>(in[n - 1].first), static_cast<float>(in[n - 1].second)});
    for (const auto& p : in) cp.push_back({static_cast<float>(p.first), static_cast<float>(p.second)});
    cp.push_back({static_cast<float>(in[0].first), static_cast<float>(in[0].second)});
    cp.push_back({static_cast<float>(in[1].first), static_cast<float>(in[1].second)});
    const size_t segments = cp.size() - 3;
    const auto eval = [&cp](size_t seg, float t) {
        static constexpr float m[16] = {-0.5f, 1.0f, -0.5f, 0.0f, 1.5f, -2.5f, 0.0f, 1.0f,
                                        -1.5f, 2.0f, 0.5f, 0.0f, 0.5f, -0.5f, 0.0f, 0.0f};
        std::array<float, 2> out{0.0f, 0.0f};
        for (size_t k = 0; k < 4; ++k) {
            const float w = ((m[k * 4] * t + m[k * 4 + 1]) * t + m[k * 4 + 2]) * t + m[k * 4 + 3];
            out[0] += w * cp[seg + k][0];
            out[1] += w * cp[seg + k][1];
        }
        return out;
    };
    std::vector<float> lengths(segments, 0.0f);
    float total = 0.0f;
    for (size_t s = 0; s < segments; ++s) {
        auto prev = eval(s, 0.0f);
        float t = 0.05f;
        for (int i = 0; i < 20; ++i) {
            const auto cur = eval(s, t);
            lengths[s] += std::sqrt((cur[0] - prev[0]) * (cur[0] - prev[0]) +
                                    (cur[1] - prev[1]) * (cur[1] - prev[1]));
            prev = cur;
            t += 0.05f;
        }
        total += lengths[s];
    }
    std::vector<std::pair<int32_t, int32_t>> out;
    float t = 0.01f;
    for (int k = 0; k < samples; ++k) {
        std::array<float, 2> p{};
        if (t <= 0.0f) {
            p = cp.front();
        } else if (t >= 1.0f) {
            p = cp.back();
        } else {
            const float at = total * t;
            size_t seg = 0;
            float before = 0.0f;
            while (seg + 1 < segments && before + lengths[seg] <= at) before += lengths[seg++];
            p = eval(seg, lengths[seg] != 0.0f ? (at - before) / lengths[seg] : 0.0f);
        }
        out.emplace_back(static_cast<int32_t>(std::nearbyint(p[0])),
                         static_cast<int32_t>(std::nearbyint(p[1])));
        t = 1.0f / static_cast<float>(samples) + t;
    }
    return out;
}

bool pointInPolygon(float x, float y, const std::vector<std::pair<float, float>>& pts) {
    bool inside = false;
    const size_t n = pts.size();
    if (n < 3) return false;
    auto v0 = pts[n - 1];
    bool yflag0 = v0.second >= y;
    for (const auto& v1 : pts) {
        const bool yflag1 = v1.second >= y;
        if (yflag0 != yflag1 &&
            (((v1.second - y) * (v0.first - v1.first) >= (v1.first - x) * (v0.second - v1.second)) ==
             yflag1)) {
            inside = !inside;
        }
        yflag0 = yflag1;
        v0 = v1;
    }
    return inside;
}

void Blob::bound() {
    if (pts.empty()) return;
    minU = maxU = pts[0].first;
    minV = maxV = pts[0].second;
    for (const auto& p : pts) {
        minU = std::min(minU, p.first);
        maxU = std::max(maxU, p.first);
        minV = std::min(minV, p.second);
        maxV = std::max(maxV, p.second);
    }
}

bool objectiveOpen(uint32_t mask, int32_t objectiveIndex) {
    return (mask & (1u << (objectiveIndex & 31))) != 0;
}

void merge(std::vector<Blob>& blobs, uint32_t mask) {
    const Settings& set = settings();
    if (!set.merging || mask == 0xffffffffu) return;
    for (size_t i = 0; i < blobs.size(); ++i) {
        if (!blobs[i].shown || !objectiveOpen(mask, blobs[i].objectiveIndex)) continue;
        for (size_t j = i + 1; j < blobs.size(); ++j) {
            if (!blobs[j].shown || !objectiveOpen(mask, blobs[j].objectiveIndex)) continue;
            Blob& a = blobs[i];
            Blob& c = blobs[j];
            if (!(std::max(a.minU, c.minU) < std::min(a.maxU, c.maxU) &&
                  std::max(a.minV, c.minV) < std::min(a.maxV, c.maxV))) {
                continue;
            }
            const float areaA = (a.maxV - a.minV) * (a.maxU - a.minU);
            const float areaC = (c.maxV - c.minV) * (c.maxU - c.minU);
            Blob& larger = areaA < areaC ? c : a;
            Blob& smaller = areaA < areaC ? a : c;
            size_t inside = 0;
            for (const auto& p : smaller.pts) {
                if (pointInPolygon(p.first, p.second, larger.pts)) ++inside;
            }
            if (smaller.pts.empty() ||
                static_cast<float>(inside) / static_cast<float>(smaller.pts.size()) <= set.mergeThreshold) {
                continue;
            }
            smaller.shown = false;
            for (int32_t o : smaller.objectives) {
                if (larger.objectives.size() >= 4) break;
                if (std::find(larger.objectives.begin(), larger.objectives.end(), o) == larger.objectives.end())
                    larger.objectives.push_back(o);
            }
        }
    }
}

}  // namespace wowee::game::quest_blobs
