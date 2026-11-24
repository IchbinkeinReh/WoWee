#pragma once

/// A frame's attributes (0x0049f2d0, 0x0049f610, 0x00493230): by their names in
/// any case, with nothing kept of who set them.

#include <string>

struct lua_State;

namespace wowee::addons {

/// The name as it is kept, and as OnAttributeChanged has it (0x0076f6e0).
std::string attributeKey(const char* name);

/// The value at the top of the stack becomes the attribute of the frame at
/// `frame`, and is taken off.
void storeFrameAttribute(lua_State* L, int frame, const char* name);

/// Pushes the attribute of the frame at `frame` with the caller's taint, as the
/// client pushes one by its reference; whether it has one (nothing pushed
/// where it has not).
bool pushFrameAttribute(lua_State* L, int frame, const std::string& name);

/// frame:GetAttribute("name") or (prefix, "name", suffix) with the frame at 1:
/// the name with both about it, "*" for either, "*" for both, the name alone.
/// The usage line names the frame `who`.
int getFrameAttribute(lua_State* L, const char* who);

/// Pushes what OnAttributeChanged is called with (0x0048ec10): the frame at
/// `frame`, the name in lower case and the value at `value` - nothing of whoever
/// sets it on any of the three.
void pushAttributeChange(lua_State* L, int frame, const char* name, int value);

}  // namespace wowee::addons
