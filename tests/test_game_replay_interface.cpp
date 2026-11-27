// What the interface's calls do through the Lua engine itself, with a client
// behind them. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "addons/lua_api_helpers.hpp"
#include "addons/lua_engine.hpp"
#include "core/app_clock.hpp"
#include "core/env.hpp"
#include "core/input.hpp"
#include "core/preview_dressing.hpp"
#include "game/dressing_room.hpp"
#include "game/inventory_handler.hpp"
#include "game/name_rules.hpp"
#include "game/player_display_flags.hpp"
#include "game/chat_gesture.hpp"
#include "game/client_text_copy.hpp"
#include "game/quest_handler.hpp"
#include "game/quest_link.hpp"
#include "game_replay.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/animation/emote_registry.hpp"
#include "ui/framexml_emitter.hpp"
#include "ui/widget_tree.hpp"
#include "ui/xml_parser.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::Opcode;
using wowee::game::UF;

namespace {

struct Interface {
    explicit Interface(Session& s) : session(s) {
        REQUIRE(engine.initialize());
        engine.setGameHandler(&s.handler());
        s.handler().setAddonEventCallback(
            [this](const std::string& event, const std::vector<std::string>& args) { engine.fireEvent(event, args); });
    }
    void run(const std::string& code) {
        INFO(code);
        const bool ok = engine.executeString(code);
        INFO(engine.lastError());
        REQUIRE(ok);
    }
    bool is(const std::string& expression) { return engine.evaluateBoolean(expression); }
    void layout() { engine.widgets().layout(1024.0f, 768.0f); }
    // The session outlives this and still fires events on its way out
    // (MIRROR_TIMER_STOP and others from onLeaveWorld): they must not reach
    // this engine once it is gone (ASan, stack-use-after-scope).
    ~Interface() { session.handler().setAddonEventCallback({}); }
    Interface(const Interface&) = delete;
    Interface& operator=(const Interface&) = delete;
    Session& session;
    wowee::addons::LuaEngine engine;
};

}  // namespace

// P50: 0x005b9310 asks the server of a row with 0x543 only; a holiday is
// opened as it stands.
TEST_CASE("a holiday opens without asking the server", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    wowee::game::Character self;
    self.guid = Session::kPlayerGuid;
    self.race = wowee::game::Race::BLOOD_ELF;
    s.handler().charactersRef().push_back(self);
    s.handler().setActiveCharacterGuid(self.guid);
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_CALENDAR_SEND_CALENDAR));
    const uint32_t november22 = (10u << 24) | (10u << 20) | (21u << 14);
    for (uint32_t v : {0u, 0u, 1290000000u, november22, 0u, 0u, 0u, 1u}) p.writeUInt32(v);
    for (uint32_t v : {404u, 0u, 0u, 7u, 3u}) p.writeUInt32(v);
    for (int i = 0; i < 26; ++i) p.writeUInt32(i == 0 ? november22 : 0u);
    for (int i = 0; i < 10; ++i) p.writeUInt32(i == 0 ? 24u : 0u);
    for (int i = 0; i < 10; ++i) p.writeUInt32(i == 0 ? 3u : 0u);
    p.writeString("Calendar_HarvestFestival");
    s.receive(p);
    ui.run("CalendarSetAbsMonth(11, 2010) seen = {} "
           "local f = CreateFrame('Frame') f:RegisterEvent('CALENDAR_OPEN_EVENT') "
           "f:SetScript('OnEvent', function(self, event, kind) seen[#seen + 1] = kind end) "
           "for i = 1, CalendarGetNumDayEvents(0, 22) do "
           "  local _, _, _, kind = CalendarGetDayEvent(0, 22, i) if kind == 'HOLIDAY' then row = i end end");
    REQUIRE(ui.is("row ~= nil"));
    const size_t sent = s.sent().all().size();
    ui.run("CalendarOpenEvent(0, 22, row)");
    CHECK(s.sent().all().size() == sent);
    CHECK(ui.is("#seen == 1 and seen[1] == 'HOLIDAY'"));
    CHECK(ui.is("(function() local o, d, i = CalendarGetEventIndex() return o == 0 and d == 22 and i == row end)()"));
    // Kept while another month is shown, whose days it is not among.
    ui.run("CalendarSetMonth(6)");
    CHECK(ui.is("(function() local o, d, i = CalendarGetEventIndex() return o == 0 and d == 0 and i == 0 end)()"));
    ui.run("CalendarSetMonth(-6)");
    CHECK(ui.is("(function() local o, d, i = CalendarGetEventIndex() return o == 0 and d == 22 and i == row end)()"));
    ui.run("CalendarCloseEvent()");
    CHECK(ui.is("(function() local o, d, i = CalendarGetEventIndex() return o == 0 and d == 0 and i == 0 end)()"));
}

// P51: the wheel goes down the frames that take the wheel (0x00494890), to
// one with the script (0x0048fdb0): a button over the list covers nothing.
// P52: and a scroll bar that is not shown is the handler's to leave alone.
TEST_CASE("the wheel reaches the list under its rows", "[replay][interface]") {
    Session s;
    Interface ui(s);
    ui.run(R"(
        local window = CreateFrame('Frame', nil, UIParent) window:SetAllPoints(UIParent)
        turned = 0
        list = CreateFrame('ScrollFrame', nil, window)
        list:SetPoint('BOTTOMLEFT', UIParent, 'BOTTOMLEFT', 100, 100) list:SetWidth(200) list:SetHeight(200)
        bar = CreateFrame('Slider', nil, list) bar:SetMinMaxValues(0, 100) bar:SetValue(0)
        bar:SetPoint('TOPRIGHT') bar:SetWidth(16) bar:SetHeight(200)
        list:EnableMouseWheel(true)
        list:SetScript('OnMouseWheel', function(self, delta)
            turned = turned + 1
            if not bar:IsVisible() then return end
            bar:SetValue(bar:GetValue() - delta * 10)
        end)
        local row = CreateFrame('Button', nil, window) row:EnableMouse(true) row:SetFrameLevel(20)
        row:SetPoint('BOTTOMLEFT', UIParent, 'BOTTOMLEFT', 100, 150) row:SetWidth(200) row:SetHeight(20)
        local mute = CreateFrame('Frame', nil, window) mute:EnableMouseWheel(true) mute:SetFrameLevel(30)
        mute:SetPoint('BOTTOMLEFT', UIParent, 'BOTTOMLEFT', 100, 100) mute:SetWidth(200) mute:SetHeight(200)
    )");
    ui.layout();
    const float scale = ui.engine.widgets().uiScale() > 0.0f ? ui.engine.widgets().uiScale() : 1.0f;
    const auto wheel = [&](float clicks) { return ui.engine.dispatchMouseWheel(150.0f * scale, 160.0f * scale, clicks); };
    CHECK(wheel(-1.0f));
    CHECK(ui.is("turned == 1"));
    CHECK_FALSE(ui.engine.dispatchMouseWheel(600.0f * scale, 600.0f * scale, -1.0f));
    // The bar has been seen moving, and glides from now on, while it shows.
    CHECK(wheel(-1.0f));
    CHECK(ui.is("turned == 1"));
    for (int i = 0; i < 200; ++i) ui.engine.dispatchOnUpdate(0.05f);
    ui.run("at = bar:GetValue() bar:Hide()");
    ui.layout();
    CHECK(wheel(-1.0f));
    for (int i = 0; i < 200; ++i) ui.engine.dispatchOnUpdate(0.05f);
    CHECK(ui.is("bar:GetValue() == at"));
    CHECK(ui.is("turned == 2"));
}

namespace {

// SMSG_ITEM_QUERY_SINGLE_RESPONSE as 0x0098d910 reads it, every number 0.
void answerItem(Session& s, uint32_t entry, const char* name, uint32_t sellPrice = 0) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE));
    p.writeUInt32(entry);
    for (int i = 0; i < 3; ++i) p.writeUInt32(0);
    p.writeString(name);
    for (int i = 0; i < 3; ++i) p.writeString("");
    for (int i = 0; i < 160; ++i) p.writeUInt32(i == 5 ? sellPrice : 0);
    s.receive(p);
}

void rollsGorilla(Session& s) {
    s.handler().setRandomPropertyNameResolver(
        [](int32_t id) { return id == -24 ? std::string("of the Gorilla") : std::string(); });
    s.handler().setRandomStatResolver([](int32_t id, uint32_t) {
        std::vector<wowee::game::GameHandler::RandomStatBonus> out;
        if (id == -24) out.push_back({4, 17});  // strength
        return out;
    });
}

}  // namespace

// T425: GetItemInfo 0x00516c60 reads a link whole (0x0050f630) and names the
// item and its link with the link's property (0x00706d70, 0x0061e290).
TEST_CASE("GetItemInfo of a link keeps what it rolled", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    rollsGorilla(s);
    answerItem(s, 12047, "Spectral Necklace");
    ui.run("n, l, _, _, _, _, _, _, _, tex = GetItemInfo('|cff1eff00|Hitem:12047:5:6:7:8:9:-24:35:80|h[x]|h|r')");
    CHECK(ui.is("n == 'Spectral Necklace of the Gorilla'"));
    CHECK(ui.is("strfind(l, 'item:12047:5:6:7:8:0:%-24:35:', 1) ~= nil"));
    CHECK(ui.is("strfind(l, '[Spectral Necklace of the Gorilla]', 1, true) ~= nil"));
    CHECK(ui.is("type(tex) == 'string'"));
    // An id or a link without a property: the plain name.
    CHECK(ui.is("GetItemInfo(12047) == 'Spectral Necklace'"));
    CHECK(ui.is("GetItemInfo('item:12047:0:0:0:0:0:0:0:80') == 'Spectral Necklace'"));
    CHECK(ui.is("strfind(select(2, GetItemInfo(12047)), 'item:12047:0:0:0:0:0:0:0:', 1) ~= nil"));
}

// 0x0058a5d0 / 0x0058a6c0: a gossip quest with icon 3 or 4 is active, any
// other icon available - the counts and the lists split them alike.
TEST_CASE("the gossip quest lists split by icons 3 and 4", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    wowee::network::Packet gossip(wowee::game::wireOpcode(Opcode::SMSG_GOSSIP_MESSAGE));
    gossip.writeUInt64(0xF130000BEC00150Full);
    for (uint32_t v : {0u, 1u, 0u}) gossip.writeUInt32(v);
    const std::pair<uint32_t, const char*> quests[] = {{2, "Offered"}, {3, "Taken"}, {4, "Taken too"}, {0, "Plain"}};
    gossip.writeUInt32(4);
    uint32_t id = 900;
    for (const auto& [icon, title] : quests) {
        for (uint32_t v : {id++, icon, 10u, 0u}) gossip.writeUInt32(v);
        gossip.writeUInt8(0);
        gossip.writeString(title);
    }
    s.receive(gossip);
    CHECK(ui.is("GetNumGossipAvailableQuests() == 2"));
    CHECK(ui.is("GetNumGossipActiveQuests() == 2"));
    CHECK(ui.is("select(6, GetGossipAvailableQuests()) == 'Plain'"));
    CHECK(ui.is("select(5, GetGossipActiveQuests()) == 'Taken too'"));
    CHECK(ui.is("select('#', GetGossipAvailableQuests()) == 10"));
    CHECK(ui.is("select('#', GetGossipActiveQuests()) == 8"));
}

// 0x0058b490: an active gossip quest's isComplete is its log slot's
// 0x005e0ea0(index, 1) || 0x005e10b0(index) - never the icon, which a 3.3.5
// server sends as 4 for finished and unfinished alike. A quest with nothing to
// do but hand it in is ready (found by AG in code).
TEST_CASE("GetGossipActiveQuests asks the quest log whether a quest is done", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    auto* quests = s.handler().getQuestHandler();
    for (uint32_t id : {901u, 902u, 903u}) quests->addQuestToLocalLogIfMissing(id, "Quest", "");
    for (auto& q : quests->questLogRef()) {
        if (q.questId == 901 || q.questId == 902) {
            q.killObjectives[0].npcOrGoId = 299;
            q.killObjectives[0].required = 8;
        }
        q.complete = q.questId == 901;
        q.level = 10;
    }
    wowee::network::Packet gossip(wowee::game::wireOpcode(Opcode::SMSG_GOSSIP_MESSAGE));
    gossip.writeUInt64(0xF130000BEC00150Full);
    for (uint32_t v : {0u, 1u, 0u}) gossip.writeUInt32(v);
    gossip.writeUInt32(4);
    for (uint32_t id : {901u, 902u, 903u, 904u}) {
        for (uint32_t v : {id, 4u, 10u, 0u}) gossip.writeUInt32(v);
        gossip.writeUInt8(0);
        gossip.writeString("Taken");
    }
    s.receive(gossip);
    REQUIRE(ui.is("select('#', GetGossipActiveQuests()) == 16"));
    CHECK(ui.is("select(4, GetGossipActiveQuests()) == 1"));
    CHECK(ui.is("select(8, GetGossipActiveQuests()) == nil"));
    CHECK(ui.is("select(12, GetGossipActiveQuests()) == 1"));
    CHECK(ui.is("select(16, GetGossipActiveQuests()) == nil"));
}

// P56: 0x0062f9e0 gives the tooltip the attachment's property and seed, as
// every tooltip of an item is given what it rolled (0x006277f0).
TEST_CASE("a letter's attachment is named and read with what it rolled", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    rollsGorilla(s);
    answerItem(s, 12047, "Spectral Necklace");
    // SMSG_MAIL_LIST_RESULT as WorldSession::HandleGetMailList writes it.
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_MAIL_LIST_RESULT));
    p.writeUInt32(1);
    p.writeUInt8(1);
    wowee::network::Packet m;
    m.writeUInt32(501);
    m.writeUInt8(0);
    m.writeUInt64(0x77);
    for (uint32_t v : {0u, 0u, 41u, 0u, 0u}) m.writeUInt32(v);
    m.writeFloat(29.0f);
    m.writeUInt32(0);
    m.writeString("[Spectral Necklace of the Gorilla]");
    m.writeString("");
    m.writeUInt8(1);
    m.writeUInt8(0);
    m.writeUInt32(9001);
    m.writeUInt32(12047);
    for (uint32_t e : {111u, 0u, 3001u, 3002u, 3003u, 0u, 0u}) {
        m.writeUInt32(e);
        m.writeUInt32(0);
        m.writeUInt32(0);
    }
    m.writeUInt32(static_cast<uint32_t>(-24));
    m.writeUInt32(35);
    for (uint32_t v : {1u, 0u, 0u, 0u}) m.writeUInt32(v);
    m.writeUInt8(0);
    p.writeUInt16(static_cast<uint16_t>(m.getData().size() + 2));
    p.writeBytes(m.getData().data(), m.getData().size());
    s.receive(p);
    REQUIRE(ui.is("GetInboxNumItems() == 1"));
    CHECK(ui.is("GetInboxItem(1, 1) == 'Spectral Necklace of the Gorilla'"));
    CHECK(ui.is("strfind(GetInboxItemLink(1, 1), 'item:12047:111:3001:3002:3003:0:%-24:35:') ~= nil"));
    ui.run("tip = CreateFrame('GameTooltip', 'PlayTip', UIParent) "
           "tip:SetOwner(UIParent, 'ANCHOR_NONE') tip:SetInboxItem(1, 1)");
    const auto tip = [&] {
        std::string out;
        const auto* w = ui.engine.widgets().findByName("PlayTip");
        REQUIRE(w);
        for (const auto& line : w->tooltipLines) out += line.left + "|";
        return out;
    };
    INFO(tip());
    CHECK(tip().find("Spectral Necklace of the Gorilla|") == 0);
    CHECK(tip().find("17") != std::string::npos);
    // The same by a link in the chat, however its suffix is written.
    for (const char* suffix : {"-24", "65512"}) {
        ui.run(std::string("tip:ClearLines() tip:SetHyperlink('item:12047:0:0:0:0:0:") + suffix + ":35:80')");
        CHECK(tip().find("Spectral Necklace of the Gorilla|") == 0);
    }
    ui.run("tip:ClearLines() tip:SetHyperlink('item:12047:0:0:0:0:0:0:0:80')");
    CHECK(tip().find("Gorilla") == std::string::npos);
    // And one rolled for (0x00588c00, 0x005889d0).
    ui.run("local f = CreateFrame('Frame') f:RegisterEvent('START_LOOT_ROLL') "
           "f:SetScript('OnEvent', function(self, event, id) rollId = tonumber(id) end)");
    wowee::network::Packet roll(wowee::game::wireOpcode(Opcode::SMSG_LOOT_START_ROLL));
    roll.writeUInt64(0xF130000BEC00150Eull);
    for (uint32_t v : {s.handler().getCurrentMapId(), 0u, 12047u, 35u, static_cast<uint32_t>(-24), 1u, 60000u})
        roll.writeUInt32(v);
    roll.writeUInt8(0x07);
    s.receive(roll);
    REQUIRE(ui.is("rollId ~= nil"));
    CHECK(ui.is("strfind(GetLootRollItemLink(rollId), ':%-24:35:.*Gorilla') ~= nil"));
    CHECK(ui.is("select(2, GetLootRollItemInfo(rollId)) == 'Spectral Necklace of the Gorilla'"));
    // FIXER Q: the vote mask (0x07: pass, need, greed) gates the answer
    // (0x006fbc70): disenchant goes nowhere and the window stays.
    ui.run("cancelled = nil looted = {} local g = CreateFrame('Frame') g:RegisterEvent('CANCEL_LOOT_ROLL') "
           "g:RegisterEvent('CHAT_MSG_LOOT') g:SetScript('OnEvent', function(self, event, a) "
           "if event == 'CANCEL_LOOT_ROLL' then cancelled = tonumber(a) else looted[#looted + 1] = a end end)");
    ui.run("RollOnLoot(rollId, 3)");
    CHECK(s.sent().of(Opcode::CMSG_LOOT_ROLL).empty());
    CHECK(ui.is("cancelled == nil"));
    // Passed: sent once, the window down, but the roll still answers until it is decided.
    ui.run("RollOnLoot(rollId, 0)");
    CHECK(s.sent().of(Opcode::CMSG_LOOT_ROLL).size() == 1);
    CHECK(ui.is("cancelled == rollId"));
    CHECK(ui.is("select(2, GetLootRollItemInfo(rollId)) == 'Spectral Necklace of the Gorilla'"));
    ui.run("RollOnLoot(rollId, 0)");
    CHECK(s.sent().of(Opcode::CMSG_LOOT_ROLL).size() == 1);
    // All passed: the line's link carries the property and the seed (0x006fb470), and the roll is gone.
    wowee::network::Packet passed(wowee::game::wireOpcode(Opcode::SMSG_LOOT_ALL_PASSED));
    passed.writeUInt64(0xF130000BEC00150Eull);
    for (uint32_t v : {0u, 12047u, 35u, static_cast<uint32_t>(-24)}) passed.writeUInt32(v);
    s.receive(passed);
    CHECK(ui.is("#looted == 1 and strfind(looted[1], 'item:12047:0:0:0:0:0:%-24:35:') ~= nil "
                "and strfind(looted[1], 'Gorilla') ~= nil"));
    CHECK(ui.is("GetLootRollItemInfo(rollId) == nil"));
    // And in a loot window: named and linked with what it rolled (0x00588430, 0x005884a0).
    wowee::network::Packet loot(wowee::game::wireOpcode(Opcode::SMSG_LOOT_RESPONSE));
    loot.writeUInt64(0xF130000BEC00150Full);
    loot.writeUInt8(1);
    loot.writeUInt32(0);
    loot.writeUInt8(1);
    loot.writeUInt8(0);
    for (uint32_t v : {12047u, 1u, 0u, 35u, static_cast<uint32_t>(-24)}) loot.writeUInt32(v);
    loot.writeUInt8(0);
    s.receive(loot);
    REQUIRE(ui.is("GetNumLootItems() == 1"));
    CHECK(ui.is("select(2, GetLootSlotInfo(1)) == 'Spectral Necklace of the Gorilla'"));
    CHECK(ui.is("strfind(GetLootSlotLink(1), 'item:12047:0:0:0:0:0:%-24:35:.*Gorilla') ~= nil"));
}

// P54: GetCompanionInfo answers the creature (0x0053e2c0), and SetCreature
// shows it once its template is known (0x00597960).
TEST_CASE("a model frame shows the creature it is given once that is known", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("model = CreateFrame('PlayerModel', 'PlayModel', UIParent) model:SetCreature(4269)");
    auto* w = ui.engine.widgets().findByName("PlayModel");
    REQUIRE(w);
    const auto shown = [&] {
        return w->shownDisplay([&](uint32_t entry) { return s.handler().getCreatureDisplayIdForEntry(entry); });
    };
    CHECK(shown() == 0);
    CHECK_FALSE(s.sent().of(Opcode::CMSG_CREATURE_QUERY).empty());
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_CREATURE_QUERY_RESPONSE));
    p.writeUInt32(4269);
    p.writeString("Chestnut Mare");
    for (int i = 0; i < 5; ++i) p.writeString("");
    for (uint32_t v : {0u, 0u, 0u, 0u, 0u, 0u, 2405u, 0u, 0u, 0u}) p.writeUInt32(v);
    for (int i = 0; i < 20; ++i) p.writeUInt32(0);
    s.receive(p);
    CHECK(shown() == 2405);
    // Known now, it is shown at once; the stable's paperdoll takes its place.
    ui.run("model:SetCreature(4270)");
    CHECK(shown() == 0);
    ui.run("model:SetCreature(4269)");
    CHECK(shown() == 2405);
}

// P55: 0x006301a0 describes the item by its object, not by its template - without what it is
// bound to and how worn it is, as its tenth argument says.
TEST_CASE("the socketing window describes the item itself", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    rollsGorilla(s);
    answerItem(s, 24021, "Light-Touched Breastplate");
    constexpr uint64_t kPlate = 0x4000000000000777ull;
    wowee::game::ItemDef item;
    item.guid = kPlate;
    item.itemId = 24021;
    item.soulbound = true;
    item.curDurability = 120;
    item.maxDurability = 135;
    item.randomPropertyId = -24;
    s.handler().getInventory().setBackpackSlot(3, item);
    s.handler().onlineItemsRef()[kPlate].entry = 24021;
    s.handler().openSocketing(kPlate);
    REQUIRE(s.handler().getSocketItemId() == 24021);
    ui.run("ITEM_SOULBOUND = 'Soulbound' DURABILITY_TEMPLATE = 'Durability %d / %d' "
           "tip = CreateFrame('GameTooltip', 'PlayTip', UIParent) "
           "tip:SetOwner(UIParent, 'ANCHOR_NONE') tip:SetSocketedItem()");
    std::string all;
    const auto* w = ui.engine.widgets().findByName("PlayTip");
    REQUIRE(w);
    for (const auto& line : w->tooltipLines) all += line.left + "|";
    INFO(all);
    CHECK(all.find("Light-Touched Breastplate of the Gorilla|") == 0);
    CHECK(all.find("Soulbound|") == std::string::npos);
    CHECK(all.find("120") == std::string::npos);
}

// T282: the enchants' walk of 0x006277f0 is before the price, which FrameXML hangs on the last
// line there is then; what an item rolled came after it, and after OnTooltipSetItem.
TEST_CASE("what an item rolled is before its price", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    rollsGorilla(s);
    answerItem(s, 24021, "Light-Touched Breastplate", 163793);
    wowee::game::ItemDef item;
    item.guid = 0x4000000000000777ull;
    item.itemId = 24021;
    item.name = "Light-Touched Breastplate";
    item.stackCount = 1;
    item.randomPropertyId = -24;
    s.handler().getInventory().setBackpackSlot(3, item);
    s.handler().onlineItemsRef()[item.guid].entry = 24021;
    ui.run("ITEM_MOD_STRENGTH = '%c%d Strength' tip = CreateFrame('GameTooltip', 'PlayRollTip', UIParent) "
           "tip:SetOwner(UIParent, 'ANCHOR_NONE') "
           "tip:SetScript('OnTooltipAddMoney', function(self, m) money, lines = m, self:NumLines() end) "
           "tip:SetScript('OnTooltipSetItem', function(self) ended = self:NumLines() end) ");
    const auto* w = ui.engine.widgets().findByName("PlayRollTip");
    REQUIRE(w);
    const auto text = [&] {
        std::string all;
        for (const auto& line : w->tooltipLines) all += line.left + "|";
        return all;
    };
    for (const char* shown : {"tip:SetBagItem(0, 4)", "tip:SetHyperlink('item:24021:0:0:0:0:0:-24:35')"}) {
        ui.run(std::string("money, lines, ended = nil tip:SetOwner(UIParent, 'ANCHOR_NONE') ") + shown);
        INFO(shown << " " << text());
        CHECK(text().find("Light-Touched Breastplate of the Gorilla|") == 0);
        CHECK(text().find("17 Strength|") != std::string::npos);
        CHECK(ui.is("money == 163793 and lines == tip:NumLines() and ended == lines"));
    }
    // A second time it is named once, and another item is not.
    ui.run("tip:SetBagItem(0, 4) tip:SetBagItem(0, 4)");
    CHECK(text().find("of the Gorilla of") == std::string::npos);
    answerItem(s, 24022, "Scale Leggings", 5);
    ui.run("tip:SetOwner(UIParent, 'ANCHOR_NONE') tip:SetHyperlink('item:24022:0:0:0:0:0:0:0')");
    CHECK(text().find("Scale Leggings|") == 0);
    CHECK(text().find("Strength") == std::string::npos);
}

// 0x0062f420 / 0x0062e050 hand on what 0x006277f0 answers - 1 for a line that counts down
// (a lock, a duration, an enchant's time, a cooldown) - and each has its own shapes;
// 0x0062dae0 and 0x006304a0 answer nothing.
TEST_CASE("the item tooltip setters answer as the client's do", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    answerItem(s, 24021, "Light-Touched Breastplate", 5);
    wowee::game::ItemDef item;
    item.guid = 0x4000000000000777ull;
    item.itemId = 24021;
    item.name = "Light-Touched Breastplate";
    item.stackCount = 1;
    s.handler().getInventory().setBackpackSlot(3, item);
    s.handler().onlineItemsRef()[item.guid].entry = 24021;
    ui.run("tip = CreateFrame('GameTooltip', 'PlayAnswerTip', UIParent) tip:SetOwner(UIParent, 'ANCHOR_NONE') "
           "function answers(...) return select('#', ...), ... end");
    // A plain item: nothing counts down, and the repair cost.
    CHECK(ui.is("select('#', tip:SetBagItem(0, 4)) == 2"));
    CHECK(ui.is("tip:SetBagItem(0, 4) == nil"));
    CHECK(ui.is("select(2, tip:SetBagItem(0, 4)) == 0"));
    // An empty slot or one the bag does not have: one nil. No bag there, or one
    // out of range: nothing.
    CHECK(ui.is("answers(tip:SetBagItem(0, 5)) == 1"));
    CHECK(ui.is("answers(tip:SetBagItem(0, 40)) == 1"));
    CHECK(ui.is("answers(tip:SetBagItem(1, 1)) == 0"));
    CHECK(ui.is("answers(tip:SetBagItem(-1, 1)) == 0"));
    CHECK(ui.is("answers(tip:SetBagItem(12, 1)) == 0"));
    CHECK(ui.is("answers(tip:SetBagItem('x', 1)) == 0"));
    // An item that lasts a while has a line that counts down.
    s.handler().itemInfoCacheRef()[24021].itemDuration = 3600;
    CHECK(ui.is("tip:SetBagItem(0, 4) == 1"));
    // SetInventoryItem: nil, nil, 0 for an empty slot; 1, 1, cost when worn.
    ui.run("n, a, b, c = answers(tip:SetInventoryItem('player', 1))");
    CHECK(ui.is("n == 3 and a == nil and b == nil and c == 0"));
    REQUIRE(s.handler().getInventory().setEquipSlot(wowee::game::EquipSlot::CHEST, item));
    ui.run("n, a, b, c = answers(tip:SetInventoryItem('player', 5))");
    CHECK(ui.is("n == 3 and a == 1 and b == 1 and c == 0"));
    s.handler().itemInfoCacheRef()[24021].itemDuration = 0;
    ui.run("n, a, b, c = answers(tip:SetInventoryItem('player', 5))");
    CHECK(ui.is("n == 3 and a == 1 and b == nil and c == 0"));
    // The ammo's two answers; the slots it refuses, and a unit that is not a text.
    CHECK(ui.is("answers(tip:SetInventoryItem('player', 0)) == 2"));
    CHECK(ui.is("not pcall(tip.SetInventoryItem, tip, 'player', 30)"));
    CHECK(ui.is("not pcall(tip.SetInventoryItem, tip, 'player', 'x')"));
    CHECK(ui.is("not pcall(tip.SetInventoryItem, tip, nil, 1)"));
    CHECK(ui.is("pcall(tip.SetInventoryItem, tip, 'player', 150)"));
    // SetHyperlink and SetGuildBankItem answer nothing, whatever they drew.
    ui.run("tip:SetOwner(UIParent, 'ANCHOR_NONE')");
    CHECK(ui.is("answers(tip:SetHyperlink('item:24021')) == 0"));
    CHECK(ui.is("tip:NumLines() > 0"));
    CHECK(ui.is("answers(tip:SetGuildBankItem(1, 1)) == 0"));
    CHECK(ui.is("not pcall(tip.SetGuildBankItem, tip, 'x', 1)"));
}

// 0x0051c2e0 with includeCharges: an item that does not stack counts the charges left on
// it where its template's are below nought (0x00754d50), and then the casts its spell's
// reagents allow (0x00801770 -> 0x007ffa30).
TEST_CASE("GetItemCount with the charges counts charges and reagent casts", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    constexpr uint32_t kRod = 90001, kDust = 90002, kSpell = 90003;
    answerItem(s, kRod, "Rod");
    answerItem(s, kDust, "Dust");
    auto& rod = s.handler().itemInfoCacheRef()[kRod];
    rod.maxStack = 1;
    rod.spells[0].spellId = kSpell;
    rod.spells[0].spellTrigger = 0;
    rod.spells[0].charges = -3;
    for (int i = 0; i < 2; ++i) {
        wowee::game::ItemDef item;
        item.guid = 0x4000000000000900ull + static_cast<uint64_t>(i);
        item.itemId = kRod;
        item.stackCount = 1;
        s.handler().getInventory().setBackpackSlot(i, item);
        s.handler().onlineItemsRef()[item.guid].entry = kRod;
        s.handler().onlineItemsRef()[item.guid].spellCharges0 = -2;
    }
    CHECK(ui.is("GetItemCount(90001) == 2"));
    CHECK(ui.is("GetItemCount(90001, nil, true) == 4"));
    // With reagents: five of two needed is two casts.
    s.handler().spellNameCacheRef()[kSpell].reagents[0] = {kDust, 2};
    wowee::game::ItemDef dust;
    dust.guid = 0x4000000000000910ull;
    dust.itemId = kDust;
    dust.stackCount = 5;
    s.handler().getInventory().setBackpackSlot(5, dust);
    CHECK(ui.is("GetItemCount(90001, nil, true) == 2"));
    CHECK(ui.is("GetItemCount(90001) == 2"));
    // Too few for one: the count stays.
    s.handler().spellNameCacheRef()[kSpell].reagents[0] = {kDust, 9};
    CHECK(ui.is("GetItemCount(90001, nil, true) == 4"));
}

// T161: 0x006277f0, +0x480: what the item that is not traded would be enchanted with.
TEST_CASE("an item in a trade says what it would be enchanted with", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    answerItem(s, 24021, "Light-Touched Breastplate", 5);
    ui.run("GetTradePlayerItemLink = function() return 'item:24021:0:0:0:0:0:0:0' end "
           "GetTradeTargetItemLink = GetTradePlayerItemLink "
           "GetTradePlayerItemInfo = function(i) return 'x', 't', 1, 2, i == 7 and 'Crusader' or nil end "
           "GetTradeTargetItemInfo = function(i) return 'x', 't', 1, 2, 1, i == 7 and 'Mongoose' or nil end "
           "ITEM_PROPOSED_ENCHANT, ITEM_ENCHANT_DISCLAIMER = 'Will receive %s.', 'Not traded!' "
           "tip = CreateFrame('GameTooltip', 'PlayTradeTip', UIParent) tip:SetOwner(UIParent, 'ANCHOR_NONE')");
    const auto* w = ui.engine.widgets().findByName("PlayTradeTip");
    REQUIRE(w);
    const auto after = [&](const char* call) {
        ui.run(std::string("tip:SetOwner(UIParent, 'ANCHOR_NONE') ") + call);
        std::string all;
        for (const auto& line : w->tooltipLines) all += line.left + "|";
        return all;
    };
    CHECK(after("tip:SetTradePlayerItem(7)").find("|Will receive Crusader.|Not traded!|") != std::string::npos);
    CHECK(after("tip:SetTradeTargetItem(7)").find("|Will receive Mongoose.|Not traded!|") != std::string::npos);
    // Who made the partner's, and its state (0x0062f1e0): an item and not its template.
    ui.run("ITEM_CREATED_BY, ITEM_WRAPPED_BY, DURABILITY_TEMPLATE = '<by %s>', '<gift of %s>', 'dur %d / %d' "
           "__WoweeTradeTargetInstance = function(i) "
           "  return i == 1 and {creator = 'Ann', charges0 = -3, maxDurability = 50, durability = 7} "
           "    or i == 2 and {wrappedBy = 'Bob', charges0 = 0} or nil end");
    CHECK(after("tip:SetTradeTargetItem(1)").find("|dur 7 / 50|<by Ann>|") != std::string::npos);
    const std::string gift = after("tip:SetTradeTargetItem(2)");
    CHECK(gift.find("|<gift of Bob>|") != std::string::npos);
    CHECK(gift.find("dur") == std::string::npos);
    CHECK(after("tip:SetTradeTargetItem(3)").find('<') == std::string::npos);
    CHECK(after("tip:SetTradePlayerItem(1)").find('<') == std::string::npos);
    CHECK(ui.is("__WoweeTooltipInst == nil"));
    CHECK(after("tip:SetTradeTargetItem(1)").find("receive") == std::string::npos);
    CHECK(after("tip:SetHyperlink('item:24021')").find("receive") == std::string::npos);
}

// P61: 0x005980d0 goes by the slot an item takes, not by its kind.
TEST_CASE("the dressing room puts an item where it goes", "[replay][interface]") {
    namespace room = wowee::game::dressing_room;
    using Hand = wowee::game::EquipmentItem::Hand;
    std::array<room::Item, 19> own{};
    own[4] = {.itemId = 50, .display = 504, .inventoryType = 20};
    own[15] = {.itemId = 51, .display = 515, .inventoryType = 21};
    own[16] = {.itemId = 52, .display = 516, .inventoryType = 23};
    own[17] = {.itemId = 53, .display = 517, .inventoryType = 28};
    bool dual = false;
    std::vector<uint32_t> grips;
    const room::Hands hands{.dualWield = [&] { return dual; },
                            .fitsOffHand = [&](uint32_t id) {
                                return std::find(grips.begin(), grips.end(), id) != grips.end();
                            }};
    using Tried = std::vector<room::Item>;
    const auto figure = [&](const Tried& tried, bool undressed = false) {
        std::string out;
        for (const auto& it : room::dressed(own, undressed, tried, hands)) {
            out += (it.hand == Hand::Main ? "main " : it.hand == Hand::Off ? "off " : "") +
                   std::to_string(it.displayModel) + ";";
        }
        return out;
    };
    const room::Item chest{.itemId = 1, .display = 1, .inventoryType = 5};
    const room::Item claymore{.itemId = 2, .display = 2, .inventoryType = 17};
    const room::Item greataxe{.itemId = 3, .display = 3, .inventoryType = 17};
    const room::Item sword{.itemId = 4, .display = 4, .inventoryType = 13};
    const room::Item axe{.itemId = 5, .display = 5, .inventoryType = 13};
    const room::Item shield{.itemId = 6, .display = 6, .inventoryType = 14};
    const room::Item gun{.itemId = 7, .display = 7, .inventoryType = 26};
    const room::Item bow{.itemId = 8, .display = 8, .inventoryType = 15};
    // The ranged one is left out (0x005989e0).
    CHECK(figure({}) == "504;main 515;off 516;");
    CHECK(figure({chest}) == "1;main 515;off 516;");
    // Two hands empty the other; a shield after it empties the two-hander's.
    CHECK(figure({claymore}) == "504;main 2;");
    CHECK(figure({claymore, shield}) == "504;off 6;");
    CHECK(figure({claymore, shield, sword}) == "504;main 4;off 6;");
    CHECK(figure({shield}) == "504;main 515;off 6;");
    // A gun is held in the right hand, a bow in the left, each alone.
    CHECK(figure({gun}) == "504;main 7;");
    CHECK(figure({bow}) == "504;off 8;");
    // Without the second weapon both go in the main hand, with it in turn.
    CHECK(figure({sword, axe}) == "504;main 5;off 516;");
    dual = true;
    CHECK(figure({sword}) == "504;main 4;off 516;");
    CHECK(figure({sword, axe}) == "504;main 4;off 5;");
    CHECK(figure({sword, axe, sword}) == "504;main 4;off 5;");
    CHECK(figure({sword, axe, sword, sword}) == "504;main 4;off 4;");
    // Titan's grip holds two.
    grips = {2, 3};
    CHECK(figure({claymore, greataxe}) == "504;main 2;off 3;");
    CHECK(figure({claymore, shield}) == "504;main 2;off 6;");
    // Undressed, it is bare under what comes after.
    CHECK(figure({sword}, true) == "main 4;");
    // What the model shows of them: each in its hand, not one of each kind.
    std::vector<wowee::core::PreviewItem> both = {
        {.display = 4, .inventoryType = 13, .hand = wowee::core::PreviewItem::Hand::Main},
        {.display = 5, .inventoryType = 13, .hand = wowee::core::PreviewItem::Hand::Off}};
    const auto held = wowee::core::wornDress(both).held;
    REQUIRE(held.size() == 2);
    CHECK(held[0].point == wowee::core::attachment::kHandRight);
    CHECK(held[1].point == wowee::core::attachment::kHandLeft);
    // The same of a character window's, which knows the slots.
    std::array<uint32_t, 19> ids{};
    std::array<uint8_t, 19> types{};
    ids[15] = 4;
    ids[16] = 5;
    types[15] = types[16] = 13;
    const auto worn = wowee::game::player_display::wornByPlayer(true, ids, types, {}, 0);
    REQUIRE(worn.size() == 2);
    CHECK(worn[0].hand == Hand::Main);
    CHECK(worn[1].hand == Hand::Off);
    // T281: and what hangs on them, the unit's and the tried on's.
    const auto glowing = wowee::game::player_display::wornByPlayer(true, ids, types, {}, 0, {28, 61});
    CHECK(glowing[0].enchantment == 28);
    CHECK(glowing[1].enchantment == 61);
    const auto slots = wowee::game::player_display::wornBySlot(true, ids, types, {}, 0, {28, 61});
    CHECK(slots[15].enchantment == 28);
    CHECK(slots[16].enchantment == 61);
    own[15].visual = 28;
    room::Item lit = sword;
    lit.visual = 164;
    for (const auto& it : room::dressed(own, false, {}, hands))
        if (it.hand == Hand::Main) CHECK(it.enchantment == 28);
    auto tried = room::dressed(own, true, {lit}, hands);
    REQUIRE(tried.size() == 1);
    CHECK(tried[0].enchantment == 164);
    tried = room::dressed(own, true, {lit, axe}, hands);   // in its place
    REQUIRE(tried.size() == 1);
    CHECK(tried[0].enchantment == 0);
    both[0].enchant = 164;
    CHECK(wowee::core::wornDress(both).held[0].enchant == 164);

    // 0x007082b0: two hands in one only with the grip that names the kind.
    const auto grip = [](uint32_t c, uint32_t sub) { return c == 2 && sub == 8; };
    CHECK(room::itemFitsOffHand(2, 8, 17, 0, grip));
    CHECK_FALSE(room::itemFitsOffHand(2, 10, 17, 0, grip));
    CHECK_FALSE(room::itemFitsOffHand(2, 8, 17, 0x40000000u, grip));
    CHECK(room::itemFitsOffHand(4, 6, 14, 0, grip));
    CHECK_FALSE(room::itemFitsOffHand(2, 2, 15, 0, grip));
}

// P61: a recipe is what it makes, and an item not known yet is tried on as
// the answer comes (0x005980b0).
TEST_CASE("trying on a recipe, a stranger and a ring", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    auto& gh = s.handler();
    const auto know = [&](uint32_t id, uint32_t display, uint32_t type) -> auto& {
        auto& item = gh.itemInfoCacheRef()[id];
        item.entry = id;
        item.displayInfoId = display;
        item.inventoryType = type;
        return item;
    };
    ui.run("room = CreateFrame('DressUpModel', 'PlayDressUp', UIParent)");
    auto* w = ui.engine.widgets().findByName("PlayDressUp");
    REQUIRE(w);
    know(700, 70, 5);
    gh.spellNameCacheRef()[9001].effectIds[0] = 36;
    gh.spellNameCacheRef()[9002].effectIds[0] = 24;
    gh.spellNameCacheRef()[9002].effectItemType0 = 700;
    auto& recipe = know(701, 71, 0);
    recipe.spells[0].spellId = 9001;
    recipe.spells[1].spellId = 9002;
    recipe.spells[1].spellTrigger = 6;
    ui.run("room:TryOn(701)");
    REQUIRE(w->tryOnItems.size() == 1);
    CHECK(w->tryOnItems[0].displayInfoId == 70);
    know(702, 72, 11);
    ui.run("room:TryOn(702)");
    CHECK(w->tryOnItems.size() == 1);
    ui.run("room:TryOn('item:703:0:0')");
    CHECK(w->tryOnItems.size() == 1);
    wowee::addons::tryOnAskedItems(*w, &gh);
    CHECK(w->tryOnItems.size() == 1);
    know(703, 73, 17);
    wowee::addons::tryOnAskedItems(*w, &gh);
    REQUIRE(w->tryOnItems.size() == 2);
    CHECK(w->tryOnItems[1].displayInfoId == 73);
    CHECK(w->tryOnItems[1].itemId == 703);
    wowee::addons::tryOnAskedItems(*w, &gh);
    CHECK(w->tryOnItems.size() == 2);
}

// P67: 0x00966710 makes a multi-line box as tall as its lines, which is what its scroll frame
// scrolls; 0x00964a90 tells the caret's place from where the text begins.
TEST_CASE("a multi-line box is as tall as its text", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("box = CreateFrame('EditBox', 'PlayMacroText', UIParent) box:SetMultiLine(true) "
           "box:SetWidth(200) box:SetHeight(103) box:SetPoint('TOPLEFT') box:SetTextInsets(0, 0, 0, 0) "
           "box:SetScript('OnCursorChanged', function(self, x, y) cx, cy = x, y end) "
           "box:SetText('/one') ");
    ui.layout();
    ui.engine.dispatchOnUpdate(0.05f);
    ui.run("one = box:GetHeight() oneY = cy");
    CHECK(ui.is("one < 60"));
    CHECK(ui.is("oneY == 0"));
    ui.run("box:SetText('/one\\n/two\\n/three\\n/four')");
    ui.engine.dispatchOnUpdate(0.05f);
    ui.layout();
    CHECK(ui.is("math.abs(box:GetHeight() - 4 * one) < 0.01"));
    CHECK(ui.is("math.abs(cy + 3 * one) < 0.01"));
    CHECK(ui.is("cx > 0"));
    // A long line breaks at the box's width.
    ui.run("box:SetText(('word '):rep(40))");
    ui.engine.dispatchOnUpdate(0.05f);
    ui.layout();
    CHECK(ui.is("box:GetHeight() > 4 * one"));
    // One line of another box stays as tall as it was made.
    ui.run("line = CreateFrame('EditBox', 'PlayLine', UIParent) line:SetWidth(200) line:SetHeight(20) "
           "line:SetPoint('TOPLEFT') line:SetText('word word')");
    ui.engine.dispatchOnUpdate(0.05f);
    CHECK(ui.is("line:GetHeight() == 20"));
}

// P76: the stack splitter runs the binding of each key it does not use, press and release; a
// release runs only a binding declared runOnUp (0x0055f860).
TEST_CASE("RunBinding's release runs a binding that asks for it alone", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("opened, walked = {}, {} "
           "__WoweeBindingScripts = {TOGGLECHARACTER0 = function(k) opened[#opened + 1] = k end, "
           "  MOVEFORWARD = function(k) walked[#walked + 1] = k end} "
           "__WoweeBindingRunOnUp = {MOVEFORWARD = true}");
    ui.run("RunBinding('TOGGLECHARACTER0') RunBinding('TOGGLECHARACTER0', 'up') RunBinding('TOGGLECHARACTER0', 'UP')");
    CHECK(ui.is("#opened == 1 and opened[1] == 'down'"));
    ui.run("RunBinding('MOVEFORWARD') RunBinding('MOVEFORWARD', 'up') RunBinding('MOVEFORWARD', 'Up')");
    CHECK(ui.is("#walked == 3 and walked[1] == 'down' and walked[2] == 'up' and walked[3] == 'up'"));
    // Anything else is a press, and no command is the client's own error.
    ui.run("RunBinding('TOGGLECHARACTER0', 'sideways')");
    CHECK(ui.is("#opened == 2 and opened[2] == 'down'"));
    CHECK(ui.is("(function() local ok, e = pcall(RunBinding) return not ok and e:find('Usage: RunBinding') ~= nil end)()"));
    ui.run("RunBinding('') RunBinding('NOSUCH') RunBinding('NOSUCH', 'up')");
}

// P75: the character walked and jumped under the open world map. A key a frame takes goes
// nowhere else (0x004943c0), its repeats and its release to the same frame (0x00494530, 0x00494490).
TEST_CASE("a key a frame takes is nobody else's until it is let go of", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("down, up = 0, 0 f = CreateFrame('Frame', 'MapFrame') f:SetAllPoints() f:EnableKeyboard(true) "
           "f:SetScript('OnKeyDown', function() down = down + 1 end) "
           "f:SetScript('OnKeyUp', function() up = up + 1 end) f:Hide()");
    ui.layout();
    // Pressed before the frame is up, the key is the game's, its repeats and its release too.
    CHECK_FALSE(ui.engine.dispatchFrameKey('w', true));
    ui.run("f:Show()");
    ui.layout();
    CHECK_FALSE(ui.engine.dispatchFrameKey('w', true, true));
    CHECK_FALSE(ui.engine.dispatchFrameKey('w', false));
    CHECK(ui.is("down == 0 and up == 0"));
    // Pressed under it, it is the frame's: once, however long it is held.
    CHECK(ui.engine.dispatchFrameKey('w', true));
    CHECK(ui.engine.dispatchFrameKey('w', true, true));
    CHECK(ui.engine.dispatchFrameKey('w', true, true));
    CHECK(ui.is("down == 1 and up == 0"));
    CHECK(ui.engine.dispatchFrameKey('w', false));
    CHECK(ui.is("down == 1 and up == 1"));

    // What reads the keyboard itself does not see it either, until it is let go of.
    auto& input = wowee::core::Input::getInstance();
    input.setVirtualKey(SDL_SCANCODE_W, true);
    input.setKeySpent(SDL_SCANCODE_W);
    input.update();
    CHECK_FALSE(input.isKeyPressed(SDL_SCANCODE_W));
    CHECK_FALSE(input.isKeyJustPressed(SDL_SCANCODE_W));
    input.update();
    CHECK_FALSE(input.isKeyPressed(SDL_SCANCODE_W));
    input.setVirtualKey(SDL_SCANCODE_W, false);
    input.update();
    input.setVirtualKey(SDL_SCANCODE_W, true);
    input.update();
    CHECK(input.isKeyJustPressed(SDL_SCANCODE_W));
    // A modifier is asked about as it stands.
    input.setVirtualKey(SDL_SCANCODE_LSHIFT, true);
    input.setKeySpent(SDL_SCANCODE_LSHIFT);
    input.update();
    CHECK(input.isKeyPressed(SDL_SCANCODE_LSHIFT));
    input.clearVirtualKeys();
    input.update();
}

// P77: a scroll frame's range is the one it has told of (0x0096b4c0), so what scrolls by it and
// its scroll bar go by the same one - the macro window, whose box grows with its text.
TEST_CASE("a scroll frame answers the range it has told of", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("told = {} sf = CreateFrame('ScrollFrame', 'PlayScroll', UIParent) sf:SetWidth(200) sf:SetHeight(50) "
           "sf:SetPoint('TOPLEFT') bar = CreateFrame('Slider', 'PlayScrollScrollBar', sf) "
           "bar:SetMinMaxValues(0, 0) bar:SetValue(0) "
           "bar:SetScript('OnValueChanged', function(self, v) sf:SetVerticalScroll(v) end) "
           "sf:SetScript('OnVerticalScroll', function(self, v) bar:SetValue(v) end) "
           "sf:SetScript('OnScrollRangeChanged', function(self, x, y) told[#told + 1] = y "
           "  local v = bar:GetValue() if v > y then v = y end bar:SetMinMaxValues(0, y) bar:SetValue(v) end) "
           "box = CreateFrame('EditBox', 'PlayScrollText', sf) box:SetMultiLine(true) box:SetWidth(200) "
           "box:SetHeight(50) box:SetTextInsets(0, 0, 0, 0) sf:SetScrollChild(box)");
    const auto pass = [&ui] {
        ui.engine.dispatchOnUpdate(0.05f);
        ui.layout();
        ui.engine.updateVisibility();
        ui.engine.updateScrollRanges();
    };
    pass();
    CHECK(ui.is("sf:GetVerticalScrollRange() == 0"));
    ui.run("box:SetText(('/line\\n'):rep(9))");
    pass();
    REQUIRE(ui.is("#told > 0 and told[#told] > 20"));
    ui.run("n, range = #told, told[#told]");
    CHECK(ui.is("sf:GetVerticalScrollRange() == range"));
    // Scrolled to the end, the bar has it.
    ui.run("sf:SetVerticalScroll(range)");
    CHECK(ui.is("math.abs(bar:GetValue() - range) < 0.01"));
    // The text grown: the range is the old one until it is told of, and so is the bar's.
    ui.run("box:SetText(('/line\\n'):rep(20))");
    ui.engine.dispatchOnUpdate(0.05f);
    ui.layout();
    CHECK(ui.is("#told == n and sf:GetVerticalScrollRange() == range"));
    pass();
    CHECK(ui.is("#told == n + 1 and told[#told] > range and sf:GetVerticalScrollRange() == told[#told]"));
    // And shrunk, the window is back over what there is.
    ui.run("sf:SetVerticalScroll(told[#told]) box:SetText('.die')");
    pass();
    pass();
    CHECK(ui.is("sf:GetVerticalScrollRange() == 0"));
    CHECK(ui.is("bar:GetValue() == 0 and sf:GetVerticalScroll() == 0"));
}

// 0x00967cd0: a press puts the caret where it is.
TEST_CASE("a press in an edit box puts the caret there", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("box = CreateFrame('EditBox', 'PlayPressText', UIParent) box:SetMultiLine(true) box:SetWidth(300) "
           "box:SetHeight(100) box:SetPoint('TOPLEFT') box:SetTextInsets(10, 0, 5, 0) "
           "box:SetScript('OnCursorChanged', function(self, x, y, w, h) cx, cy, ch = x, y, h end) "
           "box:SetText('aaaa\\nbbbb\\ncccc')");
    ui.layout();
    ui.engine.dispatchOnUpdate(0.05f);
    ui.layout();
    ui.run("box:HighlightText()");
    auto& tree = ui.engine.widgets();
    auto* box = tree.findByName("PlayPressText");
    REQUIRE(box != nullptr);
    const uint32_t id = box->id;
    const float top = box->bottom + box->rectH, left = box->left;
    const float line = (box->rectH - 5.0f) / 3.0f;
    ui.engine.pressInEditBox(id, left + 10.5f, top - 5.0f - line * 1.5f);
    CHECK(tree.get(id)->cursorPos == 5);
    CHECK_FALSE(tree.get(id)->hasSelection);
    CHECK(ui.is("cy < 0 and cx == 0"));
    ui.engine.pressInEditBox(id, left + 290.0f, top - 5.0f - line * 1.5f);
    CHECK(tree.get(id)->cursorPos == 9);
    ui.engine.pressInEditBox(id, left + 290.0f, top - 5.0f - line * 9.0f);
    CHECK(tree.get(id)->cursorPos == 14);
    // Left of the text it is at the text's start, whatever the line.
    ui.engine.pressInEditBox(id, left + 2.0f, top - 5.0f - line * 1.5f);
    CHECK(tree.get(id)->cursorPos == 0);

    // 0x00965660: held and moved it selects, forward and back past where the
    // press was, as long as the pointer is over the box.
    const auto selects = [&](size_t from, size_t to, size_t caret) {
        const auto* w = tree.get(id);
        return w->hasSelection && w->selStart == from && w->selEnd == to && w->cursorPos == caret;
    };
    const float second = top - 5.0f - line * 1.5f;
    wowee::addons::LuaEngine::MouseButtons held;
    held.left = true;
    const float uiScale = tree.uiScale() > 0.0f ? tree.uiScale() : 1.0f;
    const auto mouse = [&](float x, float y, const wowee::addons::LuaEngine::MouseButtons& buttons) {
        ui.engine.dispatchMouse(x * uiScale, 768.0f - y * uiScale, 768.0f, buttons);
    };
    mouse(left + 10.5f, second, held);
    CHECK(tree.get(id)->cursorPos == 5);
    CHECK_FALSE(tree.get(id)->hasSelection);
    mouse(left + 290.0f, second, held);
    CHECK(selects(5, 9, 9));
    mouse(left + 290.0f, top - 5.0f - line * 2.5f, held);
    CHECK(selects(5, 14, 14));
    mouse(left + 400.0f, second, held);
    CHECK(selects(5, 14, 14));
    mouse(left + 290.0f, top - 5.0f - line * 0.5f, held);
    // Past the press at a bound, the end that was dragged stays (0x009631a0).
    CHECK(selects(4, 14, 4));
    // Let go of, the pointer moves nothing.
    mouse(left + 290.0f, top - 5.0f - line * 0.5f, {});
    mouse(left + 290.0f, second, {});
    CHECK(selects(4, 14, 4));
    // A press lets it go and begins anew.
    mouse(left + 10.5f, second, held);
    CHECK_FALSE(tree.get(id)->hasSelection);
}

namespace {

/// Faction.dbc as 3.3.5 has it, 57 fields: each row an id, its reputation list
/// entry, the faction it is under, a name and what is said of it.
wowee::pipeline::DBCFile factionTable(
    const std::vector<std::tuple<uint32_t, uint32_t, uint32_t, std::string, std::string>>& rows) {
    std::vector<uint8_t> data;
    const auto u32 = [&data](uint32_t v) {
        for (int k = 0; k < 4; ++k) data.push_back(static_cast<uint8_t>(v >> (8 * k)));
    };
    std::string strings(1, '\0');
    std::vector<std::array<uint32_t, 57>> fields;
    for (const auto& [id, list, parent, name, description] : rows) {
        auto& f = fields.emplace_back();
        f.fill(0);
        f[0] = id;
        f[1] = list;
        f[18] = parent;
        f[23] = static_cast<uint32_t>(strings.size());
        strings += name + '\0';
        if (!description.empty()) {
            f[40] = static_cast<uint32_t>(strings.size());
            strings += description + '\0';
        }
    }
    for (const char c : {'W', 'D', 'B', 'C'}) data.push_back(static_cast<uint8_t>(c));
    u32(static_cast<uint32_t>(fields.size()));
    u32(57);
    u32(57 * 4);
    u32(static_cast<uint32_t>(strings.size()));
    for (const auto& f : fields)
        for (const uint32_t v : f) u32(v);
    data.insert(data.end(), strings.begin(), strings.end());
    wowee::pipeline::DBCFile dbc;
    REQUIRE(dbc.load(data));
    return dbc;
}

// Burning Crusade (980) with Shattrath (936) and two factions in it, Thrallmar
// right in it, and the Hydraxian Waterlords under nothing.
struct Reputation : Interface {
    explicit Reputation(Session& s)
        : Reputation(s, factionTable({{980, 0xFFFFFFFFu, 0, "The Burning Crusade", ""},
                                                   {936, 4, 980, "Shattrath City", ""},
                                                   {942, 0, 936, "Cenarion Expedition", "An expedition."},
                                                   {932, 1, 936, "The Aldor", ""},
                                                   {947, 2, 980, "Thrallmar", "Sent by Thrall."},
                                                   {749, 3, 0, "Hydraxian Waterlords", ""}}), 5) {
        s.handler().initialFactionsRef()[4].flags = 0;
    }
    Reputation(Session& s, const wowee::pipeline::DBCFile& table, int standings) : Interface(s) {
        s.handler().readFactionTable(table);
        for (int i = 0; i < standings; ++i) s.handler().initialFactionsRef().push_back({0x1, 100});
    }
    /// The rows as "name", "+name" for a heading, ">" before a child.
    bool rowsAre(const std::string& want) {
        run("text = '' for i = 1, GetNumFactions() do "
            "local name, _, _, _, _, _, _, _, isHeader, collapsed, _, _, isChild = GetFactionInfo(i) "
            "text = text .. (isChild and '>' or '') .. (isHeader and (collapsed and '-' or '+') or '') .. name .. ';' end");
        return is("text == '" + want + "'");
    }
};

}  // namespace

// P72: 0x005d0da0 answers the row's description, +0x60.
TEST_CASE("a faction's description is told", "[replay][interface][reputation]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Reputation ui(s);
    ui.run("told = {} for i = 1, GetNumFactions() do local n, d = GetFactionInfo(i) told[n] = d end");
    CHECK(ui.is("told['Thrallmar'] == 'Sent by Thrall.'"));
    CHECK(ui.is("told['Cenarion Expedition'] == 'An expedition.'"));
    CHECK(ui.is("told['The Aldor'] == ''"));
}

// P73: a faction put away goes to a group of its own at the end (0x005d1880), at once and told of.
TEST_CASE("a faction put away is in the inactive group", "[replay][interface][reputation]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Reputation ui(s);
    ui.run("updates = 0 local f = CreateFrame('Frame') f:RegisterEvent('UPDATE_FACTION') "
           "f:SetScript('OnEvent', function() updates = updates + 1 end) ");
    const std::string before = "+The Burning Crusade;Thrallmar;>+Shattrath City;>Cenarion Expedition;>The Aldor;"
                               "+Other;Hydraxian Waterlords;";
    CHECK(ui.rowsAre(before));
    ui.run("SetFactionInactive(4)");
    CHECK(ui.is("updates == 1"));
    CHECK(ui.rowsAre("+The Burning Crusade;Thrallmar;>+Shattrath City;>The Aldor;+Other;Hydraxian Waterlords;"
                     "+Inactive;Cenarion Expedition;"));
    CHECK(ui.is("IsFactionInactive(8) == 1 and not IsFactionInactive(3)"));
    CHECK(ui.is("(function() local n, d, st, lo, hi, v, war, can, head, closed, rep, watched, child = GetFactionInfo(7) "
                "return n == 'Inactive' and d == '' and st == 1 and lo == 0 and hi == 0 and v == 0 and war == nil "
                "and can == nil and head == 1 and closed == nil and rep == nil and watched == nil and child == nil end)()"));
    CHECK(s.sent().of(Opcode::CMSG_SET_FACTION_INACTIVE).size() == 1);
    // Closed, it keeps its heading alone.
    ui.run("CollapseFactionHeader(7)");
    CHECK(ui.rowsAre("+The Burning Crusade;Thrallmar;>+Shattrath City;>The Aldor;+Other;Hydraxian Waterlords;"
                     "-Inactive;"));
    ui.run("ExpandFactionHeader(7)");
    // A group is not put away, and a whole group's members take its heading along.
    ui.run("updates = 0 SetFactionInactive(1) SetFactionInactive(3)");
    CHECK(ui.is("updates == 0"));
    ui.run("SetFactionInactive(4)");
    CHECK(ui.rowsAre("+The Burning Crusade;Thrallmar;+Other;Hydraxian Waterlords;+Inactive;Cenarion Expedition;"
                     "The Aldor;"));
    ui.run("SetFactionActive(6) SetFactionActive(GetNumFactions())");
    CHECK(ui.rowsAre(before));

    // The rows are those of the flags as they are: one met since is among them.
    s.handler().initialFactionsRef()[4].flags |= 0x1;
    CHECK(ui.is("(function() for i = 1, GetNumFactions() do local n, _, _, _, _, _, _, _, _, _, rep = GetFactionInfo(i) "
                "if n == 'Shattrath City' then return rep end end end)()"));
}

// P73: by name as the client compares names - the groups (0x005d1710) and the
// factions in each (0x005d02f0), these before the groups in it. The table's
// places are the other way round, and a byte compare puts the accented last.
TEST_CASE("the reputation rows are in the order of their names", "[replay][interface][reputation]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Reputation ui(s, factionTable({{1097, 0xFFFFFFFFu, 0, "Wrath", ""},
                                               {1118, 0xFFFFFFFFu, 0, "Classic", ""},
                                               {50, 0xFFFFFFFFu, 1118, "Horde", ""},
                                               {51, 0xFFFFFFFFu, 1118, "Cartel", ""},
                                               {60, 0, 1097, "Zuldrak", ""},
                                               {61, 1, 1118, "thorium", ""},
                                               {62, 2, 1118, "Ödland", ""},
                                               {63, 3, 50, "Orgrimmar", ""},
                                               {64, 4, 51, "Ratchet", ""},
                                               {65, 5, 51, "Beutebucht", ""},
                                               {66, 6, 0, "Zweiter", ""},
                                               {67, 7, 0, "Erster", ""}}), 8);
    CHECK(ui.rowsAre("+Classic;Ödland;thorium;>+Cartel;>Beutebucht;>Ratchet;>+Horde;>Orgrimmar;+Wrath;Zuldrak;"
                     "+Other;Erster;Zweiter;"));
    // Put away, in the order of the names too.
    ui.run("SetFactionInactive(3) SetFactionInactive(2)");
    CHECK(ui.rowsAre("+Classic;>+Cartel;>Beutebucht;>Ratchet;>+Horde;>Orgrimmar;+Wrath;Zuldrak;"
                     "+Other;Erster;Zweiter;+Inactive;Ödland;thorium;"));
}

// T247: the client's table of a frame has its handle at 0 and what the interface put there.
TEST_CASE("a frame's table shows no more than what is in it", "[replay][interface]") {
    Session s;
    Interface ui(s);
    ui.run("f = CreateFrame('Button', 'PlayFields', UIParent) f:SetText('x') f:SetNormalTexture('a') "
           "f:SetHighlightTexture('b') f:SetScript('OnClick', function() end) f:RegisterEvent('BAG_UPDATE') "
           "f:SetAttribute('a', 1) f:SetID(3) f:RegisterForClicks('AnyUp') f.own = 1 f.__ouf = 2 "
           "t = f:CreateTexture('PlayFieldsT') s = f:CreateFontString() "
           "e = CreateFrame('EditBox', nil, f) e:SetAutoFocus(false) e:SetText('y') "
           "g = CreateFrame('GameTooltip', 'PlayFieldsTip', UIParent) g:SetOwner(f, 'ANCHOR_NONE') g:SetText('z') "
           "function keys(x) local out = {} for k in pairs(x) do out[#out + 1] = tostring(k) end "
           "table.sort(out) return table.concat(out, ' ') end");
    CHECK(ui.is("keys(f) == '0 __ouf own'"));
    for (const char* other : {"t", "s", "e", "g"}) {
        INFO(other);
        CHECK(ui.is(std::string("keys(") + other + ") == '0'"));
    }
    for (const char* type : {"Frame", "Button", "CheckButton", "StatusBar", "Slider", "ScrollFrame", "MessageFrame",
                             "ScrollingMessageFrame", "SimpleHTML", "ColorSelect", "Cooldown", "Model", "PlayerModel",
                             "DressUpModel"}) {
        INFO(type);
        CHECK(ui.is(std::string("keys(CreateFrame('") + type + "')) == '0'"));
    }
    CHECK(ui.is("next(f) ~= nil and next(t, 0) == nil and rawget(f, '__wid') ~= nil"));
    ui.run("seen = '' table.foreach(f, function(k) seen = seen .. (type(k) == 'string' and k:sub(1, 5) == '__wid' and 'bad' or '') end)");
    CHECK(ui.is("seen == '' and pairs({}) == next"));
}

// 0x0096f4a0: a button's label has the font object of the button's state.
TEST_CASE("a button's label takes the font of its state", "[replay][interface]") {
    Session s;
    Interface ui(s);
    ui.run("FN = CreateFont('FN') FN:SetTextColor(1, .8, 0) FH = CreateFont('FH') FH:SetTextColor(1, 1, 1) "
           "FD = CreateFont('FD') FD:SetTextColor(.5, .5, .5) "
           "b = CreateFrame('Button', 'PlayBt', UIParent) b:SetNormalFontObject('FN') b:SetHighlightFontObject(FH) "
           "b:SetDisabledFontObject('FD') b:SetText('x') f = b:GetFontString() "
           "function red() return math.floor(f:GetTextColor() * 10 + .5) end");
    CHECK(ui.is("f:GetFontObject() == FN and b:GetHighlightFontObject() == FH and b:GetDisabledFontObject() == FD"));
    ui.run("b:Disable()");
    CHECK(ui.is("f:GetFontObject() == FD and red() == 5"));
    ui.run("b:LockHighlight()");   // a disabled one's all the same
    CHECK(ui.is("f:GetFontObject() == FD"));
    ui.run("b:Enable()");
    CHECK(ui.is("f:GetFontObject() == FH and red() == 10"));
    ui.run("b:UnlockHighlight()");
    CHECK(ui.is("f:GetFontObject() == FN"));
    ui.run("b:SetButtonState('DISABLED')");
    CHECK(ui.is("f:GetFontObject() == FD"));
    ui.run("b:SetButtonState('NORMAL')");
    CHECK(ui.is("f:GetFontObject() == FN"));
    // What the label has set itself is its own.
    ui.run("f:SetTextColor(.3, 0, 0) b:Disable()");
    CHECK(ui.is("f:GetFontObject() == FD and red() == 3"));
    // Another label, of the state as it is, and one without a font follows none.
    ui.run("g = b:CreateFontString() b:SetFontString(g)");
    CHECK(ui.is("g:GetFontObject() == FD"));
    ui.run("c = CreateFrame('Button', 'PlayBt2', UIParent) h = c:CreateFontString() h:SetFontObject('FN') c:SetFontString(h)");
    CHECK(ui.is("h:GetFontObject() == nil and select(2, h:GetTextColor()) > .7"));
}

// T248: each element of an XML file is built for itself; an error in one ends that one alone,
// a script that does not compile is not set, an OnLoad's error ends the OnLoad.
TEST_CASE("an error in an addon's XML ends no more than itself", "[replay][interface]") {
    Session s;
    Interface ui(s);
    const auto build = [&](const std::string& xml) {
        wowee::ui::XmlNode root;
        std::string error;
        REQUIRE(wowee::ui::parseXml("<Ui>" + xml + "</Ui>", root, error));
        std::vector<std::string> files;
        const bool whole = ui.engine.buildElements(wowee::ui::emitFrameXml(root), "t.xml",
                                                   [&](const auto& at) { files.push_back(at.file); });
        CHECK((files.empty() || files == std::vector<std::string>{"F.lua"}));
        return whole;
    };
    CHECK(build("<Frame name=\"Xa1\"/><Script file=\"F.lua\"/><Frame name=\"Xa2\" parent=\"Xa1\"/>"));
    CHECK(ui.is("Xa2:GetParent() == Xa1"));
    CHECK_FALSE(build("<Frame name=\"Xb1\"/><Script>error('inline')</Script><Frame name=\"Xb2\"/>"
                      "<Script>syntax(</Script><Frame name=\"Xb3\"/>"));
    CHECK(ui.is("rawget(_G, 'Xb1') and rawget(_G, 'Xb2') and rawget(_G, 'Xb3')"));
    // An OnLoad that raises, of the frame's child too.
    build("<Frame name=\"Xc1\"><Frames><Frame name=\"Xc2\"><Scripts><OnLoad>error('load')</OnLoad></Scripts></Frame>"
          "<Frame name=\"Xc3\"/></Frames><Scripts><OnLoad>xc = self:GetName() error('too')</OnLoad></Scripts></Frame>"
          "<Frame name=\"Xc4\"/>");
    CHECK(ui.is("rawget(_G, 'Xc3') and rawget(_G, 'Xc4') and xc == 'Xc1'"));
    // One that does not compile is left out, the others of the frame are set.
    CHECK_FALSE(build("<Frame name=\"Xd1\" virtual=\"true\"><Scripts><OnLoad>oops(</OnLoad><OnShow>xd = 1</OnShow></Scripts></Frame>"
                      "<Frame name=\"Xd2\" inherits=\"Xd1\"><Scripts><OnHide>\nthis is none\n</OnHide></Scripts></Frame>"
                      "<Frame name=\"Xd3\" inherits=\"NoSuchTemplate\"/>"));
    CHECK(ui.is("Xd2:GetScript('OnShow') and not Xd2:GetScript('OnLoad') and not Xd2:GetScript('OnHide')"));
    CHECK(ui.is("rawget(_G, 'Xd3') and Xd3:IsShown()"));
}

// 0x00564130 -> 0x00562990: what /click clicks.
TEST_CASE("a frame to be clicked is found by its name", "[replay][interface]") {
    Session s;
    Interface ui(s);
    ui.run("b = CreateFrame('Button', 'PlayClick', UIParent) c = CreateFrame('CheckButton', 'PlayClick2', UIParent) "
           "f = CreateFrame('Frame', 'PlayClick3', UIParent) PlayClick4 = b PlayClick5 = {} PlayClick6 = 'PlayClick'");
    CHECK(ui.is("GetClickFrame('PlayClick') == b and GetClickFrame('PlayClick2') == c"));
    CHECK(ui.is("GetClickFrame('PlayClick3') == f"));
    // Under a name that is not its own, no frame, nothing.
    CHECK(ui.is("GetClickFrame('PlayClick4') == nil and GetClickFrame('PlayClick5') == nil"));
    CHECK(ui.is("GetClickFrame('PlayClick6') == nil and select('#', GetClickFrame('PlayClick7')) == 1"));
    CHECK(ui.is("not pcall(GetClickFrame)"));
}

// T198: 0x008167e0: a table of methods for each type.
TEST_CASE("an object has the methods of its type", "[replay][interface]") {
    Session s;
    Interface ui(s);
    ui.run("function index(o) return getmetatable(o).__index end "
           "f, f2, b, c, e = CreateFrame('Frame'), CreateFrame('Frame'), CreateFrame('Button'), CreateFrame('CheckButton'), "
           "  CreateFrame('EditBox') e:SetAutoFocus(false) cd, cd2 = CreateFrame('Cooldown'), CreateFrame('Cooldown') "
           "tip = CreateFrame('GameTooltip', 'PlayTypes') m, p = CreateFrame('Model'), CreateFrame('DressUpModel') "
           "sb, sl = CreateFrame('StatusBar'), CreateFrame('Slider') t, w = f:CreateTexture(), f:CreateFontString()");
    for (const char* none : {"f.GetChecked", "f.SetCooldown", "f.GetText", "f.SetNormalTexture", "f.SetMinMaxValues",
                             "b.GetChecked", "b.SetCooldown", "b.AddLine", "b.SetCursorPosition", "e.SetNormalTexture",
                             "e.GetChecked", "cd.SetText", "tip.SetNormalTexture", "m.SetUnit", "sb.SetValueStep", "sl.SetStatusBarColor",
                             "t.GetStringHeight", "t.SetText", "t.GetScale", "w.GetTexture", "w.SetTexCoord", "w.SetScript"}) {
        INFO(none);
        CHECK(ui.is(std::string(none) + " == nil"));
    }
    for (const char* some : {"f.SetScript", "f.SetBackdrop", "f.GetParent", "f.IsMouseOver", "b.SetText", "b.Click", "b.SetScript",
                             "c.SetChecked", "c.SetText", "e.SetText", "e.SetFontObject", "cd.SetCooldown", "tip.AddLine",
                             "tip.SetUnit", "m.SetModel", "p.SetModel", "p.SetUnit", "p.TryOn", "sb.SetStatusBarColor",
                             "sl.SetValueStep", "t.SetTexture", "t.SetPoint", "w.SetText", "w.GetName"}) {
        INFO(some);
        CHECK(ui.is(std::string("type(") + some + ") == 'function'"));
    }
    CHECK(ui.is("index(f) == index(f2) and index(f) ~= index(b) and index(b) ~= index(c) and index(t) ~= index(w)"));
    CHECK(ui.is("f.Own == nil and f.own == nil and t.Own == nil"));
    // A hook in a type's table is of that type alone.
    ui.run("n = 0 hooksecurefunc(index(cd), 'SetCooldown', function(self) n = n + 1 who = self end) cd2:SetCooldown(1, 2) "
           "k = 0 hooksecurefunc(index(b), 'SetText', function() k = k + 1 end) b:SetText('x') c:SetText('y') e:SetText('z') "
           "tip:SetText('w')");
    CHECK(ui.is("n == 1 and who == cd2 and k == 1"));
    // What an element's attributes are not its own, its type does not read; a bar's layer is.
    wowee::ui::XmlNode root;
    std::string error;
    REQUIRE(wowee::ui::parseXml("<Ui><Frame name=\"PlayOdd\" letters=\"5\" checked=\"true\" minValue=\"1\" "
                                "maxValue=\"2\" valueStep=\"1\" text=\"x\" maxLines=\"3\" "
                                "motionScriptsWhileDisabled=\"true\" drawEdge=\"true\"/>"
                                "<StatusBar name=\"PlayLayer\" drawLayer=\"BORDER\" minValue=\"1\" maxValue=\"7\"/></Ui>",
                                root, error));
    CHECK(ui.engine.buildElements(wowee::ui::emitFrameXml(root), "t.xml", [](const auto&) {}));
    CHECK(ui.is("rawget(_G, 'PlayOdd') and select(2, PlayLayer:GetMinMaxValues()) == 7 and PlayLayer.SetDrawLayer == nil"));
    const auto* bar = ui.engine.widgets().findByName("PlayLayer");
    REQUIRE(bar);
    CHECK(bar->barLayer == wowee::ui::DrawLayer::Border);
    // Put in by an addon.
    ui.run("index(f).PlayOwn = function(self) return self end");
    CHECK(ui.is("f2:PlayOwn() == f2 and b.PlayOwn == nil"));
}

// V339: and an animation those of its kind, a group and a control point theirs.
TEST_CASE("an animation has the methods of its kind", "[replay][interface]") {
    Session s;
    Interface ui(s);
    ui.run("function index(o) return getmetatable(o).__index end "
           "function count(o) local n = 0 for k, v in pairs(index(o)) do n = n + 1 if type(v) ~= 'function' then n = 1000 end end "
           "  return n end "
           "g = CreateFrame('Frame'):CreateAnimationGroup('PlayGroup') function new(kind) return g:CreateAnimation(kind) end "
           "a, t, r, sc, p, al = new('Animation'), new('Translation'), new('Rotation'), new('Scale'), new('Path'), new('Alpha') "
           "cp = p:CreateControlPoint()");
    CHECK(ui.is("count(a) == 35 and count(t) == 37 and count(r) == 41 and count(sc) == 39 and count(p) == 40 and count(al) == 37"));
    CHECK(ui.is("count(g) == 26 and count(cp) == 9"));
    CHECK(ui.is("a.SetOffset == nil and sc.SetOffset == nil and t.SetChange == nil and al.SetDegrees == nil and p.SetScale == nil"));
    CHECK(ui.is("g.Resume == nil and g.SetParent == nil and g.__index == nil and a.__index == nil and a.Resume == nil"));
    CHECK(ui.is("t.SetOffset and al.SetChange and r.SetDegrees and sc.SetScale and p.SetCurve and cp.SetOffset and true"));
    CHECK(ui.is("index(a) ~= index(t) and index(t) == index(new('translation'))"));
    CHECK(ui.is("a:GetObjectType() == 'Animation' and r:GetObjectType() == 'Rotation' and r:IsObjectType('animation') == 1"));
    CHECK(ui.is("g:GetObjectType() == 'AnimationGroup' and cp:GetObjectType() == 'ControlPoint' and cp:GetParent() == p"));
    ui.run("index(sc).PlayOwn = function() return 1 end");
    CHECK(ui.is("sc:PlayOwn() == 1 and r.PlayOwn == nil"));
    ui.run("other = CreateFrame('Frame'):CreateAnimationGroup() a:SetParent(other) other2 = new('Path') cp:SetParent(other2)");
    CHECK(ui.is("a:GetParent() == other and cp:GetParent() == other2 and not pcall(cp.SetParent, cp, r)"));
}

// T161's rest: 0x006277f0's sixth argument of a gem, and the skill an added socket asks for.
TEST_CASE("a gem to be destroyed is said to be", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    answerItem(s, 24021, "Ruby");
    answerItem(s, 24022, "Bracer");
    ui.run("DESTROY_GEM, CURRENTLY_EQUIPPED = 'destroyed', 'worn' SOCKET_ITEM_MIN_SKILL = 'needs %s (%d)' "
           "EMPTY_SOCKET_NO_COLOR, EMPTY_SOCKET_RED, SOCKET_ITEM_REQ_SKILL = 'empty', 'red', 'needs %s' "
           "_GetItemTooltipData = function(id) "
           "  if id == 24021 then return {gemProperty = {text = '+10', ok = true}} end return {sockets = {{color = 2}}} end "
           "tip = CreateFrame('GameTooltip', 'PlaySock', UIParent) "
           "function fill(id, inst, header) tip:SetOwner(UIParent, 'ANCHOR_NONE') __WoweeTooltipEquippedHeader = header "
           "  _WoweePopulateItemTooltip(tip, id, inst) __WoweeTooltipEquippedHeader = nil end");
    const auto* w = ui.engine.widgets().findByName("PlaySock");
    REQUIRE(w);
    // Each line with its red and green in tenths.
    const auto lines = [&](const std::string& args) {
        ui.run("fill(" + args + ")");
        std::string all = "|";
        for (const auto& line : w->tooltipLines)
            all += line.left + ":" + std::to_string(static_cast<int>(line.lc[0] * 10 + .5f)) + "," +
                   std::to_string(static_cast<int>(line.lc[1] * 10 + .5f)) + "|";
        return all;
    };
    const auto has = [](const std::string& all, const char* what) { return all.find(what) != std::string::npos; };
    CHECK(lines("24021, nil, true") == "|destroyed:10,1|Ruby:5,5|+10:5,5|");
    const std::string plain = lines("24021");
    CHECK(has(plain, "|+10:10,10|"));
    CHECK_FALSE(has(plain, "destroyed"));
    CHECK(has(lines("24022, nil, true"), "|worn:5,5|Bracer:"));
    // The socket an enchantment put in.
    CHECK(has(lines("24022, {prismatic = true}"), "|red:5,5|empty:5,5|"));
    CHECK(has(lines("24022, {prismatic = true, prismaticShort = true, prismaticSkill = 'Smith', prismaticRank = 400}"),
              "|red:5,5|empty:10,1|needs Smith (400):10,1|"));
    CHECK(has(lines("24022, {prismatic = true, prismaticShort = true}"), "|empty:10,1|"));
    ui.run("gem = {text = 'ruby', skillOk = false, levelOk = true, conditionsMet = true, skillName = 'Cut', skillRank = 0} "
           "ENCHANT_ITEM_REQ_SKILL = 'asks %s'");
    const std::string own = lines("24022, {gems = {gem}}");
    CHECK(has(own, "|ruby:10,1|asks Cut:10,1|"));
    CHECK_FALSE(has(own, "needs"));
    ui.run("gem.added = true");
    CHECK(has(lines("24022, {gems = {gem}}"), "|asks Cut:10,1|needs Cut:10,1|"));
}

// T295: the comparison hid its own tooltip, and with that let go of its owner: nothing was put into
// it. T296: a tooltip without the money's script has no price.
TEST_CASE("an item's comparison shows what is worn", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    answerItem(s, 24021, "Dagger", 70);
    answerItem(s, 24022, "Sword", 90);
    wowee::game::ItemDef item;
    item.guid = 0x4000000000000778ull;
    item.itemId = 24022;
    item.name = "Sword";
    item.stackCount = 1;
    s.handler().getInventory().setEquipSlot(wowee::game::EquipSlot::MAIN_HAND, item);
    s.handler().onlineItemsRef()[item.guid].entry = 24022;
    ui.run("local real = GetItemInfo GetItemInfo = function(id) local a, b, q, d, e, f, g, h, _, j, k = real(id) "
           "  return a, b, q, d, e, f, g, h, 'INVTYPE_WEAPONMAINHAND', j, k end "
           "GetInventoryItemLink = function(u, slot) if slot == 16 then return 'item:24022:0:0:0:0:0:0:0' end end "
           "tip = CreateFrame('GameTooltip', 'PlayCompare', UIParent) anchor = CreateFrame('Frame')");
    const auto* w = ui.engine.widgets().findByName("PlayCompare");
    REQUIRE(w);
    // Asked before it has an owner, as GameTooltip_ShowCompareItem does, and again with one.
    CHECK(ui.is("tip:SetHyperlinkCompareItem('item:24021:0:0:0:0:0:0:0', 1)"));
    ui.run("tip:SetOwner(anchor, 'ANCHOR_NONE')");
    CHECK(ui.is("tip:SetHyperlinkCompareItem('item:24021:0:0:0:0:0:0:0', 1)"));
    REQUIRE(w->tooltipLines.size() >= 2);
    CHECK(w->tooltipLines[1].left == "Sword");
    CHECK(ui.is("tip:IsShown()"));
    for (const auto& line : w->tooltipLines) CHECK(line.left.find("|T") == std::string::npos);
    // Nothing worn there: gone.
    ui.run("GetInventoryItemLink = function() end");
    CHECK(ui.is("not tip:SetHyperlinkCompareItem('item:24021:0:0:0:0:0:0:0', 1) and not tip:IsShown()"));
    // The script has it.
    ui.run("tip:SetScript('OnTooltipAddMoney', function(self, m) money = m end) tip:SetOwner(anchor, 'ANCHOR_NONE') "
           "GetInventoryItemLink = function(u, slot) if slot == 16 then return 'item:24022:0:0:0:0:0:0:0' end end "
           "tip:SetHyperlinkCompareItem('item:24021:0:0:0:0:0:0:0', 1)");
    CHECK(ui.is("money == 90"));
}

// T301: 0x0056f770, 0x0056f8d0: a letter that was emptied is deleted.
TEST_CASE("a letter emptied of all it held is deleted", "[replay][interface]") {
    struct Letter {
        uint8_t type;
        const char* body;
        uint32_t money;
        bool goes;
    };
    const Letter letter = GENERATE(Letter{0, "", 0, true}, Letter{0, "Hello", 0, false}, Letter{0, "", 500, false},
                                   Letter{3, "", 0, true});
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("f = CreateFrame('Frame') f:RegisterEvent('CLOSE_INBOX_ITEM') f:SetScript('OnEvent', function(self, e, a) closed = a end)");
    s.handler().getInventoryHandler()->openMailbox(0xF110000000000099ull);
    wowee::network::Packet m;
    m.writeUInt32(501);
    m.writeUInt8(letter.type);
    if (letter.type == 0) m.writeUInt64(0x77);
    else m.writeUInt32(0x77);
    for (uint32_t v : {0u, 0u, 41u, letter.money, 0u}) m.writeUInt32(v);
    m.writeFloat(29.0f);
    m.writeUInt32(0);
    m.writeString("Parcel");
    m.writeString(letter.body);
    m.writeUInt8(1);
    m.writeUInt8(0);
    m.writeUInt32(9001);
    m.writeUInt32(12047);
    for (int i = 0; i < 21; ++i) m.writeUInt32(0);
    for (uint32_t v : {0u, 0u, 1u, 0u, 0u, 0u}) m.writeUInt32(v);
    m.writeUInt8(0);
    wowee::network::Packet list(wowee::game::wireOpcode(Opcode::SMSG_MAIL_LIST_RESULT));
    list.writeUInt32(1);
    list.writeUInt8(1);
    list.writeUInt16(static_cast<uint16_t>(m.getData().size() + 2));
    list.writeBytes(m.getData().data(), m.getData().size());
    s.receive(list);
    REQUIRE(ui.is("GetInboxNumItems() == 1"));
    wowee::network::Packet taken(wowee::game::wireOpcode(Opcode::SMSG_SEND_MAIL_RESULT));
    for (uint32_t v : {501u, 2u, 0u, 9001u, 1u}) taken.writeUInt32(v);
    s.receive(taken);
    auto deleted = s.sent().of(Opcode::CMSG_MAIL_DELETE);
    CHECK(deleted.size() == (letter.goes ? 1u : 0u));
    CHECK(ui.is(letter.goes ? "closed == 1" : "closed == nil"));
    if (letter.goes) {
        CHECK(deleted.front().readUInt64() == 0xF110000000000099ull);
        CHECK(deleted.front().readUInt32() == 501);
        CHECK(deleted.front().readUInt32() == 3);
    }
    if (letter.money != 0) {
        wowee::network::Packet paid(wowee::game::wireOpcode(Opcode::SMSG_SEND_MAIL_RESULT));
        for (uint32_t v : {501u, 1u, 0u}) paid.writeUInt32(v);
        s.receive(paid);
        deleted = s.sent().of(Opcode::CMSG_MAIL_DELETE);
        REQUIRE(deleted.size() == 1);
        deleted.front().readUInt64();
        deleted.front().readUInt32();
        CHECK(deleted.front().readUInt32() == 2);
    }
}

// T278: 0x004fdbc0: what a chat line's event has.
TEST_CASE("the client's own chat lines are from no one", "[replay][interface]") {
    Session s;
    Interface ui(s);
    ui.run("f = CreateFrame('Frame') f:RegisterEvent('CHAT_MSG_SYSTEM') f:RegisterEvent('CHAT_MSG_LOOT') "
           "f:RegisterEvent('CHAT_MSG_EMOTE') f:SetScript('OnEvent', function(self, e, ...) "
           "  got = {...} n = select('#', ...) event = e end)");
    s.handler().addLocalChatLine(wowee::game::ChatType::LOOT, "Ihr erhaltet Beute: x.");
    CHECK(ui.is("event == 'CHAT_MSG_LOOT' and n == 13 and got[2] == '' and got[3] == '' and got[12] == ''"));
    CHECK(ui.is("got[7] == 0 and got[8] == 0 and got[10] == 0 and got[11] == 0 and got[13] == 0"));
    // A line that is a number is text.
    s.handler().addLocalChatLine(wowee::game::ChatType::SYSTEM, "25");
    CHECK(ui.is("event == 'CHAT_MSG_SYSTEM' and got[1] == '25'"));
    // A creature's guid is none.
    wowee::game::MessageChatData line;
    line.type = wowee::game::ChatType::EMOTE;
    line.message = "x";
    line.senderName = "y";
    line.senderGuid = 0xF130000000000001ull;
    s.handler().addLocalChatMessage(line);
    CHECK(ui.is("got[2] == 'y' and got[12] == ''"));
    line.senderGuid = 0xCull;
    s.handler().addLocalChatMessage(line);
    CHECK(ui.is("got[12] == '0x000000000000000C'"));
}

// T110: 0x009748f0: each element of a SimpleHTML has a font of its own (0x0096cc90).
TEST_CASE("a SimpleHTML's headings have their own fonts", "[replay][interface]") {
    Session s;
    Interface ui(s);
    ui.run("h = CreateFrame('SimpleHTML', 'PlayHtml') h:SetFont('Fonts\\\\FRIZQT__.TTF', 12) "
           "h:SetFont('h1', 'Fonts\\\\MORPHEUS.TTF', 24) h:SetJustifyH('h2', 'RIGHT') h:SetSpacing('H1', 5) "
           "h:SetTextColor('H1', 1, 0, 0) h:SetTextColor(0, 1, 0) big = CreateFont('PlayBig') "
           "big:SetFont('Fonts\\\\ARIALN.TTF', 18) h:SetFontObject('H3', 'PlayBig')");
    CHECK(ui.is("select(2, h:GetFont('H1')) == 24 and select(2, h:GetFont()) == 12 and select(2, h:GetFont('p')) == 12"));
    CHECK(ui.is("h:GetFont('h1') == 'Fonts\\\\MORPHEUS.TTF' and h:GetFont('h2') == nil"));
    CHECK(ui.is("h:GetFont('h3') == 'Fonts\\\\ARIALN.TTF' and h:GetFontObject('h3') == big"));
    CHECK(ui.is("h:GetJustifyH('h2') == 'RIGHT' and h:GetJustifyH() ~= 'RIGHT'"));
    CHECK(ui.is("h:GetSpacing('h1') == 5 and h:GetSpacing() == 0"));
    CHECK(ui.is("h:GetTextColor('h1') > .99 and h:GetTextColor() == 0"));
    // It is nothing of the frame's.
    CHECK(ui.is("h:GetNumRegions() == 0 and h:GetNumChildren() == 0"));
    ui.run("n = '' for k in pairs(h) do n = n .. tostring(k) end");
    CHECK(ui.is("n == '0'"));
    const auto* html = ui.engine.widgets().findByName("PlayHtml");
    REQUIRE(html != nullptr);
    const auto* first = ui.engine.widgets().get(html->htmlHeadingFont[0]);
    REQUIRE(first != nullptr);
    CHECK(first->fontHeight == 24.0f);
    CHECK_FALSE(first->shown);

    wowee::ui::XmlNode root;
    std::string error;
    REQUIRE(wowee::ui::parseXml(
        "<Ui><SimpleHTML name=\"PlayHtml2\"><FontString inherits=\"PlayBig\"/>"
        "<FontStringHeader1 inherits=\"PlayBig\" spacing=\"4\" justifyH=\"CENTER\"><Color r=\"0\" g=\"0\" b=\"1\"/>"
        "</FontStringHeader1></SimpleHTML></Ui>",
        root, error));
    CHECK(ui.engine.buildElements(wowee::ui::emitFrameXml(root), "t.xml", [](const auto&) {}));
    CHECK(ui.is("PlayHtml2:GetFontObject('H1') == big and PlayHtml2:GetSpacing('H1') == 4 and PlayHtml2:GetSpacing() == 0"));
    CHECK(ui.is("PlayHtml2:GetJustifyH('H1') == 'CENTER' and select(3, PlayHtml2:GetTextColor('H1')) > .99"));
    CHECK(ui.is("PlayHtml2:GetFont('H2') == nil"));
}

// T274: 0x0048b6c0 -> 0x0048b540, 0x00819880.
TEST_CASE("$parent in the name of what a script makes", "[replay][interface]") {
    Session s;
    Interface ui(s);
    ui.run("tip = CreateFrame('GameTooltip', 'PlayPar') a = tip:CreateFontString('$parentTextLeft1') "
           "t = tip:CreateTexture('$ParentTex') f = CreateFrame('Frame', '$parentKid', tip) "
           "n = CreateFrame('Frame', nil, tip) u = n:CreateTexture('$parentIn') k = CreateFrame('Frame', '$parentKid2', n) "
           "o = CreateFrame('Frame', '$parentZ') g = n:CreateAnimationGroup('$parentAnim')");
    CHECK(ui.is("a:GetName() == 'PlayParTextLeft1' and rawget(_G, 'PlayParTextLeft1') == a"));
    CHECK(ui.is("t:GetName() == 'PlayParTex' and rawget(_G, 'PlayParTex') == t"));
    CHECK(ui.is("f:GetName() == 'PlayParKid' and rawget(_G, 'PlayParKid') == f"));
    CHECK(ui.is("u:GetName() == 'PlayParIn' and k:GetName() == 'PlayParKid2' and rawget(_G, 'PlayParKid2') == k"));
    CHECK(ui.is("g:GetName() == 'PlayParAnim' and rawget(_G, 'PlayParAnim') == g"));
    CHECK(ui.is("o:GetName() == 'TopZ' and rawget(_G, 'TopZ') == o"));
    // No string is no name.
    CHECK(ui.is("tip:CreateTexture(true):GetName() == nil and CreateFrame('Frame', {}):GetName() == nil"));
    // What is there already keeps the name in _G, whatever it is.
    ui.run("x = CreateFrame('Frame', 'PlayDup') y = CreateFrame('Frame', 'PlayDup') z = y:CreateTexture('PlayDup') "
           "PlayOwn = 5 w = CreateFrame('Frame', 'PlayOwn')");
    CHECK(ui.is("PlayDup == x and y:GetName() == 'PlayDup' and z:GetName() == 'PlayDup'"));
    CHECK(ui.is("PlayOwn == 5 and w:GetName() == 'PlayOwn'"));
    // But for what the client has made in the interface's place.
    ui.run("before = WorldFrame world = CreateFrame('Frame', 'WorldFrame')");
    CHECK(ui.is("WorldFrame == world and before ~= world"));
    ui.run("CreateFrame('Frame', 'WorldFrame')");
    CHECK(ui.is("WorldFrame == world"));

    // A template on a frame without a name, and points by such a name.
    wowee::ui::XmlNode root;
    std::string error;
    REQUIRE(wowee::ui::parseXml(
        "<Ui><Frame name=\"PlayParT\" virtual=\"true\"><Layers><Layer><Texture name=\"$parentA\"/>"
        "<Texture name=\"$parentB\"><Anchors><Anchor point=\"LEFT\" relativeTo=\"$parentA\"/></Anchors></Texture>"
        "</Layer></Layers><Frames><Frame><Frames><Frame name=\"$parentC\"/></Frames></Frame></Frames></Frame></Ui>",
        root, error));
    CHECK(ui.engine.buildElements(wowee::ui::emitFrameXml(root), "t.xml", [](const auto&) {}));
    ui.run("CreateFrame('Frame', 'PlayParN', nil, 'PlayParT') CreateFrame('Frame', nil, PlayParN, 'PlayParT') "
           "q = CreateFrame('Frame', 'PlayParQ') m = CreateFrame('Frame', nil, q, 'PlayParT')");
    CHECK(ui.is("PlayParNA:GetParent() == PlayParN and select(2, PlayParNB:GetPoint()) == PlayParNA"));
    CHECK(ui.is("PlayParNC:GetParent():GetParent() == PlayParN"));
    CHECK(ui.is("PlayParQA:GetParent() == m and select(2, PlayParQB:GetPoint()) == PlayParQA and PlayParQC ~= nil"));
}

// T273: the player's scale is the UIParent's of the interface (0x00513240 -> 0x0048f580), which
// is none the tree has of its own; and scroll frames of a scale (0x0096b4c0, 0x0096b1c0).
TEST_CASE("UIParent has the player's scale", "[replay][interface]") {
    Session s;
    Interface ui(s);
    auto& tree = ui.engine.widgets();
    tree.setUserScale(0.8f);
    ui.layout();
    ui.run("function near(a, b) return math.abs(a - b) < .01 end "
           "function parentIs(o) return near(o:GetScale(), .8) and near(o:GetEffectiveScale(), .8) "
           "  and near(o:GetWidth(), 1280) and near(o:GetHeight(), 960) end");
    CHECK(ui.is("parentIs(UIParent)"));
    // The XML's, which takes its place.
    ui.run("CreateFrame('Frame', 'UIParent') UIParent:SetAllPoints() kid = CreateFrame('Frame', nil, UIParent) "
           "kid:SetWidth(100) kid:SetHeight(100) kid:SetPoint('TOPLEFT') own = CreateFrame('Frame') own:SetAllPoints()");
    ui.layout();
    CHECK(ui.is("parentIs(UIParent) and near(kid:GetEffectiveScale(), .8) and near(kid:GetRight(), 100)"));
    CHECK(ui.is("near(own:GetEffectiveScale(), 1) and near(own:GetHeight(), 768)"));
    CHECK(ui.is("near(GetScreenHeight(), 960)"));
    ui.run("UIParent:SetScale(.5)");
    CHECK(ui.is("near(UIParent:GetScale(), .5) and near(UIParent:GetHeight(), 1536) and near(kid:GetEffectiveScale(), .5)"));
    tree.setUserScale(0.8f);
    CHECK(ui.is("parentIs(UIParent)"));

    for (const char* parent : {"UIParent", "kid", "nil"}) {
        INFO(parent);
        ui.run(std::string("kid:SetScale(.7) f = CreateFrame('ScrollFrame', nil, ") + parent + ") f:SetWidth(100) f:SetHeight(100) "
               "f:SetPoint('TOPLEFT', UIParent, 'TOPLEFT', 10, -10) c = CreateFrame('Frame', nil, f) c:SetWidth(150) c:SetHeight(240) "
               "f:SetScrollChild(c)");
        ui.layout();
        ui.engine.updateScrollRanges();
        CHECK(ui.is("near(f:GetVerticalScrollRange(), 140) and near(f:GetHorizontalScrollRange(), 50)"));
        ui.run("f:SetVerticalScroll(50) f:SetHorizontalScroll(20)");
        ui.layout();
        CHECK(ui.is("near(c:GetTop() - f:GetTop(), 50) and near(c:GetLeft() - f:GetLeft(), -20)"));
    }
}

// T269: 0x0048c3e0, 0x0048d030, 0x0048c1c0, 0x0048cf30, 0x0048d970, 0x0048c4c0 and 0x00487ab0.
TEST_CASE("a texture's and a string's alpha is its colour's", "[replay][interface]") {
    Session s;
    Interface ui(s);
    ui.run("t = UIParent:CreateTexture('PlayAlphaT') w = UIParent:CreateFontString() F = CreateFrame('Frame') "
           "function near(a, b) return math.abs(a - b) < 1e-6 end "
           "function alphas(o, a) local c = 1 if o.GetVertexColor and o == t then c = select(4, o:GetVertexColor()) "
           "  elseif o == w then c = select(4, o:GetTextColor()) end "
           "  return near(o:GetAlpha(), a / 255) and near(c, a * .00392156) end");
    ui.run("t:SetVertexColor(1, 1, 1, .5)");
    CHECK(ui.is("alphas(t, 128)"));
    ui.run("t:SetAlpha(1)");
    CHECK(ui.is("alphas(t, 255)"));
    // SetAlpha cuts (0x0048c46f), a colour rounds (0x0048bd20).
    ui.run("t:SetAlpha(.5) t:SetVertexColor(1, 0, 0)");
    CHECK(ui.is("alphas(t, 127) and t:GetVertexColor() > .99 and select(2, t:GetVertexColor()) == 0"));
    // A table in the fourth place is no number; what is none of the first three is 0.
    ui.run("t:SetVertexColor(nil, 2, -1, {})");
    CHECK(ui.is("alphas(t, 127) and t:GetVertexColor() == 0 and select(2, t:GetVertexColor()) > .99 "
                "and select(3, t:GetVertexColor()) == 0"));
    ui.run("t:SetVertexColor(1, 1, 1, 1)");
    CHECK(ui.is("alphas(t, 255)"));
    // A string's: SetTextColor has an alpha of its own, SetVertexColor leaves it.
    ui.run("w:SetTextColor(1, 1, 1, .5)");
    CHECK(ui.is("alphas(w, 128)"));
    ui.run("w:SetAlpha(1)");
    CHECK(ui.is("alphas(w, 255)"));
    ui.run("w:SetAlpha(.5) w:SetVertexColor(1, 1, 0)");
    CHECK(ui.is("alphas(w, 127)"));
    ui.run("w:SetTextColor(1, 1, 1)");
    CHECK(ui.is("alphas(w, 255)"));
    // Four colours are read as white, and set again they are one.
    ui.run("t:SetVertexColor(1, 0, 0, .5) t:SetGradientAlpha('HORIZONTAL', 1, 0, 0, .5, 0, 0, 1, .5)");
    CHECK(ui.is("alphas(t, 255) and select(2, t:GetVertexColor()) > .99"));
    const auto* shown = ui.engine.widgets().findByName("PlayAlphaT");
    REQUIRE(shown != nullptr);
    CHECK(shown->gradient);
    ui.run("t:SetAlpha(.25)");
    CHECK_FALSE(ui.engine.widgets().findByName("PlayAlphaT")->gradient);
    CHECK(ui.is("alphas(t, 63)"));
    // A frame's is its own.
    ui.run("F:SetAlpha(.5)");
    CHECK(ui.is("near(F:GetAlpha(), 127 / 255)"));
}

// T268: 0x0048c0e0, 0x0048d890, 0x004a3ca0, 0x0048d9e0, 0x00971800: a byte times 0.00392156.
TEST_CASE("colours are answered as they are kept", "[replay][interface]") {
    Session s;
    Interface ui(s);
    ui.run("function same(...) local want = {128, 64, 26, 178} for i = 1, 4 do "
           "  if math.abs((select(i, ...)) - want[i] * .00392156) > 1e-7 then return false end end return true end "
           "t = UIParent:CreateTexture() t:SetVertexColor(.5, .25, .1, .7) "
           "w = UIParent:CreateFontString() w:SetTextColor(.5, .25, .1, .7) w:SetShadowColor(.5, .25, .1, .7) "
           "e = CreateFrame('EditBox') e:SetAutoFocus(false) e:SetTextColor(.5, .25, .1, .7) "
           "b = CreateFrame('StatusBar') b:SetStatusBarColor(.5, .25, .1, .7)");
    CHECK(ui.is("same(t:GetVertexColor())"));
    CHECK(ui.is("same(w:GetTextColor())"));
    CHECK(ui.is("same(w:GetShadowColor())"));
    CHECK(ui.is("same(e:GetTextColor())"));
    // Without a texture the bar's colour is white.
    CHECK(ui.is("math.abs(b:GetStatusBarColor() - 255 * .00392156) < 1e-6"));
    ui.run("b:SetStatusBarTexture('Interface\\\\TargetingFrame\\\\UI-StatusBar')");
    CHECK(ui.is("same(b:GetStatusBarColor())"));
}

// T268: 0x0048d780 and 0x0048d800.
TEST_CASE("a string without a font takes no text", "[replay][interface]") {
    Session s;
    Interface ui(s);
    ui.run("w = UIParent:CreateFontString('PlayNoFont') n = UIParent:CreateFontString() "
           "ok, why = pcall(w.SetText, w, 'x') ok2, why2 = pcall(n.SetFormattedText, n, '%d', 1)");
    CHECK(ui.is("not ok and why:find('PlayNoFont:SetText%(%): Font not set')"));
    CHECK(ui.is("not ok2 and why2:find('<unnamed>:SetFormattedText%(%): Font not set')"));
    CHECK(ui.is("w:GetText() == nil"));
    // T201: 0x0048d510, 0x009778e0.
    CHECK(ui.is("w:GetFont() == nil and select(2, w:GetFont()) == 0 and select(3, w:GetFont()) == ''"));
    CHECK(ui.is("CreateFrame('Button'):GetFontString() == nil"));
    ui.run("w:SetFont('Fonts\\\\FRIZQT__.TTF', 12) w:SetText('x') w:SetFormattedText('%d', 7)");
    CHECK(ui.is("w:GetText() == '7'"));
    // A button's own says it, with a font or without, and so does the XML's.
    ui.run("b = CreateFrame('Button') b:SetText('y')");
    CHECK(ui.is("b:GetText() == 'y' and b:GetFontString():GetText() == 'y'"));
    wowee::ui::XmlNode root;
    std::string error;
    REQUIRE(wowee::ui::parseXml("<Ui><Frame name=\"PlayNoFontXml\"><Layers><Layer>"
                                "<FontString name=\"$parentText\" text=\"z\"/></Layer></Layers></Frame></Ui>",
                                root, error));
    CHECK(ui.engine.buildElements(wowee::ui::emitFrameXml(root), "t.xml", [](const auto&) {}));
    CHECK(ui.is("PlayNoFontXmlText:GetText() == 'z'"));
}

// T262: 0x004a8340 and 0x004a4220 answer nil for one without a name; 0x0048b540: what is above
// an animation is its group.
TEST_CASE("an animation's name is of its group", "[replay][interface]") {
    Session s;
    Interface ui(s);
    ui.run("f = CreateFrame('Frame', 'PlayAn', UIParent) g = f:CreateAnimationGroup() a = g:CreateAnimation('Path') "
           "p = a:CreateControlPoint()");
    CHECK(ui.is("select('#', g:GetName()) == 1 and g:GetName() == nil"));
    CHECK(ui.is("select('#', a:GetName()) == 1 and a:GetName() == nil"));
    CHECK(ui.is("select('#', p:GetName()) == 1 and p:GetName() == nil"));
    wowee::ui::XmlNode root;
    std::string error;
    REQUIRE(wowee::ui::parseXml(
        "<Ui><Frame name=\"PlayAnT\" virtual=\"true\"><Animations><AnimationGroup name=\"$parentG\">"
        "<Path name=\"$parentA\"><ControlPoints><ControlPoint name=\"$parentP\"/></ControlPoints></Path></AnimationGroup>"
        "<AnimationGroup><Alpha name=\"$parentB\"/></AnimationGroup></Animations></Frame>"
        "<Frame name=\"PlayAn2\" inherits=\"PlayAnT\"/></Ui>",
        root, error));
    CHECK(ui.engine.buildElements(wowee::ui::emitFrameXml(root), "t.xml", [](const auto&) {}));
    CHECK(ui.is("rawget(_G, 'PlayAn2G') and PlayAn2GA:GetParent() == PlayAn2G and PlayAn2GAP:GetParent() == PlayAn2GA"));
    // A group without a name: what it is on.
    CHECK(ui.is("rawget(_G, 'PlayAn2B') and PlayAn2B:GetName() == 'PlayAn2B'"));
    CHECK(ui.is("not rawget(_G, 'PlayAnTG') and not rawget(_G, 'PlayAn2A')"));
    // A script's too.
    ui.run("h = PlayAn2:CreateAnimationGroup('$parentH') b = h:CreateAnimation('Alpha', '$parentC')");
    CHECK(ui.is("h:GetName() == 'PlayAn2H' and b:GetName() == 'PlayAn2HC' and PlayAn2HC == b"));
}

// 0x0061e6b0: every kind of link shows its tooltip, a quest's once it is known.
TEST_CASE("a quest's link shows its tooltip", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("tip = CreateFrame('GameTooltip', 'PlayQuestTip', UIParent) tip:Hide() tip:SetOwner(UIParent, 'ANCHOR_NONE') "
           "tip:SetHyperlink('quest:435:20')");
    CHECK(ui.is("not tip:IsShown()"));
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_QUEST_QUERY_RESPONSE));
    for (int i = 0; i < 65; ++i) p.writeUInt32(i == 0 ? 435u : i == 1 ? 2u : i == 2 ? 20u : 0u);
    for (const char* text : {"Escorting Erland", "Escort Erland.", "Details.", "", ""}) p.writeString(text);
    for (int i = 0; i < 16 + 12; ++i) p.writeUInt32(0);
    for (int i = 0; i < 4; ++i) p.writeString("");
    s.receive(p);
    ui.run("__WoweeQuestQueryAnswered(435)");
    CHECK(ui.is("tip:IsShown() and tip:NumLines() == 3"));
    ui.run("tip:SetHyperlink('quest:435:20')");   // and its second click closes it
    CHECK(ui.is("not tip:IsShown()"));
    ui.run("tip:SetOwner(UIParent, 'ANCHOR_NONE') tip:SetHyperlink('|cffffff00|Hquest:435:20|h[x]|h|r')");
    CHECK(ui.is("tip:IsShown() and tip:NumLines() == 3"));
}

// 0x0052c680, 0x0052c8c0, 0x005726f0, 0x00573200: the party is the four of its
// slots - in a raid the player's own subgroup - and the player itself, the raid
// its whole roster; whoever of them is out of view is a player all the same.
TEST_CASE("who is in the player's party and in its raid", "[replay][interface]") {
    constexpr uint64_t kNear = 0x1001, kFar = 0x1002, kPet = 0x5001, kOther = 0x2001;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    auto& gh = s.handler();
    CHECK(ui.is("UnitInParty('player') == 1 and not UnitInRaid('player')"));
    const auto group = [&](uint8_t type) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_GROUP_LIST));
        p.writeUInt8(type);
        for (int i = 0; i < 3; ++i) p.writeUInt8(0);
        p.writeUInt64(0x1234);
        p.writeUInt32(1);
        p.writeUInt32(2);
        for (int i = 0; i < 2; ++i) {
            p.writeString(i == 0 ? "Nah" : "Fern");
            p.writeUInt64(i == 0 ? kNear : kFar);
            p.writeUInt8(1);
            p.writeUInt8(static_cast<uint8_t>(i));   // its subgroup
            p.writeUInt8(0);
            p.writeUInt8(0);
        }
        p.writeUInt64(Session::kPlayerGuid);
        for (int i = 0; i < 12; ++i) p.writeUInt8(0);
        s.receive(p);
    };
    group(0);   // no raid's roster in a party
    CHECK(ui.is("GetNumRaidMembers() == 0 and not UnitExists('raid1') and UnitExists('party1')"));
    group(2);
    s.receive(UpdateObject()
                  .create(kNear, wowee::game::ObjectType::PLAYER,
                          {{UF::UNIT_FIELD_HEALTH, 10}, {UF::UNIT_FIELD_MAXHEALTH, 10},
                           {UF::UNIT_FIELD_SUMMON, static_cast<uint32_t>(kPet)}}));
    s.receive(UpdateObject()
                  .create(kPet, wowee::game::ObjectType::UNIT,
                          {{UF::OBJECT_FIELD_ENTRY, 416}, {UF::UNIT_FIELD_HEALTH, 10}, {UF::UNIT_FIELD_MAXHEALTH, 10},
                           {UF::UNIT_FIELD_PETNUMBER, 77}}));
    s.receive(UpdateObject().create(kOther, wowee::game::ObjectType::PLAYER,
                                    {{UF::UNIT_FIELD_HEALTH, 10}, {UF::UNIT_FIELD_MAXHEALTH, 10}}));
    const auto all = [&](uint64_t target, const char* party, const char* raid, const char* withPetParty,
                         const char* withPetRaid, const char* player) {
        gh.setTarget(target);
        const auto same = [&](const std::string& call, const char* answer) {
            INFO(target << " " << call);
            CHECK(ui.is(call + "('target') == " + answer));
        };
        same("UnitInParty", party);
        ui.run("inRaid = UnitInRaid('target') and 1");
        CHECK(ui.is(std::string("inRaid == ") + raid));
        same("UnitPlayerOrPetInParty", withPetParty);
        same("UnitPlayerOrPetInRaid", withPetRaid);
        same("UnitIsPlayer", player);
    };
    all(kNear, "1", "1", "1", "1", "1");
    all(kFar, "nil", "1", "nil", "1", "1");       // another subgroup, out of view
    all(kPet, "nil", "nil", "1", "1", "nil");
    all(kOther, "nil", "nil", "nil", "nil", "1");
    CHECK(ui.is("UnitInParty('player') == 1 and UnitPlayerOrPetInParty('player') == nil"));
    CHECK(ui.is("UnitPlayerOrPetInRaid('player') == 1"));
    // The raid's roster has the player last (0x006d8870).
    CHECK(ui.is("GetNumRaidMembers() == 3 and UnitInRaid('player') == 2"));
    CHECK(ui.is("UnitIsUnit('raid3', 'player') and not UnitExists('raid4')"));
    CHECK(ui.is("select(3, GetRaidRosterInfo(3)) == 1 and GetRaidRosterInfo(4) == nil"));
    CHECK(gh.unitTokensForGuid(Session::kPlayerGuid).back() == "raid3");
    ui.run("SetRaidRosterSelection(3)");
    CHECK(ui.is("GetRaidRosterSelection() == 3"));

    // 0x00718a00: a pet is of no rank, whatever its kind's.
    wowee::network::Packet creature(wowee::game::wireOpcode(Opcode::SMSG_CREATURE_QUERY_RESPONSE));
    creature.writeUInt32(416);
    creature.writeString("Wichtel");
    for (int i = 0; i < 5; ++i) creature.writeString("");
    for (uint32_t v : {0u, 3u, 23u, 1u}) creature.writeUInt32(v);
    for (uint32_t v : {0u, 0u, 4449u, 0u, 0u, 0u}) creature.writeUInt32(v);
    for (int i = 0; i < 12; ++i) creature.writeUInt32(0);
    s.receive(creature);
    CHECK(gh.getCreatureRank(416) == 1);
    gh.setTarget(kPet);
    CHECK(ui.is("UnitClassification('target') == 'normal'"));
}

// 0x005efb00: a mount display; not one water can be walked on with.
TEST_CASE("a macro's mounted is true on water too", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100},
                   {UF::UNIT_FIELD_MOUNTDISPLAYID, 2404}});
    Interface ui(s);
    auto& gh = s.handler();
    REQUIRE(gh.isMounted());
    CHECK(ui.is("SecureCmdOptionParse('[mounted] a; b') == 'a'"));
    gh.movementInfoRef().flags |= static_cast<uint32_t>(wowee::game::MovementFlags::WATER_WALK);
    CHECK(ui.is("SecureCmdOptionParse('[mounted] a; b') == 'a'"));
}

// 0x005cf040: a guild's charter is not for one in a guild.
TEST_CASE("a guild's charter is signed by those without a guild", "[replay][interface]") {
    constexpr uint64_t kOwner = 0x1001, kCharter = 0x4000000000000444ull;
    for (const bool arena : {false, true}) {
        for (const bool guilded : {false, true}) {
            INFO(arena << guilded);
            Session s;
            s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
            Interface ui(s);
            wowee::game::Character self;
            self.guid = Session::kPlayerGuid;
            self.guildId = guilded ? 5 : 0;
            s.handler().charactersRef().push_back(self);
            s.handler().setActiveCharacterGuid(self.guid);
            wowee::network::Packet list(wowee::game::wireOpcode(Opcode::SMSG_PETITION_SHOW_SIGNATURES));
            list.writeUInt64(kCharter);
            list.writeUInt64(kOwner);
            list.writeUInt32(77);
            list.writeUInt8(0);
            s.receive(list);
            wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_PETITION_QUERY_RESPONSE));
            p.writeUInt32(77);
            p.writeUInt64(kOwner);
            p.writeString("The Seven");
            p.writeUInt8(0);
            for (uint32_t v : {9u, 9u, arena ? 2u : 0u}) p.writeUInt32(v);
            s.receive(p);
            CHECK(ui.is("CanSignPetition() == 1") == (arena || !guilded));
        }
    }
}

// 0x00545950, 0x005459c0 -> 0x00544140: by the map the place is on, wherever the
// player is (T302: a corpse on the other continent had no place).
TEST_CASE("a corpse on another map has its place on that map", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 1}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::PLAYER_FLAGS, 0x10}});
    Interface ui(s);
    uint32_t askedMap = 0;
    glm::vec3 askedAt(0.0f);
    wowee::addons::LuaServices services;
    services.mapUVForPlace = [&](uint32_t map, float x, float y, float z, float& u, float& v) {
        askedMap = map;
        askedAt = glm::vec3(x, y, z);
        u = 0.25f;
        v = 0.75f;
        return map == 1;
    };
    ui.engine.setLuaServices(services);
    const auto answer = [&](uint32_t map) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::MSG_CORPSE_QUERY));
        p.writeUInt8(1);
        p.writeUInt32(map);
        for (float f : {1318.0f, -4655.0f, 30.0f}) p.writeFloat(f);
        p.writeUInt32(map);
        p.writeUInt32(0);
        s.receive(p);
    };
    REQUIRE(s.handler().getCurrentMapId() != 1);
    answer(1);
    ui.run("x, y = GetCorpseMapPosition()");
    CHECK(ui.is("x == 0.25 and y == 0.75"));
    CHECK(askedMap == 1);
    CHECK(askedAt == glm::vec3(-4655.0f, 1318.0f, 30.0f));
    answer(530);
    ui.run("x, y = GetCorpseMapPosition()");
    CHECK(ui.is("x == 0 and y == 0"));
}

// T305: 0x0058dd50 keeps what the page shown gives, the giver's offer and its
// reward alike; the spell's line goes by the trade spell's attribute and by
// what the spell cast at completion does (0x0058d670).
TEST_CASE("a quest's page shows all it gives", "[replay][interface]") {
    constexpr uint64_t kGiver = 0xF130000BEC00150Eull;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    auto& gh = s.handler();
    gh.spellNameCacheRef()[3100].name = "Blacksmithing";
    gh.spellNameCacheRef()[3100].attr = 0x20;
    gh.spellNameCacheRef()[3101].name = "Learning";
    gh.spellNameCacheRef()[3101].effectIds[0] = 36;
    const auto rewards = [](wowee::network::Packet& p) {
        p.writeUInt32(500);
        p.writeUInt32(250);
        p.writeUInt32(1240);   // honor x 10
        p.writeFloat(0.0f);
    };
    wowee::network::Packet offer(wowee::game::wireOpcode(Opcode::SMSG_QUESTGIVER_QUEST_DETAILS));
    offer.writeUInt64(kGiver);
    offer.writeUInt64(0);
    offer.writeUInt32(861);
    for (const char* text : {"The Way", "Details.", "Objectives."}) offer.writeString(text);
    offer.writeUInt8(0);
    for (int i = 0; i < 2; ++i) offer.writeUInt32(0);
    offer.writeUInt8(0);
    for (int i = 0; i < 2; ++i) offer.writeUInt32(0);
    rewards(offer);
    for (uint32_t v : {3100u, 3101u, 0u, 2u, 15u, 0u}) offer.writeUInt32(v);
    for (int i = 0; i < 16; ++i) offer.writeUInt32(0);
    s.receive(offer);
    CHECK(ui.is("GetRewardHonor() == 124 and GetRewardTalents() == 2 and GetRewardArenaPoints() == 15"));
    CHECK(ui.is("select('#', GetRewardSpell()) == 4"));
    CHECK(ui.is("select(2, GetRewardSpell()) == 'Blacksmithing' and select(3, GetRewardSpell()) == 1"));
    CHECK(ui.is("select(4, GetRewardSpell()) == 1 and GetRewardSpell() == nil"));   // no icon's file here
    CHECK(ui.is("GetQuestSpellLink():find('|Hspell:3100|h', 1, true) ~= nil"));
    CHECK(ui.is("select('#', GetQuestLogRewardSpell()) == 4 and GetQuestLogRewardTitle() == nil"));

    wowee::network::Packet pay(wowee::game::wireOpcode(Opcode::SMSG_QUESTGIVER_OFFER_REWARD));
    pay.writeUInt64(kGiver);
    pay.writeUInt32(862);
    for (const char* text : {"The Way", "Well done."}) pay.writeString(text);
    pay.writeUInt8(0);
    for (int i = 0; i < 5; ++i) pay.writeUInt32(0);   // flags, players, emotes, choices, items
    rewards(pay);
    for (uint32_t v : {8u, 3101u, 0u, 0u, 1u, 7u, 0u}) pay.writeUInt32(v);
    for (int i = 0; i < 15; ++i) pay.writeUInt32(0);
    s.receive(pay);
    CHECK(ui.is("GetRewardHonor() == 124 and GetRewardTalents() == 1 and GetRewardArenaPoints() == 7"));
    CHECK(ui.is("select(2, GetRewardSpell()) == 'Learning' and not select(3, GetRewardSpell())"));
    CHECK(ui.is("not select(4, GetRewardSpell())"));
    CHECK(ui.is("GetQuestSpellLink():find('|Hspell:3101|h', 1, true) ~= nil"));
}

// T325: 0x00401480 puts down what the cursor holds (0x005194c0).
TEST_CASE("a change of map empties the cursor", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::PLAYER_FIELD_COINAGE, 5000}});
    Interface ui(s);
    ui.run("PickupPlayerMoney(10)");
    REQUIRE(ui.is("GetCursorInfo() == 'money'"));
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_TRANSFER_PENDING));
    p.writeUInt32(571);
    s.receive(p);
    CHECK(ui.is("GetCursorInfo() == nil"));
}

// T329: 0x0052e9f0 stands the player up as the barber's window goes; and a stand
// state that is no chair's takes the window (0x006e2b30).
TEST_CASE("the barber's chair and window go together", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    auto& gh = s.handler();
    const auto state = [&](uint8_t v) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_STANDSTATE_UPDATE));
        p.writeUInt8(v);
        s.receive(p);
    };
    const auto open = [&] {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_ENABLE_BARBER_SHOP));
        s.receive(p);
        state(5);
        REQUIRE(gh.isBarberShopOpen());
    };
    const auto stood = [&] { return s.sent().of(Opcode::CMSG_STANDSTATECHANGE).size(); };
    open();
    ui.run("CancelBarberShop()");
    CHECK_FALSE(gh.isBarberShopOpen());
    REQUIRE(stood() == 1);
    CHECK(s.sent().of(Opcode::CMSG_STANDSTATECHANGE).back().readUInt32() == 0);
    state(0);
    CHECK(stood() == 1);
    CHECK_FALSE(gh.takeBarberLeft());
    open();
    state(0);
    CHECK_FALSE(gh.isBarberShopOpen());
    CHECK(stood() == 1);
    CHECK(gh.takeBarberLeft());
}

// 0x0057f1b0 / 0x0057f4f0: a tracking spell that runs is shown by its
// ActiveIconID, where it has one; one that does not run by its SpellIconID.
TEST_CASE("the tracking that runs is drawn with its active icon", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 80}});
    auto& gh = s.handler();
    constexpr uint32_t kHerbs = 2383, kMinerals = 2580;
    for (uint32_t id : {kHerbs, kMinerals}) {
        auto& e = gh.spellNameCacheRef()[id];
        e.name = id == kHerbs ? "Find Herbs" : "Find Minerals";
        e.effectIds[0] = 6;
        e.effectAuraIds[0] = 45;
        e.activeIconId = id == kHerbs ? 900 : 0;
    }
    gh.setSpellIconPathResolver([](uint32_t id) { return "spell" + std::to_string(id); });
    gh.setIconPathResolver([](uint32_t id) { return "icon" + std::to_string(id); });
    Interface ui(s);
    {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_INITIAL_SPELLS));
        p.writeUInt8(0);
        p.writeUInt16(2);
        for (uint32_t id : {kHerbs, kMinerals}) {
            p.writeUInt32(id);
            p.writeUInt16(0);
        }
        p.writeUInt16(0);
        s.receive(p);
    }
    const auto aura = [&](uint8_t slot, uint32_t spell) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_AURA_UPDATE));
        p.writePackedGuid(Session::kPlayerGuid);
        p.writeUInt8(slot);
        p.writeUInt32(spell);
        if (spell != 0) {
            p.writeUInt8(0x10 | 0x08 | 0x01);
            p.writeUInt8(80);
            p.writeUInt8(0);
        }
        s.receive(p);
    };
    // By name: Find Herbs first.
    CHECK(ui.is("select(2, GetTrackingInfo(1)) == 'spell2383' and not select(3, GetTrackingInfo(1))"));
    aura(3, kHerbs);
    CHECK(ui.is("select(2, GetTrackingInfo(1)) == 'icon900' and select(3, GetTrackingInfo(1)) == 1"));
    CHECK(ui.is("GetTrackingTexture() == 'icon900'"));
    CHECK(ui.is("select(2, GetTrackingInfo(2)) == 'spell2580'"));
    aura(3, 0);
    aura(4, kMinerals);   // no active icon of its own
    CHECK(ui.is("GetTrackingTexture() == 'spell2580'"));
    CHECK(ui.is("select(2, GetTrackingInfo(1)) == 'spell2383'"));
    // 0x0057f1b0 rounds the index: 1.6 is the second.
    CHECK(ui.is("select(2, GetTrackingInfo(1.6)) == 'spell2580'"));
}

// T335, T336: what the companion tab lists and which one is out, as 12340 asks
// them. 0x008009b0: a first effect SUMMON whose first EffectMiscValueB's
// SummonProperties row has Slot 5 is a critter - row 41 or any other of that
// slot (the argent squire's 1341) - a first aura MOUNTED a mount. 0x00802cb0:
// a critter is out when the player's UNIT_FIELD_CRITTER carries its entry
// (a summon leaves no aura), and DismissCompanion (0x0053e6b0 -> 0x00802f80)
// sends it away with CMSG_DISMISS_CRITTER.
TEST_CASE("a companion is filed, shown out and sent away as 12340 does", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    constexpr uint32_t kSquire = 62609, kCub = 69536, kGuardian = 4073, kWolf = 6654, kSecondAura = 7001;
    constexpr uint32_t kSquireEntry = 33238, kCubEntry = 36909;
    struct Row {
        uint32_t id;
        const char* name;
        uint32_t effects[3], auras[3], misc0, miscB0, slot0;
    };
    const Row rows[] = {
        {kSquire, "Argent Squire", {28, 0, 0}, {0, 0, 0}, kSquireEntry, 1341, 5},
        {kCub, "Wind Rider Cub", {28, 0, 0}, {0, 0, 0}, kCubEntry, 41, 5},
        {kGuardian, "Mechanical Dragonling", {28, 0, 0}, {0, 0, 0}, 2671, 64, 0},
        {kWolf, "Brown Wolf", {6, 6, 0}, {78, 32, 0}, 14334, 0, 0},
        {kSecondAura, "Not a mount", {6, 6, 0}, {4, 78, 0}, 0, 0, 0},
    };
    for (const Row& r : rows) {
        auto& e = s.handler().spellNameCacheRef()[r.id];
        e.name = r.name;
        for (int k = 0; k < 3; ++k) {
            e.effectIds[k] = r.effects[k];
            e.effectAuraIds[k] = r.auras[k];
        }
        e.effectMiscValues[0] = r.misc0;
        e.effectMiscValuesB[0] = r.miscB0;
        e.summonSlot0 = r.slot0;
    }
    wowee::network::Packet known(wowee::game::wireOpcode(Opcode::SMSG_INITIAL_SPELLS));
    known.writeUInt8(0);
    known.writeUInt16(static_cast<uint16_t>(std::size(rows)));
    for (const Row& r : rows) {
        known.writeUInt32(r.id);
        known.writeUInt16(0);
    }
    known.writeUInt16(0);
    s.receive(known);

    CHECK(ui.is("GetNumCompanions('CRITTER') == 2"));
    CHECK(ui.is("GetNumCompanions('MOUNT') == 1"));
    // By name: the squire first.
    CHECK(ui.is("select(3, GetCompanionInfo('CRITTER', 1)) == 62609 and select(1, GetCompanionInfo('CRITTER', 1)) == 33238"));
    CHECK(ui.is("select(3, GetCompanionInfo('MOUNT', 1)) == 6654"));
    CHECK(ui.is("not select(5, GetCompanionInfo('CRITTER', 1)) and not select(5, GetCompanionInfo('CRITTER', 2))"));

    ui.run("heard = {} local f = CreateFrame('Frame') f:RegisterEvent('COMPANION_UPDATE') "
           "f:SetScript('OnEvent', function(self, event, kind) heard[#heard + 1] = kind end)");
    constexpr uint64_t kSquireGuid = 0xF130000000000000ull | (uint64_t{kSquireEntry} << 24) | 0x1234u;
    const auto critter = static_cast<uint16_t>(wowee::game::fieldIndex(UF::UNIT_FIELD_CRITTER));
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{critter, static_cast<uint32_t>(kSquireGuid)},
                                                             {static_cast<uint16_t>(critter + 1),
                                                              static_cast<uint32_t>(kSquireGuid >> 32)}}));
    CHECK(ui.is("select(5, GetCompanionInfo('CRITTER', 1)) == 1 and not select(5, GetCompanionInfo('CRITTER', 2))"));
    CHECK(ui.is("#heard == 1 and heard[1] == 'CRITTER'"));

    ui.run("DismissCompanion('MOUNT')");
    CHECK(s.sent().of(Opcode::CMSG_DISMISS_CRITTER).empty());
    ui.run("DismissCompanion('CRITTER')");
    const auto away = s.sent().of(Opcode::CMSG_DISMISS_CRITTER);
    REQUIRE(away.size() == 1);
    wowee::network::Packet p = away[0];
    CHECK(p.readUInt64() == kSquireGuid);
}

// T298: GetRepairAllCost as 0x00585990: 0 alone without a merchant that
// repairs (UNIT_NPC_FLAGS 0x1000); with one, the cost and 1 where something
// worn or carried - not in the bank, not flagged 0x8 - is damaged, nil where
// nothing is (MerchantFrame greys the button by it), whatever the money.
TEST_CASE("repairing all asks what is damaged of a merchant that repairs", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    const auto count = [&] {
        ui.run("n = select('#', GetRepairAllCost()) cost, damaged = GetRepairAllCost()");
    };
    count();
    CHECK(ui.is("n == 1 and cost == 0"));

    const auto openMerchant = [&](uint64_t guid, uint32_t npcFlags) {
        s.receive(UpdateObject().create(guid, wowee::game::ObjectType::UNIT,
                                        {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100},
                                         {UF::UNIT_NPC_FLAGS, npcFlags}},
                                        false));
        wowee::network::Packet list(wowee::game::wireOpcode(Opcode::SMSG_LIST_INVENTORY));
        list.writeUInt64(guid);
        list.writeUInt8(0);
        list.writeUInt8(0);
        s.receive(list);
        REQUIRE(s.handler().isVendorWindowOpen());
    };
    // A vendor that does not repair: still 0 alone.
    openMerchant(0xF130000C67000001ull, 0x80);
    count();
    CHECK(ui.is("n == 1 and cost == 0"));

    openMerchant(0xF130000C67000002ull, 0x80 | 0x1000);
    auto& items = s.handler().onlineItemsRef();
    const auto carry = [&](uint64_t guid, int slot, uint32_t cur, uint32_t max, uint32_t flags) {
        wowee::game::ItemDef item;
        item.guid = guid;
        item.itemId = 2000 + static_cast<uint32_t>(slot);
        if (slot >= 0) s.handler().getInventory().setBackpackSlot(slot, item);
        auto& held = items[guid];
        held.entry = item.itemId;
        held.curDurability = cur;
        held.maxDurability = max;
        held.flags = flags;
    };
    carry(0x4000000000000901ull, 0, 30, 30, 0);
    count();
    CHECK(ui.is("n == 2 and cost == 0 and damaged == nil"));
    // Damaged, but flagged 0x8, or not carried (the bank's): nothing to repair.
    carry(0x4000000000000902ull, 1, 10, 30, 0x8);
    carry(0x4000000000000903ull, -1, 10, 30, 0);
    count();
    CHECK(ui.is("n == 2 and damaged == nil"));
    // Damaged in the backpack: 1, whether or not it can be paid for.
    carry(0x4000000000000904ull, 2, 10, 30, 0);
    count();
    CHECK(ui.is("n == 2 and damaged == 1"));
}

// T341: Wintergrasp's "join the battle?" dialog was empty - StaticPopup's
// BFMGR_INVITED_TO_ENTER takes its countdown from GetWorldPVPQueueStatus(1),
// which answered "none" whatever the realm had said. The client keeps the
// battle, its status, zone and when the invitation runs out (0x00549500)
// and answers from it (0x0054c8a0).
TEST_CASE("the world PvP queue is what the realm told of", "[replay][interface][battlefield]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    const auto packet = [&](Opcode op, std::initializer_list<uint32_t> words, std::initializer_list<uint8_t> bytes) {
        wowee::network::Packet p(wowee::game::wireOpcode(op));
        for (uint32_t w : words) p.writeUInt32(w);
        for (uint8_t b : bytes) p.writeUInt8(b);
        s.receive(p);
    };
    // The server's clock, a fixed time whatever the machine's.
    packet(Opcode::SMSG_WORLD_STATE_UI_TIMER_UPDATE, {1000000u}, {});
    ui.run("q = {GetWorldPVPQueueStatus(1)}");
    CHECK(ui.is("q[1] == 'none' and q[2] == nil and q[3] == 0 and q[4] == 0"));
    // Queued: the realm took the request for battle 1 in Wintergrasp (4197).
    packet(Opcode::SMSG_BATTLEFIELD_MGR_QUEUE_REQUEST_RESPONSE, {1u, 4197u}, {1, 0, 0});
    ui.run("q = {GetWorldPVPQueueStatus(1)}");
    CHECK(ui.is("q[1] == 'queued' and q[3] == 1 and q[4] == 0"));
    // Invited to enter until twenty seconds on by the server's time.
    packet(Opcode::SMSG_BATTLEFIELD_MGR_ENTRY_INVITE, {1u, 4197u, 1000020u}, {});
    ui.run("q = {GetWorldPVPQueueStatus(1)}");
    CHECK(ui.is("q[1] == 'confirm' and q[3] == 1 and q[4] >= 19 and q[4] <= 20"));
    CHECK(ui.is("select('#', GetWorldPVPQueueStatus(2)) == 0"));
    // In the battle: none again.
    packet(Opcode::SMSG_BATTLEFIELD_MGR_ENTERED, {1u}, {1, 0, 0});
    ui.run("q = {GetWorldPVPQueueStatus(1)}");
    CHECK(ui.is("q[1] == 'none' and q[3] == 1 and q[4] == 0"));
}

// The eject's reason is one of the realm's numbers, read as 0x005499c0 does:
// 8 the player left, 10 too low a level; the battle is on at a status of 2.
TEST_CASE("an eject from the battlefield says why as the client reads it", "[replay][interface][battlefield]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("seen = {} local f = CreateFrame('Frame') f:RegisterEvent('BATTLEFIELD_MGR_EJECTED') "
           "f:SetScript('OnEvent', function(self, event, ...) seen[#seen + 1] = {...} end)");
    const auto eject = [&](uint8_t reason, uint8_t status, uint8_t relocated) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_BATTLEFIELD_MGR_EJECTED));
        p.writeUInt32(1u);
        p.writeUInt8(reason);
        p.writeUInt8(status);
        p.writeUInt8(relocated);
        s.receive(p);
    };
    eject(10, 2, 0);
    eject(8, 1, 1);
    REQUIRE(ui.is("#seen == 2"));
    // battleID, playerExited, relocated, battleActive, lowLevel.
    CHECK(ui.is("not seen[1][2] and not seen[1][3] and seen[1][4] ~= nil and seen[1][5] ~= nil"));
    CHECK(ui.is("seen[2][2] ~= nil and seen[2][3] ~= nil and not seen[2][4] and not seen[2][5]"));
}

// T339 (c): the "consolidate buffs" box. BuffFrame.lua puts a buff in the
// summary box when UnitAura's tenth value says so, which 0x006147c0 answers
// 1 for a spell with AttributesEx7 0x10000000 and nil for the rest; always
// nil, the box changed nothing.
TEST_CASE("a buff the spell marks for it is to be consolidated", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 80}});
    auto& gh = s.handler();
    constexpr uint32_t kKings = 20217, kShield = 17;
    gh.spellNameCacheRef()[kKings].name = "Blessing of Kings";
    gh.spellNameCacheRef()[kKings].attrEx7 = 0x10000000u;
    gh.spellNameCacheRef()[kShield].name = "Power Word: Shield";
    Interface ui(s);
    const auto aura = [&](uint8_t slot, uint32_t spell) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_AURA_UPDATE));
        p.writePackedGuid(Session::kPlayerGuid);
        p.writeUInt8(slot);
        p.writeUInt32(spell);
        p.writeUInt8(0x10 | 0x08 | 0x01);
        p.writeUInt8(80);
        p.writeUInt8(0);
        s.receive(p);
    };
    aura(0, kKings);
    aura(1, kShield);
    CHECK(ui.is("select(10, UnitAura('player', 'Blessing of Kings')) == 1"));
    CHECK(ui.is("select(10, UnitAura('player', 'Power Word: Shield')) == nil"));
    CHECK(ui.is("select(11, UnitAura('player', 'Power Word: Shield')) == 17"));
}

// Found by AD in code: UnitAura's eighth value is the caster's unit token
// as 0x0060b420 finds it - also for a caster that is a totem or guardian of
// someone with a token (CREATEDBY / CHARMEDBY), here the player's own totem
// -> "player"; WoWee knew player/target/focus/pet only, so the player's
// totem buffs read nil. An aura not found answers no value (0x006147c0
// returns 0; WoWee pushed a nil).
TEST_CASE("an aura's caster answers by its token or its maker's (0x0060b420)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 80}});
    auto& gh = s.handler();
    constexpr uint64_t kTotem = 0xF130000F5A000001ull, kWolf = 0xF130000299000002ull, kStray = 0xF130000299000003ull;
    const uint32_t lo = static_cast<uint32_t>(Session::kPlayerGuid), hi = static_cast<uint32_t>(Session::kPlayerGuid >> 32);
    s.receive(UpdateObject().create(kTotem, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 5}, {UF::UNIT_FIELD_MAXHEALTH, 5},
                                     {UF::UNIT_FIELD_CREATEDBY, lo}}));
    REQUIRE(hi == 0);   // the maker's guid is its low half alone
    s.receive(UpdateObject().create(kWolf, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 5}, {UF::UNIT_FIELD_MAXHEALTH, 5}}));
    s.receive(UpdateObject().create(kStray, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 5}, {UF::UNIT_FIELD_MAXHEALTH, 5}}));
    constexpr uint32_t kStrength = 8076, kHowl = 1002, kBite = 1003;
    gh.spellNameCacheRef()[kStrength].name = "Strength of Earth";
    gh.spellNameCacheRef()[kHowl].name = "Howl";
    gh.spellNameCacheRef()[kBite].name = "Bite";
    Interface ui(s);
    gh.setTarget(kWolf);
    const auto aura = [&](uint8_t slot, uint32_t spell, uint64_t caster) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_AURA_UPDATE));
        p.writePackedGuid(Session::kPlayerGuid);
        p.writeUInt8(slot);
        p.writeUInt32(spell);
        p.writeUInt8(0x10 | 0x01);   // no 0x08: the caster's guid follows
        p.writeUInt8(80);
        p.writeUInt8(0);
        p.writePackedGuid(caster);
        s.receive(p);
    };
    aura(0, kStrength, kTotem);
    aura(1, kHowl, kWolf);
    aura(2, kBite, kStray);
    CHECK(ui.is("select(8, UnitAura('player', 'Strength of Earth')) == 'player'"));
    CHECK(ui.is("select(8, UnitAura('player', 'Howl')) == 'target'"));
    CHECK(ui.is("select(8, UnitAura('player', 'Bite')) == nil"));
    CHECK(ui.is("select('#', UnitAura('player', 'Nothing')) == 0"));
    CHECK(ui.is("select('#', UnitBuff('player', 9)) == 0"));
}

// Found by AD in code: CancelUnitBuff (0x00804220) finds the aura as UnitAura
// does - index or name and rank, and the filter - and cancels only one whose
// aura flags say cancelable (0x10) or of a channelled spell (AttributesEx
// 0x4). WoWee cancelled any buff and ignored rank and filter.
TEST_CASE("CancelUnitBuff takes only a cancelable aura, by the filter (0x00804220)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 80}});
    s.setConnected(true);
    auto& gh = s.handler();
    constexpr uint32_t kKings = 20217, kFixed = 20218, kWeak = 6788, kDrain = 5138;
    gh.spellNameCacheRef()[kKings].name = "Blessing of Kings";
    gh.spellNameCacheRef()[kFixed].name = "Fixed";
    gh.spellNameCacheRef()[kWeak].name = "Weakened Soul";
    gh.spellNameCacheRef()[kDrain].name = "Drain Mana";
    gh.spellNameCacheRef()[kDrain].attrEx = 0x4;   // channelled
    Interface ui(s);
    const auto aura = [&](uint8_t slot, uint32_t spell, uint8_t flags) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_AURA_UPDATE));
        p.writePackedGuid(Session::kPlayerGuid);
        p.writeUInt8(slot);
        p.writeUInt32(spell);
        p.writeUInt8(flags);
        p.writeUInt8(80);
        p.writeUInt8(0);
        s.receive(p);
    };
    aura(0, kFixed, 0x08 | 0x01);          // not cancelable
    aura(1, kKings, 0x10 | 0x08 | 0x01);
    aura(2, kWeak, 0x80 | 0x08 | 0x01);    // a debuff, not cancelable
    aura(3, kDrain, 0x08 | 0x01);          // channelled
    const auto cancels = [&] { return s.sent().of(Opcode::CMSG_CANCEL_AURA).size(); };
    const size_t before = cancels();
    ui.run("CancelUnitBuff('player', 'Fixed')");                     // not cancelable
    ui.run("CancelUnitBuff('player', 'Weakened Soul', nil, 'HARMFUL')");
    ui.run("CancelUnitBuff('player', 'Blessing of Kings', 'Rank 9')");  // the rank does not match
    ui.run("CancelUnitBuff('player', 'Weakened Soul')");             // HELPFUL by default: none
    CHECK(cancels() == before);
    ui.run("CancelUnitBuff('player', 'Drain Mana')");                // channelled
    REQUIRE(cancels() == before + 1);
    CHECK(s.sent().of(Opcode::CMSG_CANCEL_AURA).back().readUInt32() == kDrain);
    ui.run("CancelUnitBuff('player', 'blessing of kings')");
    REQUIRE(cancels() == before + 2);
    CHECK(s.sent().of(Opcode::CMSG_CANCEL_AURA).back().readUInt32() == kKings);
}

// Found by AD in code: GetBindingKey (0x0055e9b0) pushes the keys bound to
// the command and nothing else - no value for an unbound one, one for a
// command with one key; WoWee always answered a pair padded with nils.
TEST_CASE("GetBindingKey answers only the keys there are (0x0055e9b0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    CHECK(ui.is("select('#', GetBindingKey('AD_NOTHING_BOUND')) == 0"));
    ui.run("SetBinding('CTRL-F9', 'TOGGLEWORLDSTATESCORES')");
    CHECK(ui.is("GetBindingKey('TOGGLEWORLDSTATESCORES') == 'CTRL-F9'"));
    CHECK(ui.is("select('#', GetBindingKey('TOGGLEWORLDSTATESCORES')) == 1"));
}

// Found by AD in code (AC's note): GetFriendInfo (0x006b4130) and
// GetIgnoreName (0x006b4620) round the index to nearest (fistp), and a
// friend's name is found without case (0x006b3570 -> 0x0076ea40); WoWee
// truncated the index and compared the name exactly.
TEST_CASE("friend and ignore indices round, friend names match without case", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    auto& gh = s.handler();
    wowee::game::ContactEntry a, b, c;
    a.guid = 0x101; a.name = "Anna"; a.flags = 0x1; a.level = 10;
    b.guid = 0x102; b.name = "Bert"; b.flags = 0x1; b.level = 20;
    c.guid = 0x103; c.name = "Cleo"; c.flags = 0x2;
    gh.contactsRef() = {a, b, c};
    Interface ui(s);
    CHECK(ui.is("GetFriendInfo(1.6) == 'Bert'"));
    CHECK(ui.is("GetFriendInfo(1.2) == 'Anna'"));
    CHECK(ui.is("select(2, GetFriendInfo('bERT')) == 20"));
    CHECK(ui.is("GetIgnoreName(0.7) == 'Cleo'"));
}

// UnitAura's ninth value (0x0053d680): a buff on a unit the player may attack
// that the player's spellsteal could take. The dispel types come from the
// last spell learned whose first effect is STEAL_BENEFICIAL_BUFF (126) at
// an enemy (target 6): its SpellDispelType row's mask, else the type's bit
// (0x00542030, 0x00be5d68). It was nil always, so the target frame's
// stealable buffs were never lit.
TEST_CASE("a buff the player's spellsteal could take is stealable", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 80},
                   {UF::UNIT_FIELD_FLAGS, 0x8}});
    auto& gh = s.handler();
    constexpr uint64_t kMammoth = 0xF130005D7C000001ull;
    s.receive(UpdateObject().create(kMammoth, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 100},
                                     {UF::UNIT_FIELD_MAXHEALTH, 100},
                                     {UF::UNIT_FIELD_FACTIONTEMPLATE, 14}}));
    REQUIRE(gh.unitMayAttack(Session::kPlayerGuid, kMammoth));
    constexpr uint32_t kSpellsteal = 30449, kWild = 1126, kChannel = 12051, kCurse = 1714, kRegrowth = 8936;
    auto& cache = gh.spellNameCacheRef();
    cache[kSpellsteal].name = "Spellsteal";
    cache[kSpellsteal].effectIds[0] = 126;
    cache[kSpellsteal].implicitTargetsA[0] = 6;
    cache[kSpellsteal].effectMiscValues[0] = 1;  // magic
    cache[kWild].name = "Mark of the Wild";
    cache[kWild].dispelType = 1;
    cache[kChannel].name = "Evocation";
    cache[kChannel].dispelType = 1;
    cache[kChannel].attrEx = 0x4;  // channelled
    cache[kCurse].name = "Curse of Tongues";
    cache[kCurse].dispelType = 2;
    cache[kRegrowth].name = "Regrowth";
    cache[kRegrowth].dispelType = 1;
    Interface ui(s);
    gh.setTarget(kMammoth);
    const auto aura = [&](uint8_t slot, uint32_t spell, uint8_t flags) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_AURA_UPDATE));
        p.writePackedGuid(kMammoth);
        p.writeUInt8(slot);
        p.writeUInt32(spell);
        p.writeUInt8(flags);
        p.writeUInt8(80);
        p.writeUInt8(0);
        s.receive(p);
    };
    aura(0, kWild, 0x10 | 0x08 | 0x01);
    aura(1, kChannel, 0x10 | 0x08 | 0x01);
    aura(2, kCurse, 0x10 | 0x08 | 0x01);
    aura(3, kRegrowth, 0x80 | 0x08 | 0x01);  // a harmful one
    // No spellsteal known: nothing is stealable.
    CHECK(ui.is("select(9, UnitAura('target', 'Mark of the Wild')) == nil"));
    wowee::network::Packet learn(wowee::game::wireOpcode(Opcode::SMSG_LEARNED_SPELL));
    learn.writeUInt32(kSpellsteal);
    learn.writeUInt16(0);
    s.receive(learn);
    CHECK(ui.is("select(9, UnitAura('target', 'Mark of the Wild')) == 1"));
    CHECK(ui.is("select(9, UnitAura('target', 'Evocation')) == nil"));
    CHECK(ui.is("select(9, UnitAura('target', 'Curse of Tongues')) == nil"));
    CHECK(ui.is("select(9, UnitAura('target', 'Regrowth', nil, 'HARMFUL')) == nil"));
    CHECK(ui.is("select(11, UnitAura('target', 'Mark of the Wild')) == 1126"));
    // The player's own buff: not on a unit the player may attack.
    wowee::network::Packet own(wowee::game::wireOpcode(Opcode::SMSG_AURA_UPDATE));
    own.writePackedGuid(Session::kPlayerGuid);
    own.writeUInt8(0);
    own.writeUInt32(kWild);
    own.writeUInt8(0x10 | 0x08 | 0x01);
    own.writeUInt8(80);
    own.writeUInt8(0);
    s.receive(own);
    CHECK(ui.is("select(9, UnitAura('player', 'Mark of the Wild')) == nil"));
}

// T347: 0x005210d0 refuses an item outside the spell's item class
// (SPELL_FAILED_BAD_TARGETS) and one milling or prospecting cannot take
// (CANT_BE_MILLED / _PROSPECTED) the same way: 0x00808200 says it, nothing is
// sent, and the cursor still waits. The milling refusal put it down.
TEST_CASE("an item milling cannot take leaves the cursor waiting", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    auto& gh = s.handler();
    constexpr uint64_t kTusk = 0x4000000000000901ull;
    constexpr uint32_t kTuskEntry = 3169, kMilling = 51005, kProspecting = 31252;
    s.receive(UpdateObject().create(kTusk, wowee::game::ObjectType::ITEM,
                                    {{UF::OBJECT_FIELD_ENTRY, kTuskEntry}, {UF::ITEM_FIELD_STACK_COUNT, 1}}));
    auto& tusk = gh.itemInfoCacheRef()[kTuskEntry];
    tusk.entry = kTuskEntry;
    tusk.valid = true;
    tusk.itemFlags = 0;
    gh.spellNameCacheRef()[kMilling].name = "Milling";
    gh.spellNameCacheRef()[kMilling].effectIds[0] = 158;
    gh.spellNameCacheRef()[kProspecting].name = "Prospecting";
    gh.spellNameCacheRef()[kProspecting].effectIds[0] = 127;
    for (const uint32_t spell : {kMilling, kProspecting}) {
        INFO(spell);
        gh.beginSpellItemTargeting(spell, "spell");
        REQUIRE(gh.isAwaitingItemTarget());
        gh.completeItemUseOnItem(kTusk);
        CHECK(gh.isAwaitingItemTarget());
        CHECK(s.sent().of(Opcode::CMSG_CAST_SPELL).empty());
        gh.cancelItemTargeting();
    }
}

// T339 (c): the video panel's "full screen glow" box writes ffxGlow, which the
// client asks before the glow is drawn (0x008c1270); it reached no setting,
// so the box moved and the glow stayed. It is the screen glow's row now.
TEST_CASE("the full screen glow's box turns the glow", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    std::map<std::string, std::string> settings{{"screenglow", "1"}};
    wowee::addons::LuaServices services;
    services.getClientSetting = [&](const std::string& key) {
        const auto it = settings.find(key);
        return it == settings.end() ? std::string{} : it->second;
    };
    services.setClientSetting = [&](const std::string& key, const std::string& value) { settings[key] = value; };
    ui.engine.setLuaServices(services);
    CHECK(ui.is("GetCVar('ffxGlow') == '1'"));
    ui.run("SetCVar('ffxGlow', '0')");
    CHECK(settings["screenglow"] == "0");
    settings["screenglow"] = "1";
    CHECK(ui.is("GetCVar('ffxGlow') == '1'"));
}

// T344: CancelItemTempEnchantment(hand) asks for the hand's equipment slot
// (0x008021d0 -> 0x006d7120: hand + 14, 15 the main hand, 16 the off hand),
// and only where a weapon there has a temporary enchant. 0 and 1 went out,
// the head's and the neck's slot, and the enchant stayed.
TEST_CASE("a weapon's temporary enchant is cancelled by its slot", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    auto& gh = s.handler();
    constexpr uint64_t kSword = 0x4000000000000101ull, kShield = 0x4000000000000102ull;
    gh.equipSlotGuidsRef()[15] = kSword;
    gh.equipSlotGuidsRef()[16] = kShield;
    gh.onlineItemsRef()[kSword].entry = 25;
    gh.onlineItemsRef()[kSword].temporaryEnchantId = 3609;
    gh.onlineItemsRef()[kShield].entry = 2129;
    ui.run("CancelItemTempEnchantment(1)");
    auto sent = s.sent().of(Opcode::CMSG_CANCEL_TEMP_ENCHANTMENT);
    REQUIRE(sent.size() == 1);
    CHECK(sent.front().readUInt32() == 15);
    // The off hand has none: nothing goes out.
    ui.run("CancelItemTempEnchantment(2)");
    CHECK(s.sent().of(Opcode::CMSG_CANCEL_TEMP_ENCHANTMENT).size() == 1);
    gh.onlineItemsRef()[kShield].temporaryEnchantId = 3609;
    ui.run("CancelItemTempEnchantment(2)");
    sent = s.sent().of(Opcode::CMSG_CANCEL_TEMP_ENCHANTMENT);
    REQUIRE(sent.size() == 2);
    CHECK(sent.back().readUInt32() == 16);
}

// T346: GetCurrentTitle (0x0051b3b0) is the player's PLAYER_CHOSEN_TITLE as
// it is - 0 without a title, as a create that leaves the field out says.
TEST_CASE("a character without a title has title 0", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    CHECK(ui.is("GetCurrentTitle() == 0"));
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_CHOSEN_TITLE, 42}}));
    CHECK(ui.is("GetCurrentTitle() == 42"));
}

// T348: the minimap's zoom setter (0x007f3ae0) keeps the level in the
// client's own CVar - minimapInsideZoom indoors, minimapZoom outside - so
// GetCVar("minimapZoom") follows the map. It stayed "3", the level kept under
// a name of WoWee's own.
TEST_CASE("the minimap's zoom is kept in minimapZoom and minimapInsideZoom", "[replay][interface]") {
    // The CVar store is saved as it changes: into a corner of this test's own.
    const int tag = 0;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("wowee_minimap_zoom_" + std::to_string(reinterpret_cast<std::uintptr_t>(&tag)));
    std::filesystem::create_directories(root);
    wowee::core::setEnvVar("WOWEE_CONFIG_ROOT", root.string().c_str());
    {
        Session s;
        s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
        Interface ui(s);
        ui.run("CreateFrame('Minimap', 'Minimap')");
        ui.run("Minimap:SetZoom(5)");
        CHECK(ui.is("Minimap:GetZoom() == 5"));
        CHECK(ui.is("GetCVar('minimapZoom') == '5'"));
        ui.run("Minimap:SetZoom(1)");
        CHECK(ui.is("GetCVar('minimapZoom') == '1'"));
        // Indoors the other one, the outside level left as it was.
        auto* map = ui.engine.widgets().findByName("Minimap");
        REQUIRE(map != nullptr);
        map->zoomIndoors = true;
        ui.run("Minimap:SetZoom(4)");
        CHECK(ui.is("GetCVar('minimapInsideZoom') == '4'"));
        CHECK(ui.is("GetCVar('minimapZoom') == '1'"));
        // Another frame sent SetZoom is not the map.
        map->zoomIndoors = false;
        ui.run("CreateFrame('Minimap', 'NotTheMap'):SetZoom(3)");
        CHECK(ui.is("GetCVar('minimapZoom') == '1'"));
    }
    wowee::core::unsetEnvVar("WOWEE_CONFIG_ROOT");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// T349: GetLatestCompletedAchievements (0x005b1c20) walks the earned list as
// 0x005b28d0 keeps it - the latest first by the whole time, of one time the
// larger UI order first - and leaves out the Statistics (0x005b0d50). By the
// day alone with the id breaking ties, five earned on one day came out by id.
TEST_CASE("the latest achievements come by the time of day, then by their order", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    using wowee::game::GameHandlerTestAccess;
    auto& gh = s.handler();
    GameHandlerTestAccess::setAchievementCategory(gh, 92, -1, false);  // General
    GameHandlerTestAccess::setAchievementCategory(gh, 1, -1, true);    // Statistics
    GameHandlerTestAccess::setAchievementCategory(gh, 130, 1, true);
    GameHandlerTestAccess::setAchievement(gh, 2078, 92, 5);
    GameHandlerTestAccess::setAchievement(gh, 2097, 92, 1);
    GameHandlerTestAccess::setAchievement(gh, 545, 92, 9);
    GameHandlerTestAccess::setAchievement(gh, 868, 92, 2);
    GameHandlerTestAccess::setAchievement(gh, 100, 130, 1);
    // Packed: minute, hour, weekday, day - 1, month - 1, year - 2000 from the low bits up.
    const auto at = [](uint32_t day, uint32_t hour, uint32_t minute) {
        return 26u << 24 | 10u << 20 | (day - 1) << 14 | 2u << 11 | hour << 6 | minute;
    };
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_ALL_ACHIEVEMENT_DATA));
    for (const auto& [id, time] : std::vector<std::pair<uint32_t, uint32_t>>{
             {2078, at(24, 18, 5)}, {2097, at(24, 9, 0)}, {545, at(24, 18, 5)}, {868, at(23, 23, 59)},
             {100, at(25, 12, 0)}}) {
        p.writeUInt32(id);
        p.writeUInt32(time);
    }
    p.writeUInt32(0xFFFFFFFFu);
    p.writeUInt32(0xFFFFFFFFu);
    s.receive(p);
    ui.run("latest = {GetLatestCompletedAchievements()}");
    CHECK(ui.is("#latest == 4"));
    CHECK(ui.is("latest[1] == 545 and latest[2] == 2078 and latest[3] == 2097 and latest[4] == 868"));
}

namespace {
// One item in the backpack's first slot, its template known.
void carry(Session& s, uint32_t entry, uint8_t inventoryType, uint32_t bindType, uint64_t guid = 0,
           uint32_t sellPrice = 0) {
    auto& gh = s.handler();
    auto& info = gh.itemInfoCacheRef()[entry];
    info.entry = entry;
    info.valid = true;
    info.name = "Item " + std::to_string(entry);
    info.inventoryType = inventoryType;
    info.bindType = bindType;
    wowee::game::ItemDef def;
    def.itemId = entry;
    def.name = info.name;
    def.inventoryType = inventoryType;
    def.bindType = bindType;
    def.guid = guid;
    def.sellPrice = sellPrice;
    info.sellPrice = sellPrice;
    REQUIRE(gh.inventoryRef().setBackpackSlot(0, def));
}
}  // namespace

// T351: a drop on the backpack (or a bag button) moved the item but left the
// cursor's kind holding it - GetCursorInfo and CursorHasItem still answered
// the item, and the next click was taken as another drop. 0x006df890 puts the
// cursor down (0x00519280) once the move is sent.
TEST_CASE("an item put in the backpack leaves the hand", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    carry(s, 1251, 0, 0);
    ui.run("PickupContainerItem(0, 1)");
    REQUIRE(ui.is("CursorHasItem() == 1"));
    ui.run("PutItemInBackpack()");
    CHECK(ui.is("not CursorHasItem()"));
    CHECK(ui.is("GetCursorInfo() == nil"));
}

// T352 / T351: a bind-on-equip piece dropped on its slot is parked for the
// EQUIP_BIND prompt and stays in the hand while the question is open
// (0x006df890 returns before putting the cursor down) - put down at once, the
// interface's CURSOR_UPDATE hid the prompt and its OnHide cancelled the equip.
// Accepted (EquipPendingItem, 0x006e01a0), the move is made and the hand is
// empty.
TEST_CASE("a bind-on-equip piece is held while its question is open", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    carry(s, 10019, 10, 2);  // hands, bind on equip
    ui.run("asked = nil local f = CreateFrame('Frame') f:RegisterEvent('EQUIP_BIND_CONFIRM') "
           "f:SetScript('OnEvent', function(self, event, index) asked = index end)");
    ui.run("PickupContainerItem(0, 1)");
    REQUIRE(ui.is("CursorHasItem() == 1"));
    const size_t sent = s.sent().all().size();
    ui.run("PickupInventoryItem(10)");
    CHECK(ui.is("asked == 0"));
    CHECK(ui.is("CursorHasItem() == 1"));
    CHECK(s.sent().all().size() == sent);
    ui.run("EquipPendingItem(0)");
    CHECK(s.sent().all().size() > sent);
    CHECK(ui.is("not CursorHasItem()"));
}

// T353: PickupMerchantItem (0x005853a0) asks what the hand holds before the
// index: an item held is sold to the open merchant and the cursor put down,
// whatever index came - a drop on the item list arrives as
// PickupMerchantItem(n) through MerchantItemButton_OnReceiveDrag. Item n was
// taken up instead and the held one went back to its bag unsold.
TEST_CASE("an item dropped on a merchant's item is sold", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    constexpr uint64_t kVendor = 0xF130000000000101ull;
    wowee::network::Packet list(wowee::game::wireOpcode(Opcode::SMSG_LIST_INVENTORY));
    list.writeUInt64(kVendor);
    list.writeUInt8(1);
    for (uint32_t v : {1u, 2901u, 100u, 0xFFFFFFFFu, 10u, 0u, 1u, 0u}) list.writeUInt32(v);
    s.receive(list);
    REQUIRE(s.handler().getVendorItems().items.size() == 1);
    carry(s, 15213, 13, 0, 0x4000000000000777ull, 100);
    ui.run("PickupContainerItem(0, 1)");
    REQUIRE(ui.is("CursorHasItem() == 1"));
    ui.run("PickupMerchantItem(1)");
    CHECK(s.sent().of(Opcode::CMSG_SELL_ITEM).size() == 1);
    CHECK(ui.is("GetCursorInfo() == nil"));
    // With nothing held the same click takes the merchant's item up.
    ui.run("PickupMerchantItem(1)");
    CHECK(ui.is("GetCursorInfo() == 'merchant'"));
}

// T350: PetCanBeRenamed (0x005d38c0) reads the pet - the player's first
// controlled unit, summoned by the player - and its UNIT_CAN_BE_RENAMED bit
// (UNIT_FIELD_BYTES_2's third byte, 0x01), not a note SMSG_PET_RENAMEABLE
// left: that was nil for a renameable pet after a relog and 1 for any pet
// afterwards.
TEST_CASE("a pet can be renamed while its own field says so", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    constexpr uint64_t kPet = 0xF140000000000042ull;
    const auto summoner = static_cast<uint32_t>(Session::kPlayerGuid);
    const auto summonerHi = static_cast<uint32_t>(Session::kPlayerGuid >> 32);
    s.receive(UpdateObject().create(kPet, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 50},
                                     {UF::UNIT_FIELD_MAXHEALTH, 50},
                                     {UF::UNIT_FIELD_SUMMONEDBY_LO, summoner},
                                     {UF::UNIT_FIELD_SUMMONEDBY_HI, summonerHi},
                                     {UF::UNIT_FIELD_BYTES_2, 0x00010000u}}));
    s.handler().setActivePet(kPet);
    CHECK(ui.is("PetCanBeRenamed() == 1"));
    s.receive(UpdateObject().values(kPet, {{UF::UNIT_FIELD_BYTES_2, 0u}}));
    CHECK(ui.is("PetCanBeRenamed() == nil"));
    // The packet that opens the naming dialog makes no pet renameable.
    s.receive(wowee::network::Packet(wowee::game::wireOpcode(Opcode::SMSG_PET_RENAMEABLE)));
    CHECK(ui.is("PetCanBeRenamed() == nil"));
}

// T354: UnitName (0x0060e740) names an object that is not a unit by its kind
// - an item by its item record, a game object by its own name - before it
// says UNKNOWNOBJECT. A quest an item starts names the item as "questnpc", and
// the quest frame's title said "Unbekannt".
TEST_CASE("an item or a game object a token names is called by its name", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    constexpr uint64_t kSkin = 0x40000000003BC2A0ull;
    auto& info = s.handler().itemInfoCacheRef()[20310];
    info.entry = 20310;
    info.valid = true;
    info.name = "Geschundene Daemonenhaut";
    s.receive(UpdateObject().create(kSkin, wowee::game::ObjectType::ITEM,
                                    {{UF::OBJECT_FIELD_ENTRY, 20310}, {UF::ITEM_FIELD_STACK_COUNT, 1}}));
    // The quest it starts, offered by the item (PlayerMenu::SendQuestGiverQuestDetails).
    wowee::network::Packet details(wowee::game::wireOpcode(Opcode::SMSG_QUESTGIVER_QUEST_DETAILS));
    details.writeUInt64(kSkin);
    details.writeUInt64(0);
    details.writeUInt32(7583);
    details.writeString("Der Verderber");
    details.writeString("Details.");
    details.writeString("Objectives.");
    details.writeUInt8(0);
    for (int i = 0; i < 2; ++i) details.writeUInt32(0);
    details.writeUInt8(0);
    for (int i = 0; i < 40; ++i) details.writeUInt32(0);
    s.receive(details);
    REQUIRE(ui.is("UnitGUID('questnpc') ~= nil"));
    CHECK(ui.is("UnitName('questnpc') == 'Geschundene Daemonenhaut'"));
}

// T355: SMSG_CANCEL_AUTO_REPEAT (0x0072d130) stops the player's own repeat
// only when the guid it names is the player's, and 0x00807560 fires
// STOP_AUTOREPEAT_SPELL only while a repeat runs: the realm's answer to a stop
// the client made itself fires nothing, and another unit's cancel leaves the
// player shooting. It fired the event for any guid and twice per stop.
TEST_CASE("an auto-repeat stops once and only for the player's own cancel", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    ui.run("stops = 0 local f = CreateFrame('Frame') f:RegisterEvent('STOP_AUTOREPEAT_SPELL') "
           "f:SetScript('OnEvent', function() stops = stops + 1 end)");
    auto cancel = [&](uint64_t guid) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_CANCEL_AUTO_REPEAT));
        p.writePackedGuid(guid);
        s.receive(p);
    };
    s.handler().startAutoRepeat(5019);
    cancel(0xF130000000000101ull);
    CHECK(s.handler().isAutoRepeating());
    CHECK(ui.is("stops == 0"));
    s.handler().stopAutoRepeat(true);
    CHECK(ui.is("stops == 1"));
    cancel(Session::kPlayerGuid);
    CHECK(ui.is("stops == 1"));
    s.handler().startAutoRepeat(5019);
    cancel(Session::kPlayerGuid);
    CHECK_FALSE(s.handler().isAutoRepeating());
    CHECK(ui.is("stops == 2"));
}

// T356: SpellTargetItem (0x008007e0) hands the player's own item - by id
// (0x00754a20), link (0x00754af0) or a name's beginning, case-blind
// (0x00753b20) - worn ones included, to the spell waiting for an item, as a
// click on it would (0x005210d0), and answers nothing; an argument that is
// neither raises its usage error. It answered false and did nothing.
TEST_CASE("SpellTargetItem gives the waiting spell the named item", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    auto& gh = s.handler();
    carry(s, 34336, 13, 0, 0x4000000000000901ull);
    {
        auto& info = gh.itemInfoCacheRef()[2000];
        info.entry = 2000;
        info.valid = true;
        info.name = "Mace 2000";
        info.inventoryType = 13;
        wowee::game::ItemDef def;
        def.itemId = 2000;
        def.name = info.name;
        def.inventoryType = 13;
        def.guid = 0x4000000000000902ull;
        REQUIRE(gh.inventoryRef().setEquipSlot(wowee::game::EquipSlot::MAIN_HAND, def));
    }
    constexpr uint32_t kStone = 999001;
    gh.spellNameCacheRef()[kStone].name = "Stone";
    gh.spellNameCacheRef()[kStone].effectIds[0] = 54;
    const auto casts = [&] { return s.sent().of(Opcode::CMSG_CAST_SPELL).size(); };

    gh.beginSpellItemTargeting(kStone, "spell");
    REQUIRE(gh.isAwaitingItemTarget());
    CHECK(ui.is("select('#', SpellTargetItem('no such thing')) == 0"));
    CHECK(gh.isAwaitingItemTarget());
    CHECK(casts() == 0);
    CHECK(ui.is("not pcall(SpellTargetItem, {})"));
    ui.run("SpellTargetItem(2000)");
    CHECK_FALSE(gh.isAwaitingItemTarget());
    CHECK(casts() == 1);

    gh.beginSpellItemTargeting(kStone, "spell");
    ui.run("SpellTargetItem('ITEM 343')");
    CHECK_FALSE(gh.isAwaitingItemTarget());
    CHECK(casts() == 2);

    // Without a spell waiting nothing is checked or done.
    CHECK(ui.is("pcall(SpellTargetItem, {})"));
}

// Holidays Flags bit 0: SMSG_LOGIN_SETTIMESPEED's third field (0x007e28e0)
// and SMSG_GAMETIME_SET's second (0x007e2b40) are the realm's zone, the game
// clock's +0x1c that 0x0076cba0 moves such a holiday's dates by. It was not
// read.
TEST_CASE("the realm's time zone comes with its time", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    auto& gh = s.handler();
    wowee::network::Packet speed(wowee::game::wireOpcode(Opcode::SMSG_LOGIN_SETTIMESPEED));
    speed.writeUInt32((26u << 24) | (10u << 20) | (25u << 14) | (12u << 6));
    speed.writeFloat(1.0f / 60.0f);
    speed.writeUInt32(3);
    s.receive(speed);
    CHECK(gh.realmZoneHours() == 3);
    CHECK(gh.holidayZoneShift() != 0);
    wowee::network::Packet set(wowee::game::wireOpcode(Opcode::SMSG_GAMETIME_SET));
    set.writeUInt32((26u << 24) | (10u << 20) | (25u << 14) | (13u << 6));
    set.writeUInt32(0);
    s.receive(set);
    CHECK(gh.realmZoneHours() == 0);
    CHECK(gh.holidayZoneShift() == 0);
}

// 12340 has no LibStub and no CallbackHandler-1.0 (no such string in
// wow.exe): an addon's own copies register. A built-in CallbackHandler of
// minor 7 refused the embedded one (Ace3 of 3.3.5: minor 6) and never called
// OnUsed, so AceEvent-3.0 registered no event.
TEST_CASE("an addon's own LibStub libraries register", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    CHECK(ui.is("rawget(_G, 'LibStub') == nil"));
    // LibStub.lua as Ace3 ships it (minor 2), then a library of minor 6.
    ui.run("local LIBSTUB_MAJOR, LIBSTUB_MINOR = 'LibStub', 2 "
           "local LibStub = _G[LIBSTUB_MAJOR] "
           "if not LibStub or LibStub.minor < LIBSTUB_MINOR then "
           "  LibStub = LibStub or {libs = {}, minors = {}} "
           "  _G[LIBSTUB_MAJOR] = LibStub LibStub.minor = LIBSTUB_MINOR "
           "  function LibStub:NewLibrary(major, minor) "
           "    minor = assert(tonumber(strmatch(minor, '%d+')), 'Minor version must either be a number or contain a number.') "
           "    local oldminor = self.minors[major] "
           "    if oldminor and oldminor >= minor then return nil end "
           "    self.minors[major], self.libs[major] = minor, self.libs[major] or {} "
           "    return self.libs[major], oldminor end "
           "  function LibStub:GetLibrary(major, silent) "
           "    if not self.libs[major] and not silent then error('no ' .. tostring(major), 2) end "
           "    return self.libs[major], self.minors[major] end "
           "  setmetatable(LibStub, { __call = LibStub.GetLibrary }) "
           "end "
           "ownCbh = LibStub:NewLibrary('CallbackHandler-1.0', 6)");
    CHECK(ui.is("ownCbh ~= nil"));
    CHECK(ui.is("LibStub('CallbackHandler-1.0') == ownCbh"));
}

// SMSG_GAMESPEED_SET (0x007e2840) is one float, the clock's speed
// (0x0076cfa0); the time of day stays. It was read as a packed time and a
// float, so the realm's 4-byte packet changed nothing.
TEST_CASE("SMSG_GAMESPEED_SET sets the clock's speed only", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    auto& gh = s.handler();
    wowee::network::Packet login(wowee::game::wireOpcode(Opcode::SMSG_LOGIN_SETTIMESPEED));
    login.writeUInt32((26u << 24) | (10u << 20) | (25u << 14) | (12u << 6));
    login.writeFloat(1.0f / 60.0f);
    login.writeUInt32(0);
    s.receive(login);
    const auto date = gh.getGameDate();
    REQUIRE(date.has_value());
    wowee::network::Packet speed(wowee::game::wireOpcode(Opcode::SMSG_GAMESPEED_SET));
    speed.writeFloat(0.5f);
    s.receive(speed);
    CHECK(gh.getTimeSpeed() == 0.5f);
    CHECK(gh.getGameTime() >= 12.0f);
    CHECK(gh.getGameTime() < 12.1f);
    REQUIRE(gh.getGameDate().has_value());
    CHECK(gh.getGameDate()->hour == date->hour);
    wowee::network::Packet fast(wowee::game::wireOpcode(Opcode::SMSG_GAMESPEED_SET));
    fast.writeFloat(100.0f);
    s.receive(fast);
    CHECK(gh.getTimeSpeed() == 60.0f);
}

// T357: AutoEquipCursorItem (0x006dfc40) wears the item ON the cursor (its bag
// and slot, 0x00513680): a bind-on-equip one not yet bound is parked and
// AUTOEQUIP_BIND_CONFIRM asked with the item kept in the hand, else
// CMSG_AUTOEQUIP_ITEM. It used the first item of that entry instead.
TEST_CASE("AutoEquipCursorItem wears the held item", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    ui.run("asked = nil local f = CreateFrame('Frame') f:RegisterEvent('AUTOEQUIP_BIND_CONFIRM') "
           "f:SetScript('OnEvent', function(self, event, index) asked = index end)");
    ui.run("ClearCursor()");  // the cursor is the process's, as the client's
    carry(s, 10019, 10, 2);  // hands, bind on equip
    ui.run("PickupContainerItem(0, 1)");
    REQUIRE(ui.is("CursorHasItem() == 1"));
    ui.run("AutoEquipCursorItem()");
    CHECK(ui.is("asked ~= nil"));
    CHECK(ui.is("CursorHasItem() == 1"));
    CHECK(s.sent().of(Opcode::CMSG_USE_ITEM).empty());
    ui.run("ClearCursor()");
    carry(s, 2361, 17, 0);  // a two-hand mace, binds on nothing
    ui.run("PickupContainerItem(0, 1)");
    ui.run("AutoEquipCursorItem()");
    CHECK(s.sent().of(Opcode::CMSG_AUTOEQUIP_ITEM).size() == 1);
    CHECK(s.sent().of(Opcode::CMSG_USE_ITEM).empty());
    CHECK(ui.is("not CursorHasItem()"));
}

// T359: the bank's slots and its bags' contents were filled from the item's
// template alone, so a bound item there showed the template's bind line
// (0x006277f0 reads the instance's ITEM_FIELD_FLAGS wherever it is held).
TEST_CASE("an item in the bank keeps its own bound flag", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    constexpr uint64_t kItem = 0x4000000000000931ull;
    s.receive(UpdateObject().create(kItem, wowee::game::ObjectType::ITEM,
                                    {{UF::OBJECT_FIELD_ENTRY, 50966}, {UF::ITEM_FIELD_STACK_COUNT, 1}}));
    const uint16_t bank = wowee::game::fieldIndex(UF::PLAYER_FIELD_BANK_SLOT_1);
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid,
                                      {{bank, static_cast<uint32_t>(kItem)},
                                       {static_cast<uint16_t>(bank + 1), static_cast<uint32_t>(kItem >> 32)}}));
    // ITEM_FIELD_FLAGS, seven fields after the stack count: bound.
    const uint16_t flags = static_cast<uint16_t>(wowee::game::fieldIndex(UF::ITEM_FIELD_STACK_COUNT) + 7);
    s.receive(UpdateObject().valuesAt(kItem, {{flags, 1}}));
    const auto& slot = s.handler().getInventory().getBankSlot(0);
    REQUIRE(slot.item.itemId == 50966);
    CHECK(slot.item.soulbound);
    CHECK(slot.item.guid == kItem);
}

// 12340 has no handler for SMSG_PET_UNLEARN_CONFIRM (0x2f1) nor for
// SMSG_SPELL_CHANCE_PROC_LOG (0x3aa): not among 0x006b0b80's opcodes, no
// switch case. Ours opened a confirmation of its own and put up a PROC text.
TEST_CASE("packets 12340 does not handle do nothing", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    wowee::network::Packet unlearn(wowee::game::wireOpcode(Opcode::SMSG_PET_UNLEARN_CONFIRM));
    unlearn.writeUInt64(0xF140000001000001ull);
    unlearn.writeUInt32(10000);
    s.receive(unlearn);
    CHECK_FALSE(s.handler().showPetUnlearnDialog());
    const size_t texts = s.handler().getCombatText().size();
    wowee::network::Packet proc(wowee::game::wireOpcode(Opcode::SMSG_SPELL_CHANCE_PROC_LOG));
    proc.writePackedGuid(0xF130000C2E000010ull);
    proc.writePackedGuid(Session::kPlayerGuid);
    proc.writeUInt32(16870);
    for (int i = 0; i < 4; ++i) proc.writeUInt32(0);
    s.receive(proc);
    CHECK(s.handler().getCombatText().size() == texts);
}

// RegisterAllEvents (0x0049f0e0 -> 0x0081b470) is every event of 12340's
// table; this client's WOWEE_* notices are not among them and reached such a
// frame all the same.
TEST_CASE("RegisterAllEvents does not take in this client's own notices", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("seen = {} local f = CreateFrame('Frame') f:RegisterAllEvents() "
           "f:SetScript('OnEvent', function(self, event) seen[event] = true end)");
    s.handler().fireAddonEvent("WOWEE_ITEM_INFO_RECEIVED", {"2589"});
    s.handler().fireAddonEvent("BAG_UPDATE", {"0"});
    CHECK(ui.is("seen.BAG_UPDATE == true"));
    CHECK(ui.is("seen.WOWEE_ITEM_INFO_RECEIVED == nil"));
}

// 0x0052e5b0: a haircut paid for is BARBER_SHOP_SUCCESS and nothing more -
// the shop stays open (BARBER_SHOP_CLOSE is 0x0052e9f0's, Abbrechen or
// standing up); the changed appearance bytes then raise
// BARBER_SHOP_APPEARANCE_APPLIED (0x006da7f0 and its siblings -> 0x0052e4d0).
TEST_CASE("a haircut leaves the barber's window open", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    wowee::game::GameHandlerTestAccess::setCharacter(s.handler(), "A", "Locki", Session::kPlayerGuid);
    std::vector<std::string> events;
    s.handler().setAddonEventCallback([&](const std::string& name, const std::vector<std::string>&) {
        if (name.rfind("BARBER_SHOP_", 0) == 0) events.push_back(name);
    });
    wowee::network::Packet ok(wowee::game::wireOpcode(Opcode::SMSG_BARBER_SHOP_RESULT));
    ok.writeUInt32(0);
    s.receive(ok);
    REQUIRE(events.size() == 1);
    CHECK(events[0] == "BARBER_SHOP_SUCCESS");
    events.clear();
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_BYTES, 0x01020304}}));
    CHECK(std::count(events.begin(), events.end(), "BARBER_SHOP_APPEARANCE_APPLIED") == 1);
    s.handler().setAddonEventCallback(nullptr);
}

// T364: PickupInventoryItem takes the bank's slots (40-67) and its bags'
// slots (68-74) while a banker is open (0x005e7c00, 0x005e85d0 refuses them
// only with none open). It did nothing for any slot past 23.
TEST_CASE("PickupInventoryItem takes up a bank slot while the bank is open", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("ClearCursor()");
    constexpr uint64_t kItem = 0x4000000000000932ull;
    s.receive(UpdateObject().create(kItem, wowee::game::ObjectType::ITEM,
                                    {{UF::OBJECT_FIELD_ENTRY, 2589}, {UF::ITEM_FIELD_STACK_COUNT, 5}}));
    const uint16_t bank = static_cast<uint16_t>(wowee::game::fieldIndex(UF::PLAYER_FIELD_BANK_SLOT_1) + 2);
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid,
                                      {{bank, static_cast<uint32_t>(kItem)},
                                       {static_cast<uint16_t>(bank + 1), static_cast<uint32_t>(kItem >> 32)}}));
    REQUIRE(s.handler().getInventory().getBankSlot(1).item.itemId == 2589);
    // Closed: refused.
    ui.run("PickupInventoryItem(41)");
    CHECK(ui.is("not CursorHasItem()"));
    wowee::network::Packet open(wowee::game::wireOpcode(Opcode::SMSG_SHOW_BANK));
    open.writeUInt64(0xF130000C2E000010ull);
    s.receive(open);
    ui.run("PickupInventoryItem(41)");
    CHECK(ui.is("CursorHasItem() == 1"));
    // T371: and the slot answers locked, as GetContainerItemInfo(-1, 2) does.
    CHECK(ui.is("IsInventoryItemLocked(41) == 1"));
    CHECK(ui.is("select(3, GetContainerItemInfo(-1, 2)) == 1"));
    ui.run("ClearCursor()");
    CHECK(ui.is("IsInventoryItemLocked(41) == nil"));
}

// T358: names 12340's widgets do not have (later clients' SetShown, the
// animation's Play/Stop/Reset, FrameXML's data fields OnUpdate/UpdateTooltip,
// globals such as ShowUIPanel and SetBinding) answered on every frame and
// region with a silent no-op, so an addon's `if frame.SetShown` took the
// branch that does nothing. They are nil, as in 12340's method tables.
TEST_CASE("a widget has no methods 12340's widgets do not have", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("wf = CreateFrame('Frame') wt = wf:CreateTexture() ws = wf:CreateFontString()");
    for (const char* name : {"SetShown", "Play", "Stop", "Reset", "IsUnderMouse", "UpdateTooltip", "OnUpdate",
                             "ShowUIPanel", "SetBinding", "RunAttribute"}) {
        INFO(name);
        const std::string n = name;
        CHECK(ui.is("wf." + n + " == nil"));
        CHECK(ui.is("wt." + n + " == nil"));
        CHECK(ui.is("ws." + n + " == nil"));
    }
    // A real one still answers.
    CHECK(ui.is("type(wf.Show) == 'function' and type(wt.SetTexture) == 'function'"));
}

// T379: Blizzard_CombatLog's refilter walks the log from the newest entry back
// and adds each older line with backFill (AddMessage(text, r, g, b, nil, true)),
// which 12340 puts at the oldest end (0x00972f20 -> 0x0096a9f0). Appended at the
// bottom instead, every refilter showed the combat log upside down.
TEST_CASE("a back-filled line goes above the ones there (0x0096a9f0)", "[replay][interface]") {
    Session s;
    Interface ui(s);
    ui.run("c = CreateFrame('ScrollingMessageFrame') c:AddMessage('A') c:AddMessage('B', 1, 1, 0, nil, true) "
           "function line(i) return (c:GetMessageInfo(i)) end");
    CHECK(ui.is("c:GetNumMessages() == 2 and line(1) == 'B' and line(2) == 'A'"));
    // A full ring: the back-filled line takes the oldest one's place.
    ui.run("c:SetMaxLines(2) c:Clear() c:AddMessage('A') c:AddMessage('C') c:AddMessage('B', 1, 1, 1, nil, true)");
    CHECK(ui.is("c:GetNumMessages() == 2 and line(1) == 'B' and line(2) == 'C'"));
    // Only a boolean true back-fills.
    ui.run("c:Clear() c:AddMessage('A') c:AddMessage('B', 1, 1, 1, nil, 1)");
    CHECK(ui.is("line(1) == 'A' and line(2) == 'B'"));
    // Without all three of r, g, b the rest move up: typeID, backFill, accessID.
    ui.run("c:Clear() c:AddMessage('A') c:AddMessage('B', 5, true, 7)");
    CHECK(ui.is("line(1) == 'B' and select(2, c:GetMessageInfo(1)) == 7 and select(3, c:GetMessageInfo(1)) == 5"));
}

// P's note (FIXER Q): a plain MessageFrame's AddMessage (0x009747c0) colours the
// line white unless r, g, b are all numbers, reads an alpha at 6 (0x00960420),
// and that colour is the line's font string's one colour (0x00968210 ->
// 0x00487a10) - the frame's font colour (ErrorFont's yellow on UIErrorsFrame)
// does not show. The alpha holds until the fade writes its own (0x00968a60).
TEST_CASE("a plain message frame's line is white unless coloured, alpha at 6 (0x009747c0)",
          "[replay][interface]") {
    Session s;
    Interface ui(s);
    ui.run("m = CreateFrame('MessageFrame', 'QPlainMessages') m:SetTextColor(1, 0.82, 0) "
           "m:SetTimeVisible(5) m:SetFadeDuration(2) "
           "m:AddMessage('A') m:AddMessage('B', 1, 0.1, 0.1, 0.5) m:AddMessage('C', 1, 0, 0, 'x')");
    const auto* w = ui.engine.widgets().findByName("QPlainMessages");
    REQUIRE(w);
    REQUIRE(w->messages.size() == 3);
    const auto& a = w->messages[0];
    const auto& b = w->messages[1];
    const auto& c = w->messages[2];
    CHECK(a.text == "A");
    CHECK(a.color[0] == 1.0f);
    CHECK(a.color[1] == 1.0f);
    CHECK(a.color[2] == 1.0f);
    CHECK(a.color[3] == 1.0f);
    CHECK(b.color[1] == Catch::Approx(0.1f));
    CHECK(b.color[3] == Catch::Approx(0.5f));
    CHECK(c.color[3] == 1.0f);
    // Held through the whole display time (5 s), then the fade's own alpha
    // over the 2 s after it (0x00968a60: hold counter, then fade counter).
    ui.engine.expireMessages(4.5f);
    CHECK(w->messages[0].color[3] == 1.0f);
    CHECK(w->messages[1].color[3] == Catch::Approx(0.5f));
    ui.engine.expireMessages(1.0f);   // 5.5 s: 1.5 of the 2 fade seconds left
    CHECK(w->messages[0].color[3] == Catch::Approx(0.75f));
    CHECK(w->messages[1].color[3] == Catch::Approx(0.75f));
    ui.engine.expireMessages(2.0f);   // 7.5 s: gone
    CHECK(w->messages[0].color[3] == 0.0f);
}

// FIXER Q (trade review): 0x005873e0 case 1 takes a trade at once
// (CMSG_BEGIN_TRADE, 0x00703e00) without TRADE_REQUEST; case 9 only clears the
// partner's accept (0x00586390(0)); case 12 says why the trade failed
// (0x00586520: result 4 with the target flag -> ERR_TRADE_BAG_FULL).
TEST_CASE("a trade asked for opens without a popup and says why it failed (0x005873e0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("seen = {} errors = {} local f = CreateFrame('Frame') "
           "for _, e in ipairs({'TRADE_REQUEST', 'TRADE_SHOW', 'TRADE_CLOSED', 'TRADE_ACCEPT_UPDATE'}) do f:RegisterEvent(e) end "
           "f:RegisterEvent('UI_ERROR_MESSAGE') "
           "f:SetScript('OnEvent', function(self, event, a, b) "
           "if event == 'UI_ERROR_MESSAGE' then errors[#errors + 1] = a else seen[#seen + 1] = event .. (a or '') .. (b or '') end end) "
           "ERR_TRADE_BAG_FULL = 'BAG_FULL' ERR_TRADE_TARGET_BAG_FULL = 'TARGET_BAG_FULL'");
    const auto status = [&](uint32_t code, auto&& more) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_TRADE_STATUS));
        p.writeUInt32(code);
        more(p);
        s.receive(p);
    };
    s.receive(UpdateObject().create(0x42, wowee::game::ObjectType::PLAYER,
                                    {{UF::UNIT_FIELD_HEALTH, 10}, {UF::UNIT_FIELD_MAXHEALTH, 10}}));
    status(1, [](wowee::network::Packet& p) { p.writeUInt64(0x0000000000000042ull); });
    CHECK(s.sent().of(Opcode::CMSG_BEGIN_TRADE).size() == 1);
    CHECK(ui.is("#seen == 0"));
    status(2, [](wowee::network::Packet& p) { p.writeUInt32(0); });
    CHECK(ui.is("seen[1] == 'TRADE_SHOW'"));
    status(4, [](wowee::network::Packet&) {});
    status(4, [](wowee::network::Packet&) {});
    CHECK(ui.is("#seen == 2 and seen[2] == 'TRADE_ACCEPT_UPDATE01'"));
    // Taken back: the window stays.
    status(9, [](wowee::network::Packet&) {});
    CHECK(ui.is("#seen == 3 and seen[3] == 'TRADE_ACCEPT_UPDATE00'"));
    CHECK(s.handler().isTradeOpen());
    status(12, [](wowee::network::Packet& p) {
        p.writeUInt32(4);
        p.writeUInt8(1);
        p.writeUInt32(0);
    });
    CHECK(ui.is("errors[1] ~= nil and seen[4] == 'TRADE_CLOSED'"));
}

// FIXER R (trade rest): 0x005873e0 case 1 answers busy for a dead asker, needs
// the asker known; case 12 result 0x1c is said only for this side and 0x54 names
// the item's limit category (0x00586520); cases 22/23 empty the refused slot of
// this half (0x00586410); an offer raises no TRADE_UPDATE (only 0x00586080).
TEST_CASE("a refused trade slot is emptied, an offer raises no TRADE_UPDATE (0x005873e0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("seen = {} errors = {} local f = CreateFrame('Frame') "
           "for _, e in ipairs({'TRADE_UPDATE', 'TRADE_PLAYER_ITEM_CHANGED', 'TRADE_TARGET_ITEM_CHANGED', "
           "'PLAYER_TRADE_MONEY', 'TRADE_SHOW'}) do f:RegisterEvent(e) end "
           "f:RegisterEvent('UI_ERROR_MESSAGE') "
           "f:SetScript('OnEvent', function(self, event, a) "
           "if event == 'UI_ERROR_MESSAGE' then errors[#errors + 1] = a else seen[#seen + 1] = event .. (a or '') end end)");
    const auto status = [&](uint32_t code, auto&& more) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_TRADE_STATUS));
        p.writeUInt32(code);
        more(p);
        s.receive(p);
    };
    // Nobody known by that guid: no answer at all.
    status(1, [](wowee::network::Packet& p) { p.writeUInt64(0x43); });
    CHECK(s.sent().of(Opcode::CMSG_BEGIN_TRADE).empty());
    CHECK(s.sent().of(Opcode::CMSG_BUSY_TRADE).empty());
    // A dead asker: busy.
    s.receive(UpdateObject().create(0x43, wowee::game::ObjectType::PLAYER,
                                    {{UF::UNIT_FIELD_HEALTH, 0}, {UF::UNIT_FIELD_MAXHEALTH, 10}}));
    status(1, [](wowee::network::Packet& p) { p.writeUInt64(0x43); });
    CHECK(s.sent().of(Opcode::CMSG_BEGIN_TRADE).empty());
    CHECK(s.sent().of(Opcode::CMSG_BUSY_TRADE).size() == 1);
    // A living one: taken.
    s.receive(UpdateObject().create(0x44, wowee::game::ObjectType::PLAYER,
                                    {{UF::UNIT_FIELD_HEALTH, 10}, {UF::UNIT_FIELD_MAXHEALTH, 10}}));
    status(1, [](wowee::network::Packet& p) { p.writeUInt64(0x44); });
    CHECK(s.sent().of(Opcode::CMSG_BEGIN_TRADE).size() == 1);
    status(2, [](wowee::network::Packet& p) { p.writeUInt32(0); });
    // This player's offer with an item in slot 2 (index 1).
    wowee::network::Packet ext(wowee::game::wireOpcode(Opcode::SMSG_TRADE_STATUS_EXTENDED));
    ext.writeUInt8(0);
    ext.writeUInt32(0);
    ext.writeUInt32(7);
    ext.writeUInt32(7);
    ext.writeUInt32(0);
    ext.writeUInt32(0);
    for (uint8_t slot = 0; slot < 7; ++slot) {
        ext.writeUInt8(slot);
        ext.writeUInt32(slot == 1 ? 6948u : 0u);
        for (int i = 0; i < 3; ++i) ext.writeUInt32(slot == 1 ? 1u : 0u);
        ext.writeUInt64(0);
        for (int i = 0; i < 4; ++i) ext.writeUInt32(0);
        ext.writeUInt64(0);
        for (int i = 0; i < 6; ++i) ext.writeUInt32(0);
    }
    s.receive(ext);
    CHECK(ui.is("seen[1] == 'TRADE_SHOW' and seen[2] == 'TRADE_PLAYER_ITEM_CHANGED2' and #seen == 2"));
    // The realm refuses slot index 1: the error, and the slot is emptied.
    status(23, [](wowee::network::Packet& p) { p.writeUInt8(1); });
    CHECK(ui.is("#errors == 1 and seen[3] == 'TRADE_PLAYER_ITEM_CHANGED2'"));
    CHECK(s.handler().getMyTradeSlots()[1].itemId == 0);
    status(23, [](wowee::network::Packet& p) { p.writeUInt8(0xff); });
    CHECK(ui.is("seen[4] == 'PLAYER_TRADE_MONEY'"));
    // Failed with 0x1c: said for this side only.
    status(12, [](wowee::network::Packet& p) { p.writeUInt32(0x1c); p.writeUInt8(1); p.writeUInt32(0); });
    CHECK(ui.is("#errors == 2"));
    status(12, [](wowee::network::Packet& p) { p.writeUInt32(0x1c); p.writeUInt8(0); p.writeUInt32(0); });
    CHECK(ui.is("#errors == 3 and errors[3] ~= errors[1]"));
}

// FIXER Q (talent review): GetTalentInfo's empty answer (0x005c7800,
// LAB_005c7c49) and its usage line; GetUnspentTalentPoints of an inspect that
// has no record is 0, not the player's own (0x005c6310).
TEST_CASE("GetTalentInfo answers no talent as 12340 does (0x005c7800)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("a, b, c, d, e, f, g, h, i, j = GetTalentInfo(99, 99)");
    CHECK(ui.is("a == nil and b == nil and c == 1 and d == 1 and e == 0 and f == 0 and g == 0 "
                "and h == nil and i == 0 and j == nil"));
    CHECK(ui.is("select('#', GetTalentInfo(99, 99)) == 10"));
    CHECK(ui.is("not pcall(GetTalentInfo, 'x', 1)"));
    CHECK(ui.is("GetUnspentTalentPoints(true) == 0"));
}

// T381: SetQuestRewardSpell (0x006263e0) describes the quest frame's reward
// spell (0x00c0d680) of the page shown; it read the detail page's record, empty
// on the reward page.
TEST_CASE("the reward page's spell is the one its tooltip describes (0x006263e0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_QUESTGIVER_OFFER_REWARD));
    p.writeUInt64(0xF130000BEC00150Eull);
    p.writeUInt32(3639);
    p.writeString("Zeigt Eure Arbeit");
    p.writeString("Text");
    p.writeUInt8(0);
    for (int i = 0; i < 60; ++i) p.writeUInt32(i == 10 ? 20222u : 0u);
    s.receive(p);
    REQUIRE(s.handler().isQuestOfferRewardOpen());
    CHECK(s.handler().getQuestDetails().rewardSpellId == 0);
    CHECK(wowee::addons::questFrameRewardSpell(&s.handler()) == 20222);
}

// FIXER Q (quest review): past the end of the greeting's lists the title is
// nil (0x0058be30, 0x0058bed0), not "".
TEST_CASE("a greeting title past the end is nil (0x0058be30)", "[replay][interface]") {
    Session s;
    Interface ui(s);
    CHECK(ui.is("GetAvailableTitle(5) == nil and GetActiveTitle(5) == nil"));
}

// FIXER R (Q's quest #6): GetQuestItemInfo (0x0058d980 -> 0x0058d190) raises
// its error for a type other than reward/choice/required (any case) and for an
// index outside 1..7; an empty slot in range answers five values.
TEST_CASE("GetQuestItemInfo of an empty slot answers five values (0x0058d190)", "[replay][interface]") {
    Session s;
    Interface ui(s);
    CHECK(ui.is("not pcall(GetQuestItemInfo, 'bogus', 1)"));
    CHECK(ui.is("not pcall(GetQuestItemInfo, 'reward', 0)"));
    CHECK(ui.is("not pcall(GetQuestItemInfo, 'choice', 8)"));
    CHECK(ui.is("select('#', GetQuestItemInfo('REWARD', 7)) == 5"));
    ui.run("n, t, c, q, u = GetQuestItemInfo('required', 1)");
    CHECK(ui.is("n == '' and t == 'Interface\\\\Icons\\\\INV_Misc_QuestionMark' and c == 0 and q == 0 and u == 1"));
}

// FIXER R (Q's loot shapes): GetLootSlotInfo (0x00588570) answers five values
// for every slot - quality -1 for an item not in the cache, nil/""/0/0/nil past
// the table; LootSlot on the coin empties it at once (0x00589140: money -1,
// LOOT_SLOT_CLEARED 1); an uncached item is not taken; ConfirmLootSlot acts
// only for the slot that asked (0x00589300).
TEST_CASE("a loot slot answers five values and the coin goes at once (0x00588570)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("cleared = {} local f = CreateFrame('Frame') f:RegisterEvent('LOOT_SLOT_CLEARED') "
           "f:SetScript('OnEvent', function(self, event, a) cleared[#cleared + 1] = a end)");
    wowee::network::Packet loot(wowee::game::wireOpcode(Opcode::SMSG_LOOT_RESPONSE));
    loot.writeUInt64(0xF130000BEC001510ull);
    loot.writeUInt8(1);
    loot.writeUInt32(25);   // coin
    loot.writeUInt8(1);
    loot.writeUInt8(0);
    for (uint32_t v : {55555u, 2u, 1234u, 0u, 0u}) loot.writeUInt32(v);
    loot.writeUInt8(0);
    s.receive(loot);
    REQUIRE(s.handler().isLootWindowOpen());
    CHECK(ui.is("GetNumLootItems() == 2"));
    // The item, not in the cache: name "", quality -1.
    ui.run("t, n, c, q, l = GetLootSlotInfo(2)");
    CHECK(ui.is("t ~= nil and n == '' and c == 2 and q == -1 and l == nil"));
    CHECK(ui.is("select('#', GetLootSlotInfo(3)) == 5"));
    ui.run("t, n, c, q, l = GetLootSlotInfo(3)");
    CHECK(ui.is("t == nil and n == '' and c == 0 and q == -1 and l == nil"));
    ui.run("t, n, c, q, l = GetLootSlotInfo(40)");
    CHECK(ui.is("t == nil and n == '' and c == 0 and q == 0 and l == nil"));
    // Not in the cache: LootSlot takes nothing.
    ui.run("LootSlot(2)");
    CHECK(s.sent().of(Opcode::CMSG_AUTOSTORE_LOOT_ITEM).empty());
    // A confirmation with nothing asked takes nothing either.
    ui.run("ConfirmLootSlot(2)");
    CHECK(s.sent().of(Opcode::CMSG_AUTOSTORE_LOOT_ITEM).empty());
    // The coin: sent, and empty at once.
    ui.run("LootSlot(1)");
    CHECK(s.sent().of(Opcode::CMSG_LOOT_MONEY).size() == 1);
    CHECK(ui.is("cleared[1] == 1 and LootSlotIsCoin(1) == nil and GetLootSlotInfo(1) == nil"));
    CHECK(ui.is("select('#', GetLootSlotInfo(1)) == 5 and GetNumLootItems() == 2"));
    ui.run("LootSlot(1)");
    CHECK(s.sent().of(Opcode::CMSG_LOOT_MONEY).size() == 1);
}

// FIXER R (re-read of 0x0058cd20): AcceptQuest sends CMSG_QUESTGIVER_ACCEPT_QUEST
// (0x006d4ca0) to the details' second guid (0xc0d650, a sharer) where there is
// one, else to the giver.
TEST_CASE("a shared quest is accepted from the one who shared it (0x0058cd20)", "[replay][interface]") {
    constexpr uint64_t kGiver = 0xF130000BEC00150Eull;
    constexpr uint64_t kSharer = 0x0000000000000077ull;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    const auto details = [&](uint64_t inform) {
        wowee::network::Packet offer(wowee::game::wireOpcode(Opcode::SMSG_QUESTGIVER_QUEST_DETAILS));
        offer.writeUInt64(kGiver);
        offer.writeUInt64(inform);
        offer.writeUInt32(861);
        for (const char* text : {"The Way", "Details.", "Objectives."}) offer.writeString(text);
        offer.writeUInt8(0);
        for (int i = 0; i < 2; ++i) offer.writeUInt32(0);
        offer.writeUInt8(0);
        for (int i = 0; i < 2; ++i) offer.writeUInt32(0);
        for (int i = 0; i < 3; ++i) offer.writeUInt32(0);
        offer.writeFloat(0.0f);
        for (int i = 0; i < 22; ++i) offer.writeUInt32(0);
        s.receive(offer);
    };
    details(kSharer);
    ui.run("AcceptQuest()");
    auto sent = s.sent().of(Opcode::CMSG_QUESTGIVER_ACCEPT_QUEST);
    REQUIRE(sent.size() == 1);
    CHECK(sent.back().readUInt64() == kSharer);
}

// FIXER R (mail review): SetSendMailMoney takes only what the player has
// (0x0056fda0), SetSendMailCOD only with an item attached (0x0056d610); a
// letter is emptied only by the realm's success (0x005717b0 -> 0x0056d4d0),
// and every answer ends with MAIL_SUCCESS or MAIL_FAILED without arguments.
TEST_CASE("a refused letter keeps its money (0x005717b0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::PLAYER_FIELD_COINAGE, 500}});
    Interface ui(s);
    ui.run("seen = {} local f = CreateFrame('Frame') "
           "for _, e in ipairs({'MAIL_SUCCESS', 'MAIL_FAILED', 'MAIL_SEND_SUCCESS', 'SEND_MAIL_MONEY_CHANGED'}) do f:RegisterEvent(e) end "
           "f:SetScript('OnEvent', function(self, event, a) seen[#seen + 1] = event .. (a or '') end)");
    wowee::network::Packet box(wowee::game::wireOpcode(Opcode::SMSG_SHOW_MAILBOX));
    box.writeUInt64(0xF1100000000000AAull);
    s.receive(box);
    CHECK(ui.is("SetSendMailMoney(600) == nil and GetSendMailMoney() == 0"));
    CHECK(ui.is("SetSendMailMoney(100) == 1 and GetSendMailMoney() == 100"));
    CHECK(ui.is("seen[1] == 'SEND_MAIL_MONEY_CHANGED'"));
    ui.run("SetSendMailCOD(50)");
    CHECK(ui.is("GetSendMailCOD() == 0"));
    CHECK(ui.is("not pcall(SendMail)"));
    const auto result = [&](uint32_t action, uint32_t error) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_SEND_MAIL_RESULT));
        p.writeUInt32(0);
        p.writeUInt32(action);
        p.writeUInt32(error);
        s.receive(p);
    };
    result(0, 3);
    CHECK(ui.is("seen[2] == 'MAIL_FAILED' and GetSendMailMoney() == 100"));
    result(0, 0);
    CHECK(ui.is("GetSendMailMoney() == 0 and seen[#seen] == 'MAIL_SUCCESS'"));
    CHECK(ui.is("seen[#seen - 1] == 'MAIL_SEND_SUCCESS'"));
    // A take answered: MAIL_SUCCESS too.
    result(1, 0);
    CHECK(ui.is("seen[#seen] == 'MAIL_SUCCESS'"));
}

// T383: CloseQuest from Lua (0x0058d350 -> 0x0058ca70(0, quest, 1)) fires
// QUEST_FINISHED once while a page is open; again with none open, nothing.
TEST_CASE("CloseQuest from a macro closes the quest frame (0x0058ca70)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("finished = 0 local f = CreateFrame('Frame') f:RegisterEvent('QUEST_FINISHED') "
           "f:SetScript('OnEvent', function() finished = finished + 1 end)");
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_QUESTGIVER_OFFER_REWARD));
    p.writeUInt64(0xF130000BEC00150Eull);
    p.writeUInt32(3639);
    p.writeString("Zeigt Eure Arbeit");
    p.writeString("Text");
    p.writeUInt8(0);
    for (int i = 0; i < 60; ++i) p.writeUInt32(0);
    s.receive(p);
    REQUIRE(s.handler().isQuestOfferRewardOpen());
    ui.run("CloseQuest()");
    CHECK_FALSE(s.handler().isQuestOfferRewardOpen());
    CHECK(ui.is("finished == 1"));
    ui.run("CloseQuest()");
    CHECK(ui.is("finished == 1"));
}

// T382: GetAuctionItemInfo's level is the required level but 1 where there is
// none (0x0059d5e0); canUse asks CanUseItem (0x006dc3f0) - an item not known
// yet is not refused.
TEST_CASE("an auction row without a required level says level 1 (0x0059d5e0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.handler().getInventoryHandler()->openAuctionHouse(0x77);
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_AUCTION_LIST_RESULT));
    p.writeUInt32(1);
    p.writeUInt32(9);
    p.writeUInt32(10940);
    for (int i = 0; i < 21; ++i) p.writeUInt32(0);
    for (int i = 0; i < 2; ++i) p.writeUInt32(0);
    p.writeUInt32(2);
    for (int i = 0; i < 2; ++i) p.writeUInt32(0);
    p.writeUInt64(0x1001);
    for (uint32_t v : {150u, 0u, 300u, 60000u}) p.writeUInt32(v);
    p.writeUInt64(0);
    p.writeUInt32(0);
    p.writeUInt32(1);
    p.writeUInt32(300);
    s.receive(p);
    REQUIRE(ui.is("GetNumAuctionItems('list') == 1"));
    // Not in the cache yet: the empty row (FIXER R, 0x0059d5e0's tail).
    CHECK(ui.is("select('#', GetAuctionItemInfo('list', 1)) == 13 and GetAuctionItemInfo('list', 1) == nil"));
    CHECK(ui.is("select(4, GetAuctionItemInfo('list', 1)) == -1 and select(6, GetAuctionItemInfo('list', 1)) == 0"));
    auto& dust = s.handler().itemInfoCacheRef()[10940];
    dust.entry = 10940;
    dust.valid = true;
    dust.name = "Seltsamer Staub";
    CHECK(ui.is("select(6, GetAuctionItemInfo('list', 1)) == 1"));
    CHECK(ui.is("select(5, GetAuctionItemInfo('list', 1)) == 1"));
    // An unknown seller is nil, not "" (0x0067d770 without a name).
    CHECK(ui.is("select(12, GetAuctionItemInfo('list', 1)) == nil"));
}

namespace {
// An item of `entry` in backpack slot `slot` (from 0), its template known and
// its object held, for the auction house's sell box.
void carryForAuction(Session& s, int slot, uint64_t guid, uint32_t entry, uint32_t count,
                     uint32_t templateFlags = 0, uint32_t fieldFlags = 0) {
    auto& gh = s.handler();
    auto& info = gh.itemInfoCacheRef()[entry];
    info.entry = entry;
    info.valid = true;
    info.name = "Item " + std::to_string(entry);
    info.itemFlags = templateFlags;
    info.maxStack = 20;
    wowee::game::ItemDef def;
    def.itemId = entry;
    def.name = info.name;
    def.guid = guid;
    def.stackCount = count;
    REQUIRE(gh.inventoryRef().setBackpackSlot(slot, def));
    gh.backpackSlotGuidsRef()[static_cast<size_t>(slot)] = guid;
    auto& held = gh.onlineItemsRef()[guid];
    held.entry = entry;
    held.stackCount = count;
    held.flags = fieldFlags;
}
}  // namespace

// FIXER S: ClickAuctionSellItemButton (0x0059d410) asks 0x0059d0b0 first - a
// conjured item is refused with ERR_AUCTION_CONJURED_ITEM and stays in the
// hand; an item offered while the box holds another takes the box and the old
// one comes up onto the cursor; a click with nothing carried lifts the boxed
// item out.
TEST_CASE("the auction sell box refuses and swaps as 0x0059d410", "[replay][interface][auction]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    s.handler().getInventoryHandler()->openAuctionHouse(0x77);
    ui.run("ClearCursor()");
    carryForAuction(s, 0, 0x4000000000000A01ull, 5512, 1, 0x2);   // conjured
    carryForAuction(s, 1, 0x4000000000000A02ull, 2589, 5);
    carryForAuction(s, 2, 0x4000000000000A03ull, 2592, 3);
    ui.run("PickupContainerItem(0, 1) ClickAuctionSellItemButton()");
    CHECK(ui.is("CursorHasItem() == 1"));
    CHECK(ui.is("GetAuctionSellItemInfo() == nil"));
    ui.run("ClearCursor() PickupContainerItem(0, 2) ClickAuctionSellItemButton()");
    CHECK(ui.is("not CursorHasItem()"));
    CHECK(ui.is("GetAuctionSellItemInfo() == 'Item 2589'"));
    ui.run("PickupContainerItem(0, 3) ClickAuctionSellItemButton()");
    CHECK(ui.is("GetAuctionSellItemInfo() == 'Item 2592'"));
    CHECK(ui.is("select(2, GetCursorInfo()) == 2589"));
    ui.run("ClearCursor() ClickAuctionSellItemButton()");
    CHECK(ui.is("select(2, GetCursorInfo()) == 2592"));
    CHECK(ui.is("GetAuctionSellItemInfo() == nil"));
    ui.run("ClearCursor()");
}

// FIXER S: StartAuction (0x0059f990) - the first three arguments are required;
// a buyout under the bid is raised to it; a runTime outside 1..3 is 24 hours;
// one auction the boxed stack covers goes out from that stack alone.
TEST_CASE("StartAuction posts one stack from the boxed item (0x0059f990)", "[replay][interface][auction]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    s.handler().getInventoryHandler()->openAuctionHouse(0x77);
    constexpr uint64_t kBoxed = 0x4000000000000B01ull;
    carryForAuction(s, 0, kBoxed, 2589, 20);
    carryForAuction(s, 1, 0x4000000000000B02ull, 2589, 20);
    // The cursor is the bindings' static state: start with an empty hand.
    ui.run("ClearCursor() PickupContainerItem(0, 1) ClickAuctionSellItemButton()");
    REQUIRE(ui.is("GetAuctionSellItemInfo() == 'Item 2589'"));
    CHECK(ui.is("not pcall(StartAuction, 100, 50)"));
    ui.run("StartAuction(100, 50, 7, 5)");
    auto sells = s.sent().of(Opcode::CMSG_AUCTION_SELL_ITEM);
    REQUIRE(sells.size() == 1);
    auto& p = sells.back();
    CHECK(p.readUInt64() == 0x77);
    CHECK(p.readUInt32() == 1);
    CHECK(p.readUInt64() == kBoxed);
    CHECK(p.readUInt32() == 5);
    CHECK(p.readUInt32() == 100);
    CHECK(p.readUInt32() == 100);
    CHECK(p.readUInt32() == 1440);
    CHECK(ui.is("GetAuctionSellItemInfo() == nil"));
}

// FIXER S: GetAuctionItemTimeLeft (0x0059fd60) answers 0 for no such row and
// counts a row down from when it came; a waiting sale at its end is dropped
// (0x0059f2f0). GetNumAuctionItems wants its type (0x0059c1a0); a search goes
// only once the last one's wait is over (0x0059bcf0).
TEST_CASE("auction rows count down and searches wait (0x0059fd60, 0x0059bcf0)", "[replay][interface][auction]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    s.handler().getInventoryHandler()->openAuctionHouse(0x77);
    CHECK(ui.is("not pcall(GetNumAuctionItems)"));
    ui.run("QueryAuctionItems('a') QueryAuctionItems('b')");
    CHECK(s.sent().of(Opcode::CMSG_AUCTION_LIST_ITEMS).size() == 1);
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_AUCTION_LIST_RESULT));
    p.writeUInt32(1);
    p.writeUInt32(9);
    p.writeUInt32(10940);
    for (int i = 0; i < 21; ++i) p.writeUInt32(0);
    for (int i = 0; i < 2; ++i) p.writeUInt32(0);
    p.writeUInt32(2);
    for (int i = 0; i < 2; ++i) p.writeUInt32(0);
    p.writeUInt64(0x1001);
    for (uint32_t v : {150u, 0u, 300u, 3u * 3600000u}) p.writeUInt32(v);
    p.writeUInt64(0);
    p.writeUInt32(0);
    p.writeUInt32(1);
    p.writeUInt32(300);
    s.receive(p);
    REQUIRE(ui.is("GetNumAuctionItems('list') == 1"));
    CHECK(ui.is("GetAuctionItemTimeLeft('list', 1) == 3"));
    CHECK(ui.is("GetAuctionItemTimeLeft('LIST', 1) == 3"));
    CHECK(ui.is("GetAuctionItemTimeLeft('list', 2) == 0"));
    // A sale waiting to pay out whose time is up: 1, and gone.
    wowee::network::Packet pending(wowee::game::wireOpcode(Opcode::SMSG_AUCTION_LIST_PENDING_SALES));
    pending.writeUInt32(1);
    pending.writeString("10940:0:2:19");
    pending.writeString("0000000000000002:150:300:0:0");
    pending.writeUInt32(0);
    pending.writeUInt32(0);
    pending.writeFloat(0.0f);
    s.receive(pending);
    REQUIRE(ui.is("GetNumAuctionItems('owner') == 1"));
    CHECK(ui.is("GetAuctionItemTimeLeft('owner', 1) == 1"));
    CHECK(ui.is("GetNumAuctionItems('owner') == 0"));
}

// FIXER S: SMSG_SEND_MAIL_RESULT (0x005717b0) asks for no new list - the inbox
// is brought up to date by the answer itself.
TEST_CASE("a mail answer asks for no list (0x005717b0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    wowee::network::Packet box(wowee::game::wireOpcode(Opcode::SMSG_SHOW_MAILBOX));
    box.writeUInt64(0xF1100000000000AAull);
    s.receive(box);
    const size_t asked = s.sent().of(Opcode::CMSG_GET_MAIL_LIST).size();
    for (uint32_t action : {0u, 1u, 4u}) {
        wowee::network::Packet r(wowee::game::wireOpcode(Opcode::SMSG_SEND_MAIL_RESULT));
        r.writeUInt32(7);
        r.writeUInt32(action);
        r.writeUInt32(0);
        s.receive(r);
    }
    CHECK(s.sent().of(Opcode::CMSG_GET_MAIL_LIST).size() == asked);
}

// FIXER S (decompile review): the unit, container and cast bindings answer a
// missing argument with 12340's usage line (0x0060cc30, 0x0060d280,
// 0x0060d330, 0x0060d690, 0x0060d730, 0x0060f680, 0x00610860, 0x00610a00,
// 0x00610b60, 0x0060e310, 0x005d7d90, 0x005d7ff0, 0x005d8650, 0x0053e060,
// 0x00540310, 0x0080dc00, 0x005d65c0) - UnitIsEnemy has no one-argument form.
// SplitContainerItem (0x005d84f0) takes nothing for more than the stack.
TEST_CASE("bindings without their arguments give 12340's usage lines", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    const auto usage = [&](const std::string& call, const std::string& line) {
        INFO(call);
        CHECK(ui.is("select(2, pcall(" + call + ")):find('" + line + "', 1, true) ~= nil"));
    };
    usage("UnitIsAFK", "Usage: UnitIsAFK(\"unit\")");
    usage("UnitIsDeadOrGhost", "Usage: UnitIsDeadOrGhost(\"unit\")");
    usage("UnitDamage", "Usage: UnitDamage(\"unit\")");
    usage("UnitAttackSpeed", "Usage: UnitAttackSpeed(\"unit\")");
    usage("UnitAttackPower", "Usage: UnitAttackPower(\"unit\")");
    usage("UnitIsEnemy, 'target'", "Usage: UnitIsEnemy(\"unit\", \"otherUnit\")");
    usage("UnitReaction, 'target'", "Usage: UnitReaction(\"unit\", \"otherUnit\")");
    usage("UnitCanAttack, 'player'", "Usage: UnitCanAttack(\"unit\", \"otherUnit\")");
    usage("UnitCanAssist, 'player'", "Usage: UnitCanAssist(\"unit\", \"otherUnit\")");
    usage("GetSpellBonusDamage, 9", "Usage: GetSpellBonusDamage(school)");
    usage("GetContainerItemCooldown, 0", "Usage: GetContainerItemCooldown(index, slot)");
    usage("PickupContainerItem, 0", "Usage: PickupContainerItem(index, slot)");
    usage("UseContainerItem, 0", "Usage: UseContainerItem(index, slot[, target])");
    usage("CastSpellByID", "Usage: CastSpellByID(spellID[, target])");
    usage("CastSpellByName", "Usage: CastSpellByName(name[, target])");
    usage("SpellTargetUnit", "Usage: SpellTargetUnit(\"unit\")");
    usage("PickupPetAction", "Usage: PickupPetAction(index)");
    CHECK(ui.is("UnitIsAFK('player') == nil and UnitIsEnemy('player', 'player') == nil"));
    // UnitPowerType's index past the first: 0 and "" (0x0060f100).
    CHECK(ui.is("select(1, UnitPowerType('player', 2)) == 0 and select(2, UnitPowerType('player', 2)) == ''"));
    carryForAuction(s, 0, 0x4000000000000C01ull, 2589, 5);
    ui.run("ClearCursor() SplitContainerItem(0, 1, 6)");
    CHECK(ui.is("not CursorHasItem()"));
    ui.run("SplitContainerItem(0, 1, 2)");
    CHECK(ui.is("CursorHasItem() == 1"));
    ui.run("ClearCursor()");
}

// 0x005e5cc0: nine values; the tenth only for a CVar displayQuestID that is on.
TEST_CASE("GetQuestLogTitle answers nine values", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    CHECK(ui.is("select('#', GetQuestLogTitle(1)) == 9"));
    CHECK(ui.is("select('#', GetQuestLogTitle(0)) == 9"));
}

// 0x00573b50 / 0x00573c90: the usage line for what is not two numbers or a
// group past 8, and nothing sent for a move within one group.
TEST_CASE("raid group moves within one group send nothing", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    wowee::network::Packet g(wowee::game::wireOpcode(Opcode::SMSG_GROUP_LIST));
    g.writeUInt8(2);   // a raid
    for (int i = 0; i < 3; ++i) g.writeUInt8(0);
    g.writeUInt64(0x1234);
    g.writeUInt32(1);
    g.writeUInt32(2);
    const std::tuple<const char*, uint64_t, uint8_t> members[] = {{"Achiles", 0x1001, 0}, {"Bea", 0x1002, 1}};
    for (const auto& [name, guid, group] : members) {
        g.writeString(name);
        g.writeUInt64(guid);
        g.writeUInt8(1);
        g.writeUInt8(group);
        g.writeUInt8(0);
        g.writeUInt8(0);
    }
    g.writeUInt64(Session::kPlayerGuid);
    for (int i = 0; i < 12; ++i) g.writeUInt8(0);
    s.receive(g);
    ui.run("at = {} for i = 1, GetNumRaidMembers() do local n, _, grp = GetRaidRosterInfo(i) "
           "if n then at[n] = i end end");
    REQUIRE(ui.is("at.Achiles ~= nil and at.Bea ~= nil"));
    CHECK(ui.is("not pcall(SetRaidSubgroup)"));
    CHECK(ui.is("not pcall(SetRaidSubgroup, at.Achiles, 9)"));
    CHECK(ui.is("not pcall(SwapRaidSubgroup, at.Achiles)"));
    const auto sent = [&](Opcode o) { return s.sent().of(o).size(); };
    ui.run("SetRaidSubgroup(at.Achiles, 1)");
    CHECK(sent(Opcode::CMSG_GROUP_CHANGE_SUB_GROUP) == 0);
    ui.run("SetRaidSubgroup(at.Achiles, 3)");
    CHECK(sent(Opcode::CMSG_GROUP_CHANGE_SUB_GROUP) == 1);
    ui.run("SwapRaidSubgroup(at.Bea, at.Bea)");
    CHECK(sent(Opcode::CMSG_GROUP_SWAP_SUB_GROUP) == 0);
    ui.run("SwapRaidSubgroup(at.Achiles, at.Bea)");
    CHECK(sent(Opcode::CMSG_GROUP_SWAP_SUB_GROUP) == 1);
}

// T385: the answer that ends a ready check is still readable at its
// READY_CHECK_CONFIRM - 0x005744f0 ends the check (deadline into the past)
// only after its member loop raised the event.
TEST_CASE("the last ready check answer has its status at the confirm (0x005744f0)", "[replay][interface]") {
    constexpr uint64_t kMember = 0x1001;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    wowee::network::Packet g(wowee::game::wireOpcode(Opcode::SMSG_GROUP_LIST));
    g.writeUInt8(0);
    for (int i = 0; i < 3; ++i) g.writeUInt8(0);
    g.writeUInt64(0x1234);
    g.writeUInt32(1);
    g.writeUInt32(1);
    g.writeString("Achiles");
    g.writeUInt64(kMember);
    g.writeUInt8(1);
    g.writeUInt8(0);
    g.writeUInt8(0);
    g.writeUInt8(0);
    g.writeUInt64(Session::kPlayerGuid);
    for (int i = 0; i < 12; ++i) g.writeUInt8(0);
    s.receive(g);
    REQUIRE(ui.is("UnitExists('party1')"));
    ui.run("seenStatus = 'none' local f = CreateFrame('Frame') f:RegisterEvent('READY_CHECK_CONFIRM') "
           "f:SetScript('OnEvent', function(self, event, unit) if unit == 'party1' then seenStatus = GetReadyCheckStatus('party1') end end)");
    wowee::network::Packet start(wowee::game::wireOpcode(Opcode::MSG_RAID_READY_CHECK));
    start.writeUInt64(Session::kPlayerGuid);
    s.receive(start);
    REQUIRE(ui.is("GetReadyCheckStatus('party1') == 'waiting'"));
    wowee::network::Packet answer(wowee::game::wireOpcode(Opcode::MSG_RAID_READY_CHECK_CONFIRM));
    answer.writeUInt64(kMember);
    answer.writeUInt8(0);
    s.receive(answer);
    CHECK(ui.is("seenStatus == 'notready'"));
}

// T387: ClearFocus with no focus, or a focus set to what it is, raises no
// PLAYER_FOCUS_CHANGED (0x0051ff20 compares first).
TEST_CASE("an unchanged focus raises nothing (0x0051ff20)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("focusEvents = 0 local f = CreateFrame('Frame') f:RegisterEvent('PLAYER_FOCUS_CHANGED') "
           "f:SetScript('OnEvent', function() focusEvents = focusEvents + 1 end)");
    ui.run("ClearFocus()");
    CHECK(ui.is("focusEvents == 0"));
    s.handler().setFocus(Session::kPlayerGuid);
    s.handler().setFocus(Session::kPlayerGuid);
    CHECK(ui.is("focusEvents == 1"));
    s.handler().clearFocus();
    s.handler().clearFocus();
    CHECK(ui.is("focusEvents == 2"));
}

// T384: CompleteQuest (0x0058cf40) sends once from the progress page and marks
// it sent; the page keeps its title and items until the realm answers.
TEST_CASE("the progress page keeps its data after CompleteQuest (0x0058cf40)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_QUESTGIVER_REQUEST_ITEMS));
    p.writeUInt64(0xF130000C3E000001ull);
    p.writeUInt32(789);
    p.writeString("Stich des Skorpiden");
    p.writeString("Text");
    s.receive(p);
    REQUIRE(ui.is("GetTitleText() == 'Stich des Skorpiden'"));
    ui.run("CompleteQuest() CompleteQuest()");
    CHECK(s.sent().of(Opcode::CMSG_QUESTGIVER_REQUEST_REWARD).size() == 1);
    CHECK(ui.is("GetTitleText() == 'Stich des Skorpiden'"));
}

// T388: the login's roster arrives before this client's interface is built;
// once it is, the roster's events are raised for it.
TEST_CASE("an interface built after the roster hears the group (T388)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    wowee::network::Packet g(wowee::game::wireOpcode(Opcode::SMSG_GROUP_LIST));
    g.writeUInt8(2);
    for (int i = 0; i < 3; ++i) g.writeUInt8(0);
    g.writeUInt64(0x1234);
    g.writeUInt32(1);
    g.writeUInt32(1);
    g.writeString("Achiles");
    g.writeUInt64(0x1001);
    g.writeUInt8(0);
    g.writeUInt8(0);
    g.writeUInt8(0);
    g.writeUInt8(0);
    g.writeUInt64(Session::kPlayerGuid);
    for (int i = 0; i < 12; ++i) g.writeUInt8(0);
    s.receive(g);
    ui.run("groupEvents = {} local f = CreateFrame('Frame') "
           "for _, e in ipairs({'PARTY_MEMBERS_CHANGED', 'RAID_ROSTER_UPDATE', 'PARTY_LEADER_CHANGED'}) do f:RegisterEvent(e) end "
           "f:SetScript('OnEvent', function(self, e) groupEvents[e] = (groupEvents[e] or 0) + 1 end)");
    s.handler().setInterfaceLoaded(true);
    CHECK(ui.is("groupEvents.PARTY_MEMBERS_CHANGED == 1 and groupEvents.RAID_ROSTER_UPDATE == 1"));
    CHECK(ui.is("groupEvents.PARTY_LEADER_CHANGED == 1"));
    // T386: a member in sight answers its own level (0x00573690), whatever
    // the roster's stats say.
    s.receive(UpdateObject().create(0x1001, wowee::game::ObjectType::PLAYER,
                                    {{UF::UNIT_FIELD_HEALTH, 10}, {UF::UNIT_FIELD_MAXHEALTH, 10},
                                     {UF::UNIT_FIELD_LEVEL, 80}}));
    ui.run("achilesLevel = nil for i = 1, GetNumRaidMembers() do local n, _, _, l = GetRaidRosterInfo(i) "
           "if n == 'Achiles' then achilesLevel = l end end");
    CHECK(ui.is("achilesLevel == 80"));
}

// T389: UnitXPMax (0x0060eae0 -> 0x0060a600) is the player's field as it is,
// 0 at the cap, and 0 for any other unit.
TEST_CASE("UnitXPMax is 0 at the cap and for others (0x0060eae0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    CHECK(ui.is("UnitXPMax('player') == 0"));
    CHECK(ui.is("UnitXPMax('target') == 0"));
}

namespace {
// SMSG_BATTLEFIELD_STATUS for WotLK: the slot, its eight-byte record, the
// levels, instance, rated byte, status and the status' own tail.
wowee::network::Packet bgStatus(uint32_t slot, uint64_t record, uint32_t status, uint32_t map) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_BATTLEFIELD_STATUS));
    p.writeUInt32(slot);
    p.writeUInt64(record);
    p.writeUInt8(10); p.writeUInt8(19); p.writeUInt32(1); p.writeUInt8(0);
    p.writeUInt32(status);
    if (status == 2) { p.writeUInt32(map); p.writeUInt64(0); p.writeUInt32(80000); }
    if (status == 3) { p.writeUInt32(map); p.writeUInt64(0); p.writeUInt32(0); p.writeUInt32(1000); p.writeUInt8(0); }
    return p;
}
// Warsong Gulch: arena type 0, arena byte 0, type 2, 0x1F90.
constexpr uint64_t kWarsongRecord = (0x1F90ull << 48) | (2ull << 16);

uint64_t firstQword(wowee::network::Packet p) {
    p.setReadPos(0);
    return p.readUInt64();
}
}  // namespace

// T392: LeaveBattlefield 0x0054c250 writes the running slot's record as one
// uint64 (eight zeros without a battle); the opcode alone was dropped by the
// realm.
TEST_CASE("LeaveBattlefield carries the running slot's record (0x0054c250)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("LeaveBattlefield()");
    REQUIRE(s.sent().of(Opcode::CMSG_LEAVE_BATTLEFIELD).size() == 1);
    CHECK(s.sent().of(Opcode::CMSG_LEAVE_BATTLEFIELD)[0].getSize() == 8);
    CHECK(firstQword(s.sent().of(Opcode::CMSG_LEAVE_BATTLEFIELD)[0]) == 0);
    s.receive(bgStatus(0, kWarsongRecord, 3, 489));
    ui.run("LeaveBattlefield()");
    REQUIRE(s.sent().of(Opcode::CMSG_LEAVE_BATTLEFIELD).size() == 2);
    CHECK(s.sent().of(Opcode::CMSG_LEAVE_BATTLEFIELD)[1].getSize() == 8);
    CHECK(firstQword(s.sent().of(Opcode::CMSG_LEAVE_BATTLEFIELD)[1]) == kWarsongRecord);
}

// T393: AcceptBattlefieldPort reads its answer by 0x00815500 (0x0054da10), so
// StaticPopup's AcceptBattlefieldPort(data, 0) declines.
TEST_CASE("AcceptBattlefieldPort(i, 0) declines (0x0054da10)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    const auto answer = [&](size_t i) -> int {
        auto p = s.sent().of(Opcode::CMSG_BATTLEFIELD_PORT)[i];
        REQUIRE(p.getSize() == 9);
        CHECK(firstQword(p) == kWarsongRecord);
        return p.getData()[8];
    };
    s.receive(bgStatus(0, kWarsongRecord, 2, 489));
    ui.run("AcceptBattlefieldPort(1, 0)");
    REQUIRE(s.sent().of(Opcode::CMSG_BATTLEFIELD_PORT).size() == 1);
    CHECK(answer(0) == 0);
    s.receive(bgStatus(0, kWarsongRecord, 2, 489));
    ui.run("AcceptBattlefieldPort(1, 'false')");
    REQUIRE(s.sent().of(Opcode::CMSG_BATTLEFIELD_PORT).size() == 2);
    CHECK(answer(1) == 0);
    s.receive(bgStatus(0, kWarsongRecord, 2, 489));
    ui.run("portAnswer = AcceptBattlefieldPort(1, 1)");
    REQUIRE(s.sent().of(Opcode::CMSG_BATTLEFIELD_PORT).size() == 3);
    CHECK(answer(2) == 1);
    CHECK(ui.is("portAnswer == 1"));
    // Only two slots: a third answers nil and sends nothing.
    ui.run("portAnswer = AcceptBattlefieldPort(3, 1)");
    CHECK(ui.is("portAnswer == nil"));
    CHECK(s.sent().of(Opcode::CMSG_BATTLEFIELD_PORT).size() == 3);
}

// T390: RequestBattlefieldScoreData (0x0054dcb0 -> 0x0054cf60) asks only in a
// running battle and at most once in 5 s; the scoreboard calls it each frame.
TEST_CASE("RequestBattlefieldScoreData asks once in 5 s and only in a battle (0x0054cf60)",
          "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("for i = 1, 3 do RequestBattlefieldScoreData() end");
    CHECK(s.sent().of(Opcode::MSG_PVP_LOG_DATA).empty());
    s.receive(bgStatus(0, kWarsongRecord, 3, 489));
    ui.run("for i = 1, 10 do RequestBattlefieldScoreData() end");
    CHECK(s.sent().of(Opcode::MSG_PVP_LOG_DATA).size() == 1);
    wowee::core::advanceTestClock(4.9);
    ui.run("RequestBattlefieldScoreData()");
    CHECK(s.sent().of(Opcode::MSG_PVP_LOG_DATA).size() == 1);
    wowee::core::advanceTestClock(0.2);
    ui.run("RequestBattlefieldScoreData() RequestBattlefieldScoreData()");
    CHECK(s.sent().of(Opcode::MSG_PVP_LOG_DATA).size() == 2);
}

// T391: the scoreboard's columns are WorldStateUI.dbc's (0x0054ae40 at a
// running status; 0x00549f20, 0x0054c170): the first unbroken run of type-2
// rows of the battle's map or -1, with their text, icon and tooltip.
TEST_CASE("battleground stat columns come from WorldStateUI (0x0054c170)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    const auto row = [](uint32_t id, int32_t map, uint32_t type, const char* text, const char* icon,
                        const char* tip) {
        wowee::game::world_state_ui::Row r;
        r.id = id; r.mapId = map; r.type = type; r.text = text; r.icon = icon; r.tooltip = tip;
        return r;
    };
    s.handler().setWorldStateUiRows({row(1, 489, 1, "x", "", ""),
                                     row(42, 489, 2, "Eroberte Flaggen", "IconCap", "TipCap"),
                                     row(44, 489, 2, "Zurueckgebrachte Flaggen", "IconRet", "TipRet"),
                                     row(50, 489, 1, "y", "", ""), row(51, 489, 2, "z", "", ""),
                                     row(60, 30, 2, "Alterac", "", "")});
    CHECK(ui.is("GetNumBattlefieldStats() == 0"));
    CHECK(ui.is("select('#', GetBattlefieldStatInfo(1)) == 3 and GetBattlefieldStatInfo(1) == nil"));
    s.receive(bgStatus(0, kWarsongRecord, 3, 489));
    CHECK(ui.is("GetNumBattlefieldStats() == 2"));
    CHECK(ui.is("select(1, GetBattlefieldStatInfo(1)) == 'Eroberte Flaggen' and "
                "select(2, GetBattlefieldStatInfo(1)) == 'IconCap' and "
                "select(3, GetBattlefieldStatInfo(1)) == 'TipCap'"));
    CHECK(ui.is("GetBattlefieldStatInfo(2) == 'Zurueckgebrachte Flaggen'"));
    CHECK(ui.is("GetBattlefieldStatInfo(3) == nil"));
}

// T394: flag arguments read as the client reads a switch (0x00815500): 0 and
// 'false' are off, and each setter's own default for none.
TEST_CASE("flag setters read 0 as off (0x00815500)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("eb = CreateFrame('EditBox') sm = CreateFrame('ScrollingMessageFrame') bt = CreateFrame('Button')");
    ui.run("eb:SetMultiLine(1) eb:SetMultiLine(0) eb:SetNumeric(1) eb:SetNumeric('false') "
           "eb:SetPassword(1) eb:SetPassword(0) eb:SetAutoFocus(0) eb:SetAltArrowKeyMode(0) "
           "eb:SetCountInvisibleLetters(0) sm:SetFading(0) sm:SetHyperlinksEnabled(0)");
    CHECK(ui.is("not eb:IsMultiLine() and not eb:IsNumeric() and not eb:IsPassword() and not eb:IsAutoFocus()"));
    CHECK(ui.is("not eb:GetAltArrowKeyMode() and not eb:GetCountInvisibleLetters()"));
    CHECK(ui.is("not sm:GetFading() and not sm:GetHyperlinksEnabled()"));
    // No argument: on for these (0x00815500(L,2,1)) ...
    ui.run("eb:SetMultiLine() eb:SetNumeric() sm:SetFading()");
    CHECK(ui.is("eb:IsMultiLine() and eb:IsNumeric() and sm:GetFading()"));
    // ... off for SetMotionScriptsWhileDisabled (0x009783b0: (L,2,0)).
    ui.run("bt:SetMotionScriptsWhileDisabled(1) bt:SetMotionScriptsWhileDisabled(0)");
    CHECK(ui.is("not bt:GetMotionScriptsWhileDisabled()"));
    ui.run("bt:SetMotionScriptsWhileDisabled(1) bt:SetMotionScriptsWhileDisabled()");
    CHECK(ui.is("not bt:GetMotionScriptsWhileDisabled()"));
}

// UnitIsAFK 0x0060cc30: the active player answers 1 while it has marked itself
// away (0x00bcefec), before the realm's PLAYER_FLAGS bit comes back.
TEST_CASE("UnitIsAFK answers the player's own away mark (0x0060cc30)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    CHECK(ui.is("UnitIsAFK('player') == nil and UnitIsDND('player') == nil"));
    s.handler().afkStatusRef() = true;
    CHECK(ui.is("UnitIsAFK('player') == 1"));
    CHECK(ui.is("UnitIsDND('player') == nil"));
    s.handler().afkStatusRef() = false;
}

// SpellStopCasting 0x00809ea0: a charmed player stops nothing and answers nil
// (the first test, UNIT_FIELD_CHARMEDBY); free again, the cast is stopped.
TEST_CASE("SpellStopCasting stops nothing while charmed (0x00809ea0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::UNIT_FIELD_CHARMEDBY, 0x77}});
    s.setConnected(true);
    Interface ui(s);
    wowee::network::Packet start(wowee::game::wireOpcode(Opcode::SMSG_SPELL_START));
    start.writePackedGuid(Session::kPlayerGuid);
    start.writePackedGuid(Session::kPlayerGuid);
    start.writeUInt8(1);
    start.writeUInt32(133);
    start.writeUInt32(0);
    start.writeUInt32(3000);
    start.writeUInt32(0);
    s.receive(start);
    REQUIRE(s.handler().getSpellHandler()->hasOwnCast());
    CHECK(ui.is("SpellStopCasting() == nil"));
    CHECK(s.sent().of(Opcode::CMSG_CANCEL_CAST).empty());
    CHECK(s.handler().getSpellHandler()->hasOwnCast());
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_CHARMEDBY, 0}}));
    CHECK(ui.is("SpellStopCasting() == 1"));
    CHECK_FALSE(s.handler().getSpellHandler()->hasOwnCast());
}

// UnitIsPVP 0x0060ce20 / UnitIsPVPFreeForAll 0x0060cfb0: a unit's PvP byte of
// UNIT_FIELD_BYTES_2 (0x01 flagged, 0x04 free-for-all), not UNIT_FIELD_FLAGS.
TEST_CASE("UnitIsPVP reads the PvP byte of UNIT_FIELD_BYTES_2 (0x0060ce20)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::UNIT_FIELD_FLAGS, 0x1000}});
    Interface ui(s);
    CHECK(ui.is("UnitIsPVP('player') == nil and UnitIsPVPFreeForAll('player') == nil"));
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_BYTES_2, 0x0100}}));
    CHECK(ui.is("UnitIsPVP('player') == 1 and UnitIsPVPFreeForAll('player') == nil"));
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_BYTES_2, 0x0400}}));
    CHECK(ui.is("UnitIsPVP('player') == nil and UnitIsPVPFreeForAll('player') == 1"));
}

// T397: 0x00509dd0 fills the $-tokens of battleground announcements
// (0x24-0x26) and achievement lines (0x30, 0x31) with the message's target,
// as it does a monster's line.
TEST_CASE("a battleground announcement names its target for $n (0x00509dd0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    wowee::game::GameHandlerTestAccess::setCharacter(s.handler(), "A", "Locki", Session::kPlayerGuid);
    Interface ui(s);
    ui.run("bgLine = nil local f = CreateFrame('Frame') f:RegisterEvent('CHAT_MSG_BG_SYSTEM_HORDE') "
           "f:SetScript('OnEvent', function(self, e, msg) bgLine = msg end)");
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_MESSAGECHAT));
    p.writeUInt8(0x26);
    p.writeUInt32(0);
    p.writeUInt64(0);
    p.writeUInt32(0);
    p.writeUInt64(Session::kPlayerGuid);
    const std::string text = "$n hat die Flagge der Allianz aufgenommen!";
    p.writeUInt32(static_cast<uint32_t>(text.size() + 1));
    p.writeString(text);
    p.writeUInt8(0);
    s.receive(p);
    CHECK(ui.is("bgLine == 'Locki hat die Flagge der Allianz aufgenommen!'"));
}

// T398: an incoming quest link is recoloured for the reader (0x00509dd0 with
// 0x0061e540): a level-3 quest is grey at 80, -1 is the reader's own level,
// an escaped '||Hquest:' is text.
TEST_CASE("incoming quest links take the reader's colour (0x00509dd0)", "[replay][interface]") {
    using wowee::game::recolourQuestLinks;
    CHECK(recolourQuestLinks("Q |cffffff00|Hquest:789:3|h[Stich]|h|r", 80) == "Q |cff808080|Hquest:789:3|h[Stich]|h|r");
    CHECK(recolourQuestLinks("|cff808080|Hquest:1:-1|h[a]|h|r", 80) == "|cffffff00|Hquest:1:-1|h[a]|h|r");
    CHECK(recolourQuestLinks("|cff808080|Hquest:1:85|h[a]|h|r", 80) == "|cffff2020|Hquest:1:85|h[a]|h|r");
    CHECK(recolourQuestLinks("abcdefghij||Hquest:1:3|h", 80) == "abcdefghij||Hquest:1:3|h");
}

// T400: answers as numbers/nil where 12340 pushes them - Texture IsDesaturated
// 0x0048ca30 (1/nil) and SetDesaturated 0x0048c9a0 (1), the guild right
// queries (CanGuildPromote 0x005cbcf0 ...: 1/nil), IsConnectedToServer
// 0x004dda50 (1/nil); IsDebugBuild 0x0050fe80 pushes false; IsInLFGDungeon
// 0x00555660 answers nothing at all outside a finder dungeon.
TEST_CASE("number and nil answers as 12340 pushes them (T400)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("tx = UIParent:CreateTexture()");
    CHECK(ui.is("tx:IsDesaturated() == nil"));
    CHECK(ui.is("tx:SetDesaturated(1) == 1 and tx:IsDesaturated() == 1"));
    CHECK(ui.is("tx:SetDesaturated(0) == 1 and tx:IsDesaturated() == nil"));
    CHECK(ui.is("CanGuildPromote() == nil and CanGuildInvite() == nil and CanEditMOTD() == nil "
                "and CanViewOfficerNote() == nil and CanEditGuildInfo() == nil"));
    CHECK(ui.is("IsDebugBuild() == false"));
    CHECK(ui.is("select('#', IsInLFGDungeon()) == 0"));
    CHECK(ui.is("IsConnectedToServer() == nil"));
    s.setConnected(true);
    CHECK(ui.is("IsConnectedToServer() == 1"));
}

// T396: CalendarGetEventInfo 0x005bd8a0 answers the open event whatever its
// id - a new one being made (0x005bbe50) too, with what CalendarEventSet*
// wrote into it; the date not given yet reads 0/0/0/1999 -1:-1, the maker's
// own row (0x005f2a20: confirmed, normal invite) gives status 4 and type 1.
TEST_CASE("CalendarGetEventInfo answers the event being made (T396, 0x005bd8a0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    wowee::game::GameHandlerTestAccess::setCharacter(s.handler(), "A", "Locki", Session::kPlayerGuid);
    Interface ui(s);
    CHECK(ui.is("select(1, CalendarGetEventInfo()) == nil"));
    ui.run("CalendarNewEvent() CalendarEventSetTitle('E2 Titel') CalendarEventSetType(2) "
           "ev = {CalendarGetEventInfo()}");
    CHECK(ui.is("ev[1] == 'E2 Titel' and ev[2] == '' and ev[4] == 2 and ev[5] == 1"));
    CHECK(ui.is("type(ev[3]) == 'string'"));  // the maker's name from the name cache
    CHECK(ui.is("ev[8] == 0 and ev[9] == 0 and ev[10] == 0 and ev[11] == 1999 and ev[12] == -1 and ev[13] == -1"));
    CHECK(ui.is("ev[20] == nil and ev[21] == nil and ev[23] == 4 and ev[24] == 1 and ev[25] == 'PLAYER'"));
    ui.run("CalendarEventSetDate(12, 24, 2026) CalendarEventSetTime(20, 15) ev = {CalendarGetEventInfo()}");
    CHECK(ui.is("ev[9] == 12 and ev[10] == 24 and ev[11] == 2026 and ev[12] == 20 and ev[13] == 15"));
}

// Found by V in code: an opened (sent) event's lockout fields are its second
// packed time (+0x4dc, 0x005f2c00), not the event's date again; a field of all
// ones reads -1 (0x0076c970), so 0/0/0/1999 -1:-1 (0x005bd8a0).
TEST_CASE("CalendarGetEventInfo answers an opened event's lockout (0x005bd8a0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    const auto packed = [](uint32_t year, uint32_t month, uint32_t day, uint32_t weekday,
                           uint32_t hour, uint32_t minute) {
        return minute | (hour << 6) | (weekday << 11) | ((day - 1) << 14) | ((month - 1) << 20) |
               ((year - 2000) << 24);
    };
    const auto sendEvent = [&](uint32_t lockout) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_CALENDAR_SEND_EVENT));
        p.writeUInt8(0);
        p.writePackedGuid(0x070000000000ABCDull);
        p.writeUInt64(4242);
        p.writeString("Raid");
        p.writeString("");
        p.writeUInt8(1);
        p.writeUInt8(0);
        p.writeUInt32(25);
        p.writeUInt32(static_cast<uint32_t>(-1));
        p.writeUInt32(0);
        p.writeUInt32(packed(2026, 12, 24, 4, 20, 15));  // Thursday 24 Dec 2026 20:15
        p.writeUInt32(lockout);
        p.writeUInt32(0);
        p.writeUInt32(0);  // no invitees
        s.receive(p);
    };
    sendEvent(packed(2026, 12, 30, 3, 3, 0));  // Wednesday 30 Dec 2026 03:00
    ui.run("ev = {CalendarGetEventInfo()}");
    REQUIRE(ui.is("ev[1] == 'Raid'"));
    CHECK(ui.is("ev[8] == 5 and ev[9] == 12 and ev[10] == 24 and ev[11] == 2026 and ev[12] == 20 and ev[13] == 15"));
    CHECK(ui.is("ev[14] == 4 and ev[15] == 12 and ev[16] == 30 and ev[17] == 2026 and ev[18] == 3 and ev[19] == 0"));
    sendEvent(0x1FFFFFFFu);
    ui.run("ev = {CalendarGetEventInfo()}");
    CHECK(ui.is("ev[14] == 0 and ev[15] == 0 and ev[16] == 0 and ev[17] == 1999 and ev[18] == -1 and ev[19] == -1"));
}

// T399: the speaker's gesture (0x00509dd0): a whole-line LAUGH_WORD laughs
// (party lines only then), a yell shouts, a say ends in '?' or '!' or talks;
// the Emotes.dbc rows are chosen as 0x007165d0 does - the first row with each
// of the flags 0x8/0x10/0x20/0x40/0x100, a row counted for its first flag.
TEST_CASE("chat lines choose the speaker's gesture (T399, 0x00509dd0)", "[replay][interface]") {
    using wowee::game::ChatGesture;
    using wowee::game::chatGestureFor;
    const std::vector<std::string> laugh{"lol", "haha"};
    CHECK(chatGestureFor(1, "Hallo", laugh) == ChatGesture::Talk);
    CHECK(chatGestureFor(1, "Wirklich?", laugh) == ChatGesture::Question);
    CHECK(chatGestureFor(1, "Hallo!", laugh) == ChatGesture::Exclamation);
    CHECK(chatGestureFor(6, "Hallo?", laugh) == ChatGesture::Shout);
    CHECK(chatGestureFor(1, "lol", laugh) == ChatGesture::Laugh);
    CHECK(chatGestureFor(1, "lol!", laugh) == ChatGesture::Exclamation);
    CHECK(chatGestureFor(2, "haha", laugh) == ChatGesture::Laugh);
    CHECK(chatGestureFor(0x33, "Hallo?", laugh) == ChatGesture::None);
    CHECK(chatGestureFor(7, "Hallo", laugh) == ChatGesture::None);
    const auto rows = wowee::rendering::EmoteRegistry::chatGestureRows(
        {{1, 0x8}, {2, 0x8}, {6, 0x10}, {5, 0x28}, {7, 0x20}, {22, 0x40}, {11, 0x100}, {12, 0x110}});
    CHECK(rows == std::array<uint32_t, 5>{1, 6, 7, 22, 11});
}

// T394 leftover: SetActionBarToggles reads its four flags by 0x00815500
// (0x005a8290), so 0 and 'false' are off; CMSG_SET_ACTIONBAR_TOGGLES carries
// them as bits 0-3.
TEST_CASE("SetActionBarToggles reads 0 as off (0x005a8290)", "[replay][interface]") {
    const int tag = 0;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("wowee_bar_toggles_" + std::to_string(reinterpret_cast<std::uintptr_t>(&tag)));
    std::filesystem::create_directories(root);
    wowee::core::setEnvVar("WOWEE_CONFIG_ROOT", root.string().c_str());
    {
        Session s;
        s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
        s.setConnected(true);
        Interface ui(s);
        ui.run("SetActionBarToggles(1, 0, 'false', 1)");
        const auto sent = s.sent().of(Opcode::CMSG_SET_ACTIONBAR_TOGGLES);
        REQUIRE(sent.size() == 1);
        wowee::network::Packet p = sent.back();
        p.setReadPos(0);
        CHECK(p.readUInt8() == 0x9);
    }
    wowee::core::unsetEnvVar("WOWEE_CONFIG_ROOT");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// UnitIsConnected 0x0060f790: a unit this client has is connected; only
// without one does the member's status record (+8 bit 1) answer (found by U
// in code: a member in sight whose stats said offline answered nil).
TEST_CASE("UnitIsConnected answers 1 for a member in sight (0x0060f790)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    wowee::network::Packet g(wowee::game::wireOpcode(Opcode::SMSG_GROUP_LIST));
    g.writeUInt8(0);
    for (int i = 0; i < 3; ++i) g.writeUInt8(0);
    g.writeUInt64(0x1234);
    g.writeUInt32(1);
    g.writeUInt32(1);
    g.writeString("Achiles");
    g.writeUInt64(0x1001);
    g.writeUInt8(0);  // offline by the roster
    g.writeUInt8(0);
    g.writeUInt8(0);
    g.writeUInt8(0);
    g.writeUInt64(Session::kPlayerGuid);
    for (int i = 0; i < 12; ++i) g.writeUInt8(0);
    s.receive(g);
    CHECK(ui.is("UnitIsConnected('party1') == nil"));
    s.receive(UpdateObject().create(0x1001, wowee::game::ObjectType::PLAYER,
                                    {{UF::UNIT_FIELD_HEALTH, 10}, {UF::UNIT_FIELD_MAXHEALTH, 10},
                                     {UF::UNIT_FIELD_LEVEL, 80}}));
    CHECK(ui.is("UnitIsConnected('party1') == 1"));
}

// UnitIsConnected 0x0060f790 for a member's pet out of sight: not a member,
// so 0x00513cb0 -> 0x0052bf80 finds the pet record of an online member and
// answers 1; an offline member's pet nil (found by V in code).
TEST_CASE("UnitIsConnected answers a member's pet out of sight (0x0052bf80)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    wowee::network::Packet g(wowee::game::wireOpcode(Opcode::SMSG_GROUP_LIST));
    g.writeUInt8(0);
    for (int i = 0; i < 3; ++i) g.writeUInt8(0);
    g.writeUInt64(0x1234);
    g.writeUInt32(1);
    g.writeUInt32(1);
    g.writeString("Achiles");
    g.writeUInt64(0x1001);
    g.writeUInt8(0);
    g.writeUInt8(0);
    g.writeUInt8(0);
    g.writeUInt8(0);
    g.writeUInt64(Session::kPlayerGuid);
    for (int i = 0; i < 12; ++i) g.writeUInt8(0);
    s.receive(g);
    const auto stats = [&](uint16_t status) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_PARTY_MEMBER_STATS));
        p.writePackedGuid(0x1001);
        p.writeUInt32(0x0001 | 0x0400 | 0x0800 | 0x2000 | 0x4000 | 0x8000 | 0x10000 | 0x20000);
        p.writeUInt16(status);
        p.writeUInt64(0xF140000100002001ull);
        p.writeString("Wolfi");
        p.writeUInt32(700);   // pet health
        p.writeUInt32(900);   // pet maximum
        p.writeUInt8(2);      // focus
        p.writeUInt16(80);
        p.writeUInt16(100);
        s.receive(p);
    };
    ui.run("seen = {} local f = CreateFrame('Frame') f:RegisterEvent('UNIT_HEALTH') "
           "f:SetScript('OnEvent', function(self, event, unit) seen[#seen + 1] = unit end)");
    stats(1);
    REQUIRE(ui.is("UnitGUID('partypet1') ~= nil"));
    CHECK(ui.is("UnitIsConnected('partypet1') == 1"));
    // The pet's own stats answer for it out of sight (0x0060eb60/ec60/ed40/
    // ef40/f100 -> the pet record of 0x0052bf80), and its health moving is
    // said for its token (0x006cf740).
    CHECK(ui.is("UnitHealth('partypet1') == 700 and UnitHealthMax('partypet1') == 900"));
    CHECK(ui.is("UnitPower('partypet1') == 80 and UnitPowerMax('partypet1') == 100 "
                "and UnitPowerType('partypet1') == 2"));
    CHECK(ui.is("UnitName('partypet1') == 'Wolfi'"));
    CHECK(ui.is("(function() for _, u in ipairs(seen) do if u == 'partypet1' then return true end end end)()"));
    // The pet in sight, its owner not: still of the group (0x0052d310 reads
    // the stats' pet guid), so its feigned death shows as such.
    s.receive(UpdateObject().create(0xF140000100002001ull, wowee::game::ObjectType::UNIT,
                                    {{UF::UNIT_FIELD_HEALTH, 10}, {UF::UNIT_FIELD_MAXHEALTH, 10},
                                     {UF::UNIT_DYNAMIC_FLAGS, 0x20}}));
    stats(1);
    CHECK(ui.is("UnitIsFeignDeath('partypet1') == 1 and UnitIsDead('partypet1') == nil"));
}

// UnitInRange 0x00612f10: the player counts as of its group (0x00512a30) and
// stands at its own place - 1, as for a member at forty yards exactly; a
// stranger nil (found by U in code).
TEST_CASE("UnitInRange counts the player and forty yards (0x00612f10)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    CHECK(ui.is("UnitInRange('player') == 1"));
    CHECK(ui.is("UnitInRange('target') == nil"));
}

// FIXER W (found by V in code): the item clicks do nothing while the player
// has no control of itself - feared, charmed - unless it rides a taxi
// (0x00bcfb8c or UNIT_FIELD_FLAGS 0x100000): PickupContainerItem 0x005d7ff0,
// SplitContainerItem 0x005d84f0, PickupInventoryItem 0x005ea5f0 ->
// 0x005e85d0; PickupBagFromSlot (0x005e8c30) and PutItemInBackpack
// (0x005e8d30) want the control itself, a taxi does not do there.
TEST_CASE("item clicks wait for the player's control or a taxi (0x00bcfb8c)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    carryForAuction(s, 0, 0x4000000000000C01ull, 2589, 5);
    const auto control = [&](bool on) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_CLIENT_CONTROL_UPDATE));
        p.writePackedGuid(Session::kPlayerGuid);
        p.writeUInt8(on ? 1 : 0);
        s.receive(p);
    };
    control(false);
    REQUIRE_FALSE(s.handler().isServerMovementAllowed());
    ui.run("ClearCursor() PickupContainerItem(0, 1)");
    CHECK(ui.is("not CursorHasItem()"));
    ui.run("ClearCursor() SplitContainerItem(0, 1, 2)");
    CHECK(ui.is("not CursorHasItem()"));
    // On a taxi the pickup and the split are let through.
    s.receive(replay::UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_FLAGS, 0x100000u}}));
    ui.run("ClearCursor() PickupContainerItem(0, 1)");
    CHECK(ui.is("CursorHasItem() == 1"));
    // ... but not the drop into the backpack: 0x005e8d30 wants the control.
    CHECK(ui.is("PutItemInBackpack() == nil"));
    ui.run("ClearCursor() SplitContainerItem(0, 1, 2)");
    CHECK(ui.is("CursorHasItem() == 1"));
    ui.run("ClearCursor()");
    s.receive(replay::UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_FLAGS, 0u}}));
    control(true);
    ui.run("ClearCursor() PickupContainerItem(0, 1)");
    CHECK(ui.is("CursorHasItem() == 1"));
    ui.run("ClearCursor()");
    // GetContainerNumSlots (0x005d74a0): the usage line for no number.
    CHECK(ui.is("select(2, pcall(GetContainerNumSlots)):find('Usage: GetContainerNumSlots(index)', 1, true) ~= nil"));
    CHECK(ui.is("GetContainerNumSlots(0) == 16"));
}

// FIXER W (found by V in code): CastSpellByID (0x0053e060) casts a spell of
// the player's own list as the player's; any other goes to the pet that has
// it - the slot of its bar holding it (0x005d3560), else its book as state 1
// (0x00be7d98) - by CMSG_PET_ACTION, and one known to neither casts nothing.
// It sent CMSG_CAST_SPELL for any id.
TEST_CASE("CastSpellByID casts the pet's spell through the pet (0x0053e060)", "[replay][interface]") {
    constexpr uint64_t kImp = 0xCD;
    constexpr uint32_t kOwn = 686, kBar = 3110, kBook = 7812, kNobody = 133;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    for (uint32_t id : {kOwn, kBar, kBook, kNobody}) s.handler().spellNameCacheRef()[id].name = "Spell";
    wowee::network::Packet known(wowee::game::wireOpcode(Opcode::SMSG_INITIAL_SPELLS));
    known.writeUInt8(0);
    known.writeUInt16(1);
    known.writeUInt32(kOwn);
    known.writeUInt16(0);
    known.writeUInt16(0);
    s.receive(known);
    s.receive(UpdateObject().create(kImp, wowee::game::ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 100}}));
    wowee::network::Packet bar(wowee::game::wireOpcode(Opcode::SMSG_PET_SPELLS));
    bar.writeUInt64(kImp);
    bar.writeUInt16(0);
    bar.writeUInt32(0);
    bar.writeUInt8(0);
    bar.writeUInt8(0);
    bar.writeUInt16(0);
    for (int i = 0; i < 10; ++i) bar.writeUInt32(i == 3 ? (0xC1000000u | kBar) : 0u);
    bar.writeUInt8(2);
    bar.writeUInt32(0xC1000000u | kBar);
    bar.writeUInt32(0x01000000u | kBook);
    bar.writeUInt8(0);
    s.receive(bar);
    REQUIRE(s.handler().getPetGuid() == kImp);
    const auto actions = [&] { return s.sent().of(Opcode::CMSG_PET_ACTION); };
    const auto casts = [&] { return s.sent().of(Opcode::CMSG_CAST_SPELL).size(); };
    const auto actionValue = [&](size_t i) {
        auto p = actions().at(i);
        p.setReadPos(8);
        return p.readUInt32();
    };
    ui.run("CastSpellByID(" + std::to_string(kNobody) + ")");
    CHECK(casts() == 0);
    CHECK(actions().empty());
    ui.run("CastSpellByID(" + std::to_string(kBar) + ")");
    CHECK(casts() == 0);
    REQUIRE(actions().size() == 1);
    CHECK(actionValue(0) == (0xC1000000u | kBar));
    INFO("book ids " << s.handler().getPetSpells().size());
    ui.run("CastSpellByID(" + std::to_string(kBook) + ")");
    REQUIRE(actions().size() == 2);
    CHECK(actionValue(1) == (0x01000000u | kBook));
    ui.run("CastSpellByID(" + std::to_string(kOwn) + ")");
    CHECK(casts() == 1);
    CHECK(actions().size() == 2);
}

// FIXER W (found in code): IsSpellKnown(id, isPet) also finds a spell on a
// slot of the pet's bar that its book lacks (0x0053c3a0 -> 0x005d3560).
TEST_CASE("IsSpellKnown of the pet sees its bar (0x0053c3a0)", "[replay][interface]") {
    constexpr uint64_t kImp = 0xCD;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.receive(UpdateObject().create(kImp, wowee::game::ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 100}}));
    wowee::network::Packet bar(wowee::game::wireOpcode(Opcode::SMSG_PET_SPELLS));
    bar.writeUInt64(kImp);
    bar.writeUInt16(0);
    bar.writeUInt32(0);
    bar.writeUInt8(0);
    bar.writeUInt8(0);
    bar.writeUInt16(0);
    for (int i = 0; i < 10; ++i) bar.writeUInt32(i == 0 ? 0x07000002u : i == 4 ? (0x81000000u | 3110u) : 0u);
    bar.writeUInt8(0);
    bar.writeUInt8(0);
    s.receive(bar);
    CHECK(ui.is("IsSpellKnown(3110, true) == true"));
    CHECK(ui.is("IsSpellKnown(3110) == false"));
    CHECK(ui.is("IsSpellKnown(2, true) == false"));
}

// FIXER W (found in code): HasFullControl (0x00611600) answers nil while the
// player looks through another's eyes - the player's +0x1858 0x1, set as
// CMSG_FAR_SIGHT 1 goes (0x006e2880).
TEST_CASE("HasFullControl is nil while looking through another (0x00611600)", "[replay][interface]") {
    constexpr uint64_t kEye = 0xAB;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    CHECK(ui.is("HasFullControl() == 1"));
    s.receive(UpdateObject().create(kEye, wowee::game::ObjectType::UNIT, {{UF::UNIT_FIELD_HEALTH, 100}}));
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_FARSIGHT, static_cast<uint32_t>(kEye)}}));
    REQUIRE(s.sent().of(Opcode::CMSG_FAR_SIGHT).size() == 1);
    CHECK(ui.is("HasFullControl() == nil"));
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_FARSIGHT, 0u}}));
    CHECK(ui.is("HasFullControl() == 1"));
}

// FIXER W (found in code): a member out of view is in a vehicle by its stats'
// status 0x200 (0x006133d0 / 0x006134a0 -> 0x00513c30 -> 0x0052bee0, +8).
TEST_CASE("UnitInVehicle answers a member out of view by its status (0x006133d0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    wowee::network::Packet g(wowee::game::wireOpcode(Opcode::SMSG_GROUP_LIST));
    g.writeUInt8(0);
    for (int i = 0; i < 3; ++i) g.writeUInt8(0);
    g.writeUInt64(0x1234);
    g.writeUInt32(1);
    g.writeUInt32(1);
    g.writeString("Achiles");
    g.writeUInt64(0x1001);
    g.writeUInt8(0);
    g.writeUInt8(0);
    g.writeUInt8(0);
    g.writeUInt8(0);
    g.writeUInt64(Session::kPlayerGuid);
    for (int i = 0; i < 12; ++i) g.writeUInt8(0);
    s.receive(g);
    const auto status = [&](uint16_t flags) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_PARTY_MEMBER_STATS));
        p.writePackedGuid(0x1001);
        p.writeUInt32(0x0001);
        p.writeUInt16(flags);
        s.receive(p);
    };
    status(0x0001);
    REQUIRE(ui.is("UnitGUID('party1') ~= nil"));
    CHECK(ui.is("UnitInVehicle('party1') == nil and UnitUsingVehicle('party1') == nil"));
    status(0x0001 | 0x0200);
    CHECK(ui.is("UnitInVehicle('party1') == 1 and UnitUsingVehicle('party1') == 1"));
}

// T403: CHANNEL_ROSTER_UPDATE / CHANNEL_COUNT_UPDATE name the channel's row in
// the pane, headers counted (0x00503990 -> 0x004ff4f0) - ChannelRoster_Update
// reads GetChannelDisplayInfo(id) of it. They carried the channel's number.
TEST_CASE("the roster events name the channel's row in the pane (T403)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    const auto joined = [&](const std::string& name) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_CHANNEL_NOTIFY));
        p.writeUInt8(0x02);
        p.writeString(name);
        p.writeUInt8(0);
        p.writeUInt32(0);
        p.writeUInt32(0);
        s.receive(p);
    };
    ui.run("JoinChannelByName('alpha') JoinChannelByName('beta')");
    joined("alpha");
    joined("beta");
    ui.run("seen = {} local f = CreateFrame('Frame') f:RegisterEvent('CHANNEL_ROSTER_UPDATE') "
           "f:RegisterEvent('CHANNEL_COUNT_UPDATE') "
           "f:SetScript('OnEvent', function(self, event, id, count) seen[#seen + 1] = event .. ' ' .. tostring(id) end)");
    REQUIRE(ui.is("GetChannelDisplayInfo(5) == 'beta'"));
    wowee::network::Packet list(wowee::game::wireOpcode(Opcode::SMSG_CHANNEL_LIST));
    list.writeUInt8(1);
    list.writeString("beta");
    list.writeUInt8(0);
    list.writeUInt32(0);
    s.receive(list);
    CHECK(ui.is("#seen == 2 and seen[1] == 'CHANNEL_ROSTER_UPDATE 5' and seen[2] == 'CHANNEL_COUNT_UPDATE 5'"));
}

// T404: GetBarberShopStyleInfo(4) answers nothing for a race without skin
// styles (0x0052e850: DAT_00bd19b4), as SetNextBarberShopStyle(4) does.
TEST_CASE("the barber's skin row is nothing for a race without one (T404)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    bool skins = false;
    wowee::addons::LuaServices services;
    services.getBarberStyleInfo = [](int, std::string& name, std::string& description, uint32_t& cost,
                                     bool& isCurrent) {
        name = "Current";
        description.clear();
        cost = 111173;
        isCurrent = true;
        return true;
    };
    services.barberCanAlterSkin = [&] { return skins; };
    ui.engine.setLuaServices(services);
    CHECK(ui.is("select('#', GetBarberShopStyleInfo(4)) == 0"));
    CHECK(ui.is("select('#', GetBarberShopStyleInfo(3)) == 4"));
    skins = true;
    CHECK(ui.is("select('#', GetBarberShopStyleInfo(4)) == 4"));
}

// FIXER X (W's open item, 0x005a9e20): IsUsableAction's slot test lets a slot
// that casts nothing (0x005a8c30 = 0) - a macro showing no spell, an item
// without a use spell, an equipment set - be used only while the player has
// control of itself (0x00bcfb8c); an equipment set also wants the manager on
// (0x005ad6b0), its items unlocked (0x005ad940, 0x005ae040), and is usable on
// a taxi as it stands. A macro the slot names that is gone is (nil, nil).
// All of these were (1, nil) whatever the state.
TEST_CASE("a slot casting nothing is usable only in control (0x005a9e20)", "[replay][interface]") {
    using wowee::game::ActionBarSlot;
    constexpr uint64_t kCloth = 0x4000000000000C21ull;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    auto& gh = s.handler();
    carryForAuction(s, 0, kCloth, 2589, 5);
    gh.setMacroMeta(1, "Hi", "");
    gh.setMacroText(1, "/say hi");
    gh.setActionBarSlot(0, ActionBarSlot::MACRO, 1);
    gh.setActionBarSlot(1, ActionBarSlot::ITEM, 2589);
    gh.setActionBarSlot(2, ActionBarSlot::MACRO, 7);   // no macro 7
    wowee::network::Packet list(wowee::game::wireOpcode(Opcode::SMSG_EQUIPMENT_SET_LIST));
    list.writeUInt32(1);
    list.writePackedGuid(0x70);
    list.writeUInt32(0);
    list.writeString("Tank");
    list.writeString("INV_Shield_06");
    for (int slot = 0; slot < 19; ++slot) list.writePackedGuid(slot == 4 ? 1 : slot == 0 ? kCloth : 0);
    s.receive(list);
    gh.setActionBarSlot(3, ActionBarSlot::EQUIPMENT_SET, 0);
    const auto control = [&](bool on) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_CLIENT_CONTROL_UPDATE));
        p.writePackedGuid(Session::kPlayerGuid);
        p.writeUInt8(on ? 1 : 0);
        s.receive(p);
    };
    const auto usable = [&](int slot) {
        return "select('#', IsUsableAction(" + std::to_string(slot) + ")) == 2 and IsUsableAction(" +
               std::to_string(slot) + ") == 1";
    };
    const auto refused = [&](int slot) {
        const std::string n = std::to_string(slot);
        return "IsUsableAction(" + n + ") == nil and select(2, IsUsableAction(" + n + ")) == nil";
    };
    ui.run("SetCVar('equipmentManager', '0')");
    CHECK(ui.is(usable(1)));
    CHECK(ui.is(usable(2)));
    CHECK(ui.is(refused(3)));
    CHECK(ui.is(refused(4)));       // the manager is off
    ui.run("SetCVar('equipmentManager', '1')");
    CHECK(ui.is(usable(4)));
    // A set's item held on the cursor is locked.
    ui.run("ClearCursor() PickupContainerItem(0, 1)");
    REQUIRE(ui.is("CursorHasItem() == 1"));
    CHECK(ui.is(refused(4)));
    CHECK(ui.is("EquipmentSetContainsLockedItems('Tank') == true"));
    ui.run("ClearCursor()");
    CHECK(ui.is(usable(4)));
    CHECK(ui.is("EquipmentSetContainsLockedItems('Tank') == nil"));
    control(false);
    REQUIRE_FALSE(gh.isServerMovementAllowed());
    CHECK(ui.is(refused(1)));
    CHECK(ui.is(refused(2)));
    CHECK(ui.is(refused(4)));
    // On a taxi the set is usable; the rest still wants the control.
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_FLAGS, 0x100000u}}));
    CHECK(ui.is(usable(4)));
    CHECK(ui.is(refused(1)));
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_FLAGS, 0u}}));
    control(true);
    CHECK(ui.is(usable(1)));
    CHECK(ui.is(usable(2)));
    CHECK(ui.is(usable(4)));
}

// T406 (FIXER X): GetInventoryItemCount (0x005e9e40) answers 0 for a
// container - a bag on the bag bar showed "1" - and 1 for a slot holding
// nothing; a plain item its stack.
TEST_CASE("a bag's inventory count is no stack (T406)", "[replay][interface]") {
    using wowee::game::EquipSlot;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    auto& gh = s.handler();
    wowee::game::ItemDef bag;
    bag.itemId = 4500;
    bag.name = "Traveler's Backpack";
    bag.bagSlots = 16;
    bag.stackCount = 1;
    REQUIRE(gh.inventoryRef().setEquipSlot(EquipSlot::BAG1, bag));
    wowee::game::ItemDef ring;
    ring.itemId = 2589;
    ring.name = "Linen Cloth";
    ring.stackCount = 7;
    REQUIRE(gh.inventoryRef().setEquipSlot(EquipSlot::RING1, ring));
    CHECK(ui.is("GetInventoryItemCount('player', 20) == 0"));
    CHECK(ui.is("GetInventoryItemCount('player', 21) == 1"));    // nothing there
    CHECK(ui.is("GetInventoryItemCount('player', 11) == 7"));
}

// FIXER X (found in code): the action predicates read what the slot casts or
// uses (0x005a8c30, 0x005a8070), not only a bare spell or item slot.
// IsAttackAction (0x005a96d0) is Effect[0] 78 of that spell; IsConsumableAction
// (0x005a9360) also a spell paid in reagents, and GetActionCount (DAT_00c1e118
// via 0x007ffa30) then how many casts the carried reagents pay for - a summon
// paid in shards showed no number.
TEST_CASE("the action predicates read the slot's spell (0x005a96d0/0x005a9360)", "[replay][interface]") {
    using wowee::game::ActionBarSlot;
    constexpr uint32_t kSummon = 697, kShard = 6265, kSwing = 9999;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    auto& gh = s.handler();
    auto& summon = gh.spellNameCacheRef()[kSummon];
    summon.name = "Summon";
    summon.reagents[0] = {kShard, 1};
    auto& swing = gh.spellNameCacheRef()[kSwing];
    swing.name = "Swing";
    swing.effectIds[0] = 78;
    carryForAuction(s, 0, 0x4000000000000C31ull, kShard, 3);
    gh.setActionBarSlot(0, ActionBarSlot::SPELL, kSummon);
    gh.setActionBarSlot(1, ActionBarSlot::SPELL, kSwing);
    CHECK(ui.is("IsConsumableAction(1) == 1"));
    CHECK(ui.is("GetActionCount(1) == 3"));
    CHECK(ui.is("IsConsumableAction(2) == nil"));
    CHECK(ui.is("IsAttackAction(2) == 1"));
    CHECK(ui.is("IsAttackAction(1) == nil"));
}

// T408 (FIXER Y): GetLFGProposal (0x00554170) answers no value at all
// without a proposal; with one, hasResponded is the player's own row's
// answered flag. AcceptProposal/RejectProposal (0x00554450) mark the own row
// answered+accepted, or drop the proposal, at once and fire
// LFG_PROPOSAL_UPDATE; GetLFGProposalMember (0x00553000) answers nothing
// without a proposal and 'UNKNOWN' for a mask without a role bit.
TEST_CASE("a dungeon proposal answers as the client holds it (T408)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    auto& gh = s.handler();
    CHECK(ui.is("select('#', GetLFGProposal()) == 0"));
    CHECK(ui.is("select('#', GetLFGProposalMember(1)) == 0"));

    const auto offer = [&](uint32_t id) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_LFG_PROPOSAL_UPDATE));
        p.writeUInt32(261u | (1u << 24));   // dungeon entry (type 1)
        p.writeUInt8(0);                    // initiating
        p.writeUInt32(id);
        p.writeUInt32(0);                   // encounters done
        p.writeUInt8(0);                    // not silent
        p.writeUInt8(2);
        p.writeUInt32(0x08); for (int b : {1, 0, 0, 0, 0}) p.writeUInt8(static_cast<uint8_t>(b));
        p.writeUInt32(0x00); for (int b : {0, 0, 0, 0, 0}) p.writeUInt8(static_cast<uint8_t>(b));
        s.receive(p);
    };
    offer(77);
    REQUIRE(ui.is("select('#', GetLFGProposal()) == 12"));
    CHECK(ui.is("select(6, GetLFGProposal()) == 'DAMAGER'"));
    CHECK(ui.is("select(7, GetLFGProposal()) == false"));
    CHECK(ui.is("select(2, GetLFGProposalMember(2)) == 'UNKNOWN'"));
    CHECK(ui.is("select('#', GetLFGProposalMember(6)) == 0"));

    ui.run("updates = 0 local f = CreateFrame('Frame') f:RegisterEvent('LFG_PROPOSAL_UPDATE') "
           "f:SetScript('OnEvent', function() updates = updates + 1 end)");
    gh.lfgAcceptProposal(77, true);
    CHECK(s.sent().of(Opcode::CMSG_LFG_PROPOSAL_RESULT).size() == 1);
    CHECK(ui.is("updates == 1"));
    CHECK(ui.is("select(7, GetLFGProposal()) == true"));
    CHECK(ui.is("select(4, GetLFGProposalMember(1)) == true and select(5, GetLFGProposalMember(1)) == true"));

    offer(78);
    gh.lfgAcceptProposal(78, false);
    CHECK(ui.is("updates == 3"));   // the offer's own update, then the refusal's
    CHECK(ui.is("select('#', GetLFGProposal()) == 0"));
    CHECK(gh.getLfgProposalId() == 0);
}

// T407 (FIXER Y): AcceptBattlefieldPort (0x0054da10) only sends; the slot
// stays as the realm last described it until its next status - a queue left
// from the minimap menu read as gone at once while the realm kept it.
TEST_CASE("leaving a battleground queue waits for the realm (T407)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    auto queued = bgStatus(0, kWarsongRecord, 1, 0);
    queued.writeUInt32(60000); queued.writeUInt32(1000);   // average wait, time queued
    s.receive(queued);
    REQUIRE(ui.is("GetBattlefieldStatus(1) == 'queued'"));
    ui.run("statusEvents = 0 local f = CreateFrame('Frame') f:RegisterEvent('UPDATE_BATTLEFIELD_STATUS') "
           "f:SetScript('OnEvent', function() statusEvents = statusEvents + 1 end)");
    ui.run("AcceptBattlefieldPort(1, nil)");
    REQUIRE(s.sent().of(Opcode::CMSG_BATTLEFIELD_PORT).size() == 1);
    CHECK(ui.is("GetBattlefieldStatus(1) == 'queued'"));
    CHECK(ui.is("statusEvents == 0"));
    wowee::network::Packet none(wowee::game::wireOpcode(Opcode::SMSG_BATTLEFIELD_STATUS));
    none.writeUInt32(0);
    none.writeUInt64(0);
    s.receive(none);
    CHECK(ui.is("GetBattlefieldStatus(1) == 'none'"));
    CHECK(ui.is("statusEvents >= 1"));

    // An acceptance leaves the invitation as it is too.
    s.receive(bgStatus(0, kWarsongRecord, 2, 489));
    ui.run("AcceptBattlefieldPort(1, 1)");
    CHECK(ui.is("GetBattlefieldStatus(1) == 'confirm'"));
}

// T412 (FIXER Z): GetLatestThreeSenders (0x0056f3a0) names each letter as
// the inbox does (0x0056d960): an auction house by its AuctionHouse.dbc row,
// a creature or object by its cached name, a GM letter (paper 61) as
// GM_EMAIL_NAME - and nil when there is no name, never 'Auction House' or
// 'Unknown' in English. A slot with neither guid nor entry is skipped. Each
// letter of MSG_QUERY_NEXT_MAIL_TIME is 24 bytes (0x0056daf0); the last one
// was dropped for want of 28.
TEST_CASE("the minimap mail senders are named as the inbox names them (T412)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    wowee::network::Packet next(wowee::game::wireOpcode(Opcode::MSG_QUERY_NEXT_MAIL_TIME));
    next.writeFloat(0.0f);
    next.writeUInt32(3);
    const auto letter = [&next](uint64_t guid, uint32_t entry, uint32_t type, uint32_t paper) {
        next.writeUInt64(guid);
        next.writeUInt32(entry);
        next.writeUInt32(type);
        next.writeUInt32(paper);
        next.writeFloat(0.0f);
    };
    letter(0, 2, 2, 62);           // an auction house; no AuctionHouse.dbc here
    letter(0, 987654, 3, 41);      // a creature nobody has asked about
    letter(0x1234, 0, 0, 61);      // a GM's letter
    s.receive(next);
    CHECK(ui.is("select('#', GetLatestThreeSenders()) == 3"));
    CHECK(ui.is("select(1, GetLatestThreeSenders()) == nil"));
    CHECK(ui.is("select(2, GetLatestThreeSenders()) == nil"));
    CHECK(ui.is("select(3, GetLatestThreeSenders()) ~= nil"));
    CHECK(ui.is("select(3, GetLatestThreeSenders()) ~= 'Unknown'"));
}

// T410 (FIXER Z): a battleground row has no side on the wire; 12340 gives
// it one from the name cache (0x0054aa30: unknown player 0, else its race's
// faction group) and the faction tabs count only that side's rows. Both
// tabs listed every row.
TEST_CASE("the scoreboard's faction tabs split a battleground's rows (T410)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    wowee::network::Packet log(wowee::game::wireOpcode(Opcode::MSG_PVP_LOG_DATA));
    log.writeUInt8(0);    // a battleground
    log.writeUInt8(0);    // no winner yet
    log.writeUInt32(2);
    for (uint64_t guid : {0xF130000000000101ull, 0xF130000000000102ull}) {
        log.writeUInt64(guid);
        for (int k = 0; k < 6; ++k) log.writeUInt32(1);   // kb, hk, deaths, honour, damage, healing
        log.writeUInt32(0);   // no objectives
    }
    s.receive(log);
    ui.run("SetBattlefieldScoreFaction(nil)");
    REQUIRE(ui.is("GetNumBattlefieldScores() == 2"));
    CHECK(ui.is("select(6, GetBattlefieldScore(1)) == 0"));
    ui.run("SetBattlefieldScoreFaction(1)");
    CHECK(ui.is("GetNumBattlefieldScores() == 0"));
    ui.run("SetBattlefieldScoreFaction(0)");
    CHECK(ui.is("GetNumBattlefieldScores() == 2"));
    ui.run("SetBattlefieldScoreFaction(nil)");
}

// T411 (FIXER Z): several stacks go one at a time (0x0059f990 -> 0x0059ba80):
// AUCTION_MULTISELL_START and the first post; each auction the realm takes
// (SMSG_AUCTION_COMMAND_RESULT, 0x0059ffb0) raises AUCTION_MULTISELL_UPDATE
// done/total and sends the next; a refusal gives up (0x0059ab20:
// AUCTION_MULTISELL_FAILURE, the box let go). All stacks went at once and
// the bar ran to the end before any answer.
TEST_CASE("a multisell posts the next stack on the realm's answer (T411)", "[replay][interface][auction]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    s.handler().getInventoryHandler()->openAuctionHouse(0x77);
    carryForAuction(s, 0, 0x4000000000000C01ull, 2589, 20);
    carryForAuction(s, 1, 0x4000000000000C02ull, 2589, 20);
    carryForAuction(s, 2, 0x4000000000000C03ull, 2589, 20);
    ui.run("ClearCursor() PickupContainerItem(0, 1) ClickAuctionSellItemButton()");
    REQUIRE(ui.is("GetAuctionSellItemInfo() == 'Item 2589'"));
    ui.run("ms = '' local f = CreateFrame('Frame') "
           "for _, e in ipairs({'AUCTION_MULTISELL_START', 'AUCTION_MULTISELL_UPDATE', 'AUCTION_MULTISELL_FAILURE'}) "
           "do f:RegisterEvent(e) end "
           "f:SetScript('OnEvent', function(_, e, a, b) ms = ms .. e:sub(19) .. (a or '') .. '/' .. (b or '') .. ';' end)");
    const auto answer = [&s](uint32_t error) {
        wowee::network::Packet r(wowee::game::wireOpcode(Opcode::SMSG_AUCTION_COMMAND_RESULT));
        r.writeUInt32(1000);
        r.writeUInt32(0);   // a post
        r.writeUInt32(error);
        if (error == 1) r.writeUInt32(0);
        s.receive(r);
    };
    ui.run("StartAuction(100, 200, 1, 20, 3)");
    CHECK(s.sent().of(Opcode::CMSG_AUCTION_SELL_ITEM).size() == 1);
    CHECK(ui.is("ms == 'START3/;'"));
    CHECK(ui.is("GetAuctionSellItemInfo() == 'Item 2589'"));
    answer(0);
    CHECK(s.sent().of(Opcode::CMSG_AUCTION_SELL_ITEM).size() == 2);
    CHECK(ui.is("ms == 'START3/;UPDATE1/3;'"));
    // The second post refused (no money for the deposit): given up, nothing more.
    answer(3);
    CHECK(s.sent().of(Opcode::CMSG_AUCTION_SELL_ITEM).size() == 2);
    CHECK(ui.is("ms == 'START3/;UPDATE1/3;FAILURE/;'"));
    CHECK(ui.is("GetAuctionSellItemInfo() == nil"));
    answer(0);
    CHECK(s.sent().of(Opcode::CMSG_AUCTION_SELL_ITEM).size() == 2);

    // Two stacks taken both: two updates, the box let go after the last.
    ui.run("ClearCursor() PickupContainerItem(0, 1) ClickAuctionSellItemButton() ms = ''");
    REQUIRE(ui.is("GetAuctionSellItemInfo() == 'Item 2589'"));
    ui.run("StartAuction(100, 200, 1, 20, 2)");
    answer(0);
    CHECK(ui.is("GetAuctionSellItemInfo() == 'Item 2589'"));
    answer(0);
    CHECK(s.sent().of(Opcode::CMSG_AUCTION_SELL_ITEM).size() == 4);
    CHECK(ui.is("ms == 'START2/;UPDATE1/2;UPDATE2/2;'"));
    CHECK(ui.is("GetAuctionSellItemInfo() == nil"));
    ui.run("ClearCursor()");
}

// Y's open item (FIXER Z): AcceptBattlefieldPort's acceptance outside a
// battleground is refused before anything is sent (0x0054da10 -> 0x005216f0):
// UNIT_FLAG 0x800 is ERR_NOT_IN_COMBAT, the fatigue timer running down
// (scale below 0, 0x00513f20(0) +0xc) ERR_NOT_WHILE_FATIGUED; it answers nil.
// A refusal of the invitation is not checked.
TEST_CASE("AcceptBattlefieldPort refuses an acceptance the player cannot make (0x0054da10)",
          "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::UNIT_FIELD_FLAGS, 0x800}});
    Interface ui(s);
    s.receive(bgStatus(0, kWarsongRecord, 2, 489));
    ui.run("portErrors = {} local f = CreateFrame('Frame') f:RegisterEvent('UI_ERROR_MESSAGE') "
           "f:SetScript('OnEvent', function(_, _, a) portErrors[#portErrors + 1] = a end)");
    ui.run("portAnswer = AcceptBattlefieldPort(1, 1)");
    CHECK(ui.is("portAnswer == nil"));
    CHECK(ui.is("#portErrors == 1"));
    CHECK(s.sent().of(Opcode::CMSG_BATTLEFIELD_PORT).empty());

    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_FLAGS, 0}}));
    wowee::network::Packet fatigue(wowee::game::wireOpcode(Opcode::SMSG_START_MIRROR_TIMER));
    fatigue.writeUInt32(0);        // EXHAUSTION
    fatigue.writeUInt32(60000);
    fatigue.writeUInt32(60000);
    fatigue.writeUInt32(static_cast<uint32_t>(-1));   // running down
    fatigue.writeUInt8(0);
    fatigue.writeUInt32(0);
    s.receive(fatigue);
    ui.run("portAnswer = AcceptBattlefieldPort(1, 1)");
    CHECK(ui.is("portAnswer == nil"));
    CHECK(ui.is("#portErrors == 2"));
    CHECK(s.sent().of(Opcode::CMSG_BATTLEFIELD_PORT).empty());
    // The refusal of the invitation goes all the same.
    ui.run("portAnswer = AcceptBattlefieldPort(1, nil)");
    CHECK(ui.is("portAnswer == 1"));
    CHECK(s.sent().of(Opcode::CMSG_BATTLEFIELD_PORT).size() == 1);

    // LFGTeleport (0x00556bc0) asks the same timer by its scale too; the
    // value was asked, which is never below 0.
    ui.run("lfgAnswer = LFGTeleport(true)");
    CHECK(ui.is("lfgAnswer == false"));
    CHECK(s.sent().of(Opcode::CMSG_LFG_TELEPORT).empty());

    wowee::network::Packet stop(wowee::game::wireOpcode(Opcode::SMSG_STOP_MIRROR_TIMER));
    stop.writeUInt32(0);
    s.receive(stop);
    ui.run("portAnswer = AcceptBattlefieldPort(1, 1)");
    CHECK(ui.is("portAnswer == 1"));
    CHECK(s.sent().of(Opcode::CMSG_BATTLEFIELD_PORT).size() == 2);
}

// Arena team review (found by Z in code): GetArenaTeam asks for a team it
// has no record of (0x005a3860 -> 0x00680170, answer -> ARENA_TEAM_UPDATE
// via 0x005a28c0) and answers without stats (zero until they come); a
// change of the arena team fields asks for the rosters (0x006cdda0 ->
// 0x005a3700(1), once in 10 s per slot, 0x005a3600) and raises
// ARENA_TEAM_UPDATE; ArenaTeamRoster keeps the same 10 s; an arena team
// event lets each slot ask again with ARENA_TEAM_ROSTER_UPDATE(1)
// (0x006cc980 -> 0x005a28e0).
TEST_CASE("the own arena teams are asked for as 12340 asks", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100},
                   {UF::PLAYER_FIELD_ARENA_TEAM_INFO_1_1, 7}});
    Interface ui(s);
    ui.run("teamUpdates, rosterUpdates = 0, '' local f = CreateFrame('Frame') "
           "f:RegisterEvent('ARENA_TEAM_UPDATE') f:RegisterEvent('ARENA_TEAM_ROSTER_UPDATE') "
           "f:SetScript('OnEvent', function(_, e, a) if e == 'ARENA_TEAM_UPDATE' then teamUpdates = teamUpdates + 1 "
           "else rosterUpdates = rosterUpdates .. tostring(a) .. ';' end end)");
    CHECK(ui.is("GetArenaTeam(1) == nil"));
    CHECK(ui.is("GetArenaTeam(1) == nil"));
    REQUIRE(s.sent().of(Opcode::CMSG_ARENA_TEAM_QUERY).size() == 1);
    wowee::network::Packet answer(wowee::game::wireOpcode(Opcode::SMSG_ARENA_TEAM_QUERY_RESPONSE));
    answer.writeUInt32(7);
    answer.writeString("Die Sieben");
    answer.writeUInt32(2);
    for (int k = 0; k < 5; ++k) answer.writeUInt32(0);
    s.receive(answer);
    CHECK(ui.is("teamUpdates == 1"));
    CHECK(ui.is("GetArenaTeam(1) == 'Die Sieben'"));
    CHECK(ui.is("select(2, GetArenaTeam(1)) == 2 and select(3, GetArenaTeam(1)) == 0"));

    const uint16_t rating =
        static_cast<uint16_t>(wowee::game::fieldIndex(UF::PLAYER_FIELD_ARENA_TEAM_INFO_1_1) + 6);
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{rating, 1500}}));
    CHECK(ui.is("teamUpdates == 2"));
    CHECK(s.sent().of(Opcode::CMSG_ARENA_TEAM_ROSTER).size() == 1);
    ui.run("ArenaTeamRoster(1) ArenaTeamRoster(1) ArenaTeamRoster(2)");
    CHECK(s.sent().of(Opcode::CMSG_ARENA_TEAM_ROSTER).size() == 1);

    wowee::network::Packet event(wowee::game::wireOpcode(Opcode::SMSG_ARENA_TEAM_EVENT));
    event.writeUInt8(4);
    event.writeUInt8(2);
    event.writeString("Egon");
    event.writeString("Die Sieben");
    s.receive(event);
    CHECK(ui.is("rosterUpdates == '1;1;1;'"));
    ui.run("ArenaTeamRoster(1)");
    CHECK(s.sent().of(Opcode::CMSG_ARENA_TEAM_ROSTER).size() == 2);

    // A slot without a team is still sent, as team 0 (0x00515dd0).
    s.setConnected(true);
    ui.run("ArenaTeamLeave(3)");
    const auto leaves = s.sent().of(Opcode::CMSG_ARENA_TEAM_LEAVE);
    REQUIRE(leaves.size() == 1);
    auto leave = leaves.back();
    CHECK(leave.readUInt32() == 0);
}

// T413 (FIXER Z): every world load clears the battleground score state
// (0x006e7f50 -> 0x00528010 -> 0x0054ca50): no rows, no winner, both
// factions. After the realm's port home the scoreboard reopened itself from
// the old winner (WorldStateScoreFrame_Update) and asked for scores each frame.
TEST_CASE("the battleground score is gone after a world load (T413)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    const auto board = [](bool won) {
        wowee::network::Packet log(wowee::game::wireOpcode(Opcode::MSG_PVP_LOG_DATA));
        log.writeUInt8(0);
        log.writeUInt8(won ? 1 : 0);
        if (won) log.writeUInt8(1);
        log.writeUInt32(1);
        log.writeUInt64(0xF130000000000201ull);
        for (int k = 0; k < 6; ++k) log.writeUInt32(1);
        log.writeUInt32(0);
        return log;
    };
    s.receive(board(true));
    REQUIRE(ui.is("GetBattlefieldWinner() == 1"));
    REQUIRE(ui.is("GetNumBattlefieldScores() == 1"));
    ui.run("SetBattlefieldScoreFaction(1)");
    s.handler().setWorldEntryCallback([](uint32_t, float, float, float, bool) {});
    wowee::network::Packet world(wowee::game::wireOpcode(Opcode::SMSG_NEW_WORLD));
    world.writeUInt32(1);
    for (const float v : {-1431.29f, -68.57f, 156.87f, 0.4f}) world.writeFloat(v);
    s.receive(world);
    s.handler().setWorldEntryCallback({});
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    CHECK(ui.is("GetBattlefieldWinner() == nil"));
    CHECK(ui.is("GetNumBattlefieldScores() == 0"));
    // The filter is both again: a new board's row is counted.
    s.receive(board(false));
    CHECK(ui.is("GetNumBattlefieldScores() == 1"));
}

// Arena team review (found by Z in code): the roster's order is a list of the
// seven columns (0x005a2e80): the clicked one to the front with its own
// direction, the front one clicked again turns round, ties go to the next
// column; names start it (0x005a40e0); the column names compare without case
// (0x005a3490) and each roster with rows is announced after a sort.
TEST_CASE("the arena roster sorts by a column list as 0x005a2e80", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100},
                   {UF::PLAYER_FIELD_ARENA_TEAM_INFO_1_1, 7}});
    Interface ui(s);
    wowee::network::Packet roster(wowee::game::wireOpcode(Opcode::SMSG_ARENA_TEAM_ROSTER));
    roster.writeUInt32(7);
    roster.writeUInt8(0);
    roster.writeUInt32(3);
    roster.writeUInt32(2);
    const auto member = [&roster](uint64_t guid, const char* name, uint32_t played, uint32_t rating) {
        roster.writeUInt64(guid);
        roster.writeUInt8(1);
        roster.writeString(name);
        roster.writeUInt32(1);
        roster.writeUInt8(80);
        roster.writeUInt8(9);
        for (const uint32_t v : {played, 0u, played, 0u, rating}) roster.writeUInt32(v);
    };
    member(0x101, "Bert", 5, 1500);
    member(0x102, "Anna", 5, 1600);
    member(0x103, "Carl", 9, 1400);
    s.receive(roster);
    const auto order = [&ui]() {
        ui.run("order = '' for i = 1, 3 do order = order .. (GetArenaTeamRosterInfo(1, i) or '-') .. ';' end");
        return ui.is("order");
    };
    ui.run("rosterEvents = 0 local f = CreateFrame('Frame') f:RegisterEvent('ARENA_TEAM_ROSTER_UPDATE') "
           "f:SetScript('OnEvent', function() rosterEvents = rosterEvents + 1 end)");
    order();
    CHECK(ui.is("order == 'Anna;Bert;Carl;'"));
    ui.run("SortArenaTeamRoster('played')");
    order();
    CHECK(ui.is("order == 'Carl;Anna;Bert;'"));
    CHECK(ui.is("rosterEvents == 1"));
    ui.run("SortArenaTeamRoster('PLAYED')");
    order();
    CHECK(ui.is("order == 'Anna;Bert;Carl;'"));
    ui.run("SortArenaTeamRoster('rating')");
    order();
    CHECK(ui.is("order == 'Anna;Bert;Carl;'"));
    ui.run("SortArenaTeamRoster('rating')");
    order();
    CHECK(ui.is("order == 'Carl;Bert;Anna;'"));
}

// Guild bank review (found by Z in code): guild event 0x10 gives the tab its
// new name and icon before GUILDBANK_UPDATE_TABS (0x006d92d0 -> 0x005a4220);
// the tab kept its old name until the next full list.
TEST_CASE("a renamed guild bank tab shows its new name at once (0x005a4220)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.handler().setGuildBankTabCount(1);
    wowee::network::Packet event(wowee::game::wireOpcode(Opcode::SMSG_GUILD_EVENT));
    event.writeUInt8(0x10);
    event.writeUInt8(3);
    event.writeString("0");
    event.writeString("Kräuter");
    event.writeString("INV_Misc_Herb_01");
    s.receive(event);
    CHECK(ui.is("GetGuildBankTabInfo(1) == 'Kräuter'"));
    CHECK(ui.is("select(2, GetGuildBankTabInfo(1)):find('INV_Misc_Herb_01') ~= nil"));
}

// T433: 0x006d92d0 case 10 writes the event's rank name (string 1) at its
// rank (string 0) into the guild's cache entry, and the rank count
// (string 2); the roster carries no names, so an added or renamed rank kept
// none or its old one.
TEST_CASE("a guild rank event names the rank (0x006d92d0 case 10)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    auto& gh = s.handler();
    wowee::game::Character self;
    self.guid = Session::kPlayerGuid;
    self.guildId = 5;
    gh.charactersRef().push_back(self);
    gh.setActiveCharacterGuid(self.guid);
    wowee::network::Packet query(wowee::game::wireOpcode(Opcode::SMSG_GUILD_QUERY_RESPONSE));
    query.writeUInt32(5);
    query.writeString("Testgilde");
    const char* ranks[10] = {"Meister", "Offizier", "Veteran", "Mitglied", "Anfänger", "", "", "", "", ""};
    for (const char* r : ranks) query.writeString(r);
    for (int i = 0; i < 5; ++i) query.writeUInt32(0);
    query.writeUInt32(5);
    s.receive(query);
    REQUIRE(ui.is("GuildControlGetRankName(5) == 'Anfänger'"));
    const auto rankEvent = [&s](const char* rank, const char* name, const char* count) {
        wowee::network::Packet event(wowee::game::wireOpcode(Opcode::SMSG_GUILD_EVENT));
        event.writeUInt8(10);
        event.writeUInt8(3);
        event.writeString(rank);
        event.writeString(name);
        event.writeString(count);
        s.receive(event);
    };
    rankEvent("5", "TestRang", "6");
    CHECK(ui.is("GuildControlGetRankName(6) == 'TestRang'"));
    rankEvent("4", "Anfaenger2", "6");
    CHECK(ui.is("GuildControlGetRankName(5) == 'Anfaenger2'"));
    CHECK(ui.is("GuildControlGetRankName(1) == 'Meister'"));
    // T437: a roster with four ranks (a rank deleted) - 0x005ccc90 answers ""
    // past the roster's rank count (DAT_00c22ab8), not the old name still cached.
    wowee::network::Packet roster(wowee::game::wireOpcode(Opcode::SMSG_GUILD_ROSTER));
    roster.writeUInt32(0);
    roster.writeString("");
    roster.writeString("");
    roster.writeUInt32(4);
    for (int r = 0; r < 4; ++r)
        for (int i = 0; i < 14; ++i) roster.writeUInt32(0);
    s.receive(roster);
    REQUIRE(ui.is("GuildControlGetNumRanks() == 4"));
    CHECK(ui.is("GuildControlGetRankName(4) == 'Mitglied'"));
    CHECK(ui.is("GuildControlGetRankName(5) == ''"));
    CHECK(ui.is("GuildControlGetRankName(6) == ''"));
    CHECK(ui.is("GuildControlGetRankName(1.4) == 'Meister'"));
}

// T414 (FIXER AA, found in code beside it): GetMapLandmarkInfo for an index
// with no landmark ends in 0x00545a30's tail - seven values, nil, nil, 0, 0,
// 0, nil, nil - not a single nil.
TEST_CASE("GetMapLandmarkInfo answers seven values past the last landmark (0x00545a30)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    CHECK(ui.is("select('#', GetMapLandmarkInfo(999)) == 7"));
    CHECK(ui.is("select('#', GetMapLandmarkInfo(0)) == 7"));
    CHECK(ui.is("select(3, GetMapLandmarkInfo(999)) == 0"));
    CHECK(ui.is("select(5, GetMapLandmarkInfo(999)) == 0"));
    CHECK(ui.is("select(2, GetMapLandmarkInfo(999)) == nil"));
    CHECK(ui.is("select(7, GetMapLandmarkInfo(999)) == nil"));
}

namespace {
// A guild bank at banker 0x77 for rank 0: tab 1 deposit and ten a day, tab
// 2 view only, tab 3 deposit and none a day. Tab 1 holds 5 and 18 of 2589
// (a stack of twenty) in slots 1 and 2 and one 4306 in slot 3.
void openTestGuildBank(Session& s) {
    auto& gh = s.handler();
    wowee::game::Character self;
    self.guid = Session::kPlayerGuid;
    self.guildId = 5;
    gh.charactersRef().push_back(self);
    gh.setActiveCharacterGuid(self.guid);
    wowee::network::Packet perms(wowee::game::wireOpcode(Opcode::MSG_GUILD_PERMISSIONS));
    perms.writeUInt32(0);           // rank
    perms.writeUInt32(0);           // rights
    perms.writeUInt32(0);           // gold a day
    perms.writeUInt8(3);            // tabs bought
    const uint32_t tabs[6][2] = {{0x3, 10}, {0x1, 10}, {0x3, 0}, {0, 0}, {0, 0}, {0, 0}};
    for (const auto& t : tabs) {
        perms.writeUInt32(t[0]);
        perms.writeUInt32(t[1]);
    }
    s.receive(perms);
    auto& info = gh.itemInfoCacheRef()[2589];
    info.entry = 2589;
    info.valid = true;
    info.maxStack = 20;
    gh.openGuildBank(0x77);
    wowee::network::Packet list(wowee::game::wireOpcode(Opcode::SMSG_GUILD_BANK_LIST));
    list.writeUInt64(0);            // money
    list.writeUInt8(0);             // tab
    list.writeUInt32(10);           // withdrawals left
    list.writeUInt8(1);             // full update, with the tab names
    list.writeUInt8(3);
    for (int i = 0; i < 3; ++i) {
        list.writeString("Tab");
        list.writeString("INV_Misc_QuestionMark");
    }
    const uint32_t slots[3][3] = {{0, 2589, 5}, {1, 2589, 18}, {2, 4306, 1}};
    list.writeUInt8(3);
    for (const auto& sl : slots) {
        list.writeUInt8(static_cast<uint8_t>(sl[0]));
        list.writeUInt32(sl[1]);
        list.writeUInt32(0);        // flags
        list.writeUInt32(0);        // random property
        list.writeUInt32(sl[2]);    // stack
        list.writeUInt32(0);        // enchantment
        list.writeUInt8(0);         // charges
        list.writeUInt8(0);         // sockets
    }
    s.receive(list);
}

struct BankMove {
    uint8_t bankToBank, destTab, destSlot;
    uint32_t destEntry;
    uint8_t srcTab, srcSlot;
    uint32_t srcEntry;
    uint8_t zero;
    uint32_t amount;
};
BankMove readBankMove(wowee::network::Packet p) {
    BankMove m{};
    p.readUInt64();
    m.bankToBank = p.readUInt8();
    m.destTab = p.readUInt8();
    m.destSlot = p.readUInt8();
    m.destEntry = p.readUInt32();
    m.srcTab = p.readUInt8();
    m.srcSlot = p.readUInt8();
    m.srcEntry = p.readUInt32();
    m.zero = p.readUInt8();
    m.amount = p.readUInt32();
    return m;
}
} // namespace

// Z's open guild bank item (FIXER AA): a guild bank item put down on another
// slot of the bank is moved there (0x005a6200 -> 0x005a5410) - bankToBank 1,
// onto the same item what its stack has room for, onto an empty slot the
// whole stack; a tab without the deposit right 0x02 answers
// ERR_GUILD_PERMISSIONS and the item stays held. WoWee dropped the cursor
// and sent nothing.
TEST_CASE("a guild bank item is moved inside the bank (0x005a5410)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    openTestGuildBank(s);
    ui.run("bagslots = 0; local f = CreateFrame('Frame'); f:RegisterEvent('GUILDBANKBAGSLOTS_CHANGED');"
           " f:SetScript('OnEvent', function() bagslots = bagslots + 1 end)");
    const auto moves = [&] { return s.sent().of(Opcode::CMSG_GUILD_BANK_SWAP_ITEMS); };
    ui.run("PickupGuildBankItem(1, 1)");
    CHECK(ui.is("CursorHasItem() == 1"));
    ui.run("PickupGuildBankItem(1, 2)");
    REQUIRE(moves().size() == 1);
    {
        const BankMove m = readBankMove(moves().back());
        CHECK(m.bankToBank == 1);
        CHECK(m.destTab == 0);
        CHECK(m.destSlot == 1);
        CHECK(m.destEntry == 2589);
        CHECK(m.srcTab == 0);
        CHECK(m.srcSlot == 0);
        CHECK(m.srcEntry == 2589);
        CHECK(m.zero == 0);
        CHECK(m.amount == 2);       // twenty less the eighteen there
    }
    CHECK(ui.is("CursorHasItem() == nil"));
    CHECK(ui.is("bagslots == 1"));
    // Onto an empty slot: the whole of an item whose stack is not known.
    ui.run("PickupGuildBankItem(1, 3)");
    ui.run("PickupGuildBankItem(1, 50)");
    REQUIRE(moves().size() == 2);
    {
        const BankMove m = readBankMove(moves().back());
        CHECK(m.destSlot == 49);
        CHECK(m.destEntry == 0);
        CHECK(m.srcSlot == 2);
        CHECK(m.srcEntry == 4306);
        CHECK(m.amount == 0);
    }
    // A view-only tab: refused, the item still held.
    ui.run("PickupGuildBankItem(1, 2)");
    ui.run("PickupGuildBankItem(2, 1)");
    CHECK(moves().size() == 2);
    CHECK(ui.is("CursorHasItem() == 1"));
    ui.run("ClearCursor()");
    // Out of range: nothing at all.
    ui.run("PickupGuildBankItem(7, 1)");
    ui.run("PickupGuildBankItem(1, 99)");
    CHECK(ui.is("CursorHasItem() == nil"));
}

// Z's open guild bank item (FIXER AA): AutoStoreGuildBankItem (0x005a63f0)
// sends autoStore 1 with the slot's entry and stack - WoWee sent autoStore 0
// to bag 0 slot 0, which the realm refuses - then GUILDBANKBAGSLOTS_CHANGED;
// a tab with no items a day for the rank answers ERR_GUILD_PERMISSIONS.
TEST_CASE("AutoStoreGuildBankItem sends the slot to the bags (0x005a63f0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    openTestGuildBank(s);
    ui.run("bagslots = 0; local f = CreateFrame('Frame'); f:RegisterEvent('GUILDBANKBAGSLOTS_CHANGED');"
           " f:SetScript('OnEvent', function() bagslots = bagslots + 1 end)");
    ui.run("AutoStoreGuildBankItem(1, 1)");
    const auto sent = s.sent().of(Opcode::CMSG_GUILD_BANK_SWAP_ITEMS);
    REQUIRE(sent.size() == 1);
    auto p = sent.back();
    CHECK(p.readUInt64() == 0x77);
    CHECK(p.readUInt8() == 0);      // bankToBank
    CHECK(p.readUInt8() == 0);      // tab
    CHECK(p.readUInt8() == 0);      // slot
    CHECK(p.readUInt32() == 2589);
    CHECK(p.readUInt8() == 1);      // autoStore
    CHECK(p.readUInt32() == 5);     // the stack
    CHECK(p.readUInt8() == 1);      // toChar
    CHECK(p.readUInt32() == 0);
    CHECK(ui.is("bagslots == 1"));
    // An empty slot sends nothing.
    ui.run("AutoStoreGuildBankItem(1, 60)");
    CHECK(s.sent().of(Opcode::CMSG_GUILD_BANK_SWAP_ITEMS).size() == 1);
}

// T411's open part (FIXER AA): the bag items a multisell draws from are
// locked from its start to its end or give-up (0x0059aaa0 -> 0x00513740,
// 0x0059aae0); WoWee locked the sell box's item alone.
TEST_CASE("a multisell's source items are locked while it runs (T411, 0x0059aaa0)", "[replay][interface][auction]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    s.handler().getInventoryHandler()->openAuctionHouse(0x77);
    carryForAuction(s, 0, 0x4000000000000C01ull, 2589, 20);
    carryForAuction(s, 1, 0x4000000000000C02ull, 2589, 20);
    carryForAuction(s, 2, 0x4000000000000C03ull, 2589, 20);
    ui.run("ClearCursor() PickupContainerItem(0, 1) ClickAuctionSellItemButton()");
    REQUIRE(ui.is("GetAuctionSellItemInfo() == 'Item 2589'"));
    CHECK(ui.is("select(3, GetContainerItemInfo(0, 2)) == nil"));
    ui.run("StartAuction(100, 200, 1, 20, 3)");
    // The first post took the last source; the one left is locked.
    CHECK(ui.is("select(3, GetContainerItemInfo(0, 2)) == 1"));
    wowee::network::Packet r(wowee::game::wireOpcode(Opcode::SMSG_AUCTION_COMMAND_RESULT));
    r.writeUInt32(1000);
    r.writeUInt32(0);
    r.writeUInt32(3);   // refused: given up
    s.receive(r);
    CHECK(ui.is("select(3, GetContainerItemInfo(0, 2)) == nil"));
}

// Trade skill review (found by AA in code): GetTradeskillRepeatCount
// (0x005da270) answers the count left only for the selected recipe
// (0x00c235a8), else 1; DoTradeSkill (0x005dac00) wants a number first.
TEST_CASE("the trade skill repeat count is the selected recipe's (0x005da270)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.handler().startCraftQueue(2329, 5);
    REQUIRE(s.handler().getCraftQueueRemaining() == 5);
    CHECK(ui.is("GetTradeskillRepeatCount() == 1"));   // no recipe selected
    CHECK(ui.is("select(2, pcall(DoTradeSkill)):find('Usage: DoTradeSkill%(index %[, repeat%]%)') ~= nil"));
}

// T415 (FIXER AA): SetBattlefieldScoreFaction (0x0054c120) and
// SortBattlefieldScoreData (0x0054de00 -> 0x0054cee0) end in 0x0054aa30 -
// the board sorted over the 17-key list (0x0054a740, killing blows first
// from the first board on) and UPDATE_BATTLEFIELD_SCORE, which redraws the
// rows. Neither raised the event; the rows kept the packet's order until a
// header was clicked, one key only, and 'class'/'statN' did not sort.
TEST_CASE("the scoreboard is sorted and announced as 12340 does (T415, 0x0054aa30)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("scores = 0; local f = CreateFrame('Frame'); f:RegisterEvent('UPDATE_BATTLEFIELD_SCORE');"
           " f:SetScript('OnEvent', function() scores = scores + 1 end)");
    struct Row { uint64_t guid; const char* name; uint8_t cls; uint32_t kb; uint32_t flags; };
    const Row rows[3] = {{0xF130000000000101ull, "Anna", 1, 1, 2},
                         {0xF130000000000102ull, "Bert", 8, 3, 0},
                         {0xF130000000000103ull, "Carl", 4, 2, 1}};
    wowee::network::Packet log(wowee::game::wireOpcode(Opcode::MSG_PVP_LOG_DATA));
    log.writeUInt8(0);    // a battleground
    log.writeUInt8(0);    // no winner yet
    log.writeUInt32(3);
    for (const auto& r : rows) {
        log.writeUInt64(r.guid);
        log.writeUInt32(r.kb);
        for (int k = 0; k < 5; ++k) log.writeUInt32(1);   // hk, deaths, honour, damage, healing
        log.writeUInt32(1);   // one objective
        log.writeUInt32(r.flags);
    }
    s.receive(log);
    for (const auto& r : rows) {
        wowee::network::Packet answer(wowee::game::wireOpcode(Opcode::SMSG_NAME_QUERY_RESPONSE));
        answer.writePackedGuid(r.guid);
        answer.writeUInt8(0);
        answer.writeString(r.name);
        answer.writeString("");
        answer.writeUInt8(2);     // race
        answer.writeUInt8(0);     // gender
        answer.writeUInt8(r.cls);
        answer.writeUInt8(0);
        s.receive(answer);
    }
    REQUIRE(ui.is("scores == 1"));
    const auto order = [&] {
        ui.run("order = '' for i = 1, GetNumBattlefieldScores() do order = order .. GetBattlefieldScore(i) .. ';' end");
    };
    order();
    CHECK(ui.is("order == 'Bert;Carl;Anna;'"));   // killing blows, most first
    ui.run("SetBattlefieldScoreFaction(nil)");
    CHECK(ui.is("scores == 2"));
    ui.run("SetBattlefieldScoreFaction(5)");      // ignored, nothing raised
    CHECK(ui.is("scores == 2"));
    ui.run("SortBattlefieldScoreData('kills')");  // at the front: turned round
    CHECK(ui.is("scores == 3"));
    order();
    CHECK(ui.is("order == 'Anna;Carl;Bert;'"));
    ui.run("SortBattlefieldScoreData('CLASS')");  // mage 8, rogue 4, warrior 1: the highest first
    order();
    CHECK(ui.is("order == 'Bert;Carl;Anna;'"));
    ui.run("SortBattlefieldScoreData('stat1')");
    order();
    CHECK(ui.is("order == 'Anna;Carl;Bert;'"));
    ui.run("SortBattlefieldScoreData('stat9')");  // past stat8: nothing at all
    CHECK(ui.is("scores == 5"));
    ui.run("SortBattlefieldScoreData('name')");
    order();
    CHECK(ui.is("order == 'Anna;Bert;Carl;'"));
    ui.run("SortBattlefieldScoreData('name')");
    order();
    CHECK(ui.is("order == 'Carl;Bert;Anna;'"));
    // Back to killing blows (V536 b, 0x0054cee0): the moved key's direction
    // goes into the front slot before the shift, so kills comes with the 0
    // that 'class' left on it (most first again, its earlier turn lost) and
    // the turned 'name' is pushed to the second place carrying that 0.
    ui.run("SortBattlefieldScoreData('bogus')");
    order();
    CHECK(ui.is("order == 'Bert;Carl;Anna;'"));
    ui.run("SortBattlefieldScoreData('name')");
    order();
    CHECK(ui.is("order == 'Anna;Bert;Carl;'"));
}

// AB's open item (FIXER AC): GetNextAchievement (0x005b3ef0) walks the
// Achievement rows in the file's order and answers the first whose
// Supercedes (field 3) is the id - the earlier of two rows following one
// id, and for 0 the first row that follows nothing. WoWee kept a map whose
// later row won and answered nothing for 0.
TEST_CASE("GetNextAchievement answers the first row that follows the id (0x005b3ef0)", "[replay][interface]") {
    using wowee::game::AchievementRow;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    wowee::game::GameHandlerTestAccess::setAchievementRows(s.handler(), {{.id = 6, .previous = 0},
                                                             {.id = 7, .previous = 6},
                                                             {.id = 1200, .previous = 6},
                                                             {.id = 8, .previous = 7}});
    CHECK(ui.is("GetNextAchievement(6) == 7"));
    CHECK(ui.is("select('#', GetNextAchievement(6)) == 1"));
    CHECK(ui.is("GetNextAchievement(7.9) == 8"));
    CHECK(ui.is("GetNextAchievement(0) == 6"));
    CHECK(ui.is("select('#', GetNextAchievement(8)) == 0"));
    CHECK(ui.is("select(2, pcall(GetNextAchievement)):find('Usage: GetNextAchievement') ~= nil"));
}

// Achievement review (found by AA in code): GetAchievementInfoFromCriteria
// (0x005b1720) wants a number - the usage line, not a silent nil.
TEST_CASE("GetAchievementInfoFromCriteria wants a number (0x005b1720)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    CHECK(ui.is("select(2, pcall(GetAchievementInfoFromCriteria)):find('Usage: GetAchievementInfoFromCriteria%(criteriaID%)') ~= nil"));
    CHECK(ui.is("select('#', GetAchievementInfoFromCriteria(987654)) == 0"));
}

// Talent review (found by AA in code): LearnTalent (0x005c80e0) wants two
// numbers - the usage line - and sends nothing for a talent it cannot find.
TEST_CASE("LearnTalent wants a tab and an index (0x005c80e0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    CHECK(ui.is("select(2, pcall(LearnTalent, 1)):find('Usage: LearnTalent%(tabIndex, talentIndex%[, isPet%]%)') ~= nil"));
    ui.run("LearnTalent(1, 1)");
    CHECK(s.sent().of(Opcode::CMSG_LEARN_TALENT).empty());
}

// Glyph review (found by AA in code): RemoveGlyphFromSocket (0x005b7660)
// sends CMSG_REMOVE_GLYPH only for a socket holding a glyph
// (PLAYER_FIELD_GLYPHS, +0x1230); an empty one sent it too.
TEST_CASE("RemoveGlyphFromSocket asks only for a socket with a glyph (0x005b7660)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::PLAYER_FIELD_GLYPHS_1, 165}});
    Interface ui(s);
    ui.run("RemoveGlyphFromSocket(2)");
    CHECK(s.sent().of(Opcode::CMSG_REMOVE_GLYPH).empty());
    ui.run("RemoveGlyphFromSocket(1)");
    REQUIRE(s.sent().of(Opcode::CMSG_REMOVE_GLYPH).size() == 1);
    CHECK(s.sent().of(Opcode::CMSG_REMOVE_GLYPH).back().readUInt32() == 0);
}

// Z's open guild bank items (FIXER AA): opening sends CMSG_GUILD_BANKER_ACTIVATE
// alone with the first tab's full-list flag (0x00c13598); QueryGuildBankTab
// (0x005a5ce0 -> 0x005a5800) takes tabs 1..6 and sends each tab's own flag -
// cleared by a list of it (0x005a7250); QueryGuildBankText (0x005a5940) asks
// once while the text is stale, else answers GUILDBANK_UPDATE_TEXT at once.
TEST_CASE("the guild bank asks for a tab's list and text as 12340 asks (0x005a5800, 0x005a5940)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    openTestGuildBank(s);
    const auto sent = [&](Opcode op) { return s.sent().of(op); };
    REQUIRE(sent(Opcode::CMSG_GUILD_BANKER_ACTIVATE).size() == 1);
    {
        auto p = sent(Opcode::CMSG_GUILD_BANKER_ACTIVATE).back();
        p.readUInt64();
        CHECK(p.readUInt8() == 1);
    }
    CHECK(sent(Opcode::CMSG_GUILD_BANK_QUERY_TAB).empty());
    const auto lastFull = [&] {
        auto p = sent(Opcode::CMSG_GUILD_BANK_QUERY_TAB).back();
        p.readUInt64();
        p.readUInt8();
        return p.readUInt8();
    };
    ui.run("QueryGuildBankTab(1)");
    REQUIRE(sent(Opcode::CMSG_GUILD_BANK_QUERY_TAB).size() == 1);
    CHECK(lastFull() == 0);     // tab 1's list came
    ui.run("QueryGuildBankTab(2)");
    REQUIRE(sent(Opcode::CMSG_GUILD_BANK_QUERY_TAB).size() == 2);
    CHECK(lastFull() == 1);
    ui.run("QueryGuildBankTab(7)");
    CHECK(sent(Opcode::CMSG_GUILD_BANK_QUERY_TAB).size() == 2);
    ui.run("texts = 0; local f = CreateFrame('Frame'); f:RegisterEvent('GUILDBANK_UPDATE_TEXT');"
           " f:SetScript('OnEvent', function(_, _, tab) texts = texts + 1; textTab = tab end)");
    ui.run("QueryGuildBankText(1)");
    CHECK(sent(Opcode::MSG_QUERY_GUILD_BANK_TEXT).size() == 1);
    CHECK(ui.is("texts == 0"));
    ui.run("QueryGuildBankText(1)");
    CHECK(sent(Opcode::MSG_QUERY_GUILD_BANK_TEXT).size() == 1);
    CHECK(ui.is("texts == 1 and textTab == 1"));
    // SetGuildBankText keeps 2047 characters (0x007e1830 with 0x800) and asks
    // only for a bought tab (0x005a5c40).
    ui.run("SetGuildBankText(1, string.rep('a', 3000))");
    REQUIRE(sent(Opcode::CMSG_SET_GUILD_BANK_TEXT).size() == 1);
    {
        auto p = sent(Opcode::CMSG_SET_GUILD_BANK_TEXT).back();
        CHECK(p.readUInt8() == 0);
        CHECK(p.readString().size() == 2047);
    }
    ui.run("SetGuildBankText(5, 'x')");
    CHECK(sent(Opcode::CMSG_SET_GUILD_BANK_TEXT).size() == 1);
}

// T417 (FIXER AA): SMSG_AUCTION_COMMAND_RESULT raises no PLAYER_MONEY
// (0x0059ffb0); the money event comes with the coinage field alone.
TEST_CASE("an auction command result raises no PLAYER_MONEY (T417, 0x0059ffb0)", "[replay][interface][auction]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    s.handler().getInventoryHandler()->openAuctionHouse(0x77);
    ui.run("money = 0; local f = CreateFrame('Frame'); f:RegisterEvent('PLAYER_MONEY');"
           " f:SetScript('OnEvent', function() money = money + 1 end)");
    wowee::network::Packet r(wowee::game::wireOpcode(Opcode::SMSG_AUCTION_COMMAND_RESULT));
    r.writeUInt32(1000);
    r.writeUInt32(0);   // a post
    r.writeUInt32(0);   // taken
    s.receive(r);
    CHECK(ui.is("money == 0"));
}

// T418 (FIXER AB): SMSG_LFG_QUEUE_STATUS (0x0055bdc0 case 0x365) raises
// LFG_QUEUE_STATUS_UPDATE alone and leaves the queue state to the update
// packets; its first wait is the player's own, the second the average, and
// the time queued becomes a moment when it comes. GetLFGQueueStats
// (0x00553f90) answers nothing unless the server's record says joined and
// queued; GetLFGInfoServer (0x00552f30) reads that record.
TEST_CASE("the dungeon finder queue status is read as 12340 reads it (T418, 0x0055bdc0)", "[replay][interface][lfg]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("seen = {} local f = CreateFrame('Frame') f:RegisterEvent('LFG_UPDATE') "
           "f:RegisterEvent('LFG_QUEUE_STATUS_UPDATE') "
           "f:SetScript('OnEvent', function(self, e) seen[#seen + 1] = e end)");
    const auto status = [&](uint32_t queuedFor) {
        wowee::network::Packet q(wowee::game::wireOpcode(Opcode::SMSG_LFG_QUEUE_STATUS));
        q.writeUInt32(261);   // dungeon
        q.writeUInt32(111);   // the player's own wait
        q.writeUInt32(222);   // the average
        q.writeUInt32(static_cast<uint32_t>(-1));   // tanks: not known
        q.writeUInt32(44);
        q.writeUInt32(55);
        q.writeUInt8(0); q.writeUInt8(0); q.writeUInt8(0);   // nobody needed
        q.writeUInt32(queuedFor);
        s.receive(q);
    };
    status(60);
    auto& gh = s.handler();
    CHECK(gh.getLfgMyWaitSec() == 111);
    CHECK(gh.getLfgAvgWaitSec() == 222);
    CHECK(gh.getLfgWaitTank() == -1);
    CHECK(gh.getLfgNeedDps() == 1);
    CHECK(ui.is("#seen == 1 and seen[1] == 'LFG_QUEUE_STATUS_UPDATE'"));
    CHECK(ui.is("select('#', GetLFGQueueStats()) == 0"));   // the record says nothing yet
    CHECK(ui.is("select(3, GetLFGInfoServer()) == false"));

    // SMSG_LFG_UPDATE_PLAYER: joined (extra info) and queued, one dungeon.
    wowee::network::Packet u(wowee::game::wireOpcode(Opcode::SMSG_LFG_UPDATE_PLAYER));
    u.writeUInt8(wowee::game::lfg_update_type::kAddedToQueue);
    u.writeUInt8(1);   // extra info
    u.writeUInt8(1);   // queued
    u.writeUInt8(0); u.writeUInt8(1);
    u.writeUInt8(1);
    u.writeUInt32(261u | (1u << 24));
    u.writeString("hi");
    s.receive(u);
    CHECK(ui.is("select('#', GetLFGQueueStats()) == 13"));
    CHECK(ui.is("select(5, GetLFGQueueStats()) == 1"));
    ui.run("local i = {GetLFGInfoServer()} info = i");
    CHECK(ui.is("info[2] == true and info[3] == true and info[4] == false and info[5] == true"));
    CHECK(ui.is("info[6] == 'hi' and info[7] == 1"));
    ui.run("seen = {}");
    status(60);
    CHECK(ui.is("#seen == 1 and seen[1] == 'LFG_QUEUE_STATUS_UPDATE'"));
    const double since = gh.getLfgQueuedSince();
    CHECK(since == Catch::Approx(wowee::core::appTimeSeconds() - 60.0).margin(1.0));
}

// ITEM_LOCKED / ITEM_UNLOCKED (FIXER AB, AA's open item): 12340 raises
// ITEM_LOCK_CHANGED and then ITEM_LOCKED or ITEM_UNLOCKED, with the same
// place, whenever an item's lock bit changes (0x00706fe0 / 0x00707020 ->
// 0x0060bfe0); ClearCursor unlocks the held item before its CURSOR_UPDATE
// (0x00519280 case 1 -> 0x00513770). WoWee raised neither event, and a held
// bag item put down with ClearCursor raised no lock change at all.
TEST_CASE("an item's lock is announced as 12340 announces it (0x0060bfe0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    carry(s, 1251, 0, 0);
    ui.run("seen = {} local f = CreateFrame('Frame') "
           "for _, e in ipairs({'ITEM_LOCK_CHANGED', 'ITEM_LOCKED', 'ITEM_UNLOCKED', 'CURSOR_UPDATE'}) do "
           "f:RegisterEvent(e) end "
           "f:SetScript('OnEvent', function(self, e, a, b) "
           "seen[#seen + 1] = e .. ' ' .. tostring(a) .. ' ' .. tostring(b) .. ' ' .. "
           "tostring(select(3, GetContainerItemInfo(0, 1))) end)");
    ui.run("PickupContainerItem(0, 1)");
    REQUIRE(ui.is("CursorHasItem() == 1"));
    CHECK(ui.is("seen[#seen - 1] == 'ITEM_LOCK_CHANGED 0 1 1' and seen[#seen] == 'ITEM_LOCKED 0 1 1'"));
    ui.run("seen = {} ClearCursor()");
    INFO(ui.is("seen[1] == 'ITEM_LOCK_CHANGED 0 1 nil'"));
    CHECK(ui.is("seen[1] == 'ITEM_LOCK_CHANGED 0 1 nil'"));
    CHECK(ui.is("seen[2] == 'ITEM_UNLOCKED 0 1 nil'"));
    CHECK(ui.is("seen[3] ~= nil and seen[3]:sub(1, 13) == 'CURSOR_UPDATE'"));
    CHECK(ui.is("#seen == 3"));
}

// AB's open item (FIXER AC): the GameUI wrappers around an item's lock and
// unlock (0x00513740 / 0x00513770) rewrite the action bars' usability right
// after it (0x005aa470), so ACTIONBAR_UPDATE_USABLE follows ITEM_LOCKED /
// ITEM_UNLOCKED in the same call when a slot's answer moved - here an
// equipment set whose item is picked up. WoWee only found it at its next
// tenth-of-a-second rescan.
TEST_CASE("a lock that changes an action's use announces it at once (0x005aa470)", "[replay][interface]") {
    using wowee::game::ActionBarSlot;
    constexpr uint64_t kCloth = 0x4000000000000C21ull;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    auto& gh = s.handler();
    carryForAuction(s, 0, kCloth, 2589, 5);
    wowee::network::Packet list(wowee::game::wireOpcode(Opcode::SMSG_EQUIPMENT_SET_LIST));
    list.writeUInt32(1);
    list.writePackedGuid(0x70);
    list.writeUInt32(0);
    list.writeString("Tank");
    list.writeString("INV_Shield_06");
    for (int slot = 0; slot < 19; ++slot) list.writePackedGuid(slot == 0 ? kCloth : 0);
    s.receive(list);
    gh.setActionBarSlot(3, ActionBarSlot::EQUIPMENT_SET, 0);
    ui.run("SetCVar('equipmentManager', '1')");
    REQUIRE(ui.is("IsUsableAction(4) == 1"));
    // Once round, so the remembered answers are this session's.
    ui.run("PickupContainerItem(0, 1) ClearCursor()");
    ui.run("seen = {} local f = CreateFrame('Frame') "
           "for _, e in ipairs({'ITEM_LOCKED', 'ITEM_UNLOCKED', 'ACTIONBAR_UPDATE_USABLE'}) do "
           "f:RegisterEvent(e) end "
           "f:SetScript('OnEvent', function(self, e) seen[#seen + 1] = e .. ' ' .. tostring(IsUsableAction(4)) end)");
    ui.run("PickupContainerItem(0, 1)");
    REQUIRE(ui.is("CursorHasItem() == 1"));
    CHECK(ui.is("seen[1] == 'ITEM_LOCKED nil'"));
    CHECK(ui.is("seen[2] == 'ACTIONBAR_UPDATE_USABLE nil'"));
    CHECK(ui.is("#seen == 2"));
    ui.run("seen = {} ClearCursor()");
    CHECK(ui.is("seen[1] == 'ITEM_UNLOCKED 1'"));
    CHECK(ui.is("seen[2] == 'ACTIONBAR_UPDATE_USABLE 1'"));
    CHECK(ui.is("#seen == 2"));
    // A lock that moves no answer says nothing more.
    gh.setActionBarSlot(3, ActionBarSlot::EMPTY, 0);
    ui.run("PickupContainerItem(0, 1) ClearCursor()");
    ui.run("seen = {} PickupContainerItem(0, 1)");
    CHECK(ui.is("#seen == 1"));
    ui.run("ClearCursor()");
}

// AB's open item (FIXER AC): an item put down by a move 12340 sends stays
// locked - 0x006df890 drops the cursor with ClearCursor(0, 1), no unlock -
// until the realm answers: the slot field that now holds it (0x006dedd0 ->
// 0x00513770), a stack change (0x00707720) or SMSG_INVENTORY_CHANGE_FAILURE
// naming it (0x006e2e90). WoWee unlocked it at the drop.
TEST_CASE("a moved item stays locked until the realm answers (0x006df890)", "[replay][interface]") {
    constexpr uint64_t kLinen = 0x4000000000000B01ull;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    carryForAuction(s, 0, kLinen, 2589, 5);
    ui.engine.dispatchOnUpdate(0.0f);
    ui.run("seen = {} local f = CreateFrame('Frame') "
           "for _, e in ipairs({'ITEM_LOCKED', 'ITEM_UNLOCKED'}) do f:RegisterEvent(e) end "
           "f:SetScript('OnEvent', function(self, e, a, b) seen[#seen + 1] = e .. ' ' .. a .. ' ' .. b end)");
    ui.run("PickupContainerItem(0, 1)");
    REQUIRE(ui.is("seen[1] == 'ITEM_LOCKED 0 1'"));
    ui.run("seen = {} PickupContainerItem(0, 3)");
    REQUIRE(s.sent().of(Opcode::CMSG_SWAP_ITEM).size() == 1);
    CHECK(ui.is("not CursorHasItem()"));
    CHECK(ui.is("#seen == 0"));                    // no unlock at the drop
    ui.engine.dispatchOnUpdate(0.0f);
    CHECK(ui.is("#seen == 0"));                    // nor a second lock
    CHECK(s.handler().getInventoryHandler()->itemAwaitsRealm(kLinen));
    // The realm moves it: slot 1 empty, slot 3 holds it.
    const auto pack = static_cast<uint16_t>(wowee::game::fieldIndex(UF::PLAYER_FIELD_PACK_SLOT_1));
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid,
                                      {{pack, 0u}, {static_cast<uint16_t>(pack + 1), 0u},
                                       {static_cast<uint16_t>(pack + 4), static_cast<uint32_t>(kLinen)},
                                       {static_cast<uint16_t>(pack + 5), static_cast<uint32_t>(kLinen >> 32)}}));
    CHECK_FALSE(s.handler().getInventoryHandler()->itemAwaitsRealm(kLinen));
    ui.engine.dispatchOnUpdate(0.0f);
    CHECK(ui.is("seen[1] == 'ITEM_UNLOCKED 0 3'"));
    // Refused: the failure's items are let go.
    ui.run("PickupContainerItem(0, 3)");
    ui.run("seen = {} PickupContainerItem(0, 5)");
    REQUIRE(s.handler().getInventoryHandler()->itemAwaitsRealm(kLinen));
    wowee::network::Packet failure(wowee::game::wireOpcode(Opcode::SMSG_INVENTORY_CHANGE_FAILURE));
    failure.writeUInt8(22);       // EQUIP_ERR_ITEM_LOCKED, any refusal
    failure.writeUInt64(kLinen);
    failure.writeUInt64(0);
    failure.writeUInt8(0);
    s.receive(failure);
    CHECK_FALSE(s.handler().getInventoryHandler()->itemAwaitsRealm(kLinen));
}

// T423 (FIXER AD): an item's use goes out through the cast routine
// (0x0080ac90: CMSG_USE_ITEM, then the tail CMSG_CAST_SPELL shares), which
// fires UNIT_SPELLCAST_SENT with the spell's name and rank; WoWee raised it
// for spell casts only.
TEST_CASE("using an item raises UNIT_SPELLCAST_SENT (T423)", "[replay][interface]") {
    constexpr uint64_t kWater = 0x4000000000000B31ull;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    auto& gh = s.handler();
    carryForAuction(s, 0, kWater, 159, 5);
    auto& info = gh.itemInfoCacheRef()[159];
    info.spells[0].spellId = 430;
    info.spells[0].spellTrigger = 0;
    gh.spellNameCacheRef()[430].name = "Drink";
    ui.run("sent = nil local f = CreateFrame('Frame') f:RegisterEvent('UNIT_SPELLCAST_SENT') "
           "f:SetScript('OnEvent', function(self, e, u, name, rank, target) sent = u .. '/' .. name .. '/' .. target end)");
    ui.run("UseContainerItem(0, 1)");
    REQUIRE(s.sent().of(Opcode::CMSG_USE_ITEM).size() == 1);
    CHECK(ui.is("sent ~= nil and sent:sub(1, 13) == 'player/Drink/'"));
}

// T422 (FIXER AD): a key lifted off the keyring (container -2) read as an
// item off the action bar - the no-source marker was -2 too - so the drop
// put it back and nothing was sent. 0x005d7ff0 swaps it like any bag item
// (CMSG_SWAP_ITEM, the keyring's slots 86.. of the player's slot array).
TEST_CASE("a key moves inside the keyring (T422)", "[replay][interface]") {
    constexpr uint64_t kKey = 0x4000000000000B21ull;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    auto& gh = s.handler();
    auto& info = gh.itemInfoCacheRef()[7146];
    info.entry = 7146;
    info.valid = true;
    info.name = "Key";
    wowee::game::ItemDef key;
    key.itemId = 7146;
    key.name = "Key";
    key.guid = kKey;
    key.stackCount = 1;
    REQUIRE(gh.inventoryRef().setKeyringSlot(0, key));
    gh.keyringSlotGuidsRef()[0] = kKey;
    gh.onlineItemsRef()[kKey].entry = 7146;
    ui.run("PickupContainerItem(-2, 1)");
    REQUIRE(ui.is("CursorHasItem() == 1"));
    ui.run("PickupContainerItem(-2, 2)");
    const auto swaps = s.sent().of(Opcode::CMSG_SWAP_ITEM);
    REQUIRE(swaps.size() == 1);
    auto p = swaps.front();
    CHECK(p.readUInt8() == 0xFF);   // destination: the player's slots
    CHECK(p.readUInt8() == 87);     // keyring slot 2
    CHECK(p.readUInt8() == 0xFF);
    CHECK(p.readUInt8() == 86);     // from keyring slot 1
    CHECK(ui.is("not CursorHasItem()"));
}

// T420 (FIXER AD): a split's source (0x005d7ff0 -> 0x006db9d0, then
// ClearCursor(0, 1), no unlock) waits for its new count. AzerothCore sends
// that count in a CREATE block of the known item, which 12340 reads as a
// values update (0x004d63b0 -> 0x004d5550, so 0x00707720 runs); WoWee let
// the lock go only for a values block and the source stayed locked for good.
// A worn item put back on its own slot is put down and unlocked (0x005e85d0
// -> ClearCursor(1, 1)) - WoWee refused it as locked.
TEST_CASE("a split's source and a worn put-back are let go (T420)", "[replay][interface]") {
    constexpr uint64_t kLinen = 0x4000000000000B11ull, kRod = 0x4000000000000B12ull;
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    carryForAuction(s, 0, kLinen, 2589, 17);
    ui.engine.dispatchOnUpdate(0.0f);
    ui.run("seen = {} local f = CreateFrame('Frame') "
           "for _, e in ipairs({'ITEM_LOCKED', 'ITEM_UNLOCKED'}) do f:RegisterEvent(e) end "
           "f:SetScript('OnEvent', function(self, e, a, b) seen[#seen + 1] = e .. ' ' .. a .. ' ' .. tostring(b) end)");
    ui.run("SplitContainerItem(0, 1, 5)");
    ui.run("seen = {} PickupContainerItem(0, 3)");
    REQUIRE(s.sent().of(Opcode::CMSG_SPLIT_ITEM).size() == 1);
    REQUIRE(s.handler().getInventoryHandler()->itemAwaitsRealm(kLinen));
    // The realm: the source re-sent whole with its new count.
    s.receive(UpdateObject().create(kLinen, wowee::game::ObjectType::ITEM,
                                    {{UF::OBJECT_FIELD_ENTRY, 2589u}, {UF::ITEM_FIELD_STACK_COUNT, 12u}}));
    CHECK_FALSE(s.handler().getInventoryHandler()->itemAwaitsRealm(kLinen));
    ui.engine.dispatchOnUpdate(0.0f);
    CHECK(ui.is("seen[1] == 'ITEM_UNLOCKED 0 1'"));
    // ...and the same by a values block.
    ui.run("SplitContainerItem(0, 1, 2) PickupContainerItem(0, 4)");
    REQUIRE(s.handler().getInventoryHandler()->itemAwaitsRealm(kLinen));
    s.receive(UpdateObject().values(kLinen, {{UF::ITEM_FIELD_STACK_COUNT, 10u}}));
    CHECK_FALSE(s.handler().getInventoryHandler()->itemAwaitsRealm(kLinen));
    // A key the realm puts in a keyring slot: the same slot array's callback.
    constexpr uint64_t kKey = 0x4000000000000B13ull;
    s.handler().getInventoryHandler()->lockItemUntilRealmAnswers(kKey);
    // 3.3.5a: PLAYER_FIELD_KEYRING_SLOT_1 = 0x1f0, after the seven bank bags
    // and the twelve buyback slots (the layout file names no ring).
    const auto ring = static_cast<uint16_t>(wowee::game::fieldIndex(UF::PLAYER_FIELD_BANKBAG_SLOT_1) + 14 + 24);
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid,
                                      {{static_cast<uint16_t>(ring + 2), static_cast<uint32_t>(kKey)},
                                       {static_cast<uint16_t>(ring + 3), static_cast<uint32_t>(kKey >> 32)}}));
    CHECK_FALSE(s.handler().getInventoryHandler()->itemAwaitsRealm(kKey));

    // A worn fishing rod lifted and put back on its slot.
    auto& gh = s.handler();
    gh.itemInfoCacheRef()[6256].entry = 6256;
    gh.itemInfoCacheRef()[6256].valid = true;
    gh.itemInfoCacheRef()[6256].name = "Rod";
    wowee::game::ItemDef rod;
    rod.itemId = 6256;
    rod.name = "Rod";
    rod.guid = kRod;
    REQUIRE(gh.inventoryRef().setEquipSlot(wowee::game::EquipSlot::MAIN_HAND, rod));
    gh.equipSlotGuidsRef()[15] = kRod;
    gh.onlineItemsRef()[kRod].entry = 6256;
    ui.engine.dispatchOnUpdate(0.0f);
    ui.run("seen = {} PickupInventoryItem(16)");
    REQUIRE(ui.is("seen[1] == 'ITEM_LOCKED 16 nil'"));
    const size_t sentBefore = s.sent().all().size();
    ui.run("seen = {} PickupInventoryItem(16)");
    CHECK(ui.is("not CursorHasItem()"));
    CHECK(ui.is("seen[1] == 'ITEM_UNLOCKED 16 nil'"));
    CHECK(ui.is("not IsInventoryItemLocked(16)"));
    CHECK(s.sent().all().size() == sentBefore);   // nothing goes out
}

namespace {
// One SMSG_GUILD_BANK_LIST of tab 1 (not full) naming the given slots.
void listGuildBankSlots(Session& s, const std::vector<std::array<uint32_t, 3>>& slots) {
    wowee::network::Packet list(wowee::game::wireOpcode(Opcode::SMSG_GUILD_BANK_LIST));
    list.writeUInt64(0);
    list.writeUInt8(0);
    list.writeUInt32(10);
    list.writeUInt8(0);
    list.writeUInt8(static_cast<uint8_t>(slots.size()));
    for (const auto& sl : slots) {
        list.writeUInt8(static_cast<uint8_t>(sl[0]));
        list.writeUInt32(sl[1]);
        if (sl[1] == 0) continue;
        list.writeUInt32(0);
        list.writeUInt32(0);
        list.writeUInt32(sl[2]);
        list.writeUInt32(0);
        list.writeUInt8(0);
        list.writeUInt8(0);
    }
    s.receive(list);
}
}  // namespace

// AA's open guild bank items (FIXER AB): 0x005a7250's events. The first list
// opens the vault with GUILDBANKFRAME_OPENED alone (and GUILDBANK_UPDATE_TABS
// once per tab name before it) - no GUILDBANKBAGSLOTS_CHANGED, no
// GUILDBANK_UPDATE_MONEY; with the vault open a list announces
// GUILDBANKBAGSLOTS_CHANGED at once, or, when an item template is not cached,
// only when the last one waited on comes (0x005a47a0).
TEST_CASE("a guild bank list announces itself as 0x005a7250 does", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    ui.run("seen = '' local f = CreateFrame('Frame') "
           "for _, e in ipairs({'GUILDBANKFRAME_OPENED', 'GUILDBANKBAGSLOTS_CHANGED', 'GUILDBANK_UPDATE_TABS', "
           "'GUILDBANK_UPDATE_MONEY'}) do f:RegisterEvent(e) end "
           "f:SetScript('OnEvent', function(self, e) seen = seen .. e .. ';' end)");
    openTestGuildBank(s);
    CHECK(ui.is("seen == 'GUILDBANK_UPDATE_TABS;GUILDBANK_UPDATE_TABS;GUILDBANK_UPDATE_TABS;GUILDBANKFRAME_OPENED;'"));
    ui.run("seen = ''");
    listGuildBankSlots(s, {{4, 2589, 3}});   // a template already cached
    CHECK(ui.is("seen == 'GUILDBANKBAGSLOTS_CHANGED;'"));
    ui.run("seen = ''");
    listGuildBankSlots(s, {{5, 7001, 1}, {6, 7001, 1}, {7, 7002, 1}});
    CHECK(ui.is("seen == ''"));
    answerItem(s, 7001, "Eins");
    CHECK(ui.is("seen == ''"));
    answerItem(s, 7002, "Zwei");
    CHECK(ui.is("seen == 'GUILDBANKBAGSLOTS_CHANGED;'"));
}

// AA's open guild bank items (FIXER AB): a listed slot whose item is on the
// cursor puts the cursor down (0x005a7250: ClearCursor + 0x00513800); the
// local player made anew resets every tab's slots and flags (0x006e1bb0 ->
// 0x005a7760), so the next tab query asks for the full list again.
TEST_CASE("a guild bank list lets the held slot go, a new player starts the bank again", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    openTestGuildBank(s);
    ui.run("PickupGuildBankItem(1, 1)");
    REQUIRE(ui.is("CursorHasItem() == 1"));
    listGuildBankSlots(s, {{1, 2589, 18}});   // another slot: still held
    CHECK(ui.is("CursorHasItem() == 1"));
    listGuildBankSlots(s, {{0, 2589, 4}});    // the held one
    CHECK(ui.is("not CursorHasItem()"));
    CHECK(ui.is("select(3, GetGuildBankItemInfo(1, 1)) == nil"));

    const auto fullByte = [&s]() {
        const auto q = s.sent().of(Opcode::CMSG_GUILD_BANK_QUERY_TAB);
        REQUIRE(!q.empty());
        wowee::network::Packet p = q.back();
        p.readUInt64();
        p.readUInt8();
        return p.readUInt8();
    };
    ui.run("QueryGuildBankTab(1)");
    CHECK(fullByte() == 0);                   // tab 1 was listed
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    CHECK(ui.is("GetNumGuildBankTabs() == 0"));
    s.handler().getInventoryHandler()->openGuildBank(0x77);
    ui.run("QueryGuildBankTab(1)");
    CHECK(fullByte() == 1);
}

// T430: a guild bank slot's lock (+0x24) is set at the pickup (0x005137d0)
// and let go by every drop: ClearCursor, and a move's own put-down too -
// 0x005a5410 ends in 0x00519280(1, 1), which clears +0x24 (0x005a4ce0). A
// slot put back on itself or dropped stayed locked for good, waiting on a
// list the realm never sends for nothing moved.
// T429: 0x005a50c0 refuses a deposit before anything is sent and returns -
// the item stays on the cursor as it was picked up. Here the tab's deposit
// right (0x005a41c0 bit 2, game error 0x5f): tab 2 is view only.
TEST_CASE("a refused guild bank deposit leaves the item on the cursor (0x005a50c0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    openTestGuildBank(s);
    carry(s, 1251, 0, 0);
    ui.run("PickupContainerItem(0, 1)");
    REQUIRE(ui.is("CursorHasItem() == 1"));
    ui.run("PickupGuildBankItem(2, 10)");
    CHECK(s.sent().of(Opcode::CMSG_GUILD_BANK_SWAP_ITEMS).empty());
    CHECK(ui.is("CursorHasItem() == 1"));
    ui.run("PickupGuildBankItem(1, 10)");            // tab 1 takes it
    CHECK(s.sent().of(Opcode::CMSG_GUILD_BANK_SWAP_ITEMS).size() == 1);
    CHECK(ui.is("not CursorHasItem()"));
    // T432: the sent deposit lets the bag item's lock go with the cursor.
    CHECK(ui.is("select(3, GetContainerItemInfo(0, 1)) == nil"));
}

TEST_CASE("a guild bank slot is let go by every drop (0x00519280)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    openTestGuildBank(s);
    ui.run("PickupGuildBankItem(1, 1)");
    REQUIRE(ui.is("CursorHasItem() == 1"));
    ui.run("PickupGuildBankItem(1, 1)");               // back on its own slot
    CHECK(ui.is("not CursorHasItem()"));
    ui.run("PickupGuildBankItem(1, 1)");
    CHECK(ui.is("CursorHasItem() == 1"));
    ui.run("ClearCursor() PickupGuildBankItem(1, 1)");
    CHECK(ui.is("CursorHasItem() == 1"));
    ui.run("PickupGuildBankItem(1, 50)");              // a move to an empty slot
    REQUIRE(s.sent().of(Opcode::CMSG_GUILD_BANK_SWAP_ITEMS).size() >= 1);
    CHECK(ui.is("not CursorHasItem()"));
    ui.run("PickupGuildBankItem(1, 1)");               // no list yet: free all the same
    CHECK(ui.is("CursorHasItem() == 1"));
}

// AA's open guild bank item (FIXER AB): tab names and texts are kept as
// 0x007e1830 keeps them - every '|' left behind (0x007e17f0), a name ends at
// its first line break and holds 15 characters (16, 64 bytes, flag 1:
// 0x005a4220, 0x005a7250, 0x005a5a00), a text 2047 (0x005a4ae0, 0x005a7180).
TEST_CASE("guild bank tab names and texts are taken as 0x007e1830 takes them", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.handler().setGuildBankTabCount(1);
    wowee::network::Packet event(wowee::game::wireOpcode(Opcode::SMSG_GUILD_EVENT));
    event.writeUInt8(0x10);
    event.writeUInt8(3);
    event.writeString("0");
    event.writeString("|cffff0000Rot|r\nZweite");
    event.writeString("INV_Misc_Herb_01");
    s.receive(event);
    CHECK(ui.is("GetGuildBankTabInfo(1) == 'cffff0000Rotr'"));
    event = wowee::network::Packet(wowee::game::wireOpcode(Opcode::SMSG_GUILD_EVENT));
    event.writeUInt8(0x10);
    event.writeUInt8(3);
    event.writeString("0");
    event.writeString("ABCDEFGHIJKLMNOPQRST");
    event.writeString("INV_Misc_Herb_01");
    s.receive(event);
    CHECK(ui.is("GetGuildBankTabInfo(1) == 'ABCDEFGHIJKLMNO'"));
    wowee::network::Packet text(wowee::game::wireOpcode(Opcode::MSG_QUERY_GUILD_BANK_TEXT));
    text.writeUInt8(0);
    text.writeString("a|Hitem:1|h[b]|h\nzwei");
    s.receive(text);
    CHECK(ui.is("GetGuildBankText(1) == 'aHitem:1h[b]h' .. string.char(10) .. 'zwei'"));
    CHECK(wowee::game::clientTextCopy("x\ny", 16, 64, true) == "x");
    CHECK(wowee::game::clientTextCopy("x\\ny", 16, 64, true) == "x");
    CHECK(wowee::game::clientTextCopy(std::string(100, 'a'), 0x800, 0x40, false).size() == 63);
}

// AB's open item (FIXER AC): SetGuildBankTabInfo (0x005a6a00 -> 0x005a5a00)
// puts the name through 0x007e1fb0 - letters of one alphabet, spaces and
// digits, the apostrophe/space/three-letter rules off, so an empty name
// passes - and a refused one is worded on the error line (0x005218c0) with
// nothing sent; the request goes only for a bought tab. WoWee sent any name
// (an underscore too) and refused the empty one.
TEST_CASE("a guild bank tab name passes 0x007e1fb0 before it is sent", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    openTestGuildBank(s);
    ui.run("GetMacroIconInfo = function(i) if i == 1 then return 'Interface\\\\Icons\\\\INV_Misc_Herb_01' end end");
    ui.run("errors = {} local f = CreateFrame('Frame') f:RegisterEvent('UI_ERROR_MESSAGE') "
           "f:SetScript('OnEvent', function(self, e, a) errors[#errors + 1] = a end)");
    const auto names = [&] {
        std::vector<std::string> out;
        for (auto p : s.sent().of(Opcode::CMSG_GUILD_BANK_UPDATE_TAB)) {
            p.readUInt64();
            p.readUInt8();
            out.push_back(p.readString());
        }
        return out;
    };
    ui.run("SetGuildBankTabInfo(1, 'Kr\\195\\164uter 2', 1)");
    REQUIRE(names().size() == 1);
    CHECK(names().back() == "Kr\xC3\xA4uter 2");
    ui.run("SetGuildBankTabInfo(1, 'Tab_1', 1)");      // '_' is not allowed
    CHECK(names().size() == 1);
    CHECK(ui.is("#errors == 1"));
    ui.run("SetGuildBankTabInfo(1, 'Fooo  ', 1)");     // the strict rules are off
    REQUIRE(names().size() == 2);
    CHECK(names().back() == "Fooo  ");
    ui.run("SetGuildBankTabInfo(2, '', 1)");           // empty passes
    REQUIRE(names().size() == 3);
    CHECK(names().back().empty());
    ui.run("SetGuildBankTabInfo(4, 'Vier', 1)");       // three tabs bought
    ui.run("SetGuildBankTabInfo(1, 'Tab_1', 2)");      // no such icon: not even checked
    CHECK(names().size() == 3);
    CHECK(ui.is("#errors == 1"));
    CHECK(wowee::game::name_rules::checkGuildBankTabName(std::string(17, 'a')) == wowee::game::name_rules::kTooLong);
    CHECK(wowee::game::name_rules::checkGuildBankTabName("a'b") == wowee::game::name_rules::kInvalidCharacter);
}

// LFG review (found by AB in code): GetLFGRoleUpdate answers a boolean first,
// false without a role check (0x00554610, was nil); UnitHasLFGDeserter /
// UnitHasLFGRandomCooldown of a unit naming nobody answer nothing
// (0x00555840 / 0x00555760, was false).
TEST_CASE("LFG answers of an idle player are 12340's (0x00554610, 0x00555840)", "[replay][interface][lfg]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    CHECK(ui.is("GetLFGRoleUpdate() == false"));
    CHECK(ui.is("select('#', GetLFGRoleUpdate()) == 3"));
    CHECK(ui.is("select('#', UnitHasLFGDeserter('party3')) == 0"));
    CHECK(ui.is("select('#', UnitHasLFGRandomCooldown('party3')) == 0"));
    CHECK(ui.is("UnitHasLFGDeserter('player') == false"));
}

// Equipment manager review (found by AB in code): SaveEquipmentSet does
// nothing - no usage line either - before the session's set list came
// (0x005af9c0, 0x00c1e955 set by 0x005af490); a set name is taken as
// 0x007e1830 takes it - every '|' left behind, so "|n" is no line break and
// the name the server is told has no pipe; a real line break still refuses.
TEST_CASE("equipment set names and the set list gate are 12340's (0x005af9c0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    s.setConnected(true);
    CHECK(ui.is("pcall(SaveEquipmentSet) == true"));   // no list yet: not even the usage line
    wowee::network::Packet list(wowee::game::wireOpcode(Opcode::SMSG_EQUIPMENT_SET_LIST));
    list.writeUInt32(0);
    s.receive(list);
    CHECK(ui.is("pcall(SaveEquipmentSet) == false"));
    CHECK(ui.is("pcall(SaveEquipmentSet, 'a' .. string.char(10) .. 'b', 1) == false"));
    const size_t before = s.sent().of(Opcode::CMSG_EQUIPMENT_SET_SAVE).size();
    ui.run("SaveEquipmentSet('Tank|nSet', 1)");
    const auto saves = s.sent().of(Opcode::CMSG_EQUIPMENT_SET_SAVE);
    REQUIRE(saves.size() == before + 1);
    wowee::network::Packet p = saves.back();
    p.readPackedGuid();
    p.readUInt32();
    CHECK(p.readString() == "TanknSet");
}

// T419: an empty script element clears the handler, the template's too
// (12340's script loader 0x0048fef0 lets the old one go and leaves the slot
// empty with nothing to compile); a named function that is not one does the
// same after its warning. Blizzard_AuctionUI.xml's AuctionProgressBar turns
// CastingBarFrameTemplate's OnEvent/OnShow/OnUpdate off so - WoWee skipped
// the empty elements and the casting bar's handlers ran on every multisell.
TEST_CASE("an empty script element clears the inherited handler (T419, 0x0048fef0)", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    wowee::ui::XmlNode root;
    std::string error;
    REQUIRE(wowee::ui::parseXml(
        "<Ui><Frame name=\"PlayBarTemplate\" virtual=\"true\"><Scripts>"
        "<OnShow>shownBy = 'template'</OnShow><OnUpdate>ticks = (ticks or 0) + 1</OnUpdate>"
        "<OnHide>hiddenBy = 'template'</OnHide></Scripts></Frame>"
        "<Frame name=\"PlayBar\" inherits=\"PlayBarTemplate\" hidden=\"true\"><Scripts>"
        "<OnShow></OnShow><OnUpdate function=\"PlayNoSuchFunction\"/></Scripts></Frame></Ui>",
        root, error));
    CHECK(ui.engine.buildElements(wowee::ui::emitFrameXml(root), "t.xml", [](const auto&) {}));
    CHECK(ui.is("PlayBar:GetScript('OnShow') == nil"));
    CHECK(ui.is("PlayBar:GetScript('OnUpdate') == nil"));
    CHECK(ui.is("type(PlayBar:GetScript('OnHide')) == 'function'"));
    ui.run("PlayBar:Show()");
    CHECK(ui.is("shownBy == nil"));
}

// Found by AE in code: GetQuestLogTimeLeft (0x005e4320) reads the quest log
// slot of the selected row - slot 0 with nothing selected - and answers its
// timer less now, less one; nil for no timer or a failed slot.
TEST_CASE("GetQuestLogTimeLeft answers slot 0 with nothing selected", "[replay][interface][quest]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    const auto field = [](uint16_t slot, uint16_t word) {
        return static_cast<uint16_t>(wowee::game::fieldIndex(UF::PLAYER_QUEST_LOG_START) + slot * 5 + word);
    };
    const auto expiry = static_cast<uint32_t>(std::time(nullptr) + 600);
    s.receive(UpdateObject().values(Session::kPlayerGuid, {{UF::PLAYER_QUEST_LOG_START, 9387}}));
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{field(0, 4), expiry}}));
    REQUIRE(s.handler().getSelectedQuest() == nullptr);
    ui.run("left = GetQuestLogTimeLeft()");
    CHECK(ui.is("type(left) == 'number' and left >= 597 and left <= 599"));
    // A failed slot has no timer.
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{field(0, 1), 2}}));
    CHECK(ui.is("GetQuestLogTimeLeft() == nil"));
    // Run out: zero, not under it.
    s.receive(UpdateObject().valuesAt(Session::kPlayerGuid, {{field(0, 1), 0}, {field(0, 4), 1}}));
    CHECK(ui.is("GetQuestLogTimeLeft() == 0"));
}

// Found by AE in code: GetWhoInfo (0x006b4a80) names a race, class or zone
// the tables lack with the localized UNKNOWN, never an empty string.
TEST_CASE("GetWhoInfo names an unknown zone UNKNOWN", "[replay][interface][who]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("SetWhoToUI(1)");
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_WHO));
    p.writeUInt32(1);
    p.writeUInt32(1);
    p.writeString("Locki");
    p.writeString("");
    p.writeUInt32(80);
    p.writeUInt32(8);
    p.writeUInt32(10);
    p.writeUInt8(1);
    p.writeUInt32(987654);  // no such area
    s.receive(p);
    CHECK(ui.is("GetNumWhoResults() == 1"));
    CHECK(ui.is("select(6, GetWhoInfo(1)) == 'Unknown'"));
    CHECK(ui.is("select(1, GetWhoInfo(1)) == 'Locki'"));
}

// Found by AE in code: GetSpellTabInfo for a tab out of range answers nil,
// nil and four zeroes (0x0053be70), not two empty strings.
TEST_CASE("GetSpellTabInfo out of range: nil name and texture, zero counts", "[replay][interface][spell]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    CHECK(ui.is("(function() local n, t, o, c, ho, hc = GetSpellTabInfo(9) "
                "return n == nil and t == nil and o == 0 and c == 0 and ho == 0 and hc == 0 end)()"));
    CHECK(ui.is("select('#', GetSpellTabInfo(9)) == 6"));
}

// T427: CloseItemText is 0x0058a400 -> 0x0058a1a0(0, 0): a book used from the
// bags is let go and ITEM_TEXT_CLOSED (0x117) goes out, once - closed again
// with nothing open it is silent.
TEST_CASE("CloseItemText closes a book read from the bags", "[replay][interface]") {
    Session s;
    s.spawnPlayer({{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}});
    Interface ui(s);
    ui.run("closed = 0; local f = CreateFrame('Frame'); f:RegisterEvent('ITEM_TEXT_CLOSED');"
           "f:SetScript('OnEvent', function() closed = closed + 1 end)");
    s.handler().openBook(77, 0x4000000000000123ull);
    wowee::network::Packet page(wowee::game::wireOpcode(Opcode::SMSG_PAGE_TEXT_QUERY_RESPONSE));
    page.writeUInt32(77);
    page.writeString("Page one");
    page.writeUInt32(0);
    s.receive(page);
    REQUIRE(ui.is("ItemTextGetText() == 'Page one'"));
    ui.run("CloseItemText()");
    CHECK(ui.is("closed == 1"));
    CHECK(ui.is("ItemTextGetText() == ''"));
    ui.run("CloseItemText()");
    CHECK(ui.is("closed == 1"));
}
