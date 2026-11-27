#pragma once

// Whether the client's log (and what is written beside it) goes to the
// per-user directory rather than "logs/" under the working directory.
//
// main() moves the working directory to the executable's. Inside an Apple
// application bundle that is the bundle itself: on macOS its
// Contents/MacOS (Resources after main's move), on iOS the flat .app folder.
// A log there is inside the app, where nobody finds it - on macOS without
// Show Package Contents, on iOS never: a normal install's bundle is read-only
// (the open fails and the per-user directory is taken), but under
// LiveContainer a guest .app lives in LiveContainer's writable
// Documents/Applications, the open succeeded and wowee.log landed in the
// bundle, which the Files app does not show. So on iOS always, and for any
// executable inside a .app, the log goes to the per-user directory
// ($HOME/Documents/logs on iOS, where stall_report's crash_report.txt goes).

#include <string_view>

namespace wowee::core {

[[nodiscard]] inline bool logToPerUserDirectory(std::string_view executablePath, bool ios) {
    if (ios) return true;
    return executablePath.find(".app/") != std::string_view::npos;
}

}  // namespace wowee::core
