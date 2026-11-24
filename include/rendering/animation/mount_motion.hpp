#pragma once

// What the player's mount does besides the animations it plays.
//
// Those are its rider's unit's (rider_mount.hpp, CharacterRenderer's
// setRiderMount): a mount has no state machine of its own. What is left here
// is how it banks in flight and the bob a model without a seat is ridden at.

namespace wowee {
namespace rendering {

class MountMotion {
public:
    struct Input {
        bool moving = false;
        bool forwardOrBack = false;
        bool flying = false;
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
    };

    /// On a mount.
    void reset();
    Output update(const Input& in);

private:
    float prevYaw_ = 0.0f;
    float roll_ = 0.0f;
};

} // namespace rendering
} // namespace wowee
