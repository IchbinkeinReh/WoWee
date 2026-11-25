// What the interface's calls do through the Lua engine itself, with a client
// behind them. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "addons/lua_api_helpers.hpp"
#include "addons/lua_engine.hpp"
#include "core/input.hpp"
#include "core/preview_dressing.hpp"
#include "game/dressing_room.hpp"
#include "game/player_display_flags.hpp"
#include "game_replay.hpp"
#include "pipeline/dbc_loader.hpp"
#include "ui/widget_tree.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <tuple>
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
    ui.engine.placeEditCaret(id, left + 10.5f, top - 5.0f - line * 1.5f);
    CHECK(tree.get(id)->cursorPos == 5);
    CHECK_FALSE(tree.get(id)->hasSelection);
    CHECK(ui.is("cy < 0 and cx == 0"));
    ui.engine.placeEditCaret(id, left + 290.0f, top - 5.0f - line * 1.5f);
    CHECK(tree.get(id)->cursorPos == 9);
    ui.engine.placeEditCaret(id, left + 290.0f, top - 5.0f - line * 9.0f);
    CHECK(tree.get(id)->cursorPos == 14);
    // Left of the text it is at the text's start, whatever the line.
    ui.engine.placeEditCaret(id, left + 2.0f, top - 5.0f - line * 1.5f);
    CHECK(tree.get(id)->cursorPos == 0);
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
    explicit Reputation(Session& s) : Interface(s) {
        s.handler().readFactionTable(factionTable({{980, 0xFFFFFFFFu, 0, "The Burning Crusade", ""},
                                                   {936, 4, 980, "Shattrath City", ""},
                                                   {942, 0, 936, "Cenarion Expedition", "An expedition."},
                                                   {932, 1, 936, "The Aldor", ""},
                                                   {947, 2, 980, "Thrallmar", "Sent by Thrall."},
                                                   {749, 3, 0, "Hydraxian Waterlords", ""}}));
        for (int i = 0; i < 5; ++i) s.handler().initialFactionsRef().push_back({0x1, 100});
        s.handler().initialFactionsRef()[4].flags = 0;
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
