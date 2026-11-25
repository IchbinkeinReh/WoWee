#pragma once

/// frame[0]: the object behind a frame's table, as a light userdata, and all a
/// method needs of what it is called on (0x004a81b0). The restricted frames
/// call the methods on a table of their own with nothing else in it
/// (restrictedframes.lua, FrameHandleLookup_index), and addons tell a frame by
/// type(frame[0]) == "userdata".

#include <cstdint>
#include <vector>

struct lua_State;
extern "C" {
typedef int (*lua_CFunction)(lua_State* L);
}

namespace wowee::addons {

/// Gives the table at the top of the stack the handle of its widget, with the
/// taint set aside whoever creates it (0x00819880), and keeps the table as the
/// one behind it.
void setFrameHandle(lua_State* L, uint32_t widget);

/// Pushes the table behind a widget, as the client pushes an object - with the
/// caller's taint - or nil for a widget that has none.
void pushFrameByHandle(lua_State* L, uint32_t widget);

/// Pushes the tables behind the widgets, without those that have none, and
/// answers how many; -1, and nothing pushed, where the stack has no room.
int pushObjects(lua_State* L, const std::vector<uint32_t>& widgets);

/// The widget of a handle, 0 for a pointer that is none.
uint32_t widgetOfHandle(const void* handle);

/// What a method is called on, where that is not the frame itself but a table
/// with the frame's handle: the frame takes its place.
void takeSelfByHandle(lua_State* L);

/// Pushes a method that does so first.
void pushMethodByHandle(lua_State* L, lua_CFunction method);

/// The function at the top of the stack, as such a method.
void makeMethodByHandle(lua_State* L);

/// Every method of the table at the top of the stack that is written in Lua,
/// as such a method.
void makeMethodsByHandle(lua_State* L);

}  // namespace wowee::addons
