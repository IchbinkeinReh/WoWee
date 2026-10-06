// Entity, Unit, Player, GameObject, EntityManager tests
#include <catch_amalgamated.hpp>
#include "game/entity.hpp"
#include <algorithm>
#include <memory>

using namespace wowee::game;

TEST_CASE("Entity default construction", "[entity]") {
    Entity e;
    REQUIRE(e.getGuid() == 0);
    REQUIRE(e.getType() == ObjectType::OBJECT);
    REQUIRE(e.getX() == 0.0f);
    REQUIRE(e.getY() == 0.0f);
    REQUIRE(e.getZ() == 0.0f);
    REQUIRE(e.getOrientation() == 0.0f);
}

TEST_CASE("Entity GUID constructor", "[entity]") {
    Entity e(0xDEADBEEF);
    REQUIRE(e.getGuid() == 0xDEADBEEF);
}

TEST_CASE("Entity position set/get", "[entity]") {
    Entity e;
    e.setPosition(1.0f, 2.0f, 3.0f, 1.57f);
    REQUIRE(e.getX() == Catch::Approx(1.0f));
    REQUIRE(e.getY() == Catch::Approx(2.0f));
    REQUIRE(e.getZ() == Catch::Approx(3.0f));
    REQUIRE(e.getOrientation() == Catch::Approx(1.57f));
}

TEST_CASE("Entity ignores positive-duration movement without displacement",
          "[entity][movement]") {
    Entity e;
    e.setPosition(10.0f, 20.0f, 30.0f, 0.0f);

    e.startMoveTo(10.0f, 20.0f, 30.0f, 1.25f, 0.5f);

    REQUIRE_FALSE(e.isEntityMoving());
    REQUIRE_FALSE(e.isActivelyMoving());
    REQUIRE(e.getOrientation() == Catch::Approx(1.25f));
}

TEST_CASE("Entity still interpolates meaningful movement", "[entity][movement]") {
    Entity e;
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);

    e.startMoveTo(5.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    REQUIRE(e.isActivelyMoving());

    e.updateMovement(0.5f);
    REQUIRE(e.getX() == Catch::Approx(2.5f));
    REQUIRE(e.isActivelyMoving());
}

TEST_CASE("Entity reports the speed of the movement under way", "[entity][movement]") {
    Entity e;
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    REQUIRE(e.getMoveSpeed() == 0.0f);

    e.startMoveTo(3.0f, 4.0f, 0.0f, 0.0f, 2.0f);
    REQUIRE(e.getMoveSpeed() == Catch::Approx(2.5f));
    e.setMoveSpeed(7.0f);
    REQUIRE(e.getMoveSpeed() == Catch::Approx(7.0f));

    // Arrived: nothing under way.
    e.updateMovement(2.5f);
    REQUIRE(e.getMoveSpeed() == 0.0f);
}

TEST_CASE("A spline starts where the entity is, as the client's does",
          "[entity][movement]") {
    // FUN_0073c8e0 puts the unit's current position first; a unit redirected
    // mid-move must not jump to the packet's start.
    Entity e;
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    const std::vector<std::array<float, 3>> path = {
        {{2.0f, 0.0f, 0.0f}}, {{4.0f, 0.0f, 0.0f}}, {{6.0f, 0.0f, 0.0f}}};
    e.startMoveAlongPath(path, 0.0f, 3.0f);
    e.updateMovement(0.001f);
    REQUIRE(e.getX() < 0.1f);
    // Six yards in three seconds, from where it stood.
    REQUIRE(e.getMoveSpeed() == Catch::Approx(2.0f));
    e.updateMovement(2.998f);
    REQUIRE(e.getX() == Catch::Approx(6.0f).margin(0.01f));
}

namespace {
constexpr uint32_t kForward = 0x1, kStrafeLeft = 0x4, kTurnLeft = 0x10, kTurnRight = 0x20;
constexpr uint32_t kWalking = 0x100, kSwimming = 0x200000, kFlying = 0x2000000;
constexpr uint32_t kBackward = 0x2, kAscending = 0x400000;
constexpr float kPi = 3.14159265f;
} // namespace

TEST_CASE("A unit's speed follows its flags as GetCurrentSpeed picks it",
          "[entity][movement]") {
    MovementSpeeds speeds;
    REQUIRE(Entity::speedForFlags(speeds, 0) == 0.0f);
    REQUIRE(Entity::speedForFlags(speeds, kTurnLeft) == 0.0f);
    REQUIRE(Entity::speedForFlags(speeds, kForward) == Catch::Approx(7.0f));
    REQUIRE(Entity::speedForFlags(speeds, kBackward) == Catch::Approx(4.5f));
    REQUIRE(Entity::speedForFlags(speeds, kForward | kWalking) == Catch::Approx(2.5f));
    REQUIRE(Entity::speedForFlags(speeds, kBackward | kWalking) == Catch::Approx(2.5f));
    REQUIRE(Entity::speedForFlags(speeds, kForward | kSwimming) == Catch::Approx(4.722222f));
    REQUIRE(Entity::speedForFlags(speeds, kBackward | kSwimming) == Catch::Approx(2.5f));
    REQUIRE(Entity::speedForFlags(speeds, kAscending | kFlying) == Catch::Approx(7.0f));
    // A slowed run below the walk speed is walked at the run speed.
    speeds.run = 2.0f;
    REQUIRE(Entity::speedForFlags(speeds, kForward | kWalking) == Catch::Approx(2.0f));
}

TEST_CASE("A player starts moving on its start packet", "[entity][movement]") {
    // A start packet has no displacement: the unit moves from it at its
    // speed, not from the next heartbeat.
    Entity e;
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    e.movementSpeeds().run = 8.0f;
    REQUIRE(e.getMovementSpeeds().run == Catch::Approx(8.0f));
    e.startMoveByFlags(0.0f, 0.0f, 0.0f, 0.0f, kForward, 0, 0.12f);
    REQUIRE(e.isActivelyMoving());
    REQUIRE(e.getMoveSpeed() == Catch::Approx(8.0f));
    e.updateMovement(0.25f);
    // Canonical facing 0 is +x.
    REQUIRE(e.getX() == Catch::Approx(2.0f));
    REQUIRE(e.getY() == Catch::Approx(0.0f).margin(1e-4f));
    // The packet's position stays the latest reported one.
    REQUIRE(e.getLatestX() == Catch::Approx(0.0f));
}

TEST_CASE("A strafing player moves to its side", "[entity][movement]") {
    Entity e;
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    e.startMoveByFlags(0.0f, 0.0f, 0.0f, 0.0f, kStrafeLeft, 0, 0.12f);
    e.updateMovement(0.5f);
    // Left of +x is canonical -y (forward is (cos o, -sin o)).
    REQUIRE(e.getX() == Catch::Approx(0.0f).margin(1e-4f));
    REQUIRE(e.getY() == Catch::Approx(-3.5f));
}

TEST_CASE("A packet's correction is taken out from where the unit is drawn",
          "[entity][movement]") {
    Entity e;
    e.setPosition(1.0f, 0.0f, 0.0f, 0.0f);
    e.startMoveByFlags(0.0f, 0.0f, 0.0f, 0.0f, kForward, 0, 0.5f);
    e.updateMovement(0.0f);
    REQUIRE(e.getX() == Catch::Approx(1.0f));
    e.updateMovement(0.25f);
    REQUIRE(e.getX() == Catch::Approx(1.75f + 0.5f));
    e.updateMovement(0.25f);
    REQUIRE(e.getX() == Catch::Approx(3.5f));
}

TEST_CASE("A unit still under way is carried on past a late packet", "[entity][movement]") {
    Entity e;
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    e.startMoveByFlags(0.0f, 0.0f, 0.0f, 0.0f, kForward, 0, 0.5f);
    // The next heartbeat is late: the unit keeps going and still counts as
    // moving.
    e.updateMovement(0.6f);
    REQUIRE(e.isActivelyMoving());
    REQUIRE(e.getX() == Catch::Approx(4.2f));
    // The late packet carries on from where it was drawn.
    e.startMoveByFlags(3.5f, 0.0f, 0.0f, 0.0f, kForward, 0, 0.5f);
    e.updateMovement(0.0f);
    REQUIRE(e.getX() == Catch::Approx(4.2f));
    // No packet for too long: stopped where it was carried to.
    e.updateMovement(1.1f);
    REQUIRE_FALSE(e.isActivelyMoving());
    REQUIRE(e.getX() == Catch::Approx(10.5f));
    // A stop packet stands it at the stop's position.
    e.startMoveTo(10.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    REQUIRE(e.getX() == Catch::Approx(10.0f));
}

TEST_CASE("A player turning in place turns at its turn rate", "[entity][movement]") {
    Entity e;
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    e.startMoveByFlags(0.0f, 0.0f, 0.0f, 0.0f, kTurnLeft, 0, 0.12f);
    REQUIRE_FALSE(e.isActivelyMoving());
    e.updateMovement(0.25f);
    REQUIRE(e.getOrientation() == Catch::Approx(kPi / 4.0f));
    // No heartbeat comes while only turning; it keeps turning.
    e.updateMovement(1.25f);
    REQUIRE(e.getOrientation() == Catch::Approx(-kPi / 2.0f));
    REQUIRE(e.getX() == 0.0f);
    // Right is the other way; STOP_TURN carries no turn flag.
    e.startMoveByFlags(0.0f, 0.0f, 0.0f, 0.0f, kTurnRight, 0, 0.12f);
    e.updateMovement(0.5f);
    REQUIRE(e.getOrientation() == Catch::Approx(-kPi / 2.0f));
    e.startMoveTo(0.0f, 0.0f, 0.0f, 0.3f, 0.0f);
    e.updateMovement(0.5f);
    REQUIRE(e.getOrientation() == Catch::Approx(0.3f));
}

TEST_CASE("A player turning as it runs turns slower, along an arc", "[entity][movement]") {
    Entity e;
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    e.startMoveByFlags(0.0f, 0.0f, 0.0f, 0.0f, kForward | kTurnLeft, 0, 0.12f);
    // Three quarters of the turn rate while moving (FUN_00987770).
    const float w = 0.75f * kPi;
    const float t = 0.8f;
    e.updateMovement(t);
    REQUIRE(e.getOrientation() == Catch::Approx(w * t));
    // On a circle of radius speed / rate, still at its speed.
    const float r = 7.0f / w;
    REQUIRE(e.getX() == Catch::Approx(r * std::sin(w * t)));
    REQUIRE(e.getY() == Catch::Approx(-r * (1.0f - std::cos(w * t))));

    // MOVEMENTFLAG2_FULL_SPEED_TURNING keeps the whole rate.
    Entity full;
    full.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    full.startMoveByFlags(0.0f, 0.0f, 0.0f, 0.0f, kForward | kTurnLeft, 0x8, 0.12f);
    full.updateMovement(0.5f);
    REQUIRE(full.getOrientation() == Catch::Approx(kPi / 2.0f));
}

TEST_CASE("A unit's model turns towards its facing rather than snapping",
          "[entity][movement]") {
    Entity e;
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    e.updateMovement(0.016f);
    REQUIRE(e.getModelFacing() == 0.0f);
    e.setOrientation(1.0f);
    // Half of the way each frame (FUN_00735f60)...
    e.updateMovement(0.016f);
    REQUIRE(e.getModelFacing() == Catch::Approx(0.5f));
    e.updateMovement(0.016f);
    REQUIRE(e.getModelFacing() == Catch::Approx(0.75f));
    // ...until close enough to set.
    for (int i = 0; i < 10; ++i) e.updateMovement(0.016f);
    REQUIRE(e.getModelFacing() == 1.0f);

    // The short way round.
    e.setOrientation(3.0f);
    for (int i = 0; i < 20; ++i) e.updateMovement(0.016f);
    e.setOrientation(-3.0f);
    e.updateMovement(0.016f);
    REQUIRE(std::abs(e.getModelFacing()) > 3.0f);
}

TEST_CASE("A creature faces along its move, then the facing it was given",
          "[entity][movement]") {
    Entity e;
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    e.startMoveTo(0.0f, 10.0f, 0.0f, 1.0f, 2.0f);
    e.faceAlongMove(1.0f, std::nullopt, false);
    // Towards +y is canonical yaw -pi/2.
    REQUIRE(e.getOrientation() == Catch::Approx(-kPi / 2.0f));
    e.updateMovement(1.0f);
    REQUIRE(e.getOrientation() == Catch::Approx(-kPi / 2.0f));
    e.updateMovement(1.5f);
    REQUIRE(e.getOrientation() == Catch::Approx(1.0f));

    // Along the spline's tangent: out along +x, then round to +y.
    Entity c;
    c.setPosition(0.0f, 0.0f, 0.0f, 2.0f);
    const std::vector<std::array<float, 3>> path = {
        {{0.0f, 0.0f, 0.0f}}, {{10.0f, 0.0f, 0.0f}}, {{10.0f, 10.0f, 0.0f}}};
    c.startMoveAlongPath(path, 2.0f, 4.0f);
    c.faceAlongMove(std::nullopt, std::nullopt, false);
    REQUIRE(c.getOrientation() == Catch::Approx(0.0f).margin(0.2f));
    c.updateMovement(3.5f);
    REQUIRE(c.getOrientation() == Catch::Approx(-kPi / 2.0f).margin(0.3f));
    // No final facing: it keeps the last.
    c.updateMovement(1.0f);
    REQUIRE(c.getOrientation() == Catch::Approx(-kPi / 2.0f).margin(0.3f));

    // A spline that keeps its orientation, inverted or not.
    Entity f;
    f.setPosition(0.0f, 0.0f, 0.0f, 0.5f);
    f.startMoveTo(10.0f, 0.0f, 0.0f, 0.5f, 2.0f);
    f.faceAlongMove(std::nullopt, 0.5f, false);
    REQUIRE(f.getOrientation() == Catch::Approx(0.5f));
    Entity b;
    b.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    b.startMoveTo(10.0f, 0.0f, 0.0f, 0.0f, 2.0f);
    b.faceAlongMove(std::nullopt, std::nullopt, true);
    REQUIRE(std::abs(b.getOrientation()) == Catch::Approx(kPi));
}

TEST_CASE("A unit not under way stops at the segment's end", "[entity][movement]") {
    Entity e;
    e.setPosition(0.0f, 0.0f, 0.0f, 0.0f);
    e.startMoveTo(3.5f, 0.0f, 0.0f, 0.0f, 0.5f);
    e.updateMovement(0.6f);
    REQUIRE_FALSE(e.isActivelyMoving());
}

TEST_CASE("Entity field set/get/has", "[entity]") {
    Entity e;
    REQUIRE_FALSE(e.hasField(10));

    e.setField(10, 0xCAFE);
    REQUIRE(e.hasField(10));
    REQUIRE(e.getField(10) == 0xCAFE);

    // Overwrite
    e.setField(10, 0xBEEF);
    REQUIRE(e.getField(10) == 0xBEEF);

    // Non-existent returns 0
    REQUIRE(e.getField(999) == 0);
}

TEST_CASE("Unit construction and type", "[entity]") {
    Unit u;
    REQUIRE(u.getType() == ObjectType::UNIT);

    Unit u2(0x123);
    REQUIRE(u2.getGuid() == 0x123);
    REQUIRE(u2.getType() == ObjectType::UNIT);
}

TEST_CASE("Unit name", "[entity]") {
    Unit u;
    REQUIRE(u.getName().empty());
    u.setName("Hogger");
    REQUIRE(u.getName() == "Hogger");
}

TEST_CASE("Unit health", "[entity]") {
    Unit u;
    REQUIRE(u.getHealth() == 0);
    REQUIRE(u.getMaxHealth() == 0);

    u.setHealth(500);
    u.setMaxHealth(1000);
    REQUIRE(u.getHealth() == 500);
    REQUIRE(u.getMaxHealth() == 1000);
}

TEST_CASE("Unit power by type", "[entity]") {
    Unit u;
    u.setPowerType(0); // mana
    u.setPower(200);
    u.setMaxPower(500);

    REQUIRE(u.getPower() == 200);
    REQUIRE(u.getMaxPower() == 500);
    REQUIRE(u.getPowerByType(0) == 200);
    REQUIRE(u.getMaxPowerByType(0) == 500);

    // Set rage (type 1)
    u.setPowerByType(1, 50);
    u.setMaxPowerByType(1, 100);
    REQUIRE(u.getPowerByType(1) == 50);
    REQUIRE(u.getMaxPowerByType(1) == 100);

    // Out of bounds clamps
    REQUIRE(u.getPowerByType(7) == 0);
    REQUIRE(u.getMaxPowerByType(7) == 0);
}

TEST_CASE("Unit level, entry, displayId", "[entity]") {
    Unit u;
    REQUIRE(u.getLevel() == 1); // default
    u.setLevel(80);
    REQUIRE(u.getLevel() == 80);

    u.setEntry(1234);
    REQUIRE(u.getEntry() == 1234);

    u.setDisplayId(5678);
    REQUIRE(u.getDisplayId() == 5678);
}

TEST_CASE("Unit flags", "[entity]") {
    Unit u;
    u.setUnitFlags(0x01);
    REQUIRE(u.getUnitFlags() == 0x01);

    REQUIRE_FALSE(u.hasCreepVisibility());
    u.setVisibilityFlags(UNIT_VIS_FLAG_CREEP);
    REQUIRE(u.getVisibilityFlags() == UNIT_VIS_FLAG_CREEP);
    REQUIRE(u.hasCreepVisibility());
    u.clearCreepVisibility();
    REQUIRE_FALSE(u.hasCreepVisibility());
    REQUIRE((u.getVisibilityFlags() & UNIT_VIS_FLAG_CREEP) == 0);

    u.setDynamicFlags(0x02);
    REQUIRE(u.getDynamicFlags() == 0x02);

    u.setNpcFlags(0x04);
    REQUIRE(u.getNpcFlags() == 0x04);
    REQUIRE(u.isInteractable());

    u.setNpcFlags(0);
    REQUIRE_FALSE(u.isInteractable());
}

TEST_CASE("Corpse state uses the real dynamic dead bit", "[entity][corpse]") {
    REQUIRE(isUnitCorpseState(0, 100, 0));
    REQUIRE(isUnitCorpseState(0, 0, UNIT_DYNFLAG_DEAD));
    REQUIRE(isUnitCorpseState(0, 0, UNIT_DYNFLAG_LOOTABLE));

    REQUIRE_FALSE(isUnitCorpseState(100, 100, 0));
    // Regression: 0x08 is tapped-by-player, not dead.
    REQUIRE_FALSE(isUnitCorpseState(100, 100, UNIT_DYNFLAG_TAPPED_BY_PLAYER));
    REQUIRE(UNIT_DYNFLAG_DEAD == 0x00000020);
}

TEST_CASE("Unit faction and hostility", "[entity]") {
    Unit u;
    u.setFactionTemplate(14); // Undercity
    REQUIRE(u.getFactionTemplate() == 14);

    REQUIRE_FALSE(u.isHostile());
    u.setHostile(true);
    REQUIRE(u.isHostile());
}

TEST_CASE("Unit mount display ID", "[entity]") {
    Unit u;
    REQUIRE(u.getMountDisplayId() == 0);
    u.setMountDisplayId(14374);
    REQUIRE(u.getMountDisplayId() == 14374);
}

TEST_CASE("Player inherits Unit", "[entity]") {
    Player p(0xABC);
    REQUIRE(p.getType() == ObjectType::PLAYER);
    REQUIRE(p.getGuid() == 0xABC);

    // Player inherits Unit name - regression test for the shadowed-field fix
    p.setName("Arthas");
    REQUIRE(p.getName() == "Arthas");

    p.setLevel(80);
    REQUIRE(p.getLevel() == 80);
}

TEST_CASE("GameObject construction", "[entity]") {
    GameObject go(0x999);
    REQUIRE(go.getType() == ObjectType::GAMEOBJECT);
    REQUIRE(go.getGuid() == 0x999);

    go.setName("Mailbox");
    REQUIRE(go.getName() == "Mailbox");

    go.setEntry(42);
    REQUIRE(go.getEntry() == 42);

    go.setDisplayId(100);
    REQUIRE(go.getDisplayId() == 100);
}

TEST_CASE("EntityManager add/get/has/remove", "[entity]") {
    EntityManager mgr;
    REQUIRE(mgr.getEntityCount() == 0);

    auto unit = std::make_shared<Unit>(1);
    unit->setName("TestUnit");
    mgr.addEntity(1, unit);

    REQUIRE(mgr.getEntityCount() == 1);
    REQUIRE(mgr.hasEntity(1));
    REQUIRE_FALSE(mgr.hasEntity(2));

    auto retrieved = mgr.getEntity(1);
    REQUIRE(retrieved != nullptr);
    REQUIRE(retrieved->getGuid() == 1);

    mgr.removeEntity(1);
    REQUIRE_FALSE(mgr.hasEntity(1));
    REQUIRE(mgr.getEntityCount() == 0);
}

TEST_CASE("EntityManager clear", "[entity]") {
    EntityManager mgr;
    mgr.addEntity(1, std::make_shared<Entity>(1));
    mgr.addEntity(2, std::make_shared<Entity>(2));
    REQUIRE(mgr.getEntityCount() == 2);

    mgr.clear();
    REQUIRE(mgr.getEntityCount() == 0);
}

TEST_CASE("EntityManager null entity rejected", "[entity]") {
    EntityManager mgr;
    mgr.addEntity(1, nullptr);
    // Null should be rejected (logged warning, not stored)
    REQUIRE(mgr.getEntityCount() == 0);
}

TEST_CASE("EntityManager getEntities returns all", "[entity]") {
    EntityManager mgr;
    mgr.addEntity(10, std::make_shared<Unit>(10));
    mgr.addEntity(20, std::make_shared<Player>(20));
    mgr.addEntity(30, std::make_shared<GameObject>(30));

    const auto& all = mgr.getEntities();
    REQUIRE(all.size() == 3);
    REQUIRE(all.count(10) == 1);
    REQUIRE(all.count(20) == 1);
    REQUIRE(all.count(30) == 1);
}

TEST_CASE("EntityManager spatial query filters nearby entities", "[entity][spatial]") {
    EntityManager mgr;
    auto origin = std::make_shared<Unit>(1);
    origin->setPosition(-10.0f, -10.0f, 0.0f, 0.0f);
    auto nearby = std::make_shared<Unit>(2);
    nearby->setPosition(20.0f, 15.0f, 0.0f, 0.0f);
    auto distant = std::make_shared<Unit>(3);
    distant->setPosition(500.0f, 500.0f, 0.0f, 0.0f);
    mgr.addEntity(1, origin);
    mgr.addEntity(2, nearby);
    mgr.addEntity(3, distant);

    const auto result = mgr.getEntitiesNear(0.0f, 0.0f, 50.0f);
    REQUIRE(result.size() == 2);
    REQUIRE(std::any_of(result.begin(), result.end(), [](const auto& e) { return e->getGuid() == 1; }));
    REQUIRE(std::any_of(result.begin(), result.end(), [](const auto& e) { return e->getGuid() == 2; }));
    REQUIRE_FALSE(std::any_of(result.begin(), result.end(), [](const auto& e) { return e->getGuid() == 3; }));
}
