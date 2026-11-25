// K4: a spline a create or movement block brings is flown on from where its
// time has it (0x004f4b50, 0x006f1520, 0x0098ca00). See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "core/coordinates.hpp"
#include "game/movement_handler.hpp"
#include "game/transport_local.hpp"
#include "game/transport_manager.hpp"
#include "game_replay.hpp"

#include <cmath>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::Opcode;
using wowee::game::ObjectType;
using wowee::game::UF;
using wowee::game::UpdateType;

namespace {

constexpr uint64_t kWalker = 0xF130004B460009D8ull;
const glm::vec3 kAt(-1834.0f, 5299.0f, -12.4f);
const UpdateObject::Fields kFields{{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}};

glm::vec3 placeOf(const wowee::game::Entity& e) {
    return wowee::core::coords::canonicalToServer(glm::vec3(e.getX(), e.getY(), e.getZ()));
}
float wireFacing(const wowee::game::Entity& e) {
    return wowee::core::coords::canonicalToServerYaw(e.getOrientation());
}

// The points as MoveSplineInit has them for a straight path: the path and its
// last point again (SplineBase::InitLinear).
std::vector<glm::vec3> straight(std::vector<glm::vec3> way) {
    way.push_back(way.back());
    return way;
}
// And for a curve: a point ahead of it and its last again (InitCatmullRom).
std::vector<glm::vec3> curve(std::vector<glm::vec3> way) {
    way.insert(way.begin(), way[0] - (way[1] - way[0]));
    way.push_back(way.back());
    return way;
}

UpdateObject::Spline spline(const std::vector<glm::vec3>& points, const glm::vec3& end, uint32_t flags, uint8_t mode) {
    UpdateObject::Spline s;
    s.at = kAt;
    s.points = points;
    s.flags = flags;
    s.mode = mode;
    s.end = end;
    s.passed = 1000;
    s.duration = 4000;
    return s;
}

std::shared_ptr<wowee::game::Entity> make(Session& s, const UpdateObject::Spline& sp, uint64_t guid = kWalker) {
    s.receive(UpdateObject().onSpline(guid, kFields, sp));
    auto e = s.handler().getEntityManager().getEntity(guid);
    REQUIRE(e);
    e->updateMovement(0.0f);
    return e;
}

wowee::network::Packet monsterMove(uint64_t guid, uint32_t id, const glm::vec3& from, const glm::vec3& to, uint32_t ms,
                                   uint32_t flags = 0, float acceleration = 0.0f) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_MONSTER_MOVE));
    p.writePackedGuid(guid);
    p.writeUInt8(0);
    for (float f : {from.x, from.y, from.z}) p.writeFloat(f);
    p.writeUInt32(id);
    p.writeUInt8(0);
    p.writeUInt32(flags);
    p.writeUInt32(ms);
    if (flags & 0x800u) {
        p.writeFloat(acceleration);
        p.writeUInt32(0);
    }
    p.writeUInt32(1);
    for (float f : {to.x, to.y, to.z}) p.writeFloat(f);
    return p;
}

bool closeTo(const glm::vec3& a, const glm::vec3& b, float within = 0.15f) { return glm::length(a - b) < within; }

}  // namespace

TEST_CASE("a unit that comes into view on a curve goes on from where its time has it", "[replay][blockspline]") {
    const uint32_t flags = GENERATE(0x2000u, 0x42000u);
    Session s;
    const std::vector<glm::vec3> way = {kAt, kAt + glm::vec3(10, 0, 0), kAt + glm::vec3(20, 0, 0), kAt + glm::vec3(30, 0, 0),
                                        kAt + glm::vec3(40, 0, 0)};
    const auto e = make(s, spline(curve(way), way.back(), flags, 1));
    CHECK(closeTo(placeOf(*e), kAt + glm::vec3(10, 0, 0)));
    CHECK(e->isActivelyMoving());
    CHECK(e->getMoveSpeed() == Catch::Approx(10.0f).margin(0.1f));
    CHECK(std::cos(wireFacing(*e)) == Catch::Approx(1.0f).margin(0.01f));
    CHECK(s.handler().onMoveSpline(kWalker));
    e->updateMovement(1.5f);
    CHECK(closeTo(placeOf(*e), kAt + glm::vec3(25, 0, 0)));
    e->updateMovement(1.6f);
    CHECK(closeTo(placeOf(*e), way.back()));
    CHECK_FALSE(e->isActivelyMoving());
    CHECK((e->getReportedMoveFlags() & 0x3u) == 0);
    e->updateMovement(10.0f);
    CHECK(closeTo(placeOf(*e), way.back()));
}

// 0x004c3fd0: from point i + 1 to i + 2, the straight one too.
TEST_CASE("a straight spline is run between its first and its last point, both left out", "[replay][blockspline]") {
    Session s;
    const std::vector<glm::vec3> way = {kAt, kAt + glm::vec3(5, 0, 0), kAt + glm::vec3(25, 0, 0), kAt + glm::vec3(25, 20, 0)};
    const auto e = make(s, spline(straight(way), way.back(), 0, 0));
    // A quarter of the forty yards after its second point.
    CHECK(closeTo(placeOf(*e), kAt + glm::vec3(15, 0, 0)));
    CHECK(e->getMoveSpeed() == Catch::Approx(10.0f).margin(0.1f));
    e->updateMovement(2.0f);
    CHECK(closeTo(placeOf(*e), kAt + glm::vec3(25, 10, 0)));
    CHECK(std::sin(wireFacing(*e)) == Catch::Approx(1.0f).margin(0.01f));
    e->updateMovement(1.1f);
    CHECK(closeTo(placeOf(*e), way.back()));

    // Of one stretch the realm writes three points: it stands at its end.
    const std::vector<glm::vec3> one = {kAt, kAt + glm::vec3(8, 0, 0)};
    const auto f = make(s, spline(straight(one), one.back(), 0, 0), kWalker + 1);
    CHECK(closeTo(placeOf(*f), one.back()));
}

TEST_CASE("a spline over as its block comes leaves the unit at its end", "[replay][blockspline]") {
    Session s;
    const std::vector<glm::vec3> way = {kAt, kAt + glm::vec3(10, 0, 0), kAt + glm::vec3(20, 0, 0)};
    auto sp = spline(curve(way), way.back(), 0x22000, 1);
    sp.passed = 9000;
    sp.finalAngle = 2.0f;
    auto e = make(s, sp);
    CHECK(closeTo(placeOf(*e), way.back()));
    CHECK(wireFacing(*e) == Catch::Approx(2.0f).margin(0.01f));
    CHECK_FALSE(e->isActivelyMoving());
    CHECK_FALSE(s.handler().onMoveSpline(kWalker));
    CHECK((e->getReportedMoveFlags() & 0x3u) == 0);

    // The end point where the last is three yards off and more (0x006e9470).
    sp.end = way.back() + glm::vec3(0, 5, 0);
    e = make(s, sp, kWalker + 1);
    CHECK(closeTo(placeOf(*e), sp.end));
    sp.end = way.back() + glm::vec3(0, 2, 0);
    e = make(s, sp, kWalker + 2);
    CHECK(closeTo(placeOf(*e), way.back()));
    // Marked let go (0x400), or no flag to move by (0x0098ca00).
    sp = spline(curve(way), way.back(), 0x2400, 1);
    e = make(s, sp, kWalker + 3);
    CHECK(closeTo(placeOf(*e), kAt));
    sp = spline(curve(way), way.back(), 0x2000, 1);
    sp.moveFlags = 0x08000000;
    e = make(s, sp, kWalker + 4);
    e->updateMovement(1.0f);
    CHECK(closeTo(placeOf(*e), kAt));
    CHECK_FALSE(s.handler().onMoveSpline(kWalker + 4));
    // Frozen: kept, and nowhere to go.
    sp = spline(curve(way), way.back(), 0x402000, 1);
    e = make(s, sp, kWalker + 5);
    e->updateMovement(9.0f);
    CHECK(closeTo(placeOf(*e), kAt));
    CHECK(s.handler().onMoveSpline(kWalker + 5));
}

TEST_CASE("a block's spline takes its duration's factor", "[replay][blockspline]") {
    Session s;
    const std::vector<glm::vec3> way = {kAt, kAt + glm::vec3(10, 0, 0), kAt + glm::vec3(20, 0, 0), kAt + glm::vec3(40, 0, 0)};
    auto sp = spline(curve(way), way.back(), 0x2000, 1);
    sp.factor = 2.0f;
    const auto e = make(s, sp);
    CHECK(placeOf(*e).x - kAt.x == Catch::Approx(5.0f).margin(0.8f));
    e->updateMovement(3.0f);
    CHECK(placeOf(*e).x - kAt.x == Catch::Approx(20.0f).margin(1.0f));
    CHECK(e->isActivelyMoving());
    e->updateMovement(4.1f);
    CHECK(closeTo(placeOf(*e), way.back()));
}

// Round and round: the realm's array is the last point, the points and the
// first two again, and its end point the origin.
TEST_CASE("a cyclic spline is run round and round", "[replay][blockspline]") {
    Session s;
    const std::vector<glm::vec3> ring = {kAt, kAt + glm::vec3(20, 0, 0), kAt + glm::vec3(20, 20, 0), kAt + glm::vec3(0, 20, 0)};
    std::vector<glm::vec3> points = {ring[3], ring[0], ring[1], ring[2], ring[3], ring[0], ring[1]};
    auto sp = spline(points, glm::vec3(0.0f), 0x82000, 1);
    sp.passed = 3000;
    const auto e = make(s, sp);
    CHECK(closeTo(placeOf(*e), ring[3], 0.3f));
    e->updateMovement(2.0f);
    CHECK(closeTo(placeOf(*e), ring[1], 0.3f));
    e->updateMovement(40.0f);
    CHECK(closeTo(placeOf(*e), ring[1], 0.4f));
    CHECK(e->isActivelyMoving());
    CHECK((e->getReportedMoveFlags() & 0x1u) == 0x1u);
    CHECK(s.handler().onMoveSpline(kWalker));
    // A stop ends it.
    wowee::network::Packet stop(wowee::game::wireOpcode(Opcode::SMSG_MONSTER_MOVE));
    stop.writePackedGuid(kWalker);
    stop.writeUInt8(0);
    for (float f : {kAt.x, kAt.y, kAt.z}) stop.writeFloat(f);
    stop.writeUInt32(9);
    stop.writeUInt8(1);
    s.receive(stop);
    e->updateMovement(1.0f);
    CHECK(closeTo(placeOf(*e), kAt));
    CHECK_FALSE(s.handler().onMoveSpline(kWalker));

    // Entering its cycle: the first point once and no more (0x0098c940).
    points = {kAt + glm::vec3(-20, 0, 0), kAt + glm::vec3(-40, 0, 0), ring[0], ring[1], ring[2], ring[3], ring[0], ring[1]};
    sp = spline(points, glm::vec3(0.0f), 0x182000, 1);
    sp.passed = 0;
    sp.duration = 1000;
    const auto f = make(s, sp, kWalker + 1);
    CHECK(closeTo(placeOf(*f), kAt + glm::vec3(-40, 0, 0), 0.2f));
    for (int i = 0; i < 100; ++i) {
        f->updateMovement(0.05f);
        if (i >= 20) CHECK(placeOf(*f).x - kAt.x > -6.0f);
    }
}

// SMSG_FLIGHT_SPLINE_SYNC (0x00716940 -> 0x0098b5d0): the next round makes good
// what the unit is behind or ahead of the server.
TEST_CASE("a cyclic spline keeps the server's time", "[replay][blockspline]") {
    const std::vector<glm::vec3> ring = {kAt, kAt + glm::vec3(20, 0, 0), kAt + glm::vec3(20, 20, 0), kAt + glm::vec3(0, 20, 0)};
    const std::vector<glm::vec3> points = {ring[3], ring[0], ring[1], ring[2], ring[3], ring[0], ring[1]};
    auto sp = spline(points, glm::vec3(0.0f), 0x82000, 1);
    sp.passed = 0;
    sp.duration = 4000;
    const auto run = [](auto& e, int steps) {
        for (int i = 0; i < steps; ++i) e.updateMovement(0.05f);
    };
    const auto sync = [&](Session& s, float fraction) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_FLIGHT_SPLINE_SYNC));
        p.writeFloat(fraction);
        p.writePackedGuid(kWalker);
        s.receive(p);
    };
    Session s;
    const auto e = make(s, sp);
    run(*e, 1);
    sync(s, 0.2625f);   // a quarter of a round behind
    run(*e, 79);
    CHECK(closeTo(placeOf(*e), ring[0], 0.6f));
    run(*e, 60);
    CHECK(closeTo(placeOf(*e), ring[0], 0.6f));   // a round of three seconds
    run(*e, 80);
    CHECK(closeTo(placeOf(*e), ring[0], 0.6f));   // and then its own again

    Session t;
    const auto f = make(t, sp);
    run(*f, 20);
    sync(t, 0.0f);      // a quarter ahead
    run(*f, 60 + 80);
    run(*f, 20);
    CHECK(closeTo(placeOf(*f), ring[0], 0.6f));   // five seconds
}

TEST_CASE("a parabolic spline arcs over its path", "[replay][blockspline]") {
    Session s;
    const std::vector<glm::vec3> way = {kAt, kAt + glm::vec3(10, 0, 0), kAt + glm::vec3(20, 0, 0), kAt + glm::vec3(30, 0, 0)};
    auto sp = spline(curve(way), way.back(), 0x2800, 1);
    sp.duration = 2000;
    sp.passed = 1000;
    sp.acceleration = 19.29f;
    const auto e = make(s, sp);
    // 0.5 * a * (2 - 1) * 1 over it (0x00987d20).
    CHECK(placeOf(*e).z - kAt.z == Catch::Approx(9.645f).margin(0.05f));
    e->updateMovement(0.5f);
    CHECK(placeOf(*e).z - kAt.z == Catch::Approx(0.5f * 19.29f * 0.5f * 1.5f).margin(0.1f));
    e->updateMovement(0.6f);
    CHECK(closeTo(placeOf(*e), way.back()));
    // A monster move's the same (0x006e96c0).
    const auto leaper = make(s, sp, kWalker + 1);
    const glm::vec3 from = placeOf(*leaper);
    s.receive(monsterMove(kWalker + 1, 4, from, from + glm::vec3(10, 0, 0), 2000, 0x800, 10.0f));
    leaper->updateMovement(1.0f);
    CHECK(placeOf(*leaper).z - from.z == Catch::Approx(5.0f).margin(0.1f));
    leaper->updateMovement(1.1f);
    CHECK(closeTo(placeOf(*leaper), from + glm::vec3(10, 0, 0)));
}

// Whom it faces at the end is looked up there: not in view before, or moved.
TEST_CASE("a block's spline ends facing its target as it stands then", "[replay][blockspline]") {
    Session s;
    const std::vector<glm::vec3> way = {kAt, kAt + glm::vec3(10, 0, 0), kAt + glm::vec3(20, 0, 0)};
    auto sp = spline(curve(way), way.back(), 0x12000, 1);
    sp.finalTarget = kWalker + 9;
    const auto e = make(s, sp);
    auto target = spline({}, kAt, 0, 0);
    target.moveFlags = 0;
    target.at = way.back() + glm::vec3(0, 7, 0);
    make(s, target, kWalker + 9);
    e->updateMovement(3.1f);
    CHECK(std::sin(wireFacing(*e)) == Catch::Approx(1.0f).margin(0.01f));
    // Kept on the way where the spline says so (0x4000).
    const auto f = make(s, spline(curve(way), way.back(), 0x6000, 1), kWalker + 1);
    CHECK(wireFacing(*f) == Catch::Approx(0.5f).margin(0.01f));
    f->updateMovement(5.0f);
    CHECK(wireFacing(*f) == Catch::Approx(0.5f).margin(0.01f));
}

TEST_CASE("the next monster move takes a unit off its block's spline", "[replay][blockspline]") {
    Session s;
    const std::vector<glm::vec3> way = {kAt, kAt + glm::vec3(10, 0, 0), kAt + glm::vec3(20, 0, 0), kAt + glm::vec3(400, 0, 0)};
    const auto e = make(s, spline(curve(way), way.back(), 0x2000, 1));
    const glm::vec3 from = placeOf(*e);
    s.receive(monsterMove(kWalker, 78, from, from + glm::vec3(0, 10, 0), 2000));
    e->updateMovement(1.0f);
    CHECK(closeTo(placeOf(*e), from + glm::vec3(0, 5, 0), 0.3f));
    e->updateMovement(1.1f);
    CHECK(closeTo(placeOf(*e), from + glm::vec3(0, 10, 0)));

    // Made again, or moved by a movement block: what that says, with a spline
    // of its own or with none (0x004d63b0, 0x004d6da0).
    const auto type = GENERATE(UpdateType::CREATE_OBJECT, UpdateType::MOVEMENT);
    auto again = spline(curve(way), way.back(), 0x2000, 1);
    again.type = type;
    s.receive(UpdateObject().onSpline(kWalker, kFields, again));
    auto now = s.handler().getEntityManager().getEntity(kWalker);
    now->updateMovement(0.0f);
    CHECK(now->isActivelyMoving());
    CHECK(s.handler().onMoveSpline(kWalker));
    again.moveFlags = 0;
    again.at = kAt + glm::vec3(3, 3, 0);
    s.receive(UpdateObject().onSpline(kWalker, kFields, again));
    now = s.handler().getEntityManager().getEntity(kWalker);
    now->updateMovement(1.0f);
    CHECK(closeTo(placeOf(*now), again.at));
    CHECK_FALSE(now->isActivelyMoving());
    CHECK_FALSE(s.handler().onMoveSpline(kWalker));
}

// P45's clock: it began as long before the packet came as the block says.
TEST_CASE("a block's spline goes by the clock from before its packet came", "[replay][blockspline]") {
    using namespace std::chrono_literals;
    Session s;
    s.setConnected(true);
    auto& gh = s.handler();
    // The packets' wait and the clock it is measured by are one: ahead of the real one here.
    wowee::core::advanceTestClock(30.0);
    gh.setUnitClock(wowee::core::steadyNow() - 5s);
    gh.update(0.016f);
    std::vector<glm::vec3> way;
    for (int i = 0; i <= 100; ++i) way.push_back(kAt + glm::vec3(10.0f * static_cast<float>(i), 0, 0));
    auto sp = spline(curve(way), way.back(), 0x2000, 1);
    sp.passed = 10000;
    sp.duration = 100000;
    s.receive(UpdateObject().onSpline(kWalker, kFields, sp));
    const auto started = wowee::core::steadyNow();
    const auto e = gh.getEntityManager().getEntity(kWalker);
    REQUIRE(e);
    gh.setUnitClock(started);
    gh.update(0.016f);
    CHECK(closeTo(placeOf(*e), kAt + glm::vec3(100, 0, 0), 2.0f));
    gh.setUnitClock(started + 2s);
    gh.update(0.016f);
    CHECK(closeTo(placeOf(*e), kAt + glm::vec3(120, 0, 0), 2.0f));
    // Its packet read four seconds before it is handled.
    auto packet = UpdateObject().onSpline(kWalker + 1, kFields, sp).packet();
    packet.setReceivedMs(std::chrono::duration_cast<std::chrono::milliseconds>(wowee::core::steadyNow().time_since_epoch()).count() - 4000);
    s.receive(packet);
    const auto late = gh.getEntityManager().getEntity(kWalker + 1);
    REQUIRE(late);
    late->updateMovement(0.0f);
    CHECK(closeTo(placeOf(*late), kAt + glm::vec3(140, 0, 0), 2.0f));
}

// A login in flight: the player's own block has the spline, which carries it,
// ends with its id reported, and holds the player's own movement back.
TEST_CASE("the player made on a spline is carried by it to its end", "[replay][blockspline]") {
    Session s;
    const std::vector<glm::vec3> way = {kAt, kAt + glm::vec3(1, 0, 0), kAt + glm::vec3(2, 0, 0), kAt + glm::vec3(3, 0, 0)};
    auto sp = spline(curve(way), way.back(), 0x2000, 1);
    sp.objectType = ObjectType::PLAYER;
    sp.passed = 100;
    sp.duration = 200;
    sp.id = 4711;
    auto fields = kFields;
    fields[UF::UNIT_FIELD_FLAGS] = 0x00100000;
    s.receive(UpdateObject().onSpline(Session::kPlayerGuid, fields, sp));
    auto* mh = s.handler().getMovementHandler();
    auto self = s.handler().getEntityManager().getEntity(Session::kPlayerGuid);
    REQUIRE(self);
    self->updateMovement(0.0f);
    mh->updateClientTaxi(0.01f);
    CHECK(mh->onMoveSpline(Session::kPlayerGuid));
    CHECK(s.handler().isOnTaxiFlight());
    CHECK(closeTo(placeOf(*self), kAt + glm::vec3(1.5f, 0, 0), 0.2f));
    // Its block once more, as it flies: passed over (0x004d63b0).
    auto again = sp;
    again.at = kAt + glm::vec3(0, 300, 0);
    again.passed = 0;
    again.id = 4712;
    s.receive(UpdateObject().onSpline(Session::kPlayerGuid, fields, again));
    self = s.handler().getEntityManager().getEntity(Session::kPlayerGuid);
    self->updateMovement(0.0f);
    CHECK(closeTo(placeOf(*self), kAt + glm::vec3(1.5f, 0, 0), 0.3f));
    s.handler().sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    CHECK(s.sent().of(Opcode::MSG_MOVE_HEARTBEAT).empty());
    wowee::core::advanceTestClock(0.150);
    self->updateMovement(0.15f);
    mh->updateMoverSplineDone();
    REQUIRE(s.sent().of(Opcode::CMSG_MOVE_SPLINE_DONE).size() == 1);
    const auto done = s.sent().of(Opcode::CMSG_MOVE_SPLINE_DONE).front().getData();   // of()'s is gone after this
    REQUIRE(done.size() > 4);
    uint32_t id = 0;
    for (int i = 0; i < 4; ++i) id |= static_cast<uint32_t>(done[done.size() - 4 + i]) << (8 * i);
    CHECK(id == 4711);
    CHECK(closeTo(placeOf(*self), way.back()));
}

// On a transport the points are on it, and the place is put through it as it
// lies each frame (0x004f4460).
TEST_CASE("a spline on a transport is run on it", "[replay][blockspline]") {
    wowee::game::Entity e;
    e.setPosition(1.0f, 2.0f, 3.0f, 0.0f);
    wowee::game::Entity::BlockSpline sp;
    sp.points = {glm::vec3(-1, 0, 0), glm::vec3(0, 0, 0), glm::vec3(10, 0, 0), glm::vec3(10, 0, 0)};
    sp.endPoint = glm::vec3(10, 0, 0);
    sp.framed = true;
    sp.durationMs = 2000;
    e.startBlockSpline(sp);
    e.updateMovement(1.0f);
    // Not there yet: it stays.
    CHECK(e.getX() == 1.0f);
    CHECK(e.onFramedPath());
    glm::mat4 frame(1.0f);
    frame[3] = glm::vec4(100.0f, 50.0f, 5.0f, 1.0f);
    e.setMoveFrame(frame);
    CHECK(e.getX() == Catch::Approx(105.0f).margin(0.05f));
    // A quarter turn, and on.
    frame[0] = glm::vec4(0, 1, 0, 0);
    frame[1] = glm::vec4(-1, 0, 0, 0);
    e.setMoveFrame(frame);
    CHECK(e.getX() == Catch::Approx(100.0f).margin(0.05f));
    CHECK(e.getY() == Catch::Approx(55.0f).margin(0.05f));
    e.updateMovement(1.1f);
    CHECK(e.getY() == Catch::Approx(60.0f).margin(0.05f));
    CHECK_FALSE(e.isActivelyMoving());
}

// The same by its packet, with the transport there and come only after it.
TEST_CASE("a passenger that comes into view walking walks on its transport", "[replay][blockspline]") {
    constexpr uint64_t kShip = 0x1FC0000000000007ull;
    const bool shipFirst = GENERATE(true, false);
    const uint32_t flags = GENERATE(0x2000u, 0x22000u);
    Session s;
    auto* transports = s.handler().getTransportManager();
    REQUIRE(transports);
    transports->loadPathFromNodes(9001, {glm::vec3(0.0f)}, false, 10.0f);
    const glm::vec3 deck = wowee::core::coords::serverToCanonical(kAt);
    const auto ship = [&] { transports->registerTransport(kShip, 0, 9001, deck, 9001); };
    if (shipFirst) ship();
    const std::vector<glm::vec3> way = {glm::vec3(0), glm::vec3(10, 0, 2), glm::vec3(20, 0, 2), glm::vec3(30, 0, 2)};
    auto sp = spline(curve(way), way.back(), flags, 1);
    sp.moveFlags = 0x08000201;
    sp.transport = kShip;
    sp.duration = 3000;
    sp.at = kAt + glm::vec3(10, 0, 2);
    sp.onTransport = glm::vec3(10, 0, 2);
    const auto e = make(s, sp);
    if (!shipFirst) {
        e->updateMovement(0.5f);
        CHECK(closeTo(placeOf(*e), sp.at));
        ship();
    }
    const auto local = [&](const glm::vec3& wire) {
        const glm::vec3 c = transports->getPlayerWorldPosition(kShip, transports->serverToTransportLocal(kShip, wire));
        return wowee::core::coords::canonicalToServer(c);
    };
    s.handler().getMovementHandler()->updateAttachedTransportChildren(0.0f);
    CHECK(closeTo(placeOf(*e), local(glm::vec3(shipFirst ? 10.0f : 15.0f, 0, 2)), 0.6f));
    CHECK(e->isActivelyMoving());
    e->updateMovement(shipFirst ? 1.0f : 0.5f);
    s.handler().getMovementHandler()->updateAttachedTransportChildren(0.0f);
    CHECK(closeTo(placeOf(*e), local(glm::vec3(20, 0, 2)), 0.6f));
    // Moved under it, and at its end it stays on it.
    auto* active = transports->getTransport(kShip);
    REQUIRE(active);
    active->transform[3] += glm::vec4(50.0f, 0.0f, 0.0f, 0.0f);
    e->updateMovement(1.5f);
    s.handler().getMovementHandler()->updateAttachedTransportChildren(0.0f);
    CHECK(closeTo(placeOf(*e), local(way.back())));
    if (flags == 0x22000u) {
        const float facing = wowee::game::transport_local::worldYaw(*transports->transportFacing(kShip), 1.0f);
        CHECK(std::cos(e->getOrientation() - facing) == Catch::Approx(1.0f).margin(0.01f));
    }
}

// T287: logged in on a ship, its model still to come. 12340 keeps a unit on its transport by the
// guid (0x004f4460); the offset was taken as an M2's and swapped for the hull's, and the player let
// go of it - or sent back across the deck.
TEST_CASE("the player is kept on the transport it is made on", "[replay][transport]") {
    constexpr uint64_t kShip = 0x1FC0000000000007ull, kLift = 0xF12000001000000Aull;
    const bool ship = GENERATE(true, false);
    const uint64_t guid = ship ? kShip : kLift;
    Session s;
    auto* transports = s.handler().getTransportManager();
    REQUIRE(transports);
    transports->loadPathFromNodes(9001, {glm::vec3(0.0f)}, false, 10.0f);
    const glm::vec3 deck = wowee::core::coords::serverToCanonical(kAt);
    const glm::vec3 wire(3.0f, -7.0f, 2.0f);
    UpdateObject::Spline sp;
    sp.objectType = ObjectType::PLAYER;
    sp.moveFlags = 0x200;
    sp.transport = guid;
    sp.onTransport = wire;
    sp.at = kAt + glm::vec3(50.0f, 60.0f, 2.0f);
    sp.facing = 1.0f;
    s.receive(UpdateObject().transport(guid, {}).onSpline(Session::kPlayerGuid, {}, sp));
    auto& h = s.handler();
    CHECK(h.hasPendingPlayerTransportWorldTransfer());
    CHECK(h.awaitsTransport(guid, 0));
    // Moves before it is there have the wire's offset again.
    h.setPosition(deck.x, deck.y, deck.z);
    h.sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    const auto sent = s.sent().of(Opcode::MSG_MOVE_HEARTBEAT);
    REQUIRE(!sent.empty());
    auto beat = sent.front();
    CHECK(beat.readPackedGuid() == Session::kPlayerGuid);
    CHECK((beat.readUInt32() & 0x200u) != 0);
    beat.readUInt16();
    beat.readUInt32();
    for (int i = 0; i < 4; ++i) beat.readFloat();
    CHECK(beat.readPackedGuid() == guid);
    glm::vec3 said;
    said.x = beat.readFloat();
    said.y = beat.readFloat();
    said.z = beat.readFloat();
    CHECK(closeTo(said, wire));
    // The ground says nothing of it meanwhile.
    h.updateM2TransportBoarding(deck + glm::vec3(0.0f, 0.0f, 100.0f));
    CHECK(h.hasPendingPlayerTransportWorldTransfer());
    // And others' offsets on it are read as its kind has them.
    const glm::vec3 kept = transports->serverToTransportLocal(guid, wire);
    CHECK(closeTo(kept, ship ? wire : wowee::core::coords::serverToCanonical(wire)));
    CHECK(closeTo(transports->transportLocalToServer(guid, kept), wire));

    transports->registerTransport(guid, 0, 9001, deck, 9001, 0, !ship, 0.4f);
    CHECK(closeTo(transports->serverToTransportLocal(guid, wire), kept));
    glm::vec3 world(0.0f);
    REQUIRE(h.completePlayerTransportWorldTransfer(guid, world));
    CHECK(h.getPlayerTransportGuid() == guid);
    CHECK(closeTo(h.getPlayerTransportOffset(), kept));
    CHECK(closeTo(world, transports->getPlayerWorldPosition(guid, kept)));
    CHECK(std::cos(h.getMovementInfo().orientation -
                   wowee::game::transport_local::worldYaw(*transports->transportFacing(guid), 1.0f)) ==
          Catch::Approx(1.0f).margin(0.001f));
    CHECK_FALSE(h.hasPendingPlayerTransportWorldTransfer());
}

// Its display says what it is; and the server says where the player is.
TEST_CASE("a transport's kind is its display's, before it is there", "[replay][transport]") {
    constexpr uint64_t kShip = 0x1FC0000000000007ull, kLift = 0xF12000001000000Aull;
    using wowee::game::transport_local::unregisteredIsWmo;
    CHECK(unregisteredIsWmo(kShip, std::nullopt));
    CHECK_FALSE(unregisteredIsWmo(kLift, std::nullopt));
    CHECK(unregisteredIsWmo(kLift, true));
    CHECK_FALSE(unregisteredIsWmo(kShip, false));
    Session s;
    auto* transports = s.handler().getTransportManager();
    REQUIRE(transports);
    transports->setDisplayIsWmo([](uint64_t guid) { return std::optional<bool>(guid == 0xF12000001000000Aull); });
    const glm::vec3 wire(3.0f, -7.0f, 2.0f);
    CHECK(closeTo(transports->serverToTransportLocal(kLift, wire), wire));
    CHECK(closeTo(transports->transportLocalToServer(kShip, wire), wowee::core::coords::canonicalToServer(wire)));

    auto& h = s.handler();
    h.awaitPlayerTransport(kShip, wire, 0.0f);
    UpdateObject::Spline off;
    off.objectType = ObjectType::PLAYER;
    off.moveFlags = 0;
    off.at = kAt;
    s.receive(UpdateObject().onSpline(Session::kPlayerGuid, {}, off));
    CHECK_FALSE(h.hasPendingPlayerTransportWorldTransfer());
    // Nor is a vehicle one.
    off.moveFlags = 0x200;
    off.transport = 0xF150006D50000003ull;
    s.receive(UpdateObject().onSpline(Session::kPlayerGuid, {}, off));
    CHECK_FALSE(h.hasPendingPlayerTransportWorldTransfer());
}

// T283 (tester's): 0x00740ba0. The flags were dropped: a pet given Levitate stayed on the ground.
TEST_CASE("a unit the server moves takes its movement flags", "[replay][blockspline]") {
    using wowee::game::MovementFlags;
    constexpr uint64_t kPet = 0xF140000001000002ull;
    constexpr auto flag = [](MovementFlags f) { return static_cast<uint32_t>(f); };
    Session s;
    UpdateObject::Spline sp;
    sp.moveFlags = 0;
    sp.at = kAt;
    const uint16_t hoverField = static_cast<uint16_t>(wowee::game::fieldIndex(UF::UNIT_FIELD_MAXHEALTHMODIFIER) + 1);
    s.receive(UpdateObject().onSpline(kPet, {{UF::UNIT_FIELD_HEALTH, 100}}, sp));
    const auto pet = s.handler().getEntityManager().getEntity(kPet);
    REQUIRE(pet);
    pet->setField(hoverField, 0x3f800000u);
    const auto says = [&](Opcode opcode) {
        wowee::network::Packet p(wowee::game::wireOpcode(opcode));
        p.writePackedGuid(kPet);
        s.receive(p);
        return pet->getReportedMoveFlags();
    };
    const float ground = pet->getZ();
    uint32_t told = 0, mask = 0;
    s.handler().setUnitMoveFlagsCallback([&](uint64_t guid, uint32_t set, uint32_t m) {
        CHECK(guid == kPet);
        told = set;
        mask = m;
    });
    // Up as a jump goes, and down to a yard over the ground.
    CHECK((says(Opcode::SMSG_SPLINE_MOVE_SET_HOVER) & flag(MovementFlags::HOVER)) != 0);
    CHECK(told == flag(MovementFlags::HOVER));
    CHECK(mask == flag(MovementFlags::HOVER));
    CHECK(pet->fallsInPlace());
    CHECK(pet->isAirborne());
    pet->updateMovement(0.412f);
    CHECK(pet->getZ() - ground == Catch::Approx(1.640f).margin(0.01f));
    pet->updateMovement(0.2f);
    pet->updateMovement(0.2f);
    CHECK(pet->getZ() - ground == Catch::Approx(1.0f).margin(0.001f));
    CHECK_FALSE(pet->fallsInPlace());
    CHECK((pet->getReportedMoveFlags() & flag(MovementFlags::FALLING)) == 0);
    // Said again it is nothing.
    says(Opcode::SMSG_SPLINE_MOVE_SET_HOVER);
    CHECK_FALSE(pet->fallsInPlace());
    // And a fall from rest.
    says(Opcode::SMSG_SPLINE_MOVE_UNSET_HOVER);
    CHECK_FALSE(pet->isAirborne());
    pet->updateMovement(0.2f);
    CHECK(pet->getZ() - ground == Catch::Approx(1.0f - 0.5f * 19.291105f * 0.04f).margin(0.01f));
    pet->updateMovement(0.5f);
    CHECK(pet->getZ() == Catch::Approx(ground).margin(0.001f));
    // The others.
    for (const auto& [on, off, bit] :
         {std::tuple{Opcode::SMSG_SPLINE_MOVE_FEATHER_FALL, Opcode::SMSG_SPLINE_MOVE_NORMAL_FALL,
                     flag(MovementFlags::FEATHER_FALL)},
          std::tuple{Opcode::SMSG_SPLINE_MOVE_WATER_WALK, Opcode::SMSG_SPLINE_MOVE_LAND_WALK,
                     flag(MovementFlags::WATER_WALK)},
          std::tuple{Opcode::SMSG_SPLINE_MOVE_GRAVITY_DISABLE, Opcode::SMSG_SPLINE_MOVE_GRAVITY_ENABLE,
                     flag(MovementFlags::LEVITATING)},
          std::tuple{Opcode::SMSG_SPLINE_MOVE_START_SWIM, Opcode::SMSG_SPLINE_MOVE_STOP_SWIM,
                     flag(MovementFlags::SWIMMING)},
          std::tuple{Opcode::SMSG_SPLINE_MOVE_SET_WALK_MODE, Opcode::SMSG_SPLINE_MOVE_SET_RUN_MODE,
                     flag(MovementFlags::WALKING)},
          std::tuple{Opcode::SMSG_SPLINE_MOVE_SET_FLYING, Opcode::SMSG_SPLINE_MOVE_UNSET_FLYING,
                     flag(MovementFlags::FLYING)},
          std::tuple{Opcode::SMSG_SPLINE_MOVE_ROOT, Opcode::SMSG_SPLINE_MOVE_UNROOT, 0x800u}}) {
        CHECK((says(on) & bit) != 0);
        CHECK((says(off) & bit) == 0);
    }
    // Rooted, its directions are let go (0x0098c4f0).
    pet->setMoveFlags(0x1u, 0x1u);
    CHECK((says(Opcode::SMSG_SPLINE_MOVE_ROOT) & 0x801u) == 0x800u);
    s.handler().setUnitMoveFlagsCallback({});
}

// A vehicle's passenger is in its seat.
TEST_CASE("a passenger of a vehicle is not flown by its block", "[replay][blockspline]") {
    Session s;
    const std::vector<glm::vec3> way = {glm::vec3(0), glm::vec3(10, 0, 2), glm::vec3(20, 0, 2), glm::vec3(30, 0, 2)};
    auto sp = spline(curve(way), way.back(), 0x2000, 1);
    sp.moveFlags = 0x08000201;
    sp.transport = 0xF150006D50000003ull;
    const auto e = make(s, sp);
    e->updateMovement(1.0f);
    CHECK(closeTo(placeOf(*e), kAt));
    CHECK_FALSE(e->isActivelyMoving());
}

// A unit is where it is, not where its path ends.
TEST_CASE("a unit on a long path is found where it is", "[replay][blockspline]") {
    Session s;
    std::vector<glm::vec3> way;
    for (int i = 0; i <= 100; ++i) way.push_back(kAt + glm::vec3(10.0f * static_cast<float>(i), 0, 0));
    const auto e = make(s, spline(curve(way), way.back(), 0x2000, 1));
    const glm::vec3 c = wowee::core::coords::serverToCanonical(kAt + glm::vec3(250, 0, 0));
    std::vector<std::shared_ptr<wowee::game::Entity>> found;
    s.handler().getEntityManager().getEntitiesNear(c.x, c.y, 30.0f, found);
    CHECK(found.size() == 1);
}

// P62: 0x00987d20's other half. A flyer that dies falls, and was let down at one pace.
TEST_CASE("a falling monster move falls", "[replay][blockspline]") {
    Session s;
    s.spawnPlayer(kFields);
    s.receive(UpdateObject().create(kWalker, ObjectType::UNIT, kFields));
    const auto e = s.handler().getEntityManager().getEntity(kWalker);
    REQUIRE(e);
    const glm::vec3 top = kAt + glm::vec3(0.0f, 0.0f, 40.0f);
    const glm::vec3 c = wowee::core::coords::serverToCanonical(top);
    e->setPosition(c.x, c.y, c.z, 0.0f);
    const auto fallenAt = [](float t) { return 9.645553f * t * t; };
    const float whole = std::sqrt(40.0f / 9.645553f);  // 2.0364 s
    // The server's own time for it, which the client's cap (28 yd/s) does not shorten.
    s.receive(monsterMove(kWalker, 5, top, kAt, static_cast<uint32_t>(whole * 1000.0f), 0x200));
    float t = 0.0f;
    for (int i = 0; i < 8; ++i) {
        e->updateMovement(0.2f);
        t += 0.2f;
        CHECK(placeOf(*e).z == Catch::Approx(top.z - fallenAt(t)).margin(0.1f));
    }
    // Down it lies at the end, and no longer on its way.
    e->updateMovement(whole - t + 0.05f);
    CHECK(closeTo(placeOf(*e), kAt));
    CHECK_FALSE(e->isActivelyMoving());

    // Faster than the cap lets a spline go, its end is the floor and the fall is over
    // there: 40 yards at 28 a second would be 1.43 s, a fall takes 2.04.
    e->setPosition(c.x, c.y, c.z, 0.0f);
    s.receive(monsterMove(kWalker, 6, top, kAt, 5000, 0x200));
    e->updateMovement(1.0f);
    CHECK(placeOf(*e).z == Catch::Approx(top.z - fallenAt(1.0f)).margin(0.1f));
    e->updateMovement(1.1f);
    CHECK(closeTo(placeOf(*e), kAt));
    CHECK_FALSE(e->isActivelyMoving());

    // With 0x800 beside it, it arcs and does not fall (0x0098ca00 asks that first).
    e->setPosition(c.x, c.y, c.z, 0.0f);
    s.receive(monsterMove(kWalker, 7, top, kAt, 4000, 0xa00, 0.0f));
    e->updateMovement(1.0f);
    CHECK(placeOf(*e).z == Catch::Approx(top.z - 10.0f).margin(0.2f));
    // And none without: at its pace along the path.
    e->setPosition(c.x, c.y, c.z, 0.0f);
    s.receive(monsterMove(kWalker, 8, top, kAt, 4000));
    e->updateMovement(1.0f);
    CHECK(placeOf(*e).z == Catch::Approx(top.z - 10.0f).margin(0.2f));
    // Backwards the same, terminal speed and all (0x00988220).
    for (const float d : {0.5f, 40.0f, 93.0f, 150.0f})
        CHECK(wowee::game::fallDistance(wowee::game::fallTime(d), 0.0f, false) == Catch::Approx(d).margin(0.01f));
}
