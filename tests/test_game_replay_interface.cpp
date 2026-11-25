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
#include "ui/framexml_emitter.hpp"
#include "ui/widget_tree.hpp"
#include "ui/xml_parser.hpp"

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
    // Put in by an addon.
    ui.run("index(f).PlayOwn = function(self) return self end");
    CHECK(ui.is("f2:PlayOwn() == f2 and b.PlayOwn == nil"));
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
