// The quest log through GameHandler's own dispatch. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "game/quest_handler.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::Opcode;
using wowee::game::UF;

namespace {

constexpr const char* kTitle = "Ich arbeite... f\xc3\xbcr die Horde!";
constexpr const char* kDetails =
    "Macht Euch n\xc3\xbctzlich. Kommt zur\xc3\xbc""ck und holt Euch eine fette Belohnung ab!";

// SMSG_QUEST_QUERY_RESPONSE as each expansion's server writes it: the block of
// words, the strings, the objective records and their four texts. `ownItem` and
// `wantedItem` are WotLK's words 19 and the first item record.
wowee::network::Packet questAnswer(uint32_t questId, const std::string& expansion,
                                   uint32_t ownItem = 0, uint32_t wantedItem = 0,
                                   uint32_t wantedCount = 0) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_QUEST_QUERY_RESPONSE));
    const bool wotlk = expansion == "wotlk";
    const int words = wotlk ? 65 : (expansion == "tbc" ? 43 : 39);
    for (int i = 0; i < words; ++i) {
        uint32_t v = 0;
        if (i == 0) v = questId;
        if (i == 1) v = 2;
        if (i == 2) v = 60;
        if (wotlk && i == 19) v = ownItem;
        p.writeUInt32(v);
    }
    p.writeString(kTitle);
    p.writeString("Besorgt 20 Kalynastachel.");
    p.writeString(kDetails);
    p.writeString("");
    if (wotlk) p.writeString("Kehrt zu Megzeg zur\xc3\xbc""ck.");
    for (int i = 0; i < 16; ++i) p.writeUInt32(0);
    if (wotlk) {
        for (int i = 0; i < 6; ++i) {
            p.writeUInt32(i == 0 ? wantedItem : 0);
            p.writeUInt32(i == 0 ? wantedCount : 0);
        }
    }
    for (int i = 0; i < 4; ++i) p.writeString("");
    return p;
}

uint16_t slotField(uint16_t slot, uint16_t word, uint16_t stride) {
    return static_cast<uint16_t>(wowee::game::fieldIndex(UF::PLAYER_QUEST_LOG_START) + slot * stride + word);
}

const wowee::game::QuestHandler::QuestLogEntry* questOf(Session& s, uint32_t id) {
    for (const auto& q : s.handler().getQuestLog())
        if (q.questId == id) return &q;
    return nullptr;
}

}  // namespace

TEST_CASE("a quest's texts are read in every expansion, however they read", "[replay][quest]") {
    for (const char* expansion : {"wotlk", "tbc", "classic", "turtle"}) {
        CAPTURE(expansion);
        Session s(expansion);
        s.handler().getQuestHandler()->addQuestToLocalLogIfMissing(10086, "Quest #10086", "");
        s.receive(questAnswer(10086, expansion));
        const auto* q = questOf(s, 10086);
        REQUIRE(q);
        CHECK(q->title == kTitle);
        CHECK(q->objectives == "Besorgt 20 Kalynastachel.");
        CHECK(q->description == kDetails);
    }
}

// The server may say a quest is complete with the slot's state word alone.
TEST_CASE("any word of a quest's slot is news to the interface", "[replay][quest]") {
    std::vector<std::string> events;
    std::vector<bool> completeAtUpdate;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_QUEST_LOG_START, 9387}}));
    s.receive(questAnswer(9387, "wotlk"));
    s.handler().setAddonEventCallback([&](const std::string& name, const std::vector<std::string>&) {
        events.push_back(name);
        if (name != "QUEST_LOG_UPDATE") return;
        const auto* q = questOf(s, 9387);
        completeAtUpdate.push_back(q && q->complete);
    });
    const auto updates = [&] {
        return std::count(events.begin(), events.end(), std::string("QUEST_LOG_UPDATE"));
    };

    SECTION("complete") {
        s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(0, 1, 5), 1}}));
        CHECK(updates() == 1);
        REQUIRE(completeAtUpdate.size() == 1);
        CHECK(completeAtUpdate[0]);   // and it asks of the log as it is by then
        // Said again, nothing is new.
        s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(0, 1, 5), 1}}));
        CHECK(updates() == 1);
        s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(0, 1, 5), 0}}));
        CHECK(updates() == 2);
        CHECK_FALSE(completeAtUpdate.back());
    }
    SECTION("failed") {
        s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(0, 1, 5), 2}}));
        CHECK(updates() == 1);
        CHECK(questOf(s, 9387)->failed);
    }
    SECTION("a failed one has no timer") {
        // 0x005e4320, 0x005e6240.
        s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(0, 4, 5), 4000000000u}}));
        REQUIRE(s.handler().getQuestTimers().size() == 1);
        s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(0, 1, 5), 2}, {slotField(0, 4, 5), 1}}));
        CHECK(s.handler().getQuestTimers().empty());
    }
    SECTION("a counter or the timer") {
        s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(0, 2, 5), 3}}));
        CHECK(updates() == 1);
        s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(0, 4, 5), 1234567}}));
        CHECK(updates() == 2);
    }
    SECTION("nothing else") {
        s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_HEALTH, 65}}));
        CHECK(updates() == 0);
    }
    SECTION("gone") {
        s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_QUEST_LOG_START, 0}}));
        CHECK(updates() >= 1);
        CHECK(questOf(s, 9387) == nullptr);
    }
    s.handler().setAddonEventCallback(nullptr);
}

// questlogframe.lua:260 watches the entry QUEST_ACCEPTED names; the list has
// the headers in it (0x005deeb0).
TEST_CASE("the events name a quest by its place in the interface's list", "[replay][quest]") {
    std::vector<std::pair<std::string, std::string>> events;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    s.handler().setQuestLogRowLookup([](uint32_t questId) { return questId == 9387 ? 5 : 0; });
    s.handler().setAddonEventCallback([&](const std::string& name, const std::vector<std::string>& args) {
        if (name == "QUEST_ACCEPTED" || name == "QUEST_WATCH_UPDATE")
            events.emplace_back(name, args.empty() ? std::string() : args[0]);
    });
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_QUEST_LOG_START, 9387}}));
    s.receive(questAnswer(9387, "wotlk"));
    const auto said = [&](const char* name) {
        return std::find_if(events.begin(), events.end(), [&](const auto& e) { return e.first == name; });
    };
    REQUIRE(said("QUEST_ACCEPTED") != events.end());
    CHECK(said("QUEST_ACCEPTED")->second == "5");
    CHECK(s.handler().getQuestHandler()->questLogIndexOf(9387) == 5);
    CHECK(s.handler().getQuestHandler()->questLogIndexOf(1) == 0);
    for (const auto& e : events) CHECK(e.second == "5");
    s.handler().setQuestLogRowLookup(nullptr);
    CHECK(s.handler().getQuestHandler()->questLogIndexOf(9387) == 1);
    s.handler().setAddonEventCallback(nullptr);
}
