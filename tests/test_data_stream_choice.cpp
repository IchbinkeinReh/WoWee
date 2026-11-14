// The streaming server kept between starts (core::getDataStream /
// setDataStream) and whether a start streams (core::streamingChosen).
//
// The iOS app came back to its data screen after every restart with the
// server's address and key filled in. The fields are read from the same file
// the start reads, so these pin that what the screen keeps is what the next
// start finds and chooses - the half of that path any machine can run. Why
// the start then failed on the phone was its trust store (test_tls_trust).

#include <catch2/catch_amalgamated.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/config_paths.hpp"
#include "core/data_prompt.hpp"
#include "core/env.hpp"

namespace fs = std::filesystem;
using namespace wowee::core;

namespace {

/// A config root of its own (WOWEE_CONFIG_ROOT, which wins over everything),
/// and no streaming overrides from whoever runs the test.
struct ConfigSandbox {
    fs::path root;
    ConfigSandbox() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() /
               ("wowee_stream_choice_" + std::to_string(counter.fetch_add(1)) + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(root);
        setEnvVar("WOWEE_CONFIG_ROOT", root.string().c_str());
        unsetEnvVar("WOWEE_STREAM_URL");
        unsetEnvVar("WOWEE_STREAM_KEY_FILE");
    }
    ~ConfigSandbox() {
        unsetEnvVar("WOWEE_CONFIG_ROOT");
        unsetEnvVar("WOWEE_STREAM_URL");
        unsetEnvVar("WOWEE_STREAM_KEY_FILE");
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    fs::path file() const { return root / "data_stream.txt"; }
};

}  // namespace

TEST_CASE("Nothing kept: no server, and the start does not stream", "[data_stream_choice]") {
    ConfigSandbox box;
    const DataStreamChoice none = getDataStream();
    CHECK(none.url.empty());
    CHECK(none.key.empty());
    CHECK_FALSE(streamingChosen("", none.url));
}

TEST_CASE("What the screen keeps, the next start finds and streams from", "[data_stream_choice]") {
    ConfigSandbox box;
    REQUIRE(setDataStream({"https://ichbinkeinreh.de/wowee-data/", "not-the-real-key"}));
    REQUIRE(fs::exists(box.file()));

    // The next start, as Application::initialize reads it.
    const DataStreamChoice kept = getDataStream();
    CHECK(kept.url == "https://ichbinkeinreh.de/wowee-data/");
    CHECK(kept.key == "not-the-real-key");
    CHECK(streamingChosen("", kept.url));
    // The folder modes a developer forces still win.
    CHECK_FALSE(streamingChosen("mpq", kept.url));

    // Address on the first line, key on the second, nothing else.
    std::ifstream in(box.file());
    std::string line1;
    std::string line2;
    std::string line3;
    std::getline(in, line1);
    std::getline(in, line2);
    CHECK(line1 == kept.url);
    CHECK(line2 == kept.key);
    CHECK_FALSE(static_cast<bool>(std::getline(in, line3)));
}

TEST_CASE("A file written by hand is read without its stray whitespace", "[data_stream_choice]") {
    ConfigSandbox box;
    std::ofstream(box.file(), std::ios::binary) << "  https://server/wowee-data \r\n\tkey-with-cr\r\n";
    const DataStreamChoice kept = getDataStream();
    CHECK(kept.url == "https://server/wowee-data");
    CHECK(kept.key == "key-with-cr");
}

TEST_CASE("Stop streaming forgets the server", "[data_stream_choice]") {
    ConfigSandbox box;
    REQUIRE(setDataStream({"https://server/wowee-data/", "k"}));
    REQUIRE(setDataStream({}));
    CHECK_FALSE(fs::exists(box.file()));
    CHECK(getDataStream().url.empty());
    CHECK_FALSE(streamingChosen("", getDataStream().url));
    // Forgetting what is not kept is not a failure.
    CHECK(setDataStream({}));
}

TEST_CASE("The environment overrides what is kept", "[data_stream_choice]") {
    ConfigSandbox box;
    REQUIRE(setDataStream({"https://kept/wowee-data/", "kept-key"}));
    const fs::path keyFile = box.root / "key.txt";
    std::ofstream(keyFile) << "env-key\n";
    setEnvVar("WOWEE_STREAM_URL", "https://env/wowee-data/");
    setEnvVar("WOWEE_STREAM_KEY_FILE", keyFile.string().c_str());
    const DataStreamChoice c = getDataStream();
    CHECK(c.url == "https://env/wowee-data/");
    CHECK(c.key == "env-key");
}
