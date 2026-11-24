// A 3.3.5 flight through GameHandler's own dispatch: the server's flag and its
// splines are the flight, and the end of each is reported. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "core/coordinates.hpp"
#include "game_replay.hpp"
#include "game/movement_handler.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
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

// The frames that pass: the units moved on, the spline asked after.
void frames(Session& s, int milliseconds) {
    auto* mh = s.handler().getMovementHandler();
    for (int at = 0; at < milliseconds; at += 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
