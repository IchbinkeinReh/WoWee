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
    self.__normalFontObject = font
    local fs = self.__fontString
    if not fs then return end
    local object = type(font) == 'string' and _G[font] or font
    if object and object.justifyH then fs.justifyH = object.justifyH end
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

// WoW's bit functions take any number of values. Ours took two, so
// constants.lua's five-way bit.bor made every combat log filter the first two
// bits alone - COMBATLOG_FILTER_MINE an incomplete mask that CombatLogAddFilter
// refused on every entry into the world.
TEST_CASE("bit.band, bor and bxor take any number of values", "[addonlua]") {
    const std::string got = runChunks({
        {wowee::addons::kBitLibLua, "BitLib"},
        {R"LUA(
            -- constants.lua's own construction.
            local mine = bit.bor(0x00000001, 0x00000010, 0x00000100, 0x00000400, 0x00004000)
            local out = {
                mine,                               -- 0x4511
                bit.bor(5),                         -- one value
                bit.band(0xFF, 0x0F, 0x03),         -- 3
                bit.bxor(1, 3, 4),                  -- 6
                bit.bor(0x80000000, 1),             -- 2147483649, unsigned
                bit.band(-1, 0xFF),                 -- 255: -1 is 0xFFFFFFFF
                bit.bnot(0),                        -- 4294967295
                bit.lshift(1, 4), bit.rshift(0x4511, 8),
            }
            for i = 1, #out do out[i] = tostring(out[i]) end
            return table.concat(out, ' ')
        )LUA", "Use"},
    });
    CHECK(got == "17681 5 3 6 2147483649 255 4294967295 16 69");
}
