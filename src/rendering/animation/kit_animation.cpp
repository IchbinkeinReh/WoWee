#include "rendering/animation/kit_animation.hpp"

namespace wowee {
namespace rendering {
namespace kit_animation {

uint32_t askFlags(uint32_t kitFlags) {
    return ((kitFlags & 0x4) ? 0x2u : 0u) | ((kitFlags & 0x8) ? 0x4u : 0u) | ((kitFlags & 0x10) ? 0x8u : 0u) |
           ((kitFlags & 0x100) ? 0x20u : 0u);
}

Action onKitPlayed(const anim::AnimationDataTable& data, const KitPlay& kit, int32_t shown) {
    Action a;
    // A state kit's is an override while it is on (0x0071e400).
    if (kit.type == kTypeState) return a;
    a.askFlags = askFlags(kit.kitFlags);
    const bool precast = kit.type == kTypePrecast;  // 0x00745230 gives it 0x40
    // A PrecastKit with a StartAnimID: that, and the cast's loop is it
    // until it ends (0x007224d0 with +0xa38 0x10000).
    if (precast && kit.startAnim >= 0) {
        a.kind = Action::Kind::Start;
        a.anim = static_cast<uint32_t>(kit.startAnim);
        return a;
    }
    a.clearsStart = true;
    if (kit.anim < 0) return a;
    // A PrecastKit's AnimID only for a spell with a cast time, and not over
    // the CastKit's animation playing.
    const bool castKitShown = kit.castKitAnim > 0 && kit.castKitAnim == shown;
    if (precast && (!kit.castTime || castKitShown)) return a;
    const auto anim = static_cast<uint32_t>(kit.anim);
    // 0x0071d510: StandWound, CombatWound, CombatCritical are the wound.
    const uint32_t behavior = data.empty() ? anim : data.behavior(anim);
    if (behavior > 7 && behavior < 11) {
        a.kind = Action::Kind::Wound;
        a.critical = behavior == 10;
        return a;
    }
    a.kind = Action::Kind::Play;
    a.anim = anim;
    a.once = !precast;
    a.holdsCastLoop = kit.type == kTypePlayImpact;
    return a;
}

}  // namespace kit_animation
}  // namespace rendering
}  // namespace wowee
