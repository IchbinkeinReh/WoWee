// The quest log through GameHandler's own dispatch. See game_replay.hpp.
#include <catch_amalgamated.hpp>
#include "test_support.hpp"

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
                                   uint32_t wantedCount = 0, uint32_t wantedKill = 0, const char* text = "") {
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
    for (int i = 0; i < 16; ++i) p.writeUInt32(i == 0 ? wantedKill : i == 1 && wantedKill != 0 ? 5 : 0);
    if (wotlk) {
        for (int i = 0; i < 6; ++i) {
            p.writeUInt32(i == 0 ? wantedItem : 0);
            p.writeUInt32(i == 0 ? wantedCount : 0);
        }
    }
    for (int i = 0; i < 4; ++i) p.writeString(i == 0 ? text : "");
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

// 0x006d8030: the quest's own text for the objective first, whatever the creature
// or object that carries it is called; nothing for one that has failed.
TEST_CASE("a credit is named by the quest's objective", "[replay][quest]") {
    std::vector<std::string> lines, watched;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_QUEST_LOG_START, 13829}}));
    const uint32_t wanted = GENERATE(33341u, 0x80000000u | 33341u);
    s.receive(questAnswer(13829, "wotlk", 0, 0, 0, wanted, "\"Zusto\xc3\x9f""en\" anwenden"));
    s.handler().setAddonEventCallback([&](const std::string& name, const std::vector<std::string>& args) {
        if (name == "UI_INFO_MESSAGE" && !args.empty()) lines.push_back(args[0]);
        if (name == "QUEST_WATCH_UPDATE") watched.push_back(args.empty() ? std::string() : args[0]);
    });
    const auto credit = [&](uint32_t entry, uint32_t count) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_QUESTUPDATE_ADD_KILL));
        for (const uint32_t v : {13829u, entry, count, 5u}) p.writeUInt32(v);
        p.writeUInt64(0);
        s.receive(p);
    };
    credit(wanted, 2);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "\"Zusto\xc3\x9f""en\" anwenden: 2/5");
    CHECK(watched.size() == 1);
    // Another entry's, which is not known by name.
    credit(5u, 1);
    CHECK(lines.size() == 1);
    // Failed: nothing more.
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(0, 1, 5), 2}}));
    watched.clear();
    credit(wanted, 3);
    CHECK(lines.size() == 1);
    CHECK(watched.empty());
    s.handler().setAddonEventCallback(nullptr);
}

// 0x006d9f20, 0x0058db30
TEST_CASE("how many daily quests are left is said as one is done", "[replay][quest]") {
    std::vector<std::string> lines;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    const auto field = [](int i) {
        return static_cast<uint16_t>(wowee::game::fieldIndex(UF::PLAYER_FIELD_DAILY_QUESTS_1) + i);
    };
    const auto said = [&] {
        size_t n = 0;
        for (const auto& m : s.handler().getChatHistory())
            if (m.message.find("daily") != std::string::npos) ++n;
        return n;
    };
    CHECK(s.handler().getQuestHandler()->dailyQuestsDone() == 0);
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{field(0), 13829}}));
    CHECK(s.handler().getQuestHandler()->dailyQuestsDone() == 1);
    CHECK(said() == 1);
    // The same again: no news.
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{field(0), 13829}}));
    CHECK(said() == 1);
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{field(1), 13830}}));
    CHECK(said() == 2);
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{field(0), 0}, {field(1), 0}}));
    CHECK(s.handler().getQuestHandler()->dailyQuestsDone() == 0);
    CHECK(said() == 3);
    wowee::game::DailyQuestLine line;
    CHECK(line.changed(1) == 24);
    CHECK(line.changed(25) == 0);
    CHECK(line.changed(0) == 25);
    CHECK_FALSE(line.changed(0).has_value());
    CHECK(line.changed(1) == 24);
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

// QUEST_WATCH_UPDATE (event 0x1e5, the auto-watch's cue) for an item objective
// comes only from an item push short of the count (0x006d7de0 -> 0x006d7c30),
// after ERR_QUEST_ADD_ITEM; it came whenever the bags' count changed - at login
// for every quest whose items were carried.
TEST_CASE("only an item push short of the count is a watch update", "[replay][quest]") {
    constexpr uint64_t kItem = 0x40000000000000A1ull;
    std::vector<std::string> watched, lines;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_QUEST_LOG_START, 10120}}));
    s.receive(questAnswer(10120, "wotlk", 0, 28024, 3));
    wowee::network::Packet name(wowee::game::wireOpcode(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE));
    name.writeUInt32(28024);
    for (int i = 0; i < 3; ++i) name.writeUInt32(0);
    name.writeString("Kalynastachel");
    for (int i = 0; i < 3; ++i) name.writeString("");
    for (int i = 0; i < 160; ++i) name.writeUInt32(0);
    s.receive(name);
    s.handler().setAddonEventCallback([&](const std::string& event, const std::vector<std::string>& args) {
        if (event == "QUEST_WATCH_UPDATE") watched.push_back(args.empty() ? std::string() : args[0]);
        if (event == "UI_INFO_MESSAGE" && !args.empty()) lines.push_back(args[0]);
    });
    // Carried already, as at a login: the count is taken, nothing watched.
    s.receive(UpdateObject().create(kItem, wowee::game::ObjectType::ITEM,
                                    {{UF::OBJECT_FIELD_ENTRY, 28024}, {UF::ITEM_FIELD_STACK_COUNT, 2}}));
    const uint16_t pack = wowee::game::fieldIndex(UF::PLAYER_FIELD_PACK_SLOT_1);
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid,
                                      {{pack, static_cast<uint32_t>(kItem)},
                                       {static_cast<uint16_t>(pack + 1), static_cast<uint32_t>(kItem >> 32)}}));
    REQUIRE(s.handler().countItemInBags(28024) == 2);
    CHECK(watched.empty());
    const auto push = [&](uint32_t count, uint32_t carried) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_ITEM_PUSH_RESULT));
        p.writeUInt64(Session::kPlayerGuid);
        p.writeUInt32(0);
        p.writeUInt32(0);
        p.writeUInt32(1);
        p.writeUInt8(255);
        p.writeUInt32(23);
        p.writeUInt32(28024);
        p.writeUInt32(0);
        p.writeUInt32(0);
        p.writeUInt32(count);
        p.writeUInt32(carried);
        s.receive(p);
    };
    push(1, 3);  // 2 before, 3 wanted
    REQUIRE(watched.size() == 1);
    CHECK(watched[0] == std::to_string(s.handler().getQuestHandler()->questLogIndexOf(10120)));
    CHECK(lines.size() == 1);
    push(1, 4);  // 3 before: enough already
    CHECK(watched.size() == 1);
    s.handler().setAddonEventCallback(nullptr);
}

// 0x006d8030: no QUEST_WATCH_UPDATE for a quest joining the log, failing,
// timing out (0x196, 0x197) or completing without its EndText (0x198); a kill
// credit (0x199) has one.
TEST_CASE("a failed or new quest is no watch update", "[replay][quest]") {
    std::vector<std::string> watched;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    s.handler().setAddonEventCallback([&](const std::string& event, const std::vector<std::string>& args) {
        if (event == "QUEST_WATCH_UPDATE") watched.push_back(args.empty() ? std::string() : args[0]);
    });
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_QUEST_LOG_START, 9387}}));
    REQUIRE(questOf(s, 9387));
    CHECK(watched.empty());
    for (const Opcode op : {Opcode::SMSG_QUESTUPDATE_FAILED, Opcode::SMSG_QUESTUPDATE_FAILEDTIMER,
                            Opcode::SMSG_QUESTUPDATE_COMPLETE}) {
        wowee::network::Packet p(wowee::game::wireOpcode(op));
        p.writeUInt32(9387);
        s.receive(p);
    }
    CHECK(watched.empty());
    wowee::network::Packet kill(wowee::game::wireOpcode(Opcode::SMSG_QUESTUPDATE_ADD_KILL));
    for (const uint32_t v : {9387u, 16927u, 2u, 8u}) kill.writeUInt32(v);
    kill.writeUInt64(0xF130004221000099ull);
    s.receive(kill);
    CHECK(watched.size() == 1);
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

// The selection was a row: with a quest gone from above it, or a header folded
// away, it was another quest's.
TEST_CASE("the quest log's selection is a quest, wherever its row goes", "[replay][quest]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 181}, {UF::UNIT_FIELD_MAXHEALTH, 181}});
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(0, 0, 5), 9387},
                                                             {slotField(1, 0, 5), 9340},
                                                             {slotField(2, 0, 5), 9360}}));
    auto& gh = s.handler();
    auto* quests = gh.getQuestHandler();
    for (auto& q : quests->questLogRef()) {
        q.zoneOrSort = 3483;
        q.level = q.questId == 9387 ? 60 : q.questId == 9340 ? 61 : 62;
    }
    CHECK(gh.getSelectedQuestLogIndex() == 0);
    CHECK(gh.getSelectedQuest() == nullptr);
    gh.selectQuestLogRow(3);
    REQUIRE(gh.getSelectedQuest() != nullptr);
    CHECK(gh.getSelectedQuest()->questId == 9340);
    CHECK(gh.getSelectedQuestLogIndex() == 3);
    // The one above it goes (0x005e6940 leaves 0x00c23ad8 alone).
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(0, 0, 5), 0}}));
    CHECK(gh.getSelectedQuestLogIndex() == 2);
    REQUIRE(gh.getSelectedQuest() != nullptr);
    CHECK(gh.getSelectedQuest()->questId == 9340);
    // One comes in above it.
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(3, 0, 5), 9399}}));
    for (auto& q : quests->questLogRef()) {
        if (q.questId != 9399) continue;
        q.zoneOrSort = 3483;
        q.level = 1;
    }
    CHECK(gh.getSelectedQuestLogIndex() == 3);
    CHECK(gh.getSelectedQuest()->questId == 9340);
    // A header's row leaves it as it is (0x005dffe0), one off the end clears it.
    gh.selectQuestLogRow(1);
    CHECK(gh.getSelectedQuest()->questId == 9340);
    // Folded away it is still the quest, in the place it has there.
    quests->collapsedQuestGroups().insert(3483);
    CHECK(gh.getSelectedQuest()->questId == 9340);
    CHECK(gh.getSelectedQuestLogIndex() == quests->questLogIndexOf(9340));
    quests->collapsedQuestGroups().clear();
    // Gone itself, it has no row any more.
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{slotField(1, 0, 5), 0}}));
    CHECK(gh.getSelectedQuestLogIndex() == 0);
    gh.selectQuestLogRow(99);
    CHECK(gh.getSelectedQuest() == nullptr);
}
