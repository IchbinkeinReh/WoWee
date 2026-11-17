// Riding the ray traced water's waves ("Ride the waves"): swimmers, floating
// ships and the camera following the surface the GPU draws, which the map's
// flat liquid level knows nothing of.
//
// Drawn only. Everything here goes to the renderers' own ride - the
// character renderer's per-instance wave ride, the WMO and M2 renderers'
// visual delta - and to the camera's position after the camera controller
// has placed it. The movement, the swim state, the collision, the position
// sent to the server and every height the game asks for keep the flat level:
// nothing here writes characterPosition, an entity, an instance's position
// or a transport's transform.
#include "rendering/renderer.hpp"

#include "core/application.hpp"
#include "game/game_handler.hpp"
#include "game/transport_manager.hpp"
#include "game/transport_path_repository.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/swim_effects.hpp"
#include "rendering/water_renderer.hpp"
#include "rendering/wave_surface.hpp"
#include "rendering/wmo_renderer.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace wowee {
namespace rendering {

namespace {

namespace wsf = wave_surface;

/// A ship rides the waves only while it floats: its origin within this of the
/// flat water under it, yards. A ship's origin is at its waterline; a lift,
/// a tram or a ship in dock above the water is left alone.
constexpr float kFloatTolerance = 1.0f;
/// How far from the character a ship rides, yards: as far as the units
/// aboard are kept in step (Application's unit sync, 320), less a margin.
constexpr float kBoatRideRange = 300.0f;
/// The hull the waves are sampled along: half a ship's length and beam. The
/// ships of the sea routes are some 40 yards long and 12 across; waves much
/// shorter than that cancel along it.
constexpr float kHullHalfLength = 18.0f;
constexpr float kHullHalfWidth = 6.0f;

bool isIdentity(const glm::mat4& m) { return m == glm::mat4(1.0f); }

}  // namespace

void Renderer::rideWavesUnit(uint64_t guid, uint32_t instanceId, uint32_t mountInstanceId, const glm::vec3& feet,
                             bool swimming, uint64_t transportGuid) {
    if (!rideWaves_ || instanceId == 0 || !waterRenderer || !waterRenderer->rtWaterDrawn()) return;
    auto it = unitWaveRides_.find(guid);
    // Only what might ride: a swimmer, a passenger, or one still easing back.
    if (it == unitWaveRides_.end()) {
        if (!swimming && transportGuid == 0) return;
        it = unitWaveRides_.emplace(guid, UnitWaveRide{}).first;
    }
    UnitWaveRide& u = it->second;
    if ((u.instanceId != 0 && u.instanceId != instanceId) ||
        (u.mountInstanceId != 0 && u.mountInstanceId != mountInstanceId)) {
        // Its model changed: the old one is drawn where it stands again.
        if (characterRenderer) {
            characterRenderer->clearInstanceWaveRide(u.instanceId);
            if (u.mountInstanceId) characterRenderer->clearInstanceWaveRide(u.mountInstanceId);
        }
        u.pose = {};
    }
    u.instanceId = instanceId;
    u.mountInstanceId = mountInstanceId;
    u.feet = feet;
    u.swimming = swimming;
    u.transportGuid = transportGuid;
    u.seen = true;
}

void Renderer::setUnitRide(uint32_t instanceId, uint32_t mountInstanceId, bool riding, float heave,
                           const glm::vec2& slopes, const glm::mat4& world) {
    if (!characterRenderer || instanceId == 0) return;
    if (!riding) {
        characterRenderer->clearInstanceWaveRide(instanceId);
        if (mountInstanceId) characterRenderer->clearInstanceWaveRide(mountInstanceId);
        return;
    }
    // A rider and its mount are raised alike and not leant: each would lean
    // about its own place, and the rider would come off its saddle.
    const glm::quat tilt = mountInstanceId ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f) : wsf::tiltOf(slopes);
    characterRenderer->setInstanceWaveRide(instanceId, heave, tilt, world);
    if (mountInstanceId) characterRenderer->setInstanceWaveRide(mountInstanceId, heave, tilt, world);
}

void Renderer::clearWaveRide() {
    for (auto& [guid, u] : unitWaveRides_) setUnitRide(u.instanceId, u.mountInstanceId, false, 0.0f, {}, {});
    unitWaveRides_.clear();
    for (auto& [guid, b] : boatWaveRides_) {
        if (b.instanceId == 0) continue;
        if (b.isM2) {
            if (m2Renderer) m2Renderer->clearInstanceVisualDelta(b.instanceId);
        } else if (wmoRenderer) {
            wmoRenderer->setInstanceVisualDelta(b.instanceId, glm::mat4(1.0f));
        }
    }
    boatWaveRides_.clear();
    setUnitRide(playerRideInstance_, playerRideMount_, false, 0.0f, {}, {});
    playerRideInstance_ = 0;
    playerRideMount_ = 0;
    playerWaveRide_ = {};
    cameraRideOffset_ = glm::vec3(0.0f);
}

void Renderer::updateWaveRide(float deltaTime) {
    // Only on the traced water, which alone draws these waves; off, or
    // without it, everything is drawn where the game puts it, at once.
    const bool on = rideWaves_ && waterRenderer && waterRenderer->rtWaterDrawn();
    if (!on) {
        if (!unitWaveRides_.empty() || !boatWaveRides_.empty() || playerRideInstance_ || playerRideMount_ ||
            cameraRideOffset_ != glm::vec3(0.0f)) {
            clearWaveRide();
        }
        return;
    }
    const float dt = std::max(deltaTime, 0.0f);
    // The shader's clock: the PerFrame block's fogParams.z is globalTime,
    // advanced at the top of update() before this runs.
    const float time = globalTime;
    const glm::vec3 me = characterPosition;
    auto surfaceAt = [&](const glm::vec2& at) { return waterRenderer->waveSurfaceAt(at.x, at.y, time, true); };
    auto* gh = core::Application::getInstance().getGameHandler();

    // --- Ships ---
    // Only a ship that sails (never a zeppelin or another flier), on this
    // map, near enough, and floating: its origin at the flat water under it.
    for (auto& [guid, b] : boatWaveRides_) b.seen = false;
    if (auto* tm = gh ? gh->getTransportManager() : nullptr) {
        for (const auto& [guid, t] : tm->getTransports()) {
            BoatWaveRide& b = boatWaveRides_[guid];
            if (b.instanceId != t.wmoInstanceId || b.isM2 != t.isM2) {
                if (b.instanceId != 0) {
                    if (b.isM2) {
                        if (m2Renderer) m2Renderer->clearInstanceVisualDelta(b.instanceId);
                    } else if (wmoRenderer) {
                        wmoRenderer->setInstanceVisualDelta(b.instanceId, glm::mat4(1.0f));
                    }
                }
                b = BoatWaveRide{};
                b.instanceId = t.wmoInstanceId;
                b.isM2 = t.isM2;
            }
            b.seen = true;
            if (b.instanceId == 0) continue;
            const glm::vec3 at(t.transform[3]);
            float heave = 0.0f;
            glm::vec2 slopes(0.0f);
            const glm::vec3 fromMe = at - me;
            if (t.onThisMap && !game::isAirTransportDisplay(t.displayId) &&
                glm::dot(fromMe, fromMe) <= kBoatRideRange * kBoatRideRange) {
                const auto flat = waterRenderer->getWaterHeightAt(at.x, at.y);
                if (flat && std::abs(at.z - *flat) <= kFloatTolerance) {
                    const auto body = wsf::bodyOnWaves(
                        glm::vec2(at), glm::vec2(t.transform[0]), kHullHalfLength, kHullHalfWidth,
                        [&](const glm::vec2& p) {
                            const auto s = surfaceAt(p);
                            return s ? s->surface.height : 0.0f;
                        });
                    heave = body.heave;
                    slopes = wsf::slopesOf(glm::normalize(glm::vec3(-body.slopes, 1.0f)));
                }
            }
            b.pose.step(heave, slopes, dt);
            b.delta = b.pose.atRest() && heave == 0.0f
                          ? glm::mat4(1.0f)
                          : wsf::rideMatrix(at, b.pose.heave.value, wsf::tiltOf(b.pose.slopes()));
            b.riding = !isIdentity(b.delta);
            if (b.isM2) {
                if (m2Renderer) m2Renderer->setInstanceVisualDelta(b.instanceId, b.delta);
            } else if (wmoRenderer) {
                wmoRenderer->setInstanceVisualDelta(b.instanceId, b.delta);
            }
        }
    }
    for (auto it = boatWaveRides_.begin(); it != boatWaveRides_.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        if (it->second.instanceId != 0) {
            if (it->second.isM2) {
                if (m2Renderer) m2Renderer->clearInstanceVisualDelta(it->second.instanceId);
            } else if (wmoRenderer) {
                wmoRenderer->setInstanceVisualDelta(it->second.instanceId, glm::mat4(1.0f));
            }
        }
        it = boatWaveRides_.erase(it);
    }
    auto boatRide = [&](uint64_t transportGuid) -> const glm::mat4* {
        if (transportGuid == 0) return nullptr;
        const auto it = boatWaveRides_.find(transportGuid);
        return it != boatWaveRides_.end() && it->second.riding ? &it->second.delta : nullptr;
    };

    // --- The character ---
    // Aboard a riding ship, with the ship. Swimming near the surface, on the
    // waves where it swims, as deep as it is (a diver does not ride them).
    glm::vec3 cameraRide(0.0f);
    {
        const uint32_t instance = characterInstanceId;
        const uint32_t mount = animationController_ ? animationController_->getMountInstanceId() : 0u;
        if ((playerRideInstance_ && playerRideInstance_ != instance) ||
            (playerRideMount_ && playerRideMount_ != mount)) {
            setUnitRide(playerRideInstance_, playerRideMount_, false, 0.0f, {}, {});
            playerWaveRide_ = {};
        }
        playerRideInstance_ = instance;
        playerRideMount_ = mount;
        const bool taxi = animationController_ && animationController_->isTaxiFlight();
        if (const glm::mat4* ship = boatRide(gh ? gh->getPlayerTransportGuid() : 0); ship && !taxi) {
            playerWaveRide_ = {};
            setUnitRide(instance, mount, true, 0.0f, {}, *ship);
            cameraRide = glm::vec3(*ship * glm::vec4(me, 1.0f)) - me;
        } else {
            float heave = 0.0f;
            glm::vec2 slopes(0.0f);
            if (cameraController && cameraController->isSwimming() && !taxi && !heldCharacter_) {
                if (const auto s = surfaceAt(glm::vec2(me))) {
                    const float w = wsf::swimmerRideWeight(s->flatLevel - me.z);
                    heave = w * s->surface.height;
                    slopes = w * wsf::slopesOf(s->surface.normal);
                }
            }
            playerWaveRide_.step(heave, slopes, dt);
            const bool riding = !playerWaveRide_.atRest() || heave != 0.0f;
            setUnitRide(instance, mount, riding, playerWaveRide_.heave.value, playerWaveRide_.slopes(),
                        glm::mat4(1.0f));
            if (riding) cameraRide = glm::vec3(0.0f, 0.0f, playerWaveRide_.heave.value);
        }
    }

    // --- The other units ---
    // Swimmers within the ripples' reach (SwimEffects::kUnitRippleRange),
    // and whoever is aboard a riding ship.
    for (auto it = unitWaveRides_.begin(); it != unitWaveRides_.end();) {
        UnitWaveRide& u = it->second;
        const glm::mat4* ship = u.seen ? boatRide(u.transportGuid) : nullptr;
        bool riding = false;
        if (ship) {
            u.pose = {};
            setUnitRide(u.instanceId, u.mountInstanceId, true, 0.0f, {}, *ship);
            riding = true;
        } else {
            float heave = 0.0f;
            glm::vec2 slopes(0.0f);
            const glm::vec3 away = u.feet - me;
            constexpr float kRange = SwimEffects::kUnitRippleRange;
            if (u.seen && u.swimming && u.transportGuid == 0 && glm::dot(away, away) <= kRange * kRange) {
                if (const auto s = surfaceAt(glm::vec2(u.feet))) {
                    const float w = wsf::swimmerRideWeight(s->flatLevel - u.feet.z);
                    heave = w * s->surface.height;
                    slopes = w * wsf::slopesOf(s->surface.normal);
                }
            }
            u.pose.step(heave, slopes, dt);
            riding = !u.pose.atRest() || heave != 0.0f;
            setUnitRide(u.instanceId, u.mountInstanceId, riding, u.pose.heave.value, u.pose.slopes(),
                        glm::mat4(1.0f));
        }
        // Settled and gone from the sync, or out of the water: forgotten.
        if (!riding && (!u.seen || (!u.swimming && u.transportGuid == 0))) {
            it = unitWaveRides_.erase(it);
            continue;
        }
        u.seen = false;
        ++it;
    }

    // --- The camera ---
    // Following the character, it rides with it: the view stays with the
    // swimmer instead of the swimmer bobbing past it. It is kept on the side
    // of the drawn surface that the underwater checks put it on - they still
    // decide on the flat level, from where the camera would be without the
    // ride (getCameraRideOffset) - so a crest under the camera neither
    // swallows it nor turns the underwater view on and off.
    cameraRideOffset_ = glm::vec3(0.0f);
    if (camera && cameraController && !cinematicView_ && cameraController->isThirdPerson() &&
        cameraController->getFollowTarget() && cameraRide != glm::vec3(0.0f)) {
        const glm::vec3 eye = camera->getPosition();
        glm::vec3 ridden = eye + cameraRide;
        if (const auto s = surfaceAt(glm::vec2(eye))) {
            const float band =
                std::max(0.05f, camera->getNearPlane() * std::tan(glm::radians(camera->getFovDegrees()) * 0.5f));
            const float drawn = s->flatLevel + s->surface.height;
            if (eye.z >= s->flatLevel) {
                ridden.z = std::max(ridden.z, drawn + band);
            } else {
                ridden.z = std::min(ridden.z, drawn - band);
            }
        }
        cameraRideOffset_ = ridden - eye;
        camera->setPosition(ridden);
    }
}

}  // namespace rendering
}  // namespace wowee
