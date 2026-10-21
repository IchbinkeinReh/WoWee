// The client's quest POI icon list (CGQuestLog, 0x00c23b00): slots from the
// query response (0x005e7370, 0x005e7020), the visible count (0x005e5a50), the
// marks placed and pushed apart (0x005e2eb0) and the visible order (0x005e3840).

#include <catch_amalgamated.hpp>

#include "game/quest_poi_icons.hpp"

#include <map>
#include <set>

using namespace wowee::game;

namespace {

/// An area around (x, y): two points either side, so its mean is (x, y) and it
/// is not a single-point place.
QuestPoiArea area(int32_t objective, int32_t x, int32_t y, uint32_t map = 1,
                  int32_t priority = 0, int points = 2) {
    QuestPoiArea a;
    a.objectiveIndex = objective;
    a.mapId = map;
    a.worldMapAreaId = 14;
    a.priority = priority;
    if (points == 1) {
        a.points = {{x, y}};
    } else {
        a.points = {{x - 1, y}, {x + 1, y}};
    }
    return a;
}

QuestPoiMapView zoneView(float width = 1024.0f) {
    QuestPoiMapView v;
    v.valid = true;
    v.zoneOrFloor = true;
    v.worldMapAreaId = 14;
    v.mapId = 1;
    v.locLeft = width;
    v.locRight = 0.0f;
    v.project = [](float, float, float& u, float& w) {
        u = 0.5f;
        w = 0.5f;
        return true;
    };
    return v;
}

struct Quests {
    std::map<int32_t, QuestPoiQuestState> states;
    QuestPoiIconList::QuestState fn() const {
        return [this](int32_t id) {
            auto it = states.find(id);
            return it == states.end() ? QuestPoiQuestState{} : it->second;
        };
    }
    void open(int32_t id, int logIndex, uint32_t mask) {
        states[id] = QuestPoiQuestState{logIndex, false, false, mask};
    }
    void done(int32_t id, int logIndex) {
        states[id] = QuestPoiQuestState{logIndex, true, false, 0xffffffffu};
    }
};

const auto everyQuestInLog = [](int32_t) { return true; };

}  // namespace

TEST_CASE("A received quest takes a slot with the mean of each area's points", "[quest_poi]") {
    QuestPoiIconList list;
    QuestPoiArea a;
    a.points = {{10, 3}, {11, 4}, {-12, 0}};
    list.receive(42, {a}, everyQuestInLog);
    const auto* icon = list.iconFor(42);
    REQUIRE(icon);
    REQUIRE(icon->areas.size() == 1);
    // (10 + 11 - 12) / 3 and (3 + 4 + 0) / 3, as ints.
    CHECK(icon->areas[0].centerX == 3);
    CHECK(icon->areas[0].centerY == 2);
    CHECK(list.questIdAtSlot(0) == 42);
    CHECK(list.questIdAtSlot(1) == 0);
    CHECK(list.questIdAtSlot(25) == -1);
}

TEST_CASE("A quest already listed keeps its areas", "[quest_poi]") {
    QuestPoiIconList list;
    list.receive(42, {area(-1, 100, 100)}, everyQuestInLog);
    list.receive(42, {area(0, 5, 5), area(1, 6, 6)}, everyQuestInLog);
    REQUIRE(list.iconFor(42)->areas.size() == 1);
    CHECK(list.iconFor(42)->areas[0].objectiveIndex == -1);
}

TEST_CASE("A slot whose quest left the log is taken over, and a full list drops", "[quest_poi]") {
    QuestPoiIconList list;
    for (int32_t q = 1; q <= QuestPoiIconList::kSlots; ++q) list.receive(q, {}, everyQuestInLog);
    list.receive(100, {}, everyQuestInLog);
    CHECK_FALSE(list.contains(100));
    // Quest 7 abandoned: its slot is the first one free.
    list.receive(100, {}, [](int32_t q) { return q != 7; });
    CHECK(list.contains(100));
    CHECK(list.questIdAtSlot(6) == 100);
    CHECK_FALSE(list.contains(7));
}

TEST_CASE("The visible count follows the objective mask and the shown map", "[quest_poi]") {
    QuestPoiIconList list;
    list.receive(1, {area(0, 10, 10)}, everyQuestInLog);            // kill 0
    list.receive(2, {area(-1, 20, 20)}, everyQuestInLog);           // hand-in
    list.receive(3, {area(4, 30, 30, 0)}, everyQuestInLog);         // item 0, other map
    QuestPoiArea own = area(1, 40, 40);
    own.flags = 4;
    own.worldMapAreaId = 99;                                        // another zone's only
    list.receive(4, {own}, everyQuestInLog);
    Quests q;
    q.open(1, 2, 0x1);
    q.done(2, 3);
    q.open(3, 4, 0x10);
    q.open(4, 5, 0x2);
    CHECK(list.updateVisible(zoneView(), q.fn()) == 2);
    CHECK(list.iconFor(1)->visible);
    CHECK(list.iconFor(2)->visible);
    CHECK_FALSE(list.iconFor(3)->visible);
    CHECK_FALSE(list.iconFor(4)->visible);
    // Kill 0 done: nothing of quest 1 is selected any more.
    q.open(1, 2, 0x2);
    CHECK(list.updateVisible(zoneView(), q.fn()) == 1);
}

TEST_CASE("The mark goes on the lowest priority, then the nearest, area", "[quest_poi]") {
    QuestPoiIconList list;
    list.receive(1, {area(0, 500, 500, 1, 2), area(0, 900, 900, 1, 1), area(0, 100, 100, 1, 1)},
                 everyQuestInLog);
    list.receive(2, {area(0, 50, 50, 1, 11)}, everyQuestInLog);     // past priority ten
    Quests q;
    q.done(1, 2);  // complete ones are not squeezed, so the centre reads back as is
    q.states[1].objectiveMask = 0x1;
    q.done(2, 3);
    q.states[2].objectiveMask = 0x1;
    list.updateVisible(zoneView(), q.fn());
    list.updateIcons(zoneView(), q.fn(), 120.0f, 120.0f, 0.0f);
    CHECK(list.iconFor(1)->x == 100);
    CHECK(list.iconFor(1)->y == 100);
    CHECK(list.iconFor(2)->x == QuestPoiIconList::kNoPosition);
}

TEST_CASE("Marks of quests in progress are pushed a reach apart", "[quest_poi]") {
    // A map 1024 across: the default overlap of 12 is a reach of 24, and no
    // round moves a mark more than a twentieth of that.
    QuestPoiIconList list;
    list.receive(1, {area(0, 0, 0)}, everyQuestInLog);
    list.receive(2, {area(0, 10, 0)}, everyQuestInLog);
    Quests q;
    q.open(1, 2, 0x1);
    q.open(2, 3, 0x1);
    list.updateVisible(zoneView(), q.fn());
    list.updateIcons(zoneView(), q.fn(), 0.0f, 0.0f, 0.0f);
    // One unit each per round until they are 24 apart.
    CHECK(list.iconFor(1)->x == -7);
    CHECK(list.iconFor(2)->x == 17);
    CHECK(list.iconFor(1)->y == 0);
}

TEST_CASE("A single-point area is a place and is not moved", "[quest_poi]") {
    QuestPoiIconList list;
    list.receive(1, {area(0, 0, 0, 1, 0, 1)}, everyQuestInLog);
    list.receive(2, {area(0, 10, 0)}, everyQuestInLog);
    Quests q;
    q.open(1, 2, 0x1);
    q.open(2, 3, 0x1);
    list.updateVisible(zoneView(), q.fn());
    list.updateIcons(zoneView(), q.fn(), 0.0f, 0.0f, 0.0f);
    CHECK(list.iconFor(1)->x == 0);
    // Only the other one moves, one unit a round, to 24 away.
    CHECK(list.iconFor(2)->x == 24);
}

TEST_CASE("Completed quests' marks on one spot are fanned out by POIShiftComplete", "[quest_poi]") {
    QuestPoiIconList list;
    list.receive(1, {area(-1, 200, 200)}, everyQuestInLog);
    list.receive(2, {area(-1, 200, 200)}, everyQuestInLog);
    Quests q;
    q.done(1, 2);
    q.done(2, 3);
    list.updateVisible(zoneView(), q.fn());
    list.updateIcons(zoneView(), q.fn(), 0.0f, 0.0f, 0.6f);
    CHECK(list.iconFor(1)->x == 200);
    CHECK(list.iconFor(1)->y == 200);
    // The first offset, (50, 86), times 0.6.
    CHECK(list.iconFor(2)->x == 230);
    CHECK(list.iconFor(2)->y == 251);
    // Off, nothing moves.
    list.updateIcons(zoneView(), q.fn(), 0.0f, 0.0f, 0.0f);
    CHECK(list.iconFor(2)->x == 200);
}

TEST_CASE("A dungeon floor marks areas of its own floor and pushes nothing", "[quest_poi]") {
    QuestPoiIconList list;
    QuestPoiArea a = area(0, 0, 0);
    a.floor = 2;
    QuestPoiArea b = area(0, 1, 0);
    b.floor = 2;
    QuestPoiArea c = area(0, 0, 0);
    c.floor = 1;
    list.receive(1, {a}, everyQuestInLog);
    list.receive(2, {b}, everyQuestInLog);
    list.receive(3, {c}, everyQuestInLog);
    Quests q;
    q.open(1, 2, 0x1);
    q.open(2, 3, 0x1);
    q.open(3, 4, 0x1);
    auto view = zoneView();
    view.dungeon = true;
    view.dungeonFloor = 2;
    CHECK(list.updateVisible(view, q.fn()) == 2);
    list.updateIcons(view, q.fn(), 0.0f, 0.0f, 0.0f);
    CHECK(list.iconFor(1)->x == 0);
    CHECK(list.iconFor(2)->x == 1);
    CHECK(list.iconFor(3)->x == QuestPoiIconList::kNoPosition);
}

TEST_CASE("The visible order is completed, then watched, then the rest", "[quest_poi]") {
    QuestPoiIconList list;
    list.receive(10, {area(0, 0, 0)}, everyQuestInLog);
    list.receive(20, {area(0, 0, 0)}, everyQuestInLog);
    list.receive(30, {area(-1, 0, 0)}, everyQuestInLog);
    list.receive(40, {area(0, 0, 0)}, everyQuestInLog);
    Quests q;
    q.open(10, 2, 0x1);
    q.open(20, 3, 0x1);
    q.done(30, 4);
    q.open(40, 5, 0x1);
    REQUIRE(list.updateVisible(zoneView(), q.fn()) == 4);
    const std::vector<int32_t> watched = {40, 20};
    CHECK(list.questIdByVisibleIndex(0, q.fn(), watched) == 30);
    CHECK(list.questIdByVisibleIndex(1, q.fn(), watched) == 40);
    CHECK(list.questIdByVisibleIndex(2, q.fn(), watched) == 20);
    CHECK(list.questIdByVisibleIndex(3, q.fn(), watched) == 10);
    CHECK(list.questIdByVisibleIndex(4, q.fn(), watched) == -1);
}

TEST_CASE("The push distance is kept only when not below the overlap distance", "[quest_poi]") {
    QuestPoiIconList list;
    list.setOverlapDistance(20.0f);
    list.setOverlapPushDistance(10.0f);
    CHECK(list.overlapPushDistance() == 18.0f);
    list.setOverlapPushDistance(25.0f);
    CHECK(list.overlapPushDistance() == 25.0f);
}
