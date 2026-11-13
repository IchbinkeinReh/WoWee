#pragma once

/// What the streaming data mode is fetching, as one line on the screens the
/// player waits at: the login and character screens and the loading screen.
///
/// A first start on an empty cache fetches several hundred megabytes before
/// the world shows (about 3.5 minutes on the test server), and the loading
/// screen's bar moves by terrain tiles, so it stood still meanwhile. The line
/// is not in the original client - nor is streaming - and it is drawn only
/// while data is arriving: it goes once nothing was fetched for a few seconds.

#include <cstdint>
#include <string>

struct ImDrawList;

namespace wowee::ui {

/// The line, e.g. "Spieldaten vom Server: 123,4 MB in 456 Dateien (2,1 MB/s)".
/// `rateBytesPerSecond` < 0 leaves the rate out.
std::string streamProgressText(uint64_t bytes, uint64_t files, double rateBytesPerSecond, bool german);

/// Draws the line along the bottom of a `width` x `height` screen when the
/// streaming mode fetched something in the last few seconds; else nothing.
void drawStreamProgress(ImDrawList* drawList, float width, float height);

}  // namespace wowee::ui
