// What a frame takes from a thread of its own once it is there (BuiltBeside),
// and that the views the input-taking screens draw are made so: the character
// selection's renderer and its effects' compiled pipelines for 13 s on the
// frame that took the keys, with the driver's cache empty.
#include <catch_amalgamated.hpp>

#include "rendering/built_beside.hpp"

#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>

using wowee::rendering::BuiltBeside;

TEST_CASE("a thing made beside the frames is taken once it is there", "[built_beside]") {
    std::promise<void> gate;
    std::shared_future<void> open = gate.get_future().share();
    BuiltBeside<int> built;
    CHECK_FALSE(built.started());
    CHECK(built.take() == nullptr);
    built.begin([open]() {
        open.wait();
        return std::make_unique<int>(42);
    });
    CHECK(built.started());
    CHECK(built.pending());
    // Not waited for: the frame goes on.
    CHECK(built.take() == nullptr);
    CHECK(built.started());
    gate.set_value();
    const auto got = built.take(true);
    REQUIRE(got != nullptr);
    CHECK(*got == 42);
    // Once.
    CHECK_FALSE(built.started());
    CHECK(built.take(true) == nullptr);
}

TEST_CASE("a thing that could not be made is taken as none", "[built_beside]") {
    BuiltBeside<int> built;
    built.begin([]() { return std::unique_ptr<int>(); });
    CHECK(built.take(true) == nullptr);
    CHECK_FALSE(built.started());
}

TEST_CASE("one not taken is waited for when its holder goes", "[built_beside]") {
    std::promise<void> gate;
    std::shared_future<void> open = gate.get_future().share();
    auto done = std::make_shared<bool>(false);
    {
        auto built = std::make_unique<BuiltBeside<int>>();
        built->begin([open, done]() {
            open.wait();
            *done = true;
            return std::make_unique<int>(1);
        });
        gate.set_value();
    }
    CHECK(*done);
}

namespace {
std::string sourceText(const char* path) {
    std::ifstream in(std::string(WOWEE_SOURCE_DIR) + "/" + path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}  // namespace

// The character selection's view: the renderer and the effects' renderer are
// initialized inside BuiltBeside's work, never on the frame, and the screen
// asks without waiting.
TEST_CASE("the character selection's view is made beside the frames", "[built_beside]") {
    const std::string preview = sourceText("src/rendering/character_preview.cpp");
    REQUIRE_FALSE(preview.empty());
    CHECK(preview.find("rendererBuilding_.begin(") != std::string::npos);
    CHECK(preview.find("effectsBuilding_.begin(") != std::string::npos);
    CHECK(preview.find("charRenderer_->initialize(") == std::string::npos);
    CHECK(preview.find("effects_->initialize(") == std::string::npos);
    const std::string screen = sourceText("src/ui/character_screen.cpp");
    REQUIRE_FALSE(screen.empty());
    CHECK(screen.find("preview_->beginInitialize(assetManager_)") != std::string::npos);
    CHECK(screen.find("preview_->initializeDone()") != std::string::npos);
    CHECK(screen.find("preview_->initialize(") == std::string::npos);
}
