// The quest log through GameHandler's own dispatch. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "game/quest_handler.hpp"
#include "game/quest_objectives.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
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

// Quest 10120 in the bags as the server leaves it: the report handed over, the
// slot complete. And an ordinary item of a complete quest, counted all the same.
TEST_CASE("item objectives are counted off the bags and leave out the quest's own", "[replay][quest]") {
    constexpr uint64_t kItem = 0x4000000000000099ull;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_QUEST_LOG_START, 10120}}));
    s.receive(questAnswer(10120, "wotlk", 28024, 28024, 1));
    s.receive(UpdateObject().create(kItem, wowee::game::ObjectType::ITEM,
                                    {{UF::OBJECT_FIELD_ENTRY, 28024}, {UF::ITEM_FIELD_STACK_COUNT, 1}}));
    const uint16_t pack = wowee::game::fieldIndex(UF::PLAYER_FIELD_PACK_SLOT_1);
    // Complete before the bags are known to hold it.
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(0, 1, 5), 1}}));
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid,
                                      {{pack, static_cast<uint32_t>(kItem)},
                                       {static_cast<uint16_t>(pack + 1), static_cast<uint32_t>(kItem >> 32)}}));
    REQUIRE(questOf(s, 10120));
    REQUIRE(s.handler().countItemInBags(28024) == 1);
    auto lines = wowee::game::questObjectives(s.handler(), *questOf(s, 10120));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].text == "Besorgt 20 Kalynastachel.");
    CHECK(lines[0].finished);
    CHECK(wowee::game::questObjectiveCount(*questOf(s, 10120)) == 1);

    s.receive(questAnswer(10120, "wotlk", 0, 28024, 1));
    lines = wowee::game::questObjectives(s.handler(), *questOf(s, 10120));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].text.find("1/1") != std::string::npos);
    CHECK(lines[0].finished);
}

// Not a quest's, but of the same kind: what the client asks by itself. The time
// played went to the chat at every press of the character window's key.
TEST_CASE("the time played is asked for quietly as the player is made", "[replay][played]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    auto sent = s.sent().of(Opcode::CMSG_PLAYED_TIME);
    REQUIRE(sent.size() == 1);
    REQUIRE(sent[0].getData().size() == 1);
    CHECK(sent[0].getData()[0] == 0);

    s.handler().requestPlayedTime();   // RequestTimePlayed
    sent = s.sent().of(Opcode::CMSG_PLAYED_TIME);
    REQUIRE(sent.size() == 2);
    CHECK(sent[1].getData()[0] == 1);
}

// In the 12340 interface RequestTimePlayed is /played's alone (chatframe.lua).
TEST_CASE("no key asks for the time played", "[replay][played]") {
    std::ifstream in(std::string(WOWEE_SOURCE_DIR) + "/src/ui/game_screen.cpp");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(text.size() > 1000);
    CHECK(text.find("requestPlayedTime") == std::string::npos);
}

// Quest 9340 under its zone's header, with 9387 before it: the interface counts
// the header as well.
TEST_CASE("a quest is named by its row in the log, headers counted", "[replay][quest]") {
    std::vector<std::pair<std::string, std::string>> events;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(0, 0, 5), 9387}, {slotField(1, 0, 5), 9340}}));
    s.handler().setAddonEventCallback([&](const std::string& name, const std::vector<std::string>& args) {
        if (name == "QUEST_WATCH_UPDATE") events.emplace_back(name, args.empty() ? std::string() : args[0]);
    });
    auto* quests = s.handler().getQuestHandler();
    for (auto& q : quests->questLogRef()) {
        q.zoneOrSort = 3483;
        q.level = q.questId == 9387 ? 60 : 61;
    }
    CHECK(quests->questLogIndexOf(9387) == 2);
    CHECK(quests->questLogIndexOf(9340) == 3);
    CHECK(quests->questLogIndexOf(1) == 0);

    wowee::network::Packet kill(wowee::game::wireOpcode(Opcode::SMSG_QUESTUPDATE_ADD_KILL));
    kill.writeUInt32(9340);
    kill.writeUInt32(16927);
    kill.writeUInt32(2);
    kill.writeUInt32(8);
    kill.writeUInt64(0xF130004221000099ull);
    s.receive(kill);
    REQUIRE(events.size() == 1);
    CHECK(events[0].second == "3");

    // Folded away, they keep places behind what is shown.
    quests->collapsedQuestGroups().insert(3483);
    CHECK(quests->questLogRows().visible == 1);
    CHECK(quests->questLogIndexOf(9340) == 3);
    s.handler().setAddonEventCallback(nullptr);
}

// The reward chosen on the frame was let go by every item answer that came in
// while a quest window was open.
TEST_CASE("only an item the quest window waits for is its news", "[replay][quest]") {
    int updates = 0;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    s.handler().setAddonEventCallback([&](const std::string& name, const std::vector<std::string>&) {
        if (name == "QUEST_ITEM_UPDATE") ++updates;
    });
    // SMSG_ITEM_QUERY_SINGLE_RESPONSE as 0x0098d910 reads it, every number 0.
    const auto answer = [&](uint32_t entry) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE));
        p.writeUInt32(entry);
        for (int i = 0; i < 3; ++i) p.writeUInt32(0);
        p.writeString("Item");
        for (int i = 0; i < 3; ++i) p.writeString("");
        for (int i = 0; i < 160; ++i) p.writeUInt32(0);
        s.receive(p);
    };
    // A reward page, opened with a gossip behind it: any window will do.
    wowee::network::Packet offer(wowee::game::wireOpcode(Opcode::SMSG_QUESTGIVER_OFFER_REWARD));
    offer.writeUInt64(0xF130000BEC00150Eull);
    offer.writeUInt32(9387);
    offer.writeString("Title");
    offer.writeString("Text");
    offer.writeUInt8(0);
    for (int i = 0; i < 60; ++i) offer.writeUInt32(0);
    s.receive(offer);
    REQUIRE(s.handler().isQuestOfferRewardOpen());

    auto* quests = s.handler().getQuestHandler();
    answer(25001);   // a comparison tooltip's
    CHECK(updates == 0);
    quests->awaitQuestItem(25002);
    answer(25002);
    CHECK(updates == 1);
    answer(25002);
    CHECK(updates == 1);
    s.handler().setAddonEventCallback(nullptr);
}

// Taken, dropped and taken again at its giver: the log had a title and a
// sentence, level 0, no zone and nothing to do - and the tracker a quest done.
TEST_CASE("a quest known already is whole when it is taken again", "[replay][quest]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    auto* quests = s.handler().getQuestHandler();
    quests->addQuestToLocalLogIfMissing(10086, "Quest #10086", "");
    s.receive(questAnswer(10086, "wotlk", 0, 25802, 8));
    REQUIRE(questOf(s, 10086));
    REQUIRE(questOf(s, 10086)->level == 60);

    quests->questLogRef().clear();
    quests->addQuestToLocalLogIfMissing(10086, "Ich arbeite...", "Ein Satz.");
    const auto* q = questOf(s, 10086);
    REQUIRE(q);
    CHECK(q->title == kTitle);
    CHECK(q->level == 60);
    CHECK(q->description == kDetails);
    CHECK(q->itemObjectives[0].itemId == 25802);
    CHECK(wowee::game::questObjectiveCount(*q) == 1);
    CHECK_FALSE(q->complete);
}

TEST_CASE("a quest the server takes out of its slot is watched no more", "[replay][quest]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(0, 0, 5), 9387}, {slotField(1, 0, 5), 9340}}));
    s.handler().setQuestTracked(9387, true);
    s.handler().setQuestTracked(9340, true);
    REQUIRE(s.handler().getQuestWatchOrder().size() == 2);
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(0, 0, 5), 0}}));
    CHECK(s.handler().getQuestWatchOrder() == std::vector<uint32_t>{9340});
}

TEST_CASE("quests without a zone stand under the client's own header", "[replay][quest]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(0, 0, 5), 8021}}));
    const auto rows = s.handler().getQuestHandler()->questLogRows();
    REQUIRE(rows.rows.size() == 2);
    CHECK(rows.rows[0].headerTitle == "Missing header! (quest designers)");
}
