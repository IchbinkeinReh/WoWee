#pragma once

/// The Lua libraries the interface has, as the client opens them: the base
/// library of 0x00a47c80, string (0x00a478e0), table (0x00a47798), math
/// (0x00a47600) and bit (0x00a475a0). No io, os, debug or package, and of the
/// base library no dofile, loadfile and load - but collectgarbage, which
/// addons ask for the memory they use.

struct lua_State;

namespace wowee::addons {

void openInterfaceLibs(lua_State* L);

/// What the client's own chunk (0x00643180 of the file) sets from them besides
/// the aliases LuaEngine sets one by one: foreach and foreachi, and once
/// strtrim and its kin are there, string.trim, split, join and replace -
/// ("a b"):trim() is how an addon calls them.
void setInterfaceLibAliases(lua_State* L);

/// Takes out the functions 3.3.5 has not got, which are here for the older
/// interfaces or came in later: an addon written for several clients asks by
/// them which one it is in.
void dropForeignGlobals(lua_State* L);

}  // namespace wowee::addons
