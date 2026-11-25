// What the interface's calls do through the Lua engine itself, with a client
// behind them. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "addons/lua_api_helpers.hpp"
#include "addons/lua_engine.hpp"
#include "core/preview_dressing.hpp"
#include "game/dressing_room.hpp"
#include "game/player_display_flags.hpp"
#include "game_replay.hpp"
#include "ui/widget_tree.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::Opcode;
using wowee::game::UF;

namespace {

struct Interface {
    explicit Interface(Session& s) {
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
void answerItem(Session& s, uint32_t entry, const char* name) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE));
    p.writeUInt32(entry);
    for (int i = 0; i < 3; ++i) p.writeUInt32(0);
    p.writeString(name);
    for (int i = 0; i < 3; ++i) p.writeString("");
    for (int i = 0; i < 160; ++i) p.writeUInt32(0);
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
    CHECK(w->modelDisplayId == 0);
    CHECK(w->modelCreatureAsked == 4269);
    CHECK_FALSE(s.sent().of(Opcode::CMSG_CREATURE_QUERY).empty());
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_CREATURE_QUERY_RESPONSE));
    p.writeUInt32(4269);
    p.writeString("Chestnut Mare");
    for (int i = 0; i < 5; ++i) p.writeString("");
    for (uint32_t v : {0u, 0u, 0u, 0u, 0u, 0u, 2405u, 0u, 0u, 0u}) p.writeUInt32(v);
    for (int i = 0; i < 20; ++i) p.writeUInt32(0);
    s.receive(p);
    wowee::addons::showAskedCreature(*w, &s.handler());
    CHECK(w->modelDisplayId == 2405);
    CHECK(w->modelCreatureAsked == 0);
    // Known now, it is shown at once; the stable's paperdoll takes its place.
    w->modelDisplayId = 0;
    ui.run("model:SetCreature(4269)");
    CHECK(w->modelDisplayId == 2405);
    ui.run("model:SetCreature(4270) if SetPetStablePaperdoll then SetPetStablePaperdoll(model) end");
    CHECK(w->modelCreatureAsked == 0);
}

// P55: 0x006301a0 describes the item by its object, not by its template.
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
    CHECK(all.find("Soulbound|") != std::string::npos);
    CHECK(all.find("120") != std::string::npos);
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
