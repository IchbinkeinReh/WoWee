#pragma once

#include <string>

namespace wowee::core {

// Absolute path to the directory holding the running executable.
// Empty if it cannot be determined.
std::string getExecutableDir();

// Root directory for user config (login.cfg, settings.cfg, last_character.cfg,
// characters/). Two modes:
//   - Portable: if a "portable.txt" marker file, or an existing "config" folder,
//     sits next to the executable, config lives in <exe_dir>/config. This keeps
//     the whole client self-contained in one folder (USB sticks, clean uninstall,
//     easy backup of server profiles).
//   - Per-user (default): %APPDATA%\wowee on Windows, ~/.wowee elsewhere.
std::string getConfigRoot();

// The game data folder the player chose, or empty when they have not chosen
// one and the default search applies (WOW_DATA_PATH, then the per-user data
// folder, then Data/ beside the client). Kept in <config root>/data_folder.txt,
// one path on one line, so it can also be written or removed by hand.
std::string getCustomDataRoot();
// Remembers PATH as the game data folder, or forgets the choice when PATH is
// empty. Read once at startup, so a new choice takes effect on the next start.
// False when the file could not be written or removed.
bool setCustomDataRoot(const std::string& path);

// The access bookmark that goes with the chosen folder, where the platform
// needs one to open it again (iOS: a folder outside the app's own is only
// reachable through the security-scoped bookmark the document picker grants).
// Base64, in <config root>/data_folder_bookmark.txt; empty when there is none.
// Setting an empty one removes the file.
std::string getCustomDataBookmark();
bool setCustomDataBookmark(const std::string& base64);

// The streaming server the game data is read from instead of a folder (the
// "streaming" data mode, pipeline/data_stream): its address and access key,
// in <config root>/data_stream.txt (address on the first line, key on the
// second; the file is made readable by its owner only where the platform
// allows). Empty url = no streaming. WOWEE_STREAM_URL and
// WOWEE_STREAM_KEY_FILE (a file holding the key) override it.
struct DataStreamChoice {
    std::string url;
    std::string key;
};
DataStreamChoice getDataStream();
// Remembers the server, or forgets it when url is empty. Read once at
// startup. False when the file could not be written or removed.
bool setDataStream(const DataStreamChoice& choice);

// One-time seeding of portable config. On the first launch after the user drops
// a "portable.txt" marker next to the executable (before any config folder
// exists), copies the existing per-user config tree into <exe_dir>/config so
// saved server profiles, settings, and characters carry over. No-op afterwards,
// and a no-op when not in portable mode. Call once at startup before config is
// read.
void migratePortableConfigIfNeeded();

// Enters the directory named by WOWEE_RESOURCE_ROOT, which holds assets/ and
// Data/ in the layout a desktop install has.
//
// Only Android sets that variable, and only Android needs this: a process there
// starts in a directory holding neither, and shaders, interface art and the
// expansion profiles are all opened through relative paths. It is not enough to
// do this once at startup, because SDL and the Vulkan driver leave the working
// directory at /system/bin, so this is called again once they are up.
//
// A no-op wherever WOWEE_RESOURCE_ROOT is unset. Returns false only if the
// variable names a directory that cannot be entered.
bool enterResourceRoot();

}  // namespace wowee::core
