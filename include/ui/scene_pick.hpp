#pragma once

// Ray-picking entities under the cursor, shared by the two callers that need it.
//
// The left-click target picker and the right-click world picker each grew their
// own copy of this traversal. They agreed on everything that matters - how a
// unit is ranked against an object, that a hostile is preferred, that a corpse
// must not outrank the living - and had to be edited in lockstep to stay that
// way, which is exactly how one of them quietly diverges. What genuinely
// differs between them is expressed as parameters instead.

#include <cstdint>
#include <functional>
#include <memory>

#include <cmath>

#include <glm/glm.hpp>

#include "rendering/camera.hpp"  // rendering::Ray

namespace wowee {
namespace game { class GameHandler; class Entity; }

namespace ui {

/// Where a ray first meets a sphere, if it does at all.
///
/// The picker's own test, and the one every hand-rolled click target in the
/// interface needs. It was written out five times - once here and once in each
/// of the four files GameScreen was split into - and three of those four copies
/// were never called from the file they sat in. Identical to the character, so
/// nothing had gone wrong yet; five copies of a quadratic is simply five
/// chances for one of them to lose a sign.
inline bool raySphereIntersect(const rendering::Ray& ray, const glm::vec3& center,
                               float radius, float& tOut) {
    const glm::vec3 oc = ray.origin - center;
    const float b = glm::dot(oc, ray.direction);
    const float c = glm::dot(oc, oc) - radius * radius;
    const float disc = b * b - c;
    if (disc < 0.0f) return false;
    const float root = std::sqrt(disc);
    float t = -b - root;
    // Behind the near root means the origin is inside the sphere; the far root
    // is then the first surface the ray meets going forward.
    if (t < 0.0f) t = -b + root;
    if (t < 0.0f) return false;
    tOut = t;
    return true;
}

/// The parts of picking the two callers disagree about.
struct ScenePickParams {
    /// Fallback hit sphere for a unit with no render bounds. The right-click
    /// picker uses a slightly taller, tighter sphere than the target picker.
    float unitHitRadius = 1.8f;
    float unitHeightOffset = 1.0f;
    /// Reject what the world hides: anything whose sphere the ray reaches
    /// only past the first building or ground it meets. Without it the
    /// pointer found units and objects through floors and walls - in the
    /// Undercity, everything on the level below.
    bool occludeByWorld = true;
};

/// Everything the traversal found, before either caller applies its own rules.
struct ScenePick {
    uint64_t closestGuid = 0;          ///< Nearest hit of any pickable type.
    float    closestT = 1e30f;

    uint64_t livingUnitGuid = 0;       ///< Nearest living unit or player, by centre.
    float    livingUnitCenterT = 1e30f;
    uint64_t deadUnitGuid = 0;         ///< Nearest corpse, by centre.
    float    deadUnitCenterT = 1e30f;

    uint64_t objectGuid = 0;           ///< The interactable object best aimed at.
    float    objectCenterT = 1e30f;    ///< Its centre's distance along the ray.
    /// How far the ray passed from that object's centre, as a fraction of its
    /// radius: nought is dead centre, one is a graze. What objects are ranked
    /// by - see the picker, and the scenery-at-the-elbow it exists to stop.
    float    objectAim = 1e30f;

    /// The object best aimed at that the client lets the pointer rest on
    /// (gameObjectTakesMouseover), usable or not: an anvil or a forge is named
    /// with no cursor of its own. Plain scenery - a signpost, a banner, a
    /// shop's board - is not tracked by the client and shows no tooltip.
    uint64_t mouseoverObjectGuid = 0;
    float    mouseoverObjectAim = 1e30f;
    /// The nearest corpse the ray meets: never what a click acts on, but
    /// the pointer rests on it and the client names whose it is (0x0051f790
    /// case 0x81 -> 0x00622410).
    uint64_t mouseoverCorpseGuid = 0;
    float    mouseoverCorpseCenterT = 1e30f;

    /// The living win; a corpse is still selectable, but only when nothing alive
    /// was under the cursor - which is what makes a player standing on a body
    /// clickable, and what looting and skinning still need.
    [[nodiscard]] uint64_t unitGuid() const { return livingUnitGuid != 0 ? livingUnitGuid : deadUnitGuid; }
    [[nodiscard]] float unitCenterT() const {
        return livingUnitGuid != 0 ? livingUnitCenterT : deadUnitCenterT;
    }

    /// Resolve to the one thing under the cursor: a unit unless an object's
    /// centre is clearly in front of it, so a creature is never lost to a
    /// decorative or backing object behind it.
    ///
    /// This and not closestGuid is what a caller wants. closestGuid is the
    /// nearest *entry point* of anything pickable, and a game object's sphere
    /// is 2.5 yards against a unit's 1.8 - so a wide object beside an NPC is
    /// entered first even when the NPC is nearer, and answers closestGuid while
    /// a click still lands on the NPC. kMaxGameObjectPickRadius exists for that
    /// same reason. Every affordance drawn for what a click would do has to ask
    /// this one.
    ///
    /// Inline so the rule can be tested without linking the picker's traversal,
    /// which needs the whole client behind it.
    [[nodiscard]] uint64_t resolve() const {
        // A unit wins over a game object unless the object's centre is clearly
        // in front of the unit's.
        constexpr float kUnitOverGoBias = 2.0f;
        const uint64_t unit = unitGuid();
        if (unit != 0 && (objectGuid == 0 || unitCenterT() <= objectCenterT + kUnitOverGoBias)) {
            return unit;
        }
        if (objectGuid != 0) return objectGuid;
        return 0;
    }

    /// The one object the pointer rests on, the client's mouseover: what a
    /// click would act on, or else the tracked object a click would not use
    /// (an anvil, a forge).
    [[nodiscard]] uint64_t mouseover() const {
        const uint64_t guid = resolve();
        if (guid != 0) return guid;
        return mouseoverObjectGuid != 0 ? mouseoverObjectGuid : mouseoverCorpseGuid;
    }
};

/// Called for every entity the ray hits, so a caller can gather what only it
/// cares about (a hooked fishing bobber, quest objective objects) without
/// repeating the traversal.
using ScenePickHit = std::function<void(uint64_t guid,
                                        const std::shared_ptr<game::Entity>& entity,
                                        float hitT, float centerT)>;

ScenePick pickScene(game::GameHandler& gameHandler,
                    const rendering::Ray& ray,
                    const ScenePickParams& params,
                    const ScenePickHit& onHit = {});

} // namespace ui
} // namespace wowee
