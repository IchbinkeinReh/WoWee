// When the client asks where the game data is (core/data_prompt.hpp).
//
// iOS showed no data-folder prompt at all: Android asks in DataFolderActivity
// before the native client starts, iOS has no screen in front of the client,
// and the client's own chooser was compiled out on every phone. These pin the
// decisions the iOS screen is driven by, on a machine that cannot build iOS.

#include <catch2/catch_amalgamated.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/data_prompt.hpp"

namespace fs = std::filesystem;
using namespace wowee::core;

namespace {

struct Sandbox {
    fs::path root;
    Sandbox() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() /
               ("wowee_prompt_" + std::to_string(counter.fetch_add(1)) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(root);
    }
    ~Sandbox() { std::error_code ec; fs::remove_all(root, ec); }
    void extraction(const fs::path& at) {
        fs::create_directories(at / "expansions" / "wotlk");
        std::ofstream(at / "expansions" / "wotlk" / "manifest.json") << "{}";
    }
};

}  // namespace

TEST_CASE("iOS without data asks; with data it does not", "[data_prompt]") {
    // iOS: no pre-launch mask, no asset builder.
    CHECK(dataPromptAtStartup(false, false, false, "", false) == DataPrompt::NoData);
    CHECK(dataPromptAtStartup(false, false, true, "", false) == DataPrompt::None);
}

TEST_CASE("Android never asks in the client: its activity did", "[data_prompt]") {
    CHECK(dataPromptAtStartup(true, false, false, "", false) == DataPrompt::None);
    CHECK(dataPromptAtStartup(true, false, false, "/sdcard/gone", false) == DataPrompt::None);
}

TEST_CASE("A build with the asset builder shows that instead", "[data_prompt]") {
    CHECK(dataPromptAtStartup(false, true, false, "", false) == DataPrompt::None);
}

TEST_CASE("A chosen folder that has gone is said so", "[data_prompt]") {
    CHECK(dataPromptAtStartup(false, false, false, "/x/gone", false) ==
          DataPrompt::ChosenFolderGone);
    // Gone, but the default search found data anyway: no prompt.
    CHECK(dataPromptAtStartup(false, false, true, "/x/here", true) == DataPrompt::None);
    CHECK(dataPromptAtStartup(false, false, false, "/x/here", true) == DataPrompt::NoData);
}

TEST_CASE("The platform's own pre-launch flag", "[data_prompt]") {
#if defined(__ANDROID__)
    CHECK(kPreLaunchDataMask);
#else
    CHECK_FALSE(kPreLaunchDataMask);
#endif
}

TEST_CASE("A picked folder stands for itself or its Data folder", "[data_prompt]") {
    Sandbox s;
    CHECK(resolveDataFolderChoice({}).empty());
    CHECK(resolveDataFolderChoice(s.root).empty());

    const fs::path around = s.root / "around";
    s.extraction(around / "Data");
    CHECK(resolveDataFolderChoice(around) == around / "Data");

    const fs::path direct = s.root / "direct";
    s.extraction(direct);
    CHECK(resolveDataFolderChoice(direct) == direct);
}

TEST_CASE("What the screen says", "[data_prompt]") {
    const std::string help = "Copy it into the Files app's Wowee folder.";
    DataFolderView none = describeDataFolder("/docs", false, false, DataPrompt::NoData, help);
    CHECK(none.status == "No game data in /docs.");
    CHECK(none.help == help);
    CHECK_FALSE(none.found);
    CHECK_FALSE(none.offerDefault);

    DataFolderView gone =
        describeDataFolder("/ext/wow", true, false, DataPrompt::ChosenFolderGone, help);
    CHECK(gone.status == "The chosen game data folder /ext/wow is not there.");
    CHECK(gone.offerDefault);

    DataFolderView found = describeDataFolder("/docs", false, true, DataPrompt::NoData, help);
    CHECK(found.found);
    CHECK(found.status == "Game data found in /docs.");
    CHECK(found.help == "Close WoWee and open it again to play with it.");

    // No platform text: the general one.
    DataFolderView plain = describeDataFolder("/d", false, false, DataPrompt::NoData, "");
    CHECK(plain.help.find("Copy an extraction") == 0);
}
