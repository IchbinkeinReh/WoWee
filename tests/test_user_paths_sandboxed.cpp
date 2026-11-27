// A test finds none of the player's own folders.
//
// The client keeps its config root (settings, lua_errors.txt, the interface
// state), its data folder, its logs and the Warden cache per user, found
// through APPDATA, LOCALAPPDATA, HOME and WOWEE_CONFIG_ROOT. A test that ran
// with the developer's own environment wrote its Lua error into the player's
// %APPDATA%\wowee\lua_errors.txt. Under ctest and when started by hand alike,
// each of those answers here must lie outside the player's folders
// (WOWEE_TEST_REAL_USER_ROOTS, or the environment the run started with).
#include <catch_amalgamated.hpp>

#include "core/config_paths.hpp"
#include "core/data_paths.hpp"
#include "game/warden_cr_file.hpp"
#include "test_user_paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

using wowee::test::pathIsUnder;
using wowee::test::realUserRoots;

namespace {

void requireOutsideRealRoots(const std::filesystem::path& p, const char* what) {
    INFO(what << " = " << p.string());
    for (const auto& root : realUserRoots()) {
        INFO("the player's folder " << root.string());
        CHECK_FALSE(pathIsUnder(p, root));
    }
}

}  // namespace

TEST_CASE("a test's per-user folders are none of the player's", "[user_paths]") {
    CHECK(std::getenv("WOWEE_TEST_SANDBOX") != nullptr);
    REQUIRE_FALSE(realUserRoots().empty());
    // The player's folders are not the sandbox's own: compared with itself
    // every check below would fail (or, the other way round, pass for nothing).
    if (const char* box = std::getenv("WOWEE_TEST_SANDBOX")) {
        for (const auto& root : realUserRoots()) {
            INFO("the player's folder " << root.string() << ", the sandbox " << box);
            CHECK_FALSE(pathIsUnder(root, box));
        }
    }

    const std::string config = wowee::core::getConfigRoot();
    requireOutsideRealRoots(config, "getConfigRoot()");
    requireOutsideRealRoots(std::filesystem::path(config) / "lua_errors.txt", "lua_errors.txt");
    requireOutsideRealRoots(wowee::core::userDataRoot(), "userDataRoot()");
    requireOutsideRealRoots(wowee::game::wardenCacheDir(), "wardenCacheDir()");

    // The client's defaults beneath an emptied WOWEE_CONFIG_ROOT - what a test
    // that clears it for a case falls back on - are the sandbox's too.
    const std::string saved = config;
    wowee::core::unsetEnvVar("WOWEE_CONFIG_ROOT");
    if (const char* appdata = std::getenv("APPDATA")) {
        requireOutsideRealRoots(std::filesystem::path(appdata) / "wowee", "APPDATA/wowee");
    }
    if (const char* home = std::getenv("HOME")) {
        requireOutsideRealRoots(std::filesystem::path(home) / ".wowee", "~/.wowee");
    }
    wowee::core::setEnvVar("WOWEE_CONFIG_ROOT", saved.c_str());
}

TEST_CASE("pathIsUnder compares whole folder names", "[user_paths]") {
    CHECK(pathIsUnder("C:/Users/x/AppData/Roaming/wowee/lua_errors.txt", "C:/Users/x/AppData/Roaming/wowee"));
    CHECK(pathIsUnder("C:/Users/x/AppData/Roaming/wowee", "C:/Users/x/AppData/Roaming/wowee/"));
    CHECK_FALSE(pathIsUnder("C:/Users/x/AppData/Roaming/wowee-test/a", "C:/Users/x/AppData/Roaming/wowee"));
}
