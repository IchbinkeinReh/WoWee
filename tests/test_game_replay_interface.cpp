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

