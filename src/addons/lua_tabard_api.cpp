// lua_tabard_api.cpp - The tabard designer (TabardModelFrame.cpp,
// 0x00598dd0-0x00599b20): CloseTabardCreation, and the TabardModel frame's
// methods (0x00acf588) over the design it keeps (+0x380..+0x390).
#include "addons/lua_api_helpers.hpp"
#include "addons/lua_api_registrations.hpp"
#include "addons/lua_taint.hpp"
#include "game/game_handler.hpp"
#include "game/tabard_design.hpp"
#include "ui/widget_tree.hpp"

#include <cmath>
#include <random>

namespace wowee::addons {

namespace {

namespace tb = game::tabard;

ui::Widget* objectOf(lua_State* L, int index) {
    auto* tree = getWidgetTree(L);
    if (!tree || !lua_istable(L, index)) return nullptr;
    taint::Quiet quiet;
    lua_getfield(L, index, "__wid");
    const auto id = static_cast<uint32_t>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return tree->get(id);
}

/// The TabardModel the method was called on (0x004a81b0 with the frame
/// type's id); every other frame answers nothing.
ui::Widget* tabardModelOf(lua_State* L) {
    ui::Widget* w = objectOf(L, 1);
    return w && w->objectType == "TabardModel" ? w : nullptr;
}

tb::Design designOf(const ui::Widget& w) { return tb::Design{w.tabardDesign}; }

/// 0x004ec1c0: the design on the player's own model, which the frame shows.
void showOnPlayer(lua_State* L, const tb::Design& d) {
    if (auto* gh = getGameHandler(L)) gh->previewTabard(d.emblem());
}

int lua_InitializeTabardColors(lua_State* L) {
    // 0x005997c0 -> 0x00599670: the guild's design, else one at random.
    ui::Widget* w = tabardModelOf(L);
    auto* gh = getGameHandler(L);
    if (!w || !gh) return 0;
    static std::mt19937 rng{std::random_device{}()};
    const tb::Design d = tb::initial(gh->getPlayerGuildEmblem(), [](int32_t n) {
        return std::uniform_int_distribution<int32_t>(0, n - 1)(rng);
    });
    w->tabardDesign = d.values;
    showOnPlayer(L, d);
    return 0;
}

int lua_TabardSave(lua_State* L) {
    // 0x005994c0 -> 0x00599410 -> 0x006db320.
    ui::Widget* w = tabardModelOf(L);
    auto* gh = getGameHandler(L);
    if (w && gh) gh->saveGuildEmblem(designOf(*w));
    return 0;
}

int lua_CycleVariation(lua_State* L) {
    // 0x00599500: CycleVariation(variationIndex 1-5, delta).
    ui::Widget* w = tabardModelOf(L);
    if (!w) return 0;
    if (!lua_isnumber(L, 2) || !lua_isnumber(L, 3)) return luaL_error(L, "Usage: CycleVariation(variationIndex, delta)");
    const auto index = static_cast<int64_t>(std::llround(lua_tonumber(L, 2))) - 1;
    if (index < 0 || index > 4) return luaL_error(L, "Invalid variationIndex in CycleVariation");
    tb::Design d = designOf(*w);
    tb::cycle(d, static_cast<size_t>(index), static_cast<int32_t>(lua_tonumber(L, 3)));
    w->tabardDesign = d.values;
    showOnPlayer(L, d);
    return 0;
}

int pushFile(lua_State* L, bool emblem, bool upper) {
    ui::Widget* w = tabardModelOf(L);
    if (!w) return 0;
    const auto& v = w->tabardDesign;
    const std::string file = emblem ? tb::emblemFile(v[0], v[1], upper) : tb::backgroundFile(v[4], upper);
    lua_pushstring(L, file.c_str());
    return 1;
}
int lua_GetUpperBackgroundFileName(lua_State* L) { return pushFile(L, false, true); }   // 0x005991d0
int lua_GetLowerBackgroundFileName(lua_State* L) { return pushFile(L, false, false); }  // 0x00599240
int lua_GetUpperEmblemFileName(lua_State* L) { return pushFile(L, true, true); }        // 0x005992b0
int lua_GetLowerEmblemFileName(lua_State* L) { return pushFile(L, true, false); }       // 0x00599320

/// 0x00599890 / 0x00599b20: the emblem's half drawn on a texture - white, by
/// the emblem's own alpha alone.
int setEmblemTexture(lua_State* L, bool upper) {
    ui::Widget* w = tabardModelOf(L);
    if (!w) return 0;
    const char* method = upper ? "GetUpperEmblemTexture" : "GetLowerEmblemTexture";
    const char* name = w->name.empty() ? "<unnamed>" : w->name.c_str();
    if (!lua_istable(L, 2)) return luaL_error(L, "Usage: %s:%s(texture)", name, method);
    ui::Widget* texture = objectOf(L, 2);
    if (!texture) return luaL_error(L, "%s:%s(): Couldn't find 'this' in texture object", name, method);
    if (texture->kind != ui::WidgetKind::Texture)
        return luaL_error(L, "%s:%s(): Wrong object type, expected texture", name, method);
    const auto& v = w->tabardDesign;
    texture->texturePath = tb::emblemFile(v[0], v[1], upper);
    texture->emblemMask = true;
    return 0;
}
int lua_GetUpperEmblemTexture(lua_State* L) { return setEmblemTexture(L, true); }
int lua_GetLowerEmblemTexture(lua_State* L) { return setEmblemTexture(L, false); }

int lua_CanSaveTabardNow(lua_State* L) {
    // 0x00598fa0 -> 0x006de1c0.
    auto* gh = getGameHandler(L);
    if (tabardModelOf(L) && gh && gh->canSaveTabardNow()) {
        lua_pushnumber(L, 1);
        return 1;
    }
    return luaReturnNil(L);
}

int lua_CloseTabardCreation(lua_State* L) {
    // 0x00598f30 -> 0x00598ec0.
    if (auto* gh = getGameHandler(L)) gh->closeTabardVendor();
    return 0;
}

}  // namespace

void addTabardModelMethods(lua_State* L) {
    static const luaL_Reg kMethods[] = {
        {"InitializeTabardColors", lua_InitializeTabardColors},
        {"Save", lua_TabardSave},
        {"CycleVariation", lua_CycleVariation},
        {"GetUpperBackgroundFileName", lua_GetUpperBackgroundFileName},
        {"GetLowerBackgroundFileName", lua_GetLowerBackgroundFileName},
        {"GetUpperEmblemFileName", lua_GetUpperEmblemFileName},
        {"GetLowerEmblemFileName", lua_GetLowerEmblemFileName},
        {"GetUpperEmblemTexture", lua_GetUpperEmblemTexture},
        {"GetLowerEmblemTexture", lua_GetLowerEmblemTexture},
        {"CanSaveTabardNow", lua_CanSaveTabardNow},
    };
    for (const luaL_Reg& r : kMethods) {
        lua_pushcfunction(L, r.func);
        lua_setfield(L, -2, r.name);
    }
}

void registerTabardLuaAPI(lua_State* L) {
    lua_pushcfunction(L, lua_CloseTabardCreation);
    lua_setglobal(L, "CloseTabardCreation");
}

}  // namespace wowee::addons
