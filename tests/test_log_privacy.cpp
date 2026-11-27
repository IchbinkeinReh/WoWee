// The home directory, and so the player's account name, kept out of the log.
//
// Reported with a log that had the name redacted by hand from every path in
// it: the data folder, the config and the integrity-hash search all live under
// the home directory, and each was logged in full.
#include "catch_amalgamated.hpp"
#include "core/log_location.hpp"
#include "core/log_privacy.hpp"

#include <string>

using wowee::core::redactHome;

TEST_CASE("a path under the home directory is written from ~", "[log_privacy]") {
    CHECK(redactHome("manifest.json not found in: /Users/sam/Library/Application Support/Wowee/Data",
                     "/Users/sam") ==
          "manifest.json not found in: ~/Library/Application Support/Wowee/Data");
    CHECK(redactHome("missing: /Users/sam/twmoa_1180/wow.exe). Set", "/Users/sam") ==
          "missing: ~/twmoa_1180/wow.exe). Set");
}

TEST_CASE("every occurrence goes, and the home itself", "[log_privacy]") {
    CHECK(redactHome("/home/sam/a and /home/sam/b", "/home/sam") == "~/a and ~/b");
    CHECK(redactHome("HOME is /home/sam", "/home/sam") == "HOME is ~");
    CHECK(redactHome("'/home/sam'", "/home/sam/") == "'~'");
}

TEST_CASE("a longer name that starts the same is not a match", "[log_privacy]") {
    CHECK(redactHome("/Users/k/Data and /Users/kelsey/Data", "/Users/k") ==
          "~/Data and /Users/kelsey/Data");
    CHECK(redactHome("/home/sam.old/x", "/home/sam") == "/home/sam.old/x");
}

TEST_CASE("no home, or a root home, changes nothing", "[log_privacy]") {
    CHECK(redactHome("/Users/sam/x", "") == "/Users/sam/x");
    CHECK(redactHome("/Users/sam/x", "/") == "/Users/sam/x");
}

TEST_CASE("either separator matches", "[log_privacy]") {
    CHECK(redactHome("C:/Users/Sam/AppData/Local/Wowee", "C:\\Users\\Sam") ==
          "~/AppData/Local/Wowee");
    CHECK(redactHome("C:\\Users\\Sam\\AppData", "C:\\Users\\Sam") == "~\\AppData");
}

// Where the log goes (core/log_location.hpp): never inside an app bundle -
// under LiveContainer an iOS guest .app is writable and wowee.log landed in
// it, where the Files app does not show it.
TEST_CASE("an app bundle logs to the per-user directory", "[log_privacy][log_location]") {
    using wowee::core::logToPerUserDirectory;
    CHECK(logToPerUserDirectory("", true));   // iOS: always
    CHECK(logToPerUserDirectory("/var/mobile/Containers/Data/Application/X/Documents/Applications/wowee.app/wowee", true));
    CHECK(logToPerUserDirectory("/Applications/Wowee.app/Contents/MacOS/wowee", false));
    CHECK(logToPerUserDirectory("/private/var/containers/Bundle/Application/X/wowee.app/wowee", false));
    CHECK_FALSE(logToPerUserDirectory("/home/sam/wowee/build/bin/wowee", false));
    CHECK_FALSE(logToPerUserDirectory("C:/Games/wowee/wowee.exe", false));
}
