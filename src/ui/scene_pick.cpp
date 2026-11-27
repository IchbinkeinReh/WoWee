#include "ui/scene_pick.hpp"

#include "core/application.hpp"
#include "core/coordinates.hpp"
#include "game/game_handler.hpp"
#include "game/entity.hpp"
#include "game/world_pick.hpp"
#include "rendering/camera.hpp"
#include "rendering/ray_mesh.hpp"
#include "rendering/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace wowee {
namespace ui {

namespace {

bool isDeadUnit(const std::shared_ptr<game::Entity>& entity) {
    auto unit = std::dynamic_pointer_cast<game::Unit>(entity);
    return unit && unit->getHealth() == 0 && unit->getMaxHealth() > 0;
}

} // namespace

// The client's scene ray pick, 0x004f9930 (from 0x004f9da0): the world
// first, then the nearest object model the flags let it take in front of it.
ScenePick pickScene(game::GameHandler& gameHandler,
                    const rendering::Ray& ray,
                    const ScenePickParams& params,
                    const ScenePickHit& onHit) {
    ScenePick pick;
    // 0x004f9da0 -> 0x004f7650: what the frame may pick - every object kind
    // with no spell held, only what a held spell wants with one (a spell
    // waiting for a place picks no object at all). The hover and the click
    // ask the same.
    const uint32_t pickFlags = gameHandler.worldPickFlags();
    if ((pickFlags & game::world_pick::kObjectMask) == 0) return pick;

    // Where the ray first meets what is seen of the world - the ground, a
    // building's or a doodad's opaque faces, the liquid with the liquid flag;
    // only what the ray reaches before that can be picked. What only stands
    // in the way hides nothing: the auctioneers of Orgrimmar stand behind a
    // collision mesh nobody sees.
    // Half a yard of slack keeps a unit standing on the floor pickable where
    // its sphere dips into it.
    //
    // Traced only as far as an object the ray reaches, and only once one is:
    // the ground is marched half a yard at a time, and tracing the full
    // reach every frame for the hover cost 2.5 ms when nothing was there.
    constexpr float kOcclusionSlack = 0.5f;
    auto* occlusionRenderer = params.occludeByWorld
        ? core::Application::getInstance().getRenderer() : nullptr;
    float worldT = 1e30f;        // the first world hit, once found
    float worldTracedTo = 0.0f;  // how far the trace has looked without one
    const auto hiddenByWorld = [&](float t) {
        if (!occlusionRenderer) return false;
        if (worldT < 1e29f) return t > worldT + kOcclusionSlack;
        if (t <= worldTracedTo + kOcclusionSlack) return false;
        const bool liquid = (pickFlags & game::world_pick::kLiquid) != 0;
        if (const auto hit = occlusionRenderer->pointerBlockedAt(ray.origin, ray.direction, t, liquid)) {
            worldT = *hit;
        }
        worldTracedTo = t;
        return worldT < 1e29f && t > worldT + kOcclusionSlack;
    };

    // The units whose spheres the ray meets, living and dead apart, for the
    // triangle test below when there is more than one.
    std::vector<UnitCandidate> livingHits;
    std::vector<UnitCandidate> deadHits;

    for (const auto& [guid, entity] : gameHandler.getEntityManager().getEntities()) {
        if (!entity) continue;
        const auto type = entity->getType();
        // 0x004f9550 runs every candidate through 0x004f7530: the kinds the
        // flags want, never the player itself nor the unit it possesses
        // unless a held spell may be cast on the caster (0x004f7350,
        // 0x004f7250), and with a spell held only what the spell takes.
        if (!gameHandler.worldPickTakes(guid, pickFlags)) continue;
        if (type == game::ObjectType::CORPSE) {
            // For the pointer only, behind anything a click would take.
            glm::vec3 c;
            float r = 0.0f;
            if (!core::Application::getInstance().getRenderBoundsForGuid(guid, c, r)) continue;
            float t = 0.0f;
            if (!raySphereIntersect(ray, c, std::max(r, 0.5f), t) || hiddenByWorld(t)) continue;
            const float centerT = glm::dot(c - ray.origin, ray.direction);
            if (centerT < pick.mouseoverCorpseCenterT) {
                pick.mouseoverCorpseCenterT = centerT;
                pick.mouseoverCorpseGuid = guid;
            }
            continue;
        }
        if (type != game::ObjectType::UNIT &&
            type != game::ObjectType::PLAYER &&
            type != game::ObjectType::GAMEOBJECT) continue;

        // Nor the scenery. A trigger is an ordinary unit on the wire - it has
        // health, a name and a model - and UNIT_FLAG_NOT_SELECTABLE is the
        // server saying it is not there to be clicked.
        if (type == game::ObjectType::UNIT) {
            auto unit = std::static_pointer_cast<game::Unit>(entity);
            if (unit->getUnitFlags() & game::UNIT_FLAG_NOT_SELECTABLE) continue;
        }

        if (type == game::ObjectType::GAMEOBJECT) {
            // The client finds an object only on the triangles its model
            // draws (0x004f9550, 0x0081daf0), so a model that draws none is
            // never under the pointer, whatever its type would allow.
            if (core::Application::getInstance().gameObjectDrawsNothing(guid)) continue;
        }

        glm::vec3 hitCenter;
        float hitRadius = 0.0f;
        const bool hasBounds =
            core::Application::getInstance().getRenderBoundsForGuid(guid, hitCenter, hitRadius);
        if (!hasBounds) {
            // Match the hover-cursor sizes, so the reticle agrees with the
            // cursor affordance - otherwise NPCs feel hard to click.
            float heightOffset = params.unitHeightOffset;
            hitRadius = params.unitHitRadius;
            if (type == game::ObjectType::UNIT) {
                auto unit = std::static_pointer_cast<game::Unit>(entity);
                // Critters have very low max health.
                if (unit->getMaxHealth() > 0 && unit->getMaxHealth() < 100) {
                    hitRadius = 0.5f;
                    heightOffset = 0.3f;
                }
            } else if (type == game::ObjectType::GAMEOBJECT) {
                hitRadius = 2.5f;
                heightOffset = 1.2f;
            }
            hitCenter = core::coords::canonicalToRender(
                glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
            hitCenter.z += heightOffset;
        } else {
            hitRadius = std::max(hitRadius * 1.25f, 1.0f);
        }

        float hitT = 0.0f;
        if (!raySphereIntersect(ray, hitCenter, hitRadius, hitT)) continue;
        // An object is found on the triangles its model draws, and nowhere round it
        // (0x004f9550 -> 0x0081df10 -> 0x0081daf0).
        if (type == game::ObjectType::GAMEOBJECT) {
            const float reach = glm::dot(hitCenter - ray.origin, ray.direction) + hitRadius;
            const auto hit = core::Application::getInstance().rayHitGameObject(guid, ray.origin, ray.direction, reach);
            if (hit.known && !hit.at) continue;
            if (hit.at) hitT = *hit.at;
        }
        if (hiddenByWorld(hitT)) continue;  // behind a wall or under the floor
        const float centerT = glm::dot(hitCenter - ray.origin, ray.direction);

        if (type == game::ObjectType::UNIT || type == game::ObjectType::PLAYER) {
            // Rank the living ahead of the dead rather than by distance alone. A
            // corpse lies at ground level while a unit's sphere sits a metre up,
            // so looking down at someone standing on a body put the body nearer.
            if (isDeadUnit(entity)) {
                if (centerT < pick.deadUnitCenterT) {
                    pick.deadUnitCenterT = centerT;
                    pick.deadUnitGuid = guid;
                }
                deadHits.push_back({guid, centerT, std::max(centerT - hitRadius, 0.0f)});
            } else {
                // Whatever its reaction: 0x004f9550 ranks the models it meets by
                // what they are (0x004f6270 - living, lootable corpse, object),
                // never by whether the player may attack them.
                if (centerT < pick.livingUnitCenterT) {
                    pick.livingUnitCenterT = centerT;
                    pick.livingUnitGuid = guid;
                }
                // No nearer than its centre less its radius along the ray,
                // even from inside it.
                livingHits.push_back({guid, centerT, std::max(centerT - hitRadius, 0.0f)});
            }
        } else if (type == game::ObjectType::GAMEOBJECT) {
            // What the real client lets the pointer rest on (0x0070f580), and
            // of that what a click uses (the type handler's slot +0x18) - the
            // game's own answer, which the use asks again.
            const auto pointer = gameHandler.gameObjectPointer(guid);
            const bool tracked = pointer.tracked;
            const bool interactive = pointer.interactive;
            if (interactive && hitT < pick.objectT) {
                pick.objectT = hitT;
                pick.objectGuid = guid;
            }
            if (tracked && hitT < pick.mouseoverObjectT) {
                pick.mouseoverObjectT = hitT;
                pick.mouseoverObjectGuid = guid;
            }
        }

        if (hitT < pick.closestT) {
            pick.closestT = hitT;
            pick.closestGuid = guid;
        }

        if (onHit) onHit(guid, entity, hitT, centerT);
    }

    // Units standing in one another - two NPCs on one spot, a crowd at a
    // trainer - all answer the sphere test, and the one whose centre is
    // nearest won wherever the pointer was on them. The client takes the
    // model whose triangles the ray meets first (0x004f9550, 0x0081df10's
    // first pass): the body in front at the pointer, so pointing at what
    // shows of the one behind picks it. The spheres decide only where the ray
    // meets no unit's triangles, as their forgiving margin is for.
    // Nearest sphere first: one whose sphere the ray enters only past a body
    // already met cannot be in front of it, and is not skinned at all.
    // And against an object: whichever's triangles are in front.
    auto& app = core::Application::getInstance();
    const auto trianglesAt = [&app, &ray](uint64_t guid, float reach) {
        return app.rayHitUnit(guid, ray.origin, ray.direction, reach);
    };
    if (livingHits.size() > 1 || (!livingHits.empty() && pick.objectGuid != 0)) {
        if (const auto best = frontUnitByTriangles(livingHits, trianglesAt)) {
            pick.livingUnitGuid = livingHits[best->index].guid;
            pick.livingUnitCenterT = livingHits[best->index].centerT;
            pick.unitT = best->t;
        }
    } else if (livingHits.empty() && (deadHits.size() > 1 || (!deadHits.empty() && pick.objectGuid != 0))) {
        // Bodies lying on one another: the one whose triangles are in front
        // at the pointer, so pointing at what shows of the lower one picks
        // it (0x0081df10 tests the dead as the living).
        if (const auto best = frontUnitByTriangles(deadHits, trianglesAt)) {
            pick.deadUnitGuid = deadHits[best->index].guid;
            pick.deadUnitCenterT = deadHits[best->index].centerT;
            pick.unitT = best->t;
        }
    }

    return pick;
}

} // namespace ui
} // namespace wowee
