// What the interface's calls do through the Lua engine itself, with a client
// behind them. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "addons/lua_api_helpers.hpp"
#include "addons/lua_engine.hpp"
#include "game_replay.hpp"
#include "ui/widget_tree.hpp"

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

