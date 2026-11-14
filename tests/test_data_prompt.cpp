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
    CHECK(dataPromptAtStartup(false, false, false, "", false, false) == DataPrompt::NoData);
    CHECK(dataPromptAtStartup(false, false, true, "", false, false) == DataPrompt::None);
}

TEST_CASE("Android never asks in the client: its activity did", "[data_prompt]") {
    CHECK(dataPromptAtStartup(true, false, false, "", false, false) == DataPrompt::None);
    CHECK(dataPromptAtStartup(true, false, false, "/sdcard/gone", false, false) == DataPrompt::None);
}

TEST_CASE("A build with the asset builder shows that instead", "[data_prompt]") {
    CHECK(dataPromptAtStartup(false, true, false, "", false, false) == DataPrompt::None);
}

TEST_CASE("A chosen folder that has gone is said so", "[data_prompt]") {
    CHECK(dataPromptAtStartup(false, false, false, "/x/gone", false, false) ==
          DataPrompt::ChosenFolderGone);
    // Gone, but the default search found data anyway: no prompt.
    CHECK(dataPromptAtStartup(false, false, true, "/x/here", true, false) == DataPrompt::None);
    CHECK(dataPromptAtStartup(false, false, false, "/x/here", true, false) == DataPrompt::NoData);
}

TEST_CASE("A streaming server that did not start is said, on every platform", "[data_prompt]") {
    // iOS: the screen came back after every restart with the server's fields
    // filled in and nothing said. Android's activity let the server through,
    // so the client opened a login with no data. A desktop's asset builder
    // has no fields for a server. All three show the data screen and why.
    CHECK(dataPromptAtStartup(false, false, false, "", false, true) == DataPrompt::StreamFailed);
    CHECK(dataPromptAtStartup(true, false, false, "", false, true) == DataPrompt::StreamFailed);
    CHECK(dataPromptAtStartup(false, true, false, "", false, true) == DataPrompt::StreamFailed);
    // Even with a folder that holds data: the player chose the server.
    CHECK(dataPromptAtStartup(false, false, true, "/x/here", true, true) == DataPrompt::StreamFailed);
    // Started: the streamed archives are the data, as any other.
    CHECK(dataPromptAtStartup(false, false, true, "", false, false) == DataPrompt::None);
}

TEST_CASE("Which starts stream", "[data_prompt]") {
    // A kept server streams unless WOWEE_DATA_MODE says otherwise.
    CHECK(streamingChosen("", "https://server/wowee-data/"));
    CHECK_FALSE(streamingChosen("", ""));
    CHECK_FALSE(streamingChosen("mpq", "https://server/wowee-data/"));
    CHECK_FALSE(streamingChosen("extracted", "https://server/wowee-data/"));
    // WOWEE_DATA_MODE=streaming streams; with no server that is a failure
    // the start reports, not a quiet fallback to a folder.
    CHECK(streamingChosen("streaming", ""));
    CHECK(streamingChosen("streaming", "https://server/wowee-data/"));
}

TEST_CASE("What the screen says when streaming failed", "[data_prompt]") {
    const StreamFailureView key =
        describeStreamFailure("ichbinkeinreh.de", "the server refused the access key");
    // The heading is a line of its own, kept to 24 characters - the limit a
    // German translation of a short string has to meet as well.
    CHECK(key.heading == "Streaming failed");
    CHECK(key.heading.size() <= 24);
    CHECK(key.body.find("ichbinkeinreh.de") != std::string::npos);
    CHECK(key.body.find("the server refused the access key") != std::string::npos);
    CHECK(key.body.find("Check the access key") != std::string::npos);

    const StreamFailureView tls = describeStreamFailure(
        "ichbinkeinreh.de",
        "cannot get the file list: TLS handshake failed: certificate verify failed");
    CHECK(tls.body.find("certificate could not be checked") != std::string::npos);

    const StreamFailureView down =
        describeStreamFailure("ichbinkeinreh.de", "cannot get the file list: status 503");
    CHECK(down.body.find("data service is not answering") != std::string::npos);

    const StreamFailureView net =describeStreamFailure("nowhere.invalid", "cannot reach nowhere.invalid");
    CHECK(net.body.find("Check the address and the access key") != std::string::npos);

    // Nothing to go on is still a sentence.
    const StreamFailureView bare = describeStreamFailure("", "");
    CHECK(bare.body.find("the server") != std::string::npos);
    CHECK(bare.body.find("no reason was given") != std::string::npos);
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
