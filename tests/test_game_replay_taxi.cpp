// A 3.3.5 flight through GameHandler's own dispatch: the server's flag and its
// splines are the flight, and the end of each is reported. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "core/coordinates.hpp"
#include "game_replay.hpp"
#include "game/transport_manager.hpp"
#include "game/movement_handler.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::Opcode;
using wowee::game::UF;

namespace {

constexpr uint32_t kTaxiFlag = 0x00100000;  // UNIT_FLAG_TAXI_FLIGHT
constexpr uint32_t kDisableMove = 0x00000004;
// The border of the Plaguelands, as it lies on map 0 and on map 530.
const glm::vec3 kBorder0{3538.9f, -4506.4f, 120.0f};
const glm::vec3 kBorder530{6103.7f, -6987.5f, 137.0f};

void place(Session& s, const glm::vec3& server) {
    const glm::vec3 c = wowee::core::coords::serverToCanonical(server);
    s.handler().setPosition(c.x, c.y, c.z);
}

// The player's own create block, made where the server says: the harness's
// stands at the origin.
void spawn(Session& s, uint32_t unitFlags, uint32_t mount, const glm::vec3& server) {
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181},
                   {UF::UNIT_FIELD_MAXHEALTH, 181},
                   {UF::UNIT_FIELD_LEVEL, 80},
                   {UF::UNIT_FIELD_FLAGS, unitFlags},
                   {UF::UNIT_FIELD_MOUNTDISPLAYID, mount}});
    place(s, server);
}

// A flight's spline as the server writes it: FLYING alone, the points whole.
// FLYING | CATMULLROM is the same to the client (0x42000).
wowee::network::Packet flight(uint32_t splineId, const glm::vec3& from, const std::vector<glm::vec3>& points,
                              uint32_t durationMs, uint32_t flags = 0x2000, uint64_t guid = Session::kPlayerGuid) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_MONSTER_MOVE));
    p.writePackedGuid(guid);
    p.writeUInt8(0);
    for (float f : {from.x, from.y, from.z}) p.writeFloat(f);
    p.writeUInt32(splineId);
    p.writeUInt8(0);
    p.writeUInt32(flags);
    p.writeUInt32(durationMs);
    p.writeUInt32(static_cast<uint32_t>(points.size()));
    for (const auto& at : points)
        for (float f : {at.x, at.y, at.z}) p.writeFloat(f);
    return p;
}

wowee::network::Packet stop(uint32_t splineId, const glm::vec3& at) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_MONSTER_MOVE));
    p.writePackedGuid(Session::kPlayerGuid);
    p.writeUInt8(0);
    for (float f : {at.x, at.y, at.z}) p.writeFloat(f);
    p.writeUInt32(splineId);
    p.writeUInt8(1);
    return p;
}

wowee::network::Packet newWorld(uint32_t map, const glm::vec3& at) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_NEW_WORLD));
    p.writeUInt32(map);
    for (float f : {at.x, at.y, at.z}) p.writeFloat(f);
    p.writeFloat(0.4f);
    return p;
}

int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(wowee::core::steadyNow().time_since_epoch()).count();
}

// The frames that pass: the units moved on, the spline asked after.
void frames(Session& s, int milliseconds) {
    auto* mh = s.handler().getMovementHandler();
    for (int at = 0; at < milliseconds; at += 10) {
        wowee::core::advanceTestClock(0.010);
        if (auto self = s.handler().getEntityManager().getEntity(Session::kPlayerGuid)) self->updateMovement(0.01f);
        mh->updateMoverSplineDone();
        mh->updateClientTaxi(0.01f);
    }
}

struct Done {
    uint32_t id = 0;
    uint32_t flags = 0;
    glm::vec3 at{0.0f};
};

// CMSG_MOVE_SPLINE_DONE as 0x0071f210 writes it: the guid, the movement
// (flags, flags2, time, place, facing, fall time) and the id of the spline.
std::vector<Done> reports(const Session& s) {
    std::vector<Done> out;
    for (auto p : s.sent().of(Opcode::CMSG_MOVE_SPLINE_DONE)) {
        Done d;
        p.readPackedGuid();
        d.flags = p.readUInt32();
        p.readUInt16();
        p.readUInt32();
        d.at.x = p.readFloat();
        d.at.y = p.readFloat();
        d.at.z = p.readFloat();
        p.readFloat();
        p.readUInt32();
        d.id = p.readUInt32();
        out.push_back(d);
    }
    return out;
}

bool closeTo(const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b) < 0.05f; }

glm::vec3 standsAt(Session& s) {
    const auto& m = s.handler().getMovementInfo();
    return wowee::core::coords::canonicalToServer(glm::vec3(m.x, m.y, m.z));
}

}  // namespace

TEST_CASE("a flight to another map reports its first leg and flies its second", "[replay][taxi]") {
    Session s;
    const glm::vec3 from = kBorder0 - glm::vec3(1.0f, 0.0f, 0.0f);
    spawn(s, 0, 0, from);
    uint32_t mount = 0;
    s.handler().setMountCallback([&](uint32_t display) { mount = display; });

    // Player::ActivateTaxiPathTo: the mount and the flags, then the spline to
    // the border - and not on along the nodes of the other map.
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_FLAGS, kTaxiFlag | kDisableMove},
                                                           {UF::UNIT_FIELD_MOUNTDISPLAYID, 1149}}));
    s.receive(flight(71, from, {kBorder0}, 20));
    frames(s, 10);
    CHECK(s.handler().isOnTaxiFlight());
    CHECK(mount == 1149);
    s.handler().sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    s.handler().sendMovement(Opcode::MSG_MOVE_STOP);
    CHECK(s.sent().of(Opcode::MSG_MOVE_HEARTBEAT).empty());
    CHECK(s.sent().of(Opcode::MSG_MOVE_STOP).empty());

    frames(s, 80);
    auto done = reports(s);
    REQUIRE(done.size() == 1);
    CHECK(done[0].id == 71);
    CHECK((done[0].flags & 0xffu) == 0);
    CHECK(closeTo(done[0].at, kBorder0));
    // It waits where the spline ended, still the moving of the server.
    CHECK(s.handler().isOnTaxiFlight());
    CHECK(closeTo(standsAt(s), kBorder0));

    // The other map: the flight of the old one is over, the new player has its
    // flag and its spline.
    s.receive(newWorld(530, kBorder530));
    CHECK_FALSE(s.handler().isOnTaxiFlight());
    CHECK(s.handler().getCurrentMapId() == 530);
    CHECK(mount == 0);
    spawn(s, kTaxiFlag | kDisableMove, 1149, kBorder530);
    frames(s, 10);
    CHECK(s.handler().isOnTaxiFlight());
    CHECK(mount == 1149);
    const glm::vec3 landing = kBorder530 + glm::vec3(0.0f, 1.0f, 0.0f);
    s.receive(flight(72, kBorder530, {landing}, 20));
    frames(s, 80);
    done = reports(s);
    REQUIRE(done.size() == 2);
    CHECK(done[1].id == 72);
    CHECK(closeTo(done[1].at, landing));
    CHECK(s.handler().isOnTaxiFlight());

    // Down: the flag and the mount taken back.
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS, 0}, {UF::UNIT_FIELD_MOUNTDISPLAYID, 0}}));
    frames(s, 10);
    CHECK_FALSE(s.handler().isOnTaxiFlight());
    CHECK(mount == 0);
    CHECK(closeTo(standsAt(s), landing));
    s.handler().sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    CHECK(s.sent().of(Opcode::MSG_MOVE_START_FORWARD).size() == 1);
    s.handler().setMountCallback({});
}

TEST_CASE("a spline still under way as the flight's flag goes is flown to its end", "[replay][taxi]") {
    Session s;
    spawn(s, kTaxiFlag, 1149, kBorder0);
    frames(s, 10);
    const glm::vec3 landing = kBorder0 + glm::vec3(4.0f, 0.0f, 0.0f);
    s.receive(flight(9, kBorder0, {landing}, 100));
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS, 0}, {UF::UNIT_FIELD_MOUNTDISPLAYID, 0}}));
    frames(s, 20);
    CHECK(s.handler().isOnTaxiFlight());
    CHECK(reports(s).empty());
    frames(s, 150);
    CHECK_FALSE(s.handler().isOnTaxiFlight());
    REQUIRE(reports(s).size() == 1);
    CHECK(closeTo(reports(s)[0].at, landing));
}

TEST_CASE("a flight the server breaks off is reported with the stop's id", "[replay][taxi]") {
    Session s;
    spawn(s, kTaxiFlag, 1149, kBorder0);
    frames(s, 10);
    s.receive(flight(5, kBorder0, {kBorder0 + glm::vec3(500.0f, 0.0f, 0.0f)}, 10000));
    frames(s, 20);
    const glm::vec3 at = kBorder0 + glm::vec3(3.0f, 0.0f, 0.0f);
    s.receive(stop(6, at));
    frames(s, 10);
    const auto done = reports(s);
    REQUIRE(done.size() == 1);
    CHECK(done[0].id == 6);
    CHECK(closeTo(done[0].at, at));
}

TEST_CASE("a teleport in flight drops the spline and the flight with it", "[replay][taxi]") {
    Session s;
    spawn(s, kTaxiFlag, 1149, kBorder0);
    frames(s, 10);
    s.receive(flight(5, kBorder0, {kBorder0 + glm::vec3(2.0f, 0.0f, 0.0f)}, 40));
    frames(s, 10);
    const glm::vec3 away = kBorder0 + glm::vec3(-300.0f, 40.0f, 0.0f);
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::MSG_MOVE_TELEPORT_ACK));
    p.writePackedGuid(Session::kPlayerGuid);
    p.writeUInt32(1);
    p.writeUInt32(0);
    p.writeUInt16(0);
    p.writeUInt32(0);
    for (float f : {away.x, away.y, away.z}) p.writeFloat(f);
    p.writeFloat(0.0f);
    p.writeUInt32(0);
    s.receive(p);
    CHECK(closeTo(standsAt(s), away));
    // The flag is taken back an update later.
    frames(s, 60);
    CHECK(closeTo(standsAt(s), away));
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS, 0}, {UF::UNIT_FIELD_MOUNTDISPLAYID, 0}}));
    frames(s, 10);
    CHECK_FALSE(s.handler().isOnTaxiFlight());
    CHECK(closeTo(standsAt(s), away));
    CHECK(reports(s).empty());
}

TEST_CASE("a player whose flight has no spline stays where it is", "[replay][taxi]") {
    Session s;
    spawn(s, kTaxiFlag, 1149, kBorder0);
    frames(s, 10);
    CHECK(s.handler().isOnTaxiFlight());
    const glm::vec3 before = standsAt(s);
    frames(s, 100);
    CHECK(closeTo(standsAt(s), before));
    CHECK(s.handler().isOnTaxiFlight());
    CHECK(reports(s).empty());
}

// Logged out in the air: the server sets the character down, and the next one
// in stands where the server says.
TEST_CASE("a flight does not outlive the world it was in", "[replay][taxi]") {
    Session s;
    s.setConnected(true);
    spawn(s, kTaxiFlag, 1149, kBorder0);
    frames(s, 10);
    s.receive(flight(5, kBorder0, {kBorder0 + glm::vec3(500.0f, 0.0f, 0.0f)}, 10000));
    frames(s, 20);
    REQUIRE(s.handler().isOnTaxiFlight());
    REQUIRE(s.handler().leaveWorldForCharacterList());
    CHECK_FALSE(s.handler().isOnTaxiFlight());
    CHECK_FALSE(s.handler().onMoveSpline(Session::kPlayerGuid));
    wowee::game::GameHandlerTestAccess::enterWorld(s.handler());
    const glm::vec3 at = kBorder0 + glm::vec3(-40.0f, 0.0f, 0.0f);
    spawn(s, 0, 0, at);
    frames(s, 30);
    CHECK_FALSE(s.handler().isOnTaxiFlight());
    CHECK(closeTo(standsAt(s), at));
    CHECK(reports(s).empty());
}

namespace {

float offTheWay(const glm::vec3& p, const std::vector<glm::vec3>& way) {
    float best = 1.0e9f;
    for (size_t i = 1; i < way.size(); ++i) {
        const glm::vec3 d = way[i] - way[i - 1];
        const float t = glm::clamp(glm::dot(p - way[i - 1], d) / glm::dot(d, d), 0.0f, 1.0f);
        best = std::min(best, glm::length(p - (way[i - 1] + d * t)));
    }
    return best;
}

glm::vec3 placeOf(const wowee::game::Entity& e) {
    return wowee::core::coords::canonicalToServer(glm::vec3(e.getX(), e.getY(), e.getZ()));
}

}  // namespace

// P32: the points of a FLYING spline read as packed lay all over the 256 yards
// round the middle, and every flight flew a tangle of them.
TEST_CASE("a flight of many points flies from one to the next", "[replay][taxi]") {
    const uint32_t flags = GENERATE(0x2000u, 0x42000u);
    INFO("flags " << std::hex << flags);
    Session s;
    uint32_t mount = 0;
    s.handler().setMountCallback([&](uint32_t display) { mount = display; });
    const glm::vec3 from = kBorder0 - glm::vec3(0.0f, 0.0f, 5.0f);
    spawn(s, 0, 0, from);
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_FLAGS, kTaxiFlag | kDisableMove},
                                                           {UF::UNIT_FIELD_MOUNTDISPLAYID, 1149}}));
    const std::vector<glm::vec3> first = {from + glm::vec3(3.0f, 0.0f, 2.0f), from + glm::vec3(6.0f, 1.0f, 4.0f),
                                          from + glm::vec3(9.0f, 3.0f, 4.0f), from + glm::vec3(12.0f, 6.0f, 4.0f)};
    s.receive(flight(21, from, first, 400, flags));
    std::vector<glm::vec3> way = first;
    way.insert(way.begin(), from);
    float worst = 0.0f, farthest = 0.0f;
    for (int i = 0; i < 60; ++i) {
        frames(s, 10);
        const auto self = s.handler().getEntityManager().getEntity(Session::kPlayerGuid);
        REQUIRE(self);
        worst = std::max(worst, offTheWay(placeOf(*self), way));
        farthest = std::max(farthest, glm::length(placeOf(*self) - from));
    }
    CHECK(worst < 0.5f);
    CHECK(farthest < 15.0f);
    // The end, reported where the server has it; a stop-over: on by the next
    // spline, the flag and the mount kept.
    REQUIRE(reports(s).size() == 1);
    CHECK(reports(s)[0].id == 21);
    CHECK(closeTo(reports(s)[0].at, first.back()));
    CHECK(s.handler().isOnTaxiFlight());
    CHECK(mount == 1149);
    const std::vector<glm::vec3> second = {first.back() + glm::vec3(0.0f, 3.0f, -1.0f),
                                           first.back() + glm::vec3(-2.0f, 6.0f, -2.0f),
                                           first.back() + glm::vec3(-4.0f, 8.0f, -3.0f)};
    s.receive(flight(22, first.back(), second, 300, flags));
    way = second;
    way.insert(way.begin(), first.back());
    worst = 0.0f;
    for (int i = 0; i < 50; ++i) {
        frames(s, 10);
        worst = std::max(worst, offTheWay(standsAt(s), way));
    }
    CHECK(worst < 0.5f);
    REQUIRE(reports(s).size() == 2);
    CHECK(closeTo(reports(s)[1].at, second.back()));
    // Down: dismounted, the place kept, the keys the player's again.
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS, 0}, {UF::UNIT_FIELD_MOUNTDISPLAYID, 0}}));
    frames(s, 20);
    CHECK_FALSE(s.handler().isOnTaxiFlight());
    CHECK(mount == 0);
    CHECK(closeTo(standsAt(s), second.back()));
    s.handler().sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    REQUIRE(s.sent().of(Opcode::MSG_MOVE_START_FORWARD).size() == 1);
    s.handler().setMountCallback({});
}

// The server dismounts the player as its own reckoning of the spline ends, a
// moment before the client's: flown to its end, then down.
TEST_CASE("a flight of many points is landed where it ends though the flag goes first", "[replay][taxi]") {
    Session s;
    spawn(s, kTaxiFlag, 1149, kBorder0);
    frames(s, 10);
    const glm::vec3 landing = kBorder0 + glm::vec3(6.0f, 2.0f, -2.0f);
    s.receive(flight(31, kBorder0, {kBorder0 + glm::vec3(2.0f, 0.0f, 1.0f), kBorder0 + glm::vec3(4.0f, 1.0f, 0.0f), landing}, 250));
    frames(s, 150);
    s.receive(UpdateObject().values(Session::kPlayerGuid,
                                    {{UF::UNIT_FIELD_FLAGS, 0}, {UF::UNIT_FIELD_MOUNTDISPLAYID, 0}}));
    frames(s, 250);
    CHECK_FALSE(s.handler().isOnTaxiFlight());
    REQUIRE(reports(s).size() == 1);
    CHECK(closeTo(reports(s)[0].at, landing));
    CHECK(closeTo(standsAt(s), landing));
}

// A creature's is read the same.
TEST_CASE("a flying creature flies from point to point", "[replay][taxi]") {
    Session s;
    spawn(s, 0, 0, kBorder0);
    constexpr uint64_t kDrake = 0xF130005490000042ull;
    s.receive(UpdateObject().create(kDrake, wowee::game::ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}}));
    const auto drake = s.handler().getEntityManager().getEntity(kDrake);
    REQUIRE(drake);
    const glm::vec3 from = kBorder0 + glm::vec3(30.0f, 0.0f, 0.0f);
    const glm::vec3 c = wowee::core::coords::serverToCanonical(from);
    drake->setPosition(c.x, c.y, c.z, 0.0f);
    const std::vector<glm::vec3> points = {from + glm::vec3(4.0f, 0.0f, 2.0f), from + glm::vec3(8.0f, 4.0f, 3.0f),
                                           from + glm::vec3(8.0f, 9.0f, 5.0f)};
    s.receive(flight(41, from, points, 400, 0x2000, kDrake));
    std::vector<glm::vec3> way = points;
    way.insert(way.begin(), from);
    float worst = 0.0f;
    for (int i = 0; i < 60 && drake->isActivelyMoving(); ++i) {
        worst = std::max(worst, offTheWay(placeOf(*drake), way));
        drake->updateMovement(0.01f);
    }
    CHECK(worst < 0.6f);
    CHECK(closeTo(wowee::core::coords::canonicalToServer(
                      glm::vec3(drake->getLatestX(), drake->getLatestY(), drake->getLatestZ())),
                  points.back()));
}

// 0x0071ef20: none of the mover's own movement goes out while the spline's
// flags lack 0x400, which the client sets as it ends; a charge is one as well.
TEST_CASE("while a spline carries the player nothing of its own movement goes out", "[replay][taxi]") {
    Session s;
    spawn(s, 0, 0, kBorder0);
    frames(s, 10);
    auto* mh = s.handler().getMovementHandler();
    const glm::vec3 at = kBorder0 + glm::vec3(20.0f, 0.0f, 0.0f);
    s.receive(flight(11, kBorder0, {at}, 400));
    REQUIRE(mh->onMoveSpline(Session::kPlayerGuid));
    CHECK_FALSE(s.handler().isOnTaxiFlight());
    mh->sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    mh->sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    mh->sendMovement(Opcode::MSG_MOVE_STOP);
    CHECK(s.sent().of(Opcode::MSG_MOVE_START_FORWARD).empty());
    CHECK(s.sent().of(Opcode::MSG_MOVE_HEARTBEAT).empty());
    CHECK(s.sent().of(Opcode::MSG_MOVE_STOP).empty());
    for (int i = 0; i < 40 && mh->onMoveSpline(Session::kPlayerGuid); ++i) frames(s, 50);
    frames(s, 10);
    // Over, it is reported and the rest goes out again.
    REQUIRE(reports(s).size() == 1);
    CHECK(reports(s)[0].id == 11);
    mh->sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    CHECK(s.sent().of(Opcode::MSG_MOVE_HEARTBEAT).size() == 1);
    // One the server sends with the mark already set holds nothing back.
    s.receive(flight(12, at, {kBorder0}, 400, 0x42400));
    mh->sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    CHECK(s.sent().of(Opcode::MSG_MOVE_HEARTBEAT).size() == 2);
    // 0x009904e0 lets swimming by.
    s.receive(flight(13, at, {kBorder0}, 400));
    REQUIRE(mh->onMoveSpline(Session::kPlayerGuid));
    mh->sendMovement(Opcode::MSG_MOVE_START_SWIM);
    CHECK(s.sent().of(Opcode::MSG_MOVE_START_SWIM).size() == 1);
}

// P45. The client's spline goes by the clock from the moment it is started
// (0x0098ca00: now less +0x24). A map's load was counted into the spline that
// came in it: the flight was four seconds ahead of the server from the border
// on, and went on past its end for as long.
TEST_CASE("a spline that comes after a load begins at its beginning", "[replay][taxi]") {
    using namespace std::chrono_literals;
    Session s;
    s.setConnected(true);
    spawn(s, kTaxiFlag, 1149, kBorder530);
    auto& gh = s.handler();
    const auto self = gh.getEntityManager().getEntity(Session::kPlayerGuid);
    REQUIRE(self);
    // The last update before the load, five seconds ago.
    gh.setUnitClock(wowee::core::steadyNow() - 5s);
    gh.update(0.016f);
    std::vector<glm::vec3> way;
    for (int i = 1; i <= 100; ++i) way.push_back(kBorder530 + glm::vec3(10.0f * static_cast<float>(i), 0.0f, 0.0f));
    s.receive(flight(81, kBorder530, way, 40000));
    const auto started = wowee::core::steadyNow();
    // The frame after it, as long as the load.
    gh.setUnitClock(started);
    gh.update(0.016f);
    CHECK(glm::length(placeOf(*self) - kBorder530) < 1.0f);
    // And from there by the clock, whatever the update is told of its length.
    gh.setUnitClock(started + 1s);
    gh.update(1.0f / 60.0f);
    CHECK(glm::length(placeOf(*self) - kBorder530) == Catch::Approx(25.0f).margin(2.0f));
    gh.setUnitClock(started + 2s);
    gh.update(1.0f / 60.0f);
    CHECK(glm::length(placeOf(*self) - kBorder530) == Catch::Approx(50.0f).margin(2.0f));

    // A creature's the same, and one by its movement flags.
    constexpr uint64_t kDrake = 0xF130005490000042ull;
    s.receive(UpdateObject().create(kDrake, wowee::game::ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}}));
    const auto drake = gh.getEntityManager().getEntity(kDrake);
    REQUIRE(drake);
    const glm::vec3 c = wowee::core::coords::serverToCanonical(kBorder530);
    drake->setPosition(c.x, c.y, c.z, 0.0f);
    s.receive(flight(82, kBorder530, {kBorder530 + glm::vec3(100.0f, 0.0f, 0.0f)}, 10000, 0x0, kDrake));
    // The units stand as they were three seconds before (the clock is put back).
    gh.setUnitClock(wowee::core::steadyNow() + 2s);
    gh.update(0.016f);
    CHECK(glm::length(placeOf(*drake) - kBorder530) < 22.0f);
}

// P58. Logged in in flight, the spline waited behind the world's load and began as it was
// handled: six seconds behind the server, off its mount long before the landing place. The
// client has its world before it asks for the character (0x00405540, then 0x006b1790).
TEST_CASE("a spline that waited behind a load is as far as the time it waited", "[replay][taxi]") {
    using namespace std::chrono_literals;
    Session s;
    s.setConnected(true);
    spawn(s, kTaxiFlag, 1149, kBorder530);
    auto& gh = s.handler();
    const auto self = gh.getEntityManager().getEntity(Session::kPlayerGuid);
    REQUIRE(self);
    const auto handled = wowee::core::steadyNow();
    gh.setUnitClock(handled);
    gh.update(0.016f);
    std::vector<glm::vec3> way;
    for (int i = 1; i <= 100; ++i) way.push_back(kBorder530 + glm::vec3(10.0f * static_cast<float>(i), 0.0f, 0.0f));
    const auto readAgo = [](wowee::network::Packet p, int64_t ms) {
        p.setReceivedMs(steadyMs() - ms);
        return p;
    };
    s.receive(readAgo(flight(83, kBorder530, way, 40000), 6000));  // 25 yd/s
    gh.setUnitClock(wowee::core::steadyNow());
    gh.update(0.016f);
    CHECK(glm::length(placeOf(*self) - kBorder530) == Catch::Approx(150.0f).margin(3.0f));
    gh.setUnitClock(handled + 2s);
    gh.update(0.016f);
    CHECK(glm::length(placeOf(*self) - kBorder530) == Catch::Approx(200.0f).margin(3.0f));
    CHECK(gh.onMoveSpline(Session::kPlayerGuid));

    // One whose time went by behind it is at its end, and over.
    constexpr uint64_t kDrake = 0xF130005490000042ull;
    s.receive(UpdateObject().create(kDrake, wowee::game::ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}}));
    const auto drake = gh.getEntityManager().getEntity(kDrake);
    REQUIRE(drake);
    const glm::vec3 c = wowee::core::coords::serverToCanonical(kBorder530);
    drake->setPosition(c.x, c.y, c.z, 0.0f);
    const glm::vec3 end = kBorder530 + glm::vec3(30.0f, 0.0f, 0.0f);
    s.receive(readAgo(flight(84, kBorder530, {end}, 3000, 0x0, kDrake), 4000));
    CHECK_FALSE(gh.onMoveSpline(kDrake));
    drake->updateMovement(0.01f);
    CHECK(closeTo(placeOf(*drake), end));
}

// P58, run 10: the answers the server has in the same breath are said from where the spline has
// the player, not from where the last frame left it. CMSG_MOVE_SET_COLLISION_HGT_ACK went out
// with the place of before the load, and set the player back there.
TEST_CASE("a forced change is answered from where the spline has the player now", "[replay][taxi]") {
    using namespace std::chrono_literals;
    Session s;
    s.setConnected(true);
    spawn(s, kTaxiFlag, 1149, kBorder530);
    auto& gh = s.handler();
    const auto height = [&s](uint32_t counter) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_MOVE_SET_COLLISION_HGT));
        p.writePackedGuid(Session::kPlayerGuid);
        p.writeUInt32(counter);
        p.writeFloat(2.0f);
        s.receive(p);
        auto sent = s.sent().of(Opcode::CMSG_MOVE_SET_COLLISION_HGT_ACK);
        REQUIRE(sent.size() == counter);
        auto& ack = sent.back();
        REQUIRE(ack.readPackedGuid() == Session::kPlayerGuid);
        REQUIRE(ack.readUInt32() == counter);
        const uint32_t flags = ack.readUInt32();
        CHECK((flags & 0x200u) == 0);
        ack.readUInt16();
        ack.readUInt32();
        glm::vec3 at;
        at.x = ack.readFloat();
        at.y = ack.readFloat();
        at.z = ack.readFloat();
        return at;
    };
    const auto handled = wowee::core::steadyNow();
    gh.setUnitClock(handled);
    gh.update(0.016f);
    std::vector<glm::vec3> way;
    for (int i = 1; i <= 100; ++i) way.push_back(kBorder530 + glm::vec3(10.0f * static_cast<float>(i), 0.0f, 0.0f));
    auto p = flight(91, kBorder530, way, 40000);  // 25 yd/s
    p.setReceivedMs(steadyMs() - 6000);
    s.receive(p);
    // Before the next frame has moved anybody on: six seconds along it.
    CHECK(glm::length(height(1) - kBorder530) == Catch::Approx(150.0f).margin(5.0f));
    CHECK(glm::length(height(2) - kBorder530) == Catch::Approx(150.0f).margin(5.0f));
    // The frames go on from there and count nothing twice.
    gh.setUnitClock(handled + 2s);
    gh.update(0.016f);
    const auto self = gh.getEntityManager().getEntity(Session::kPlayerGuid);
    REQUIRE(self);
    CHECK(glm::length(placeOf(*self) - kBorder530) == Catch::Approx(200.0f).margin(5.0f));

    // One that ran out behind the load: from its end.
    auto q = flight(92, kBorder530, way, 40000);
    q.setReceivedMs(steadyMs() - 60000);
    s.receive(q);
    CHECK(closeTo(height(3), way.back()));
}

TEST_CASE("an aura's time counts from when its packet came", "[replay][taxi]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 10}, {UF::UNIT_FIELD_MAXHEALTH, 10}});
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_AURA_UPDATE));
    p.writePackedGuid(Session::kPlayerGuid);
    p.writeUInt8(0);
    p.writeUInt32(20217);
    p.writeUInt8(0x20 | 0x10 | 0x08 | 0x01);
    p.writeUInt8(80);
    p.writeUInt8(0);
    p.writeUInt32(60000);
    p.writeUInt32(30000);
    p.setReceivedMs(steadyMs() - 6000);
    s.receive(p);
    const auto& auras = s.handler().getPlayerAuras();
    REQUIRE_FALSE(auras.empty());
    // Counted by the real clock, as the interface counts it.
    const auto left = auras[0].getRemainingMs(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()));
    CHECK(left <= 24000);
    CHECK(left > 22000);
}

TEST_CASE("a flight stays where its spline ends until the server takes the flag", "[replay][taxi]") {
    Session s;
    spawn(s, kTaxiFlag, 1149, kBorder0);
    frames(s, 10);
    const glm::vec3 landing = kBorder0 + glm::vec3(40.0f, 0.0f, 0.0f);
    s.receive(flight(91, kBorder0, {landing}, 1000));
    const auto self = s.handler().getEntityManager().getEntity(Session::kPlayerGuid);
    REQUIRE(self);
    auto* mh = s.handler().getMovementHandler();
    float farthest = 0.0f;
    for (int i = 0; i < 30; ++i) {
        self->updateMovement(0.1f);
        mh->updateClientTaxi(0.1f);
        farthest = std::max(farthest, glm::length(standsAt(s) - kBorder0));
    }
    CHECK(farthest < 40.1f);
    CHECK(closeTo(standsAt(s), landing));
    CHECK(s.handler().isOnTaxiFlight());
}

// P46: a fall the player was in as the flight's spline came (put into the air
// at a border, on the far side of a load) went on after the flight from where
// and when it had begun: forty yards over the landing place, and down.
TEST_CASE("a spline ends the fall it finds the player in and begins one at its end", "[replay][taxi]") {
    Session s;
    spawn(s, kTaxiFlag, 1149, kBorder0);
    int over = 0, placed = 0;
    s.handler().setFallEndsCallback([&] { ++over; });
    s.handler().setPlacedFallCallback([&] { ++placed; });
    frames(s, 10);
    constexpr uint32_t kFallFlags = 0x3000;
    s.handler().getMovementHandler()->startFall(0);
    s.handler().getMovementHandler()->setFallingFar();
    REQUIRE((s.handler().getMovementInfo().flags & kFallFlags) == kFallFlags);
    s.receive(flight(101, kBorder0, {kBorder0 + glm::vec3(4.0f, 0.0f, 0.0f)}, 100));
    CHECK(over == 1);
    CHECK(placed == 0);
    CHECK((s.handler().getMovementInfo().flags & kFallFlags) == 0);
    // One sent while another runs ends nothing more (0x006ed7e0).
    s.receive(flight(102, kBorder0, {kBorder0 + glm::vec3(5.0f, 0.0f, 0.0f)}, 100));
    CHECK(over == 1);
    for (int i = 0; i < 40 && placed == 0; ++i) frames(s, 10);
    CHECK(placed == 1);
    CHECK(reports(s).size() == 1);
    // A stop: over in any case, and a fall from where it stands.
    s.receive(flight(103, kBorder0, {kBorder0 + glm::vec3(90.0f, 0.0f, 0.0f)}, 10000));
    CHECK(over == 2);
    s.receive(stop(104, kBorder0));
    frames(s, 10);
    CHECK(over == 3);
    CHECK(placed == 2);
    // Somebody else's says nothing of the player.
    constexpr uint64_t kDrake = 0xF130005490000042ull;
    s.receive(UpdateObject().create(kDrake, wowee::game::ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}}));
    s.receive(flight(105, kBorder0, {kBorder0 + glm::vec3(4.0f, 0.0f, 0.0f)}, 20, 0x2000, kDrake));
    frames(s, 60);
    CHECK(over == 3);
    CHECK(placed == 2);
    s.handler().setFallEndsCallback({});
    s.handler().setPlacedFallCallback({});
}

namespace {

struct Walker {
    Walker() {
        if (!ImGui::GetCurrentContext()) context = ImGui::CreateContext();
        controller.setFollowTarget(&at);
    }
    ~Walker() {
        if (context) ImGui::DestroyContext(context);
    }
    void run(float seconds) {
        for (float t = 0.0f; t < seconds; t += 0.02f) controller.update(0.02f);
    }
    ImGuiContext* context = nullptr;
    wowee::rendering::Camera camera;
    wowee::rendering::CameraController controller{&camera};
    glm::vec3 at{0.0f};
};

}  // namespace

TEST_CASE("a fall does not outlive the flight it was taken from", "[taxi][camera]") {
    // Put into the air at the border, where it falls for a second.
    const auto fall = [](Walker& w) {
        w.at = glm::vec3(-4456.97f, 3447.56f, 147.191f);
        w.controller.teleportTo(w.at);
        w.run(1.04f);
        REQUIRE(w.at.z < 140.0f);
        REQUIRE(w.at.z > 120.0f);
    };
    const glm::vec3 landing(-5287.77f, 2327.91f, 81.878f);
    const auto carry = [&](Walker& w) {
        w.controller.setExternalFollow(true);
        w.run(0.1f);
        w.at = landing;
        w.run(0.1f);
        w.controller.setExternalFollow(false);
    };
    SECTION("taken by the spline") {
        Walker w;
        fall(w);
        // Falling far, as the Fall animation goes by it: over with it.
        REQUIRE(w.controller.isFallingFar());
        w.controller.endFall();
        CHECK_FALSE(w.controller.isFallingFar());
        carry(w);
        CHECK_FALSE(w.controller.isFallingFar());
        w.run(0.1f);
        CHECK(w.at.z == Catch::Approx(landing.z).margin(0.01f));
        CHECK(w.controller.isGrounded());
        // The fall from the spline's end is its own.
        w.controller.queuePlacedFall();
        w.run(0.2f);
        CHECK(w.at.z < landing.z);
        CHECK(w.at.z > landing.z - 1.0f);
    }
    SECTION("put elsewhere by anything else") {
        Walker w;
        fall(w);
        carry(w);
        w.run(0.1f);
        CHECK(w.at.z <= landing.z);
        CHECK(w.at.z > landing.z - 1.0f);
    }
    SECTION("the keys moving another unit from here") {
        Walker w;
        fall(w);
        w.controller.newMover();
        w.at = landing + glm::vec3(0.0f, 0.0f, 0.0f);
        w.run(0.1f);
        CHECK(w.at.z <= landing.z);
        CHECK(w.at.z > landing.z - 1.0f);
    }
}

// U9. 0x007a55e0: the gather fails in the box of a building that is not loaded
// yet, a game object's too, and the unit there is not moved: the platform of the
// Eye of Eternity on a slow disk.
TEST_CASE("nothing falls where a game object is still to be loaded", "[camera]") {
    namespace gc = wowee::core::ground_coming;
    const glm::vec3 platform(754.35f, 1300.87f, 256.25f);
    const gc::Box box{glm::vec3(-143.5f, -124.1f, -51.1f), glm::vec3(143.2f, 122.3f, 54.8f)};
    const glm::vec3 place(728.0f, 1327.0f, 267.5f);
    Walker w;
    bool loaded = false;
    int asked = 0;
    w.controller.setObjectComing([&](const glm::vec3& lo, const glm::vec3& hi, bool) {
        ++asked;
        CHECK(lo.z < w.at.z);
        CHECK(hi.z > w.at.z);
        return !loaded && gc::reaches(box, platform, 1.0f, lo, hi);
    });
    std::vector<uint32_t> sent;
    w.controller.setMovementCallback([&](uint32_t opcode) { sent.push_back(opcode); });
    w.at = place;
    w.controller.teleportTo(w.at);
    w.run(5.0f);
    CHECK(asked > 0);
    CHECK(w.at == place);
    CHECK(w.controller.heldForObject());
    CHECK(w.controller.followTargetOverUnstreamedGround());
    CHECK(sent.empty());
    // There, or not: it is moved as before.
    loaded = true;
    w.run(0.5f);
    CHECK_FALSE(w.controller.heldForObject());
    CHECK(w.at.z < place.z - 1.0f);

    // Nothing is waited for for ever, and none of it outside the box.
    loaded = false;
    Walker high;
    high.controller.setObjectComing([&](const glm::vec3& lo, const glm::vec3& hi, bool) {
        return gc::reaches(box, platform, 1.0f, lo, hi);
    });
    high.at = place + glm::vec3(0.0f, 0.0f, 80.0f);
    high.controller.teleportTo(high.at);
    high.run(0.5f);
    CHECK(high.at.z < place.z + 79.0f);
    gc::Hold hold;
    float seconds = 0.0f;
    while (hold.step(true, 0.5f)) seconds += 0.5f;
    CHECK(seconds == gc::kHoldSeconds);
    CHECK_FALSE(hold.step(false, 0.5f));
    CHECK(hold.step(true, 0.5f));
    CHECK_FALSE(gc::reaches(gc::Box{}, platform, 1.0f, place, place));
    CHECK(gc::isBuilding("world\\wmo\\A.WMO"));
    CHECK_FALSE(gc::isBuilding("world\\a.m2"));
}

// P44. 0x00606f90: the yaw is off the facing on a flight path, and as the
// flight begins it eases behind the unit.
TEST_CASE("the camera turns with the flight and begins behind it", "[taxi][camera]") {
    Walker w;
    auto& cc = w.controller;
    const auto off = [&] { return std::remainder(cc.getYaw() - cc.getFacingYaw(), 360.0f); };
    cc.faceAfterTeleport(10.0f);
    cc.turnWithVehicle(60.0f);
    cc.setExternalFollow(true);
    cc.faceInFlight(40.0f);
    CHECK(cc.getFacingYaw() == Catch::Approx(40.0f));
    CHECK(cc.getYaw() == Catch::Approx(100.0f));
    // The short way round.
    cc.faceInFlight(-170.0f);
    cc.faceInFlight(170.0f);
    CHECK(off() == Catch::Approx(60.0f).margin(0.01f));
    // 60 degrees at 180 a second, turning on the way.
    float facing = 170.0f;
    cc.setOnFlight(true);
    for (int i = 0; i < 10; ++i) {
        facing += 3.0f;
        cc.faceInFlight(facing);
        cc.setOnFlight(true);
        w.run(0.02f);
    }
    CHECK(off() < 45.0f);
    CHECK(off() > 5.0f);
    w.run(0.3f);
    CHECK(off() == Catch::Approx(0.0f).margin(0.1f));
    // Turned by the player, it is left there - and turns along.
    cc.turnWithVehicle(-30.0f);
    for (int i = 0; i < 50; ++i) {
        facing -= 2.0f;
        cc.faceInFlight(facing);
        cc.setOnFlight(true);
        w.run(0.02f);
    }
    CHECK(off() == Catch::Approx(-30.0f).margin(0.1f));
}

// P71: after a map change the picture stood at the spawn's pitch, 15 degrees, while the camera was
// kept at its own: restoreSaved set the one it is kept at and did not turn the view.
TEST_CASE("the camera looks by the pitch it is put back to", "[taxi][camera]") {
    Walker w;
    const auto drawn = [&w] { return glm::degrees(std::asin(glm::normalize(w.camera.getForward()).z)); };
    w.controller.setDefaultSpawn(glm::vec3(0.0f), 90.0f, -15.0f);
    w.controller.reset();
    CHECK(drawn() == Catch::Approx(-15.0f).margin(0.01f));
    for (const float pitch : {-40.0f, 10.0f, 45.0f, -20.0f}) {
        w.controller.restoreSaved(7.5f, pitch);
        CHECK(w.controller.savedPitch() == Catch::Approx(pitch));
        CHECK(drawn() == Catch::Approx(pitch).margin(0.01f));
    }
    // FlipCameraYaw turns the view with the camera too (0x005ff2c0).
    const glm::vec3 before = w.camera.getForward();
    w.controller.flipYaw(180.0f);
    const glm::vec3 after = w.camera.getForward();
    CHECK(after.x == Catch::Approx(-before.x).margin(0.001f));
    CHECK(after.y == Catch::Approx(-before.y).margin(0.001f));
    CHECK(after.z == Catch::Approx(before.z).margin(0.001f));
}
