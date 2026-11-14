#pragma once

// Where this client keeps what it can make again: the interface files taken
// out of the archives (cache/interface_mpq), the WMO floor caches.
//
// "cache" in the working directory on a desktop and on Android, whose working
// directory is the app's own writable files folder. Never inside an
// application bundle: the iOS client's working directory is its bundle, which
// is read-only, so a cache written there was not written at all - the
// interface files among it, which left the interface reading nothing. There,
// the app's Library/Caches. A macOS bundle (working directory
// Contents/Resources, see main.cpp) is signed, so ~/Library/Caches/Wowee.
// WOWEE_CACHE_ROOT overrides it. Not created here.
//
// Header-only: the editor and the tools link the renderers without the
// client's config_paths.

#include "core/platform.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace wowee::core {

inline std::string getCacheRoot() {
    if (const char* root = std::getenv("WOWEE_CACHE_ROOT"); root && *root) return root;
#if defined(WOWEE_IOS)
    if (const char* home = std::getenv("HOME"); home && *home) {
        return (std::filesystem::path(home) / "Library" / "Caches" / "wowee").string();
    }
#elif defined(__APPLE__)
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec && cwd.filename() == "Resources" && cwd.parent_path().filename() == "Contents") {
        if (const char* home = std::getenv("HOME"); home && *home) {
            return (std::filesystem::path(home) / "Library" / "Caches" / "Wowee").string();
        }
    }
#endif
    return "cache";
}

}  // namespace wowee::core
