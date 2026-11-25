#pragma once

// What is made in the world but not loaded yet, for the character's movement.
//
// The client's movement gathers the faces about the mover (0x0075ff90 ->
// 0x007a5f20), and the gather fails where a building whose box the place is in
// has not been loaded yet - a game object's as a map's, all in one list
// (0x007a55e0: no root at +0x1e0, or no groups) - and where an ADT is not
// (0x007a5a60): the unit is not moved, and does not fall. A game object's box
// is there with the object, from its display (GameObjectDisplayInfo's GeoBox),
// so that the order in which the disk answers does not matter. What the realm
// has not told of is not there for either.

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include <glm/glm.hpp>

namespace wowee::core::ground_coming {

/// How long the character is held for what is coming, at the most: a disk
/// that slow has failed.
inline constexpr float kHoldSeconds = 30.0f;

/// A display's GeoBox.
struct Box {
    glm::vec3 lo{0.0f};
    glm::vec3 hi{0.0f};
    [[nodiscard]] bool empty() const { return hi.x <= lo.x && hi.y <= lo.y && hi.z <= lo.z; }
};

/// Whether an object at `at` whose display has this box may have something in
/// the box `lo` to `hi`, however it is turned about the upright.
[[nodiscard]] inline bool reaches(const Box& box, const glm::vec3& at, float scale, const glm::vec3& lo,
                                  const glm::vec3& hi) {
    if (box.empty()) return false;
    if (at.z + box.lo.z * scale > hi.z || at.z + box.hi.z * scale < lo.z) return false;
    const float x = std::max(std::abs(box.lo.x), std::abs(box.hi.x)) * scale;
    const float y = std::max(std::abs(box.lo.y), std::abs(box.hi.y)) * scale;
    const glm::vec2 closest = glm::clamp(glm::vec2(at), glm::vec2(lo), glm::vec2(hi)) - glm::vec2(at);
    return glm::dot(closest, closest) <= x * x + y * y;
}

/// Whether a display's model is a building (a WMO).
[[nodiscard]] inline bool isBuilding(const std::string& path) {
    if (path.size() < 4) return false;
    std::string end = path.substr(path.size() - 4);
    for (char& c : end) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return end == ".wmo";
}

/// Holds the character over what is coming, for so long and no longer.
class Hold {
public:
    /// One frame: whether something is coming here. True: not moved.
    [[nodiscard]] bool step(bool coming, float seconds) {
        if (!coming) {
            held_ = 0.0f;
            return false;
        }
        held_ += seconds;
        return held_ <= kHoldSeconds;
    }

private:
    float held_ = 0.0f;
};

/// For tests: WOWEE_DEBUG_OBJECT_LOAD_MS makes each game object's model that
/// much later (on the thread that reads it). Nothing without it.
inline void debugDelay() {
    static const int ms = [] {
        const char* v = std::getenv("WOWEE_DEBUG_OBJECT_LOAD_MS");
        return v ? std::clamp(std::atoi(v), 0, 120000) : 0;
    }();
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace wowee::core::ground_coming
