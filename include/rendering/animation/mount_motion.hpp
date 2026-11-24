#pragma once

// What the player's mount does besides the animations it plays.
//
// Those are its rider's unit's (rider_mount.hpp, CharacterRenderer's
// setRiderMount): a mount has no state machine of its own. What is left here
// is how it banks in flight, the bob a model without a seat is ridden at, and
// when it makes a sound standing about.

#include <random>

namespace wowee {
namespace rendering {

class MountMotion {
public:
    struct Input {
        bool moving = false;
        bool forwardOrBack = false;
        bool flying = false;
        bool swimming = false;
        bool taxiFlight = false;
        float deltaTime = 0.0f;
        float characterYaw = 0.0f;  ///< degrees
        /// The sequence the mount plays, for the bob.
        bool haveAnimation = false;
        float animationTime = 0.0f;
        float animationDuration = 0.0f;
    };

    struct Output {
        float roll = 0.0f;  ///< radians, the bank
        float bob = 0.0f;   ///< yards up and down
        bool playIdleSound = false;
    };

    /// On a mount.
    void reset();
    Output update(const Input& in);

private:
    std::mt19937 rng_;
    float idleSoundTimer_ = 0.0f;
    float nextIdleSoundTime_ = 60.0f;
    float prevYaw_ = 0.0f;
    float roll_ = 0.0f;
};

} // namespace rendering
} // namespace wowee
