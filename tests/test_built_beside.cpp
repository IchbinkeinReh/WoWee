// What a frame takes from a thread of its own once it is there (BuiltBeside),
// and that the views the input-taking screens draw are made so: the character
// selection's renderer and its effects' compiled pipelines for 13 s on the
// frame that took the keys, with the driver's cache empty.
#include <catch_amalgamated.hpp>
#include "test_support.hpp"

#include "rendering/built_beside.hpp"

#include <chrono>
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

// The loading screen's wait: the thread that needs the thing draws a frame
// each step it is not there, and has it the moment it is.
TEST_CASE("a thing waited for keeps the screen going meanwhile", "[built_beside]") {
    std::promise<void> gate;
    std::shared_future<void> open = gate.get_future().share();
    BuiltBeside<int> built;
    built.begin([open]() {
        open.wait();
        return std::make_unique<int>(7);
    });
    int frames = 0;
    const auto got = built.takeWaiting(
        [&] {
            // The thing is made only after the screen has had frames.
            if (++frames == 3) gate.set_value();
        },
        std::chrono::milliseconds(1));
    REQUIRE(got != nullptr);
    CHECK(*got == 7);
    CHECK(frames >= 3);
    CHECK_FALSE(built.started());
    // Never begun: none, and no frame.
    BuiltBeside<int> none;
    int idle = 0;
    CHECK(none.takeWaiting([&] { ++idle; }) == nullptr);
    CHECK(idle == 0);
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

// And the interface's model frames (PlayerModel, DressUpModel, any Model):
// their M2 renderer is made beside the frames too, and taken before the
// target its pipelines are for goes.
TEST_CASE("a model frame's renderer is made beside the frames", "[built_beside]") {
    const std::string view = sourceText("src/rendering/model_frame_view.cpp");
    REQUIRE_FALSE(view.empty());
    CHECK(view.find("m2Building_.begin(") != std::string::npos);
    CHECK(view.find("m2_->initialize(") == std::string::npos);
    const size_t taken = view.find("m2_ = m2Building_.take(true);");
    const size_t destroyed = view.find("destroyTarget();");
    REQUIRE(taken != std::string::npos);
    REQUIRE(destroyed != std::string::npos);
    CHECK(taken < destroyed);
}

// Every other view of a figure: the creation screen's and the units'
// portraits begin it and take it when it is there; nothing waits on a frame.
TEST_CASE("no screen waits for a figure's renderer", "[built_beside]") {
    for (const char* path : {"src/ui/character_create_screen.cpp", "src/ui/unit_portrait.cpp"}) {
        const std::string text = sourceText(path);
        INFO(path);
        REQUIRE_FALSE(text.empty());
        CHECK(text.find("->beginInitialize(") != std::string::npos);
        CHECK(text.find("->initializeDone()") != std::string::npos);
        CHECK(text.find("preview_->initialize(") == std::string::npos);
    }
}

// The first world entry's renderers: the world's M2 renderer (some hundred and
// fifty pipelines, 17 s on a cold driver cache, one over 1.5 s), the sky's
// two and the character renderer are made beside the loading screen, which
// this thread keeps drawing while it waits; none is initialized on it.
TEST_CASE("the world's renderers are made beside the loading screen", "[built_beside]") {
    const std::string renderer = sourceText("src/rendering/renderer.cpp");
    REQUIRE_FALSE(renderer.empty());
    for (const char* begun : {"m2Building.begin(", "skyBuilding.begin(", "starsBuilding.begin(",
                              "characterBuilding.begin("}) {
        INFO(begun);
        CHECK(renderer.find(begun) != std::string::npos);
    }
    for (const char* onThread : {"m2Renderer->initialize(", "skyboxModelRenderer_->initialize(",
                                 "starsModelRenderer_->initialize(", "characterRenderer->initialize("}) {
        INFO(onThread);
        CHECK(renderer.find(onThread) == std::string::npos);
    }
    CHECK(renderer.find("takeWaiting(pump)") != std::string::npos);
    // The loading screen's bar counts the pipelines made on every thread.
    const std::string loader = sourceText("src/core/world_loader.cpp");
    REQUIRE_FALSE(loader.empty());
    CHECK(loader.find("rendering::pipelinesBuilt()") != std::string::npos);
}
