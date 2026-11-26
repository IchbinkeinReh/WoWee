// Catch2's main, with the player's own folders kept out of reach.
//
// Run by ctest, a test already has per-user folders of its own
// (cmake/WoweeTestSandbox.cmake) and the fixture there watches the player's
// real ones. Started by hand - a test executable run on its own to look at
// one case - it would still find %APPDATA%\wowee and write its Lua errors,
// settings and logs there; that is how a throwaway test's error ended up in
// the player's lua_errors.txt. So without WOWEE_TEST_SANDBOX this points every
// per-user variable at a temporary folder for the run, removes it afterwards,
// and fails the run if the player's folders changed while it ran.

#define CATCH_AMALGAMATED_CUSTOM_MAIN
#include "catch_amalgamated.cpp"

#include "test_user_paths.hpp"

#include <cstdio>

int main(int argc, char* argv[]) {
    // As Catch2's own main: keep the leak detector's registration linked.
    (void)&Catch::leakDetector;

    wowee::test::UserPathSandbox sandbox;
    int result = Catch::Session().run(argc, argv);

    const std::vector<std::string> touched = sandbox.touchedRealFiles();
    if (!touched.empty()) {
        std::fprintf(stderr, "\nThe run changed the player's own wowee folders:\n");
        for (const std::string& f : touched) std::fprintf(stderr, "  %s\n", f.c_str());
        if (result == 0) result = 1;
    }
    return result;
}
