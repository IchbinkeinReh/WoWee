#include "rendering/animation/rider_mount.hpp"

namespace wowee {
namespace rendering {
namespace rider_mount {

namespace ba = base_animation;

bool takes(const anim::AnimationDataTable& data, uint32_t animation) {
    return upper_body::isMountAnimation(data.behavior(animation));
}

ba::Pick base(const anim::AnimationDataTable& data, upper_body::Unit rider, uint32_t flags,
              const std::function<bool(uint32_t)>& has, const std::function<bool(uint32_t)>& hasShown) {
    rider.riding = true;
    // What the rider plays (0x00717260, 0x007172b0): its pose.
    const ba::Playing playing{static_cast<int32_t>(rider.riderPose), static_cast<int32_t>(rider.riderPose)};
    const ba::Ask ask{.free = ba::kMountFree, .flags = flags};
    const ba::Pick p = ba::pick(data, rider, playing, ask, has, hasShown);
    if (p.anim == ba::kKeep || takes(data, p.anim)) return p;
    // A ranged weapon left out over the ride (0x00714f90 asks no slot): the
    // mount is none of its.
    rider.rangedDrawn = false;
    return ba::pick(data, rider, playing, ask, has, hasShown);
}

ba::Pick afterOneShot(const anim::AnimationDataTable& data, const upper_body::Unit& rider, uint32_t ended,
                      const std::function<bool(uint32_t)>& has, const std::function<bool(uint32_t)>& hasShown) {
    if (const auto own = ba::afterOneShot(data, rider, ended); own && takes(data, own->anim)) return *own;
    return base(data, rider, 0x10, has, hasShown);
}

}  // namespace rider_mount
}  // namespace rendering
}  // namespace wowee
