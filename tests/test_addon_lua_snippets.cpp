// The Lua this client injects into the interface, handed to Lua itself.
//
// These are C++ string literals that nothing compiles until the client runs.
// executeString answers false on a syntax error and the client carries on -
// the only sign is a warning in a log that is warning-only, and the panels
// simply are not there. Four hundred lines of Lua with no build step is four
// hundred lines where a stray `end` costs a release.
//
// This does not run them. Running needs the whole widget shim, the settings
// bridge and the interface loaded; that is what framexml_run is for. This asks
// the narrower question that has a cheap answer: does it parse.
#include <catch_amalgamated.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>

#include "addons/addon_lua_snippets.hpp"
#include "addons/animation_lua.hpp"
#include "addons/chat_bubble_lua.hpp"
#include "addons/button_label_lua.hpp"

namespace {

/// The Lua compiler's own verdict, and its message when it refuses.
std::string compileError(const char* chunk, const char* name) {
    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);
    // No libraries opened: parsing needs none, and the vendored Lua leaves out
    // loadlib deliberately - a game client has no business dlopening things -
    // so luaL_openlibs would not even link here.
    std::string err;
    if (luaL_loadbuffer(L, chunk, std::strlen(chunk), name) != 0) {
        const char* message = lua_tostring(L, -1);
        err = message ? message : "(no message)";
    }
    lua_close(L);
    return err;
}

/// Run several chunks in one state, and answer with what the last one returned.
///
/// More than parsing, and still short of framexml_run: enough of Lua to run a
/// snippet against stand-ins for the frames it reaches for. A failure comes
/// back as the Lua error, so the message says which chunk and which line.
std::string runChunks(std::initializer_list<std::pair<const char*, const char*>> chunks) {
    lua_State* L = luaL_newstate();
    REQUIRE(L != nullptr);
    // Only what the snippets under test use: pairs, ipairs, type and rawget
    // from the base library, insert and remove from the table one.
    lua_pushcfunction(L, luaopen_base);
    lua_call(L, 0, 0);
    lua_pushcfunction(L, luaopen_table);
    lua_pushstring(L, LUA_TABLIBNAME);
    lua_call(L, 1, 0);

    std::string result;
    for (const auto& [chunk, name] : chunks) {
        if (luaL_loadbuffer(L, chunk, std::strlen(chunk), name) != 0 ||
            lua_pcall(L, 0, 1, 0) != 0) {
            const char* message = lua_tostring(L, -1);
            lua_close(L);
            return std::string(name) + ": " + (message ? message : "(no message)");
        }
        const char* value = lua_tostring(L, -1);
        result = value ? value : "";
        lua_pop(L, 1);
    }
    lua_close(L);
    return result;
}

/// The options machinery the removal snippet walks, cut down to what it calls.
constexpr const char* kFakeOptionsPanelLua = R"LUA(
local function frame(name, parent)
    local f = {
        _name = name, _parent = parent, _hidden = false, _hooks = {},
        GetName = function(self) return self._name end,
        GetParent = function(self) return self._parent end,
        GetChildren = function(self) return unpack(self._children or {}) end,
        Hide = function(self) self._hidden = true end,
        HookScript = function(self, script, fn) self._hooks[script] = fn end,
    }
    _G[name] = f
    if parent then
        parent._children = parent._children or {}
        table.insert(parent._children, f)
    end
    return f
end

local panel = frame("VideoOptionsResolutionPanel")
-- The retired checkbox, and one control that stays.
local vsync = frame("VideoOptionsResolutionPanelVSync", panel)
local scale = frame("VideoOptionsResolutionPanelUIScaleSlider", panel)
panel.controls = { vsync, scale }

-- Both carry what BlizzardOptionsPanel_SetupControl would have left on them:
-- the cached value the Okay loops commit, and for the action bar boxes the
-- uvar naming a global FrameXML reads. A retired control has to lose the
-- first and keep the second.
vsync.value = "0"
vsync.uvar = "GX_VSYNC"
scale.value = "1"

local bars = frame("InterfaceOptionsActionBarsPanel")
local bottomLeft = frame("InterfaceOptionsActionBarsPanelBottomLeft", bars)
bottomLeft.value = "1"
bottomLeft.uvar = "SHOW_MULTI_ACTIONBAR_1"
bars.controls = { bottomLeft }
)LUA";

}  // namespace

TEST_CASE("a retired control commits nothing and keeps its place", "[addonlua]") {
    // Hiding a control is not retiring it. VideoOptionsPanel_Okay walks
    // panel.controls and writes every entry's cached value back to its cvar,
    // changed or not - so the hidden vertical-sync checkbox replayed the value
    // it read at load over the Display page's own row, and pressing Okay on the
    // video window turned vertical sync off again each time it was switched on.
    //
    // Both halves are asserted here because fixing the first by unregistering
    // the control broke the second, and shipped: BlizzardOptionsPanel_SetupControl
    // runs only over panel.controls, and it is what assigns the uvar globals, so
    // the action bar checkboxes leaving the list left SHOW_MULTI_ACTIONBAR_1 nil
    // and the player with no bottom action bar.
    const std::string result = runChunks({
        {kFakeOptionsPanelLua, "FakeOptionsPanel"},
        {wowee::addons::kRemovedControlsLua, "RemovedControls"},
        {R"LUA(
            local vsync = VideoOptionsResolutionPanelVSync
            local scale = VideoOptionsResolutionPanelUIScaleSlider
            local bar = InterfaceOptionsActionBarsPanelBottomLeft
            local listed = ""
            for _, c in ipairs(VideoOptionsResolutionPanel.controls) do
                listed = listed .. c:GetName() .. " "
            end
            return "listed=" .. listed
                .. "| vsync.value=" .. tostring(vsync.value)
                .. " scale.value=" .. tostring(scale.value)
                .. " bar.value=" .. tostring(bar.value)
                .. " bar.uvar=" .. tostring(bar.uvar)
                .. " hidden=" .. tostring(vsync._hidden)
        )LUA",
         "Check"},
    });
    // The retired boxes keep their place on the panel, so setup still reaches
    // them, and lose only the value the Okay loops would have written back.
    CHECK(result ==
          "listed=VideoOptionsResolutionPanelVSync VideoOptionsResolutionPanelUIScaleSlider "
          "| vsync.value=nil scale.value=1 bar.value=nil "
          "bar.uvar=SHOW_MULTI_ACTIONBAR_1 hidden=true");
}

TEST_CASE("the options panel script parses", "[addonlua]") {
    // Twelve categories of check button, slider and dropdown, built from the
    // settings schema. Added over several passes, none of which could have
    // been told by the compiler that it had broken the one before.
    const std::string err =
        compileError(wowee::addons::kWoweeOptionsPanelLua, "WoweeOptionsPanel");
    INFO(err);
    CHECK(err.empty());
}

TEST_CASE("the coin clearance script parses", "[addonlua]") {
    // Reaches into MoneyFrame_Update, which is Blizzard's, and hooks rather
    // than edits it - so a mistake here shows up as money that draws wrong
    // rather than as anything that says "script error".
    const std::string err =
        compileError(wowee::addons::kCoinAmountClearanceLua, "CoinAmountClearance");
    INFO(err);
    CHECK(err.empty());
}

TEST_CASE("the chat box visibility script parses", "[addonlua]") {
    // The one that proves the point of this file. It lived in the settings
    // panel as a run of adjacent C++ string literals with no separator, so the
    // `end` closing its loop ran into the `end` closing its `if` and then into
    // the `if` after that. Every click of Chat Style printed a script error and
    // changed nothing, and no build ever said so.
    const std::string err =
        compileError(wowee::addons::kChatBoxVisibilityLua, "ChatBoxVisibility");
    INFO(err);
    CHECK(err.empty());
}

TEST_CASE("the chat background swatch script parses", "[addonlua]") {
    const std::string err = compileError(wowee::addons::kChatBackgroundSwatchLua,
                                         "ChatBackgroundSwatch");
    INFO(err);
    CHECK(err.empty());
}

namespace {

/// The frame metatable, a font string and the C half of SetNormalFontObject,
/// cut down to what kButtonLabelLua calls. The stand-in for the C binding does
/// what it does: keeps the font on the button and gives it to the label the
/// button already has, without making one.
constexpr const char* kFakeButtonLua = R"LUA(
QuestFontLeft = { justifyH = 'LEFT' }
GameFontNormal = { }
GameFontNormalRight = { justifyH = 'RIGHT' }

local function fontString()
    return {
        points = {}, justifyH = 'CENTER',
        GetNumPoints = function(self) return #self.points end,
        SetPoint = function(self, point) table.insert(self.points, point) end,
    }
end
newFontString = fontString

__WoweeFrameMT = {}
__WoweeFrameMT.__index = __WoweeFrameMT
function __WoweeFrameMT:CreateFontString() return fontString() end
function __WoweeFrameMT:SetNormalFontObject(font)
    local object = type(font) == 'string' and _G[font] or font
    self.__normalFontObject = object
    local fs = self.__fontString
    if not fs then return end
    if object and object.justifyH then fs.justifyH = object.justifyH end
end
function __WoweeButtonFont(button)
    if button.__normalFontObject then button:SetNormalFontObject(button.__normalFontObject) end
end
function newButton() return setmetatable({}, __WoweeFrameMT) end
)LUA";

}  // namespace

TEST_CASE("a button's font reaches a ButtonText declared after it", "[addonlua]") {
    // GossipTitleButtonTemplate and QuestTitleButtonTemplate name
    // <NormalFont style="QuestFontLeft"/> and a <ButtonText> 275 wide anchored
    // at the left. The emitter writes the font first, as the XML reads; the
    // label has to come out justified to the left all the same, which is what
    // wow.exe's SetFontString does with the font it stored.
    const std::string got = runChunks({
        {kFakeButtonLua, "FakeButton"},
        {wowee::addons::kButtonLabelLua, "ButtonLabel"},
        {R"LUA(
            local b = newButton()
            b:SetNormalFontObject('QuestFontLeft')
            local text = newFontString()
            text:SetPoint('LEFT')          -- the template's own anchor
            b:SetFontString(text)
            return text.justifyH .. ' ' .. #text.points .. ' ' ..
                   tostring(b:GetFontString() == text) .. ' ' ..
                   tostring(b:GetNormalFontObject() == QuestFontLeft)
        )LUA", "Gossip"},
    });
    CHECK(got == "LEFT 1 true true");
}

TEST_CASE("a label with no anchors sits where its button's font justifies", "[addonlua]") {
    // Left or right for a font that says so, the middle otherwise - which is
    // how every centred button label stays centred.
    const std::string got = runChunks({
        {kFakeButtonLua, "FakeButton"},
        {wowee::addons::kButtonLabelLua, "ButtonLabel"},
        {R"LUA(
            local out = {}
            for _, font in ipairs({ 'GameFontNormal', 'QuestFontLeft', 'GameFontNormalRight' }) do
                local b = newButton()
                b:SetNormalFontObject(font)
                local fs = b:GetFontString()       -- made on demand
                table.insert(out, fs.points[1] .. '/' .. fs.justifyH)
            end
            local plain = newButton():GetFontString()
            table.insert(out, plain.points[1] .. '/' .. plain.justifyH)
            return table.concat(out, ' ')
        )LUA", "Placement"},
    });
    CHECK(got == "CENTER/CENTER LEFT/LEFT RIGHT/RIGHT CENTER/CENTER");
}

// The NOT_IN_LFG notice in the client's language. It was defined in the
// bootstrap, before GetLocale had a profile to read, so a German client got
// it in English. Run again once the locale is known, it replaces what it set
// itself - and leaves a GlobalStrings' own string alone.
TEST_CASE("the NOT_IN_LFG notice follows the client's locale once it is known", "[addonlua]") {
    const char* stubs = R"LUA(
        locale = 'enUS'
        function GetLocale() return locale end
        function __WoweeNoticeFallback(word, loc) return loc .. ':' .. word end
    )LUA";
    SECTION("set early in English, then right") {
        const std::string got = runChunks({
            {stubs, "Stubs"},
            {wowee::addons::kNoticeFallbackLua, "Early"},
            {"locale = 'deDE'", "Locale"},
            {wowee::addons::kNoticeFallbackLua, "Known"},
            {"return CHAT_NOT_IN_LFG_NOTICE", "Read"},
        });
        CHECK(got == "deDE:NOT_IN_LFG");
    }
    SECTION("a GlobalStrings that has it keeps it") {
        const std::string got = runChunks({
            {stubs, "Stubs"},
            {"CHAT_NOT_IN_LFG_NOTICE = 'own'", "GlobalStrings"},
            {"locale = 'deDE'", "Locale"},
            {wowee::addons::kNoticeFallbackLua, "Known"},
            {"return CHAT_NOT_IN_LFG_NOTICE", "Read"},
        });
        CHECK(got == "own");
    }
}

// What playing animation groups make of a region, and that it is given back
// (0x0049b470, 0x00488980).
namespace {
constexpr const char* kAnimationStandIns = R"LUA(
shown = {}
function __WoweeSetAnimOffset(p, x, y) shown.x, shown.y = x, y end
function __WoweeSetAnimRotation(p, r, at) shown.r, shown.rAt = r, at end
function __WoweeSetAnimScale(p, x, y, at) shown.sx, shown.sy, shown.sAt = x, y, at end
region = { alpha = 0.8 }
function region:GetAlpha() return self.alpha end
function region:SetAlpha(a) self.alpha = a end
function group(t)
    local g = { parent = region, isPlaying = true, dx = 0, dy = 0, rad = 0, sx = 1, sy = 1 }
    for k, v in pairs(t) do g[k] = v end
    __WoweePlayingAnimations[g] = true
    return g
end
function finish(g)
    __WoweePlayingAnimations[g] = nil
    g.isPlaying = false
    __WoweeAnimGiveBack(g)
end
function state()
    return shown.x .. ' ' .. shown.y .. ' ' .. shown.r .. ' ' .. shown.sx .. ' ' .. shown.sy .. ' ' .. region.alpha
end
)LUA";
std::string animated(const char* script) {
    return runChunks({{wowee::addons::kAnimationShowLua, "animation"}, {kAnimationStandIns, "stand-ins"}, {script, "script"}});
}
}  // namespace

TEST_CASE("an animation's region is given back as it was", "[animation]") {
    CHECK(animated("local g = group{dx = 5, dy = 60, rad = 1.5, sx = 2, sy = 0.5, alphaChange = -0.3} "
                   "__WoweeShowAnimation(region) return state()") == "5 60 1.5 2 0.5 0.5");
    CHECK(animated("local g = group{dx = 5, dy = 60, rad = 1.5, sx = 2, sy = 0.5, alphaChange = -0.3} "
                   "__WoweeShowAnimation(region) finish(g) return state()") == "0 0 0 1 1 0.8");
    // What a script made of its alpha meanwhile goes with it.
    CHECK(animated("local g = group{alphaChange = -0.3} __WoweeShowAnimation(region) region:SetAlpha(0.1) "
                   "finish(g) return state()") == "0 0 0 1 1 0.8");
    // A group without an Alpha animation leaves the alpha alone.
    CHECK(animated("local g = group{dx = 1} __WoweeShowAnimation(region) region:SetAlpha(0.25) "
                   "__WoweeShowAnimation(region) finish(g) return state()") == "0 0 0 1 1 0.25");
    // Clamped, and between nothing and all.
    CHECK(animated("local g = group{alphaChange = -2} __WoweeShowAnimation(region) return state()") == "0 0 0 1 1 0");
    CHECK(animated("local g = group{alphaChange = 2} __WoweeShowAnimation(region) return state()") == "0 0 0 1 1 1");
}

TEST_CASE("two groups on a region play together", "[animation]") {
    CHECK(animated("local a, b = group{dx = 5, alphaChange = -0.5}, group{dx = 3, dy = 2, sx = 2} "
                   "__WoweeShowAnimation(region) return state()") == "8 2 0 2 1 0.3");
    // One ends: the other goes on, and the alpha it began with is kept for it.
    CHECK(animated("local a, b = group{dx = 5, alphaChange = -0.5}, group{dx = 3, dy = 2, alphaChange = -0.25} "
                   "__WoweeShowAnimation(region) finish(a) local one = state() finish(b) "
                   "return one .. ' then ' .. state()") == "3 2 0 1 1 0.55 then 0 0 0 1 1 0.8");
    // Another region's is no matter of this one.
    CHECK(animated("local a, b = group{dx = 5}, group{dx = 3, alphaChange = -0.5} b.parent = {} "
                   "__WoweeShowAnimation(region) return state()") == "5 0 0 1 1 0.8");
    // One played again by its own script is not given back.
    CHECK(animated("local a = group{dx = 5} __WoweeShowAnimation(region) __WoweeAnimGiveBack(a) return state()") ==
          "5 0 0 1 1 0.8");
    CHECK(animated("local a = group{rad = 1, rotAnim = {originPoint = 'TOP'}, scaleAnim = {originPoint = 'LEFT'}} "
                   "__WoweeShowAnimation(region) return shown.rAt .. ' ' .. shown.sAt") == "TOP LEFT");
}

namespace {
/// Frames, textures and font strings that write down what is done to them.
constexpr const char* kFakeBubbleWorldLua = R"LUA(
made = 0
local function thing()
    local t = {shown = true}
    local function keep(name) return function(self, ...) self['got' .. name] = {...} end end
    for _, n in ipairs({'SetFrameStrata', 'SetBackdrop', 'SetTexture', 'SetHeight', 'SetFont', 'SetTextColor',
                        'SetScale', 'SetAlpha', 'SetFrameLevel', 'ClearAllPoints'}) do t[n] = keep(n) end
    t.points = 0
    function t:SetPoint(...) self.points = self.points + 1; self.at = {...} end
    function t:SetWidth(w) self.w = w end
    function t:SetText(s) self.s = s end
    function t:GetStringWidth() return #self.s * 8 end
    function t:GetHeight() return 12.8 end
    function t:Show() self.shown = true end
    function t:Hide() self.shown = false end
    return t
end
WorldFrame = thing()
ChatTypeInfo = {YELL = {r = 1, g = 0.25, b = 0.25}}
frames = {}
function CreateFrame()
    made = made + 1
    local f = thing()
    function f:CreateTexture() self.tail = thing() return self.tail end
    function f:CreateFontString() self.text = thing() return self.text end
    frames[made] = f
    return f
end
)LUA";
}  // namespace

// 0x0056cad0, 0x0056c7f0, 0x0056c340
TEST_CASE("chat bubbles are the WorldFrame's frames, used again", "[addons][lua][chat_bubble]") {
    const std::string result = runChunks({
        {kFakeBubbleWorldLua, "world"},
        {(std::string("show = (function() ") + wowee::addons::kChatBubblesLua + " end)()").c_str(), "bubbles"},
        {R"LUA(
show(1.5, 1, 'Hi', 'YELL', 0.5, 100, 200)
local f = frames[1]
local out = made .. ' ' .. f.text.w .. ' ' .. f.w .. ' ' .. f.gotSetScale[1] .. ' ' .. f.gotSetAlpha[1] .. ' ' .. f.gotSetFrameLevel[1]
out = out .. ' ' .. f.at[1] .. ' ' .. f.at[3] .. ' ' .. f.at[4] .. ' ' .. f.at[5] .. ' ' .. f.text.gotSetTextColor[2]
-- Less than a unit off: where it was.
show(1.5, 1, 'Hi', 'YELL', 1, 100.9, 199.5)
out = out .. ' ' .. f.points
show(1.5, 1, 'Hi', 'YELL', 1, 101.1, 199.5)
out = out .. ' ' .. f.points .. ' ' .. f.at[4]
-- A second, long one over it; the first is gone, and its frame the next one's.
show(1.5, 1, 'Hi', 'YELL', 1, 101, 200, 2, 'x', 'SAY', 1, 0, 0)
out = out .. ' ' .. made .. ' ' .. frames[2].gotSetFrameLevel[1] .. ' ' .. frames[2].text.gotSetTextColor[2]
show(1.5, 2, 'x', 'SAY', 1, 0, 0)
out = out .. ' ' .. tostring(f.shown)
show(1.5, 2, 'x', 'SAY', 1, 0, 0, 3, '0123456789012345678901234567890123456789', 'SAY', 1, 0, 0)
out = out .. ' ' .. made .. ' ' .. tostring(f.shown) .. ' ' .. f.text.w
show(1.5)
out = out .. ' ' .. tostring(f.shown) .. ' ' .. tostring(frames[2].shown)
return out
)LUA", "check"}});
    CHECK(result == "1 32 57.6 1.5 0.5 2 BOTTOM BOTTOMLEFT 100 187.2 0.25 1 2 101.1 2 3 1 false 2 true 256 false false");
}

// The Sound and Effects pages are among the removed ones (kRemovedCategories),
// so no text may send the player to them.
TEST_CASE("the options' headings name no page that was removed", "[lua][options]") {
    const std::string removed = wowee::addons::kRemovedControlsLua;
    const std::string panel = wowee::addons::kWoweeOptionsPanelLua;
    REQUIRE(removed.find("\"AudioOptionsSoundPanel\"") != std::string::npos);
    REQUIRE(removed.find("\"VideoOptionsEffectsPanel\"") != std::string::npos);
    for (const char* named : {"game's Sound panel", "Soundfenster", "Resolution and Effects", "Auflösung und Effekte"}) {
        CAPTURE(named);
        CHECK(panel.find(named) == std::string::npos);
    }
}

// The question after a changed interface scale, in the client's words, and the
// scale taken back where the interface goes before it is answered.
TEST_CASE("the interface scale question is asked in the client's language", "[addonlua]") {
    const char* stubs = R"LUA(
        math = { ceil = function(n) return n end }
        scale = '0.64'
        function GetCVar() return scale end
        function SetCVar(_, v) scale = v end
        local function frame()
            return { _hooks = {}, RegisterEvent = function(self, e) self._event = e end,
                     SetScript = function(self, s, fn) self._hooks[s] = fn end,
                     HookScript = function(self, s, fn) self._hooks[s] = fn end }
        end
        function CreateFrame() watcher = frame() return watcher end
        VideoOptionsResolutionPanel = frame()
        VideoOptionsFrameOkay = frame()
        function StaticPopup_Show(which, arg1, arg2, data) asked = { which = which, arg1 = arg1, data = data } end
    )LUA";
    const char* german = R"LUA(
        function GetLocale() return 'deDE' end
        YES, NO, CANCEL, D_SECONDS = 'Ja', 'Nein', 'Abbrechen', '%d |4Sekunde:Sekunden;'
    )LUA";
    const char* change = R"LUA(
        VideoOptionsResolutionPanel._hooks.OnShow(VideoOptionsResolutionPanel)
        scale = '1'
        VideoOptionsFrameOkay._hooks.OnClick(VideoOptionsFrameOkay)
    )LUA";
    SECTION("the words") {
        const std::string got = runChunks({
            {stubs, "Stubs"},
            {german, "GlobalStrings"},
            {wowee::addons::kUiScaleConfirmLua, "UiScaleConfirm"},
            {change, "Change"},
            {R"LUA(
                local d = StaticPopupDialogs[asked.which]
                local said
                FooText = { SetFormattedText = function(self, f, n) said = f .. '/' .. n end }
                d.OnUpdate({ timeleft = 14, GetName = function() return 'Foo' end }, 0)
                return d.button1 .. ' ' .. d.button2 .. ' ' .. asked.arg1 .. ' ' .. said ..
                       (said == d.text .. '/14' and '' or ' but shown: ' .. d.text)
            )LUA",
             "Read"},
        });
        CHECK(got ==
              "Ja Nein 15 Möchtet Ihr diese UI-Skalierung behalten?\n\nSie wird in %d |4Sekunde:Sekunden; "
              "zurückgesetzt./14");
    }
    SECTION("unanswered when the interface goes") {
        const std::string got = runChunks({
            {stubs, "Stubs"},
            {german, "GlobalStrings"},
            {wowee::addons::kUiScaleConfirmLua, "UiScaleConfirm"},
            {change, "Change"},
            {"watcher._hooks.OnEvent(watcher, watcher._event) return watcher._event .. ' ' .. scale", "Logout"},
        });
        CHECK(got == "PLAYER_LOGOUT 0.64");
    }
    SECTION("kept when it was answered") {
        const std::string got = runChunks({
            {stubs, "Stubs"},
            {german, "GlobalStrings"},
            {wowee::addons::kUiScaleConfirmLua, "UiScaleConfirm"},
            {change, "Change"},
            {"StaticPopupDialogs[asked.which].OnAccept({}, asked.data)", "Yes"},
            {"watcher._hooks.OnEvent(watcher, watcher._event) return scale", "Logout"},
        });
        CHECK(got == "1");
    }
    // Whether the scale is used at all was left as it had been tried.
    SECTION("no takes the check box back as well") {
        const std::string got = runChunks({
            {stubs, "Stubs"},
            {german, "GlobalStrings"},
            {"cvars = { uiscale = '0.9', useUiScale = '0' } "
             "function GetCVar(n) return cvars[n] end function SetCVar(n, v) cvars[n] = v end "
             "VideoOptionsResolutionPanelUseUIScale = { SetChecked = function(self, on) box = on end }",
             "CVars"},
            {wowee::addons::kUiScaleConfirmLua, "UiScaleConfirm"},
            {"VideoOptionsResolutionPanel._hooks.OnShow(VideoOptionsResolutionPanel) "
             "cvars.uiscale, cvars.useUiScale = '0.76', '1' "
             "VideoOptionsFrameOkay._hooks.OnClick(VideoOptionsFrameOkay)",
             "Change"},
            {"StaticPopupDialogs[asked.which].OnCancel({}, asked.data, 'clicked') "
             "return cvars.uiscale .. ' ' .. cvars.useUiScale .. ' ' .. tostring(box)",
             "No"},
        });
        CHECK(got == "0.9 0 false");
    }
}

TEST_CASE("the About entry is in the client's language", "[addonlua]") {
    const char* stubs = R"LUA(
        function GetLocale() return 'deDE' end
        said = {}
        local function frame()
            local f = { _scripts = {}, SetText = function(self, t) table.insert(said, t) end,
                        SetScript = function(self, s, fn) self._scripts[s] = fn end }
            setmetatable(f, { __index = function(_, k)
                if k == 'CreateFontString' then return frame end
                if k == 'GetHeight' then return function() return 0 end end
                return function() end
            end })
            return f
        end
        function CreateFrame() last = frame() return last end
        GameMenuFrame, GameMenuButtonQuit, GameMenuButtonContinue, UIParent = frame(), frame(), frame(), frame()
        function HideUIPanel() end
    )LUA";
    const std::string got = runChunks({
        {stubs, "Stubs"},
        {wowee::addons::kAboutMenuLua, "About"},
        {"last._scripts.OnClick(last) return table.concat(said, '|')", "Click"},
    });
    CHECK(got == "Über WoWee|WoWee||Ein Client für World of Warcraft, von Grund auf neu geschrieben.|von Kelsi Davis|"
                 "https://github.com/IchbinkeinReh/WoWee|Im Browser öffnen");
}
