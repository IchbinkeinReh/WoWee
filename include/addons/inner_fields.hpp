#pragma once

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <cstring>
#include <string_view>

/// What the client keeps in its object of a frame, a texture or a font string, and this one
/// in its table. The client's table has its handle at 0 and what the interface has put there,
/// so next, pairs, table.foreach and the writer of saved variables pass these over: an addon
/// that walks a frame's fields met its parent and hid it.
namespace wowee::addons::inner_fields {

/// In order, for the search.
inline constexpr std::string_view kNames[] = {
    "__CheckedTexture", "__DisabledCheckedTexture", "__DisabledTexture", "__HighlightTexture", "__NormalTexture",
    "__PushedTexture",  "__achievementId",          "__animBaseAlpha",   "__animGroups",       "__attributes",
    "__barTexture",     "__clicks",                 "__disabledFontObject", "__events",        "__fontString",
    "__g",              "__highlightFontObject",    "__htmlFonts",      "__hyperlink",       "__id",               "__initFunc",
    "__isEditBox",      "__isSimpleHtml",           "__itemId",          "__lines",            "__linkRolled",
    "__name",           "__normalFontObject",       "__onUpdateFailures", "__own",             "__owner",
    "__parent",         "__pendingQuestId",         "__scripts",         "__scrollChild",      "__setPieces",
    "__spellId",        "__text",                   "__visible",         "__wid",              "__woweePreview",
};

[[nodiscard]] inline bool named(std::string_view key) {
    if (key.size() < 3 || key[0] != '_' || key[1] != '_') return false;
    return std::binary_search(std::begin(kNames), std::end(kNames), key);
}

/// Whether the key at `key` of the table at `table` is one of these.
[[nodiscard]] inline bool isInner(lua_State* L, int table, int key) {
    if (lua_type(L, key) != LUA_TSTRING) return false;
    size_t n = 0;
    const char* s = lua_tolstring(L, key, &n);
    if (!named(std::string_view(s, n))) return false;
    lua_pushliteral(L, "__wid");
    lua_rawget(L, table);
    const bool object = lua_type(L, -1) == LUA_TNUMBER;
    lua_pop(L, 1);
    return object;
}

/// lua_next without them; `table` an index from the bottom.
[[nodiscard]] inline bool next(lua_State* L, int table) {
    while (lua_next(L, table) != 0) {
        if (!isInner(L, table, lua_gettop(L) - 1)) return true;
        lua_pop(L, 1);
    }
    return false;
}

}  // namespace wowee::addons::inner_fields
