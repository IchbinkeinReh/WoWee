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

/// The checks every usable object type shares in the 3.3.5a client
/// (FUN_007112a0): not in use (0x1), not NOT_SELECTABLE (0x10), not marked
/// no-interact (dynamic 0x4), and a conditional one (flag 0x4) only while it
/// is lit (dynamic 0x1).
inline bool gameObjectFlagsAllowUse(uint32_t flags, uint32_t dynamicLow) {
    constexpr uint32_t kFlagInUse = 0x1, kFlagInteractCond = 0x4, kFlagNotSelectable = 0x10;
    constexpr uint32_t kDynActivate = 0x1, kDynNoInteract = 0x4;
    if (flags & (kFlagInUse | kFlagNotSelectable)) return false;
    if (dynamicLow & kDynNoInteract) return false;
    if ((flags & kFlagInteractCond) && !(dynamicLow & kDynActivate)) return false;
    return true;
}

/// Whether the 3.3.5a client lets a game object be used by a click - gives it a
/// cursor and answers the click - from its type, GAMEOBJECT_FLAGS and the low
/// half of GAMEOBJECT_DYNAMIC. Read from the client's per-type object classes:
/// the ones whose IsInteractable is a plain "no" (generic, spell focus,
/// transport, map object, duel arbiter, fishing hole, capture point, aura
/// generator, dungeon difficulty, destructible building, trap door, and the
/// three types with no class at all), and the checks the others share.
inline bool gameObjectTakesClick(uint32_t goType, uint32_t flags, uint32_t dynamicLow) {
    switch (goType) {
        case 5: case 8: case 11: case 14: case 15: case 16: case 20: case 21:
        case 25: case 28: case 29: case 30: case 31: case 33: case 35:
            return false;
        default:
            break;
    }
    return gameObjectFlagsAllowUse(flags, dynamicLow);
}

/// The quest a game object of this type is only there for, from its template
/// data, or 0. The client's per-type property table (0x00a36f34, property
/// 0x14) puts it in a chest's data8, a generic object's data5, a spell focus's
/// data4 and a goober's data1.
inline uint32_t gameObjectRequiredQuest(uint32_t goType, const uint32_t* data) {
    if (!data) return 0;
    switch (goType) {
        case 3:  return data[8];
        case 5:  return data[5];
        case 8:  return data[4];
        case 10: return data[1];
        default: return 0;
    }
}

/// Whether the 3.3.5a client lets the pointer rest on a game object at all:
/// makes it the mouseover, which is what shows its tooltip (FUN_00626720) and
/// what a click then acts on. CGWorldFrame::OnLayerTrackObject (0x004f8190)
/// drops any object whose virtual at 0xa4 says no, and for a game object that
/// is 0x0070f580, which asks the object's type class (slot 0xc):
///  - transport, map object, MO transport, dungeon difficulty, trap door, and
///    the types with no class (auction house, guard post, lottery kiosk): never;
///  - spell focus, duel arbiter, fishing hole, aura generator: always - an
///    anvil names itself with no cursor of its own;
///  - generic and capture point: only when the template's highlight is set
///    (0x0070ff80; data1 and data19, property 0x12) - a signpost or a banner
///    has none, and says nothing;
///  - a fishing node: only the player's own (0x00712030), and only while usable;
///  - destructible building: a vehicle test this client does not model, so no;
///  - the rest: whenever they could be used (0x007112a0).
/// The caller still owes the quest test that follows (see
/// gameObjectRequiredQuest): an object with a quest is tracked only while that
/// quest is in the player's log.
inline bool gameObjectTakesMouseover(uint32_t goType, const uint32_t* data,
                                     uint32_t flags, uint32_t dynamicLow,
                                     bool createdByPlayer) {
    switch (goType) {
        case 11: case 14: case 15: case 20: case 21: case 28: case 31: case 33: case 35:
            return false;
        case 8: case 16: case 25: case 30:
            return true;
        case 5:  return data && data[1] != 0;
        case 29: return data && data[19] != 0;
        case 17: return createdByPlayer && gameObjectFlagsAllowUse(flags, dynamicLow);
        default: return gameObjectFlagsAllowUse(flags, dynamicLow);
    }
}

/// The parts of picking the two callers disagree about.
struct ScenePickParams {
    /// Fallback hit sphere for a unit with no render bounds. The right-click
    /// picker uses a slightly taller, tighter sphere than the target picker.
    float unitHitRadius = 1.8f;
    float unitHeightOffset = 1.0f;
    /// Skip chair-type game objects. Their fallback sphere is wide enough to be
    /// caught while right-drag-rotating the camera near one, which sits the
    /// player down. Left-clicking a chair to target it still works.
    bool skipChairs = false;
};

/// Everything the traversal found, before either caller applies its own rules.
struct ScenePick {
    uint64_t closestGuid = 0;          ///< Nearest hit of any pickable type.
    float    closestT = 1e30f;

    uint64_t hostileUnitGuid = 0;      ///< Nearest *living* hostile unit.
    float    hostileUnitT = 1e30f;

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

    /// The living win; a corpse is still selectable, but only when nothing alive
    /// was under the cursor - which is what makes a player standing on a body
    /// clickable, and what looting and skinning still need.
    [[nodiscard]] uint64_t unitGuid() const { return livingUnitGuid != 0 ? livingUnitGuid : deadUnitGuid; }
    [[nodiscard]] float unitCenterT() const {
        return livingUnitGuid != 0 ? livingUnitCenterT : deadUnitCenterT;
    }

    /// Resolve to the one thing under the cursor: a unit unless an object's
    /// centre is clearly in front of it, so a creature is never lost to a
    /// decorative or backing object behind it. A hostile keeps its priority.
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
            return hostileUnitGuid != 0 ? hostileUnitGuid : unit;
        }
        if (objectGuid != 0) return objectGuid;
        return 0;
    }

    /// The one object the pointer rests on, the client's mouseover: what a
    /// click would act on, or else the tracked object a click would not use
    /// (an anvil, a forge).
    [[nodiscard]] uint64_t mouseover() const {
        const uint64_t guid = resolve();
        return guid != 0 ? guid : mouseoverObjectGuid;
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
