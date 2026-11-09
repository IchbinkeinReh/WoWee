#pragma once

// The collision port's test safety (not the client's): where the unit last
// stood, so that a runaway fall - a fault of the port - can be stopped and the
// character put back. The place belongs to one world: a map change, a
// teleport, a transport boarded or left all make it some other world's, and a
// fall measured against it there is no fall at all. On 2026-11-08 the
// zeppelin's map change (map 0 -> 1) was read as a fall from the deck over
// Tirisfal to Durotar, and the character put back at the map-0 deck's
// coordinates on map 1, where she fell.

#include <glm/glm.hpp>

#include <optional>

namespace wowee::rendering::collision {

class SafePlace {
public:
    /// How far below the place a fall is taken for a runaway.
    static constexpr float kRunawayDrop = 40.0f;
    /// No farther across than this is the place still this world's: a
    /// position any farther from it came by a teleport, not a fall.
    static constexpr float kSameWorldReach = 100.0f;

    /// Stood here.
    void stoodAt(const glm::vec3& at) {
        pos_ = at;
        valid_ = true;
    }
    /// Gone with the world it was in: a map change, a teleport, a transport
    /// boarded or left.
    void forget() { valid_ = false; }
    /// Moved with what carries the unit (a transport), as the unit was.
    void carry(const glm::vec3& by) { pos_ += by; }
    [[nodiscard]] bool valid() const { return valid_; }

    /// Where a fall at `at` puts the unit back: the place it last stood, if
    /// it is a runaway (more than kRunawayDrop below it, or nothing at all
    /// below) and the place is still within this world's reach. None
    /// otherwise - never a place from another map or another side of one.
    [[nodiscard]] std::optional<glm::vec3> runawayRestore(const glm::vec3& at, bool nothingBelow) const {
        if (!valid_) return std::nullopt;
        const glm::vec2 across(at.x - pos_.x, at.y - pos_.y);
        if (glm::dot(across, across) > kSameWorldReach * kSameWorldReach) return std::nullopt;
        if (at.z < pos_.z - kRunawayDrop || nothingBelow) return pos_;
        return std::nullopt;
    }

private:
    glm::vec3 pos_{0.0f};
    bool valid_ = false;
};

}  // namespace wowee::rendering::collision
