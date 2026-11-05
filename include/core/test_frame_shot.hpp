#pragma once

// A test hook (with WOWEE_AUTO_LUA only): a screenshot of the frame in hand,
// asked for from Lua (__WoweeTestFrameShot) and taken by the main loop, for a
// run of consecutive frames. Screenshot() names its files by the second.

#include <string>

namespace wowee::core {
inline std::string testFrameShotPath;
}  // namespace wowee::core
