// No weather drawn on the screen itself.
//
// The interface used to paint rain as ImGui lines over everything
// (GameScreen::renderWeatherOverlay): 300 short strokes falling down the
// window at a fixed pixel speed whenever the server named a weather, the same
// lines whatever the camera did - rain "drawn in non 3d space" over the
// world's own drops. The client draws its weather only in the world
// (0x0078ae20: the drops, their splashes and the mist sheets, all in the
// scene), and that is the only rain WoWee keeps (rendering/weather.cpp).
//
// This reads the interface's sources: nothing under src/ui or include/ui may
// ask the game for its weather and then draw strokes or dots with ImGui near
// it, and the overlay itself must not come back.
#include <catch_amalgamated.hpp>
#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

#ifdef WOWEE_SOURCE_DIR
const std::filesystem::path kRoot = WOWEE_SOURCE_DIR;
#else
const std::filesystem::path kRoot = ".";
#endif

std::vector<std::filesystem::path> uiSources() {
    std::vector<std::filesystem::path> out;
    for (const char* dir : {"src/ui", "include/ui"}) {
        const auto root = kRoot / dir;
        if (!std::filesystem::exists(root)) continue;
        for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
            const auto ext = e.path().extension().string();
            if (ext == ".cpp" || ext == ".hpp" || ext == ".h") out.push_back(e.path());
        }
    }
    return out;
}

std::string readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("the interface draws no weather of its own", "[weather][ui]") {
    const auto files = uiSources();
    REQUIRE(files.size() > 20);
    for (const auto& f : files) {
        const std::string text = readAll(f);
        INFO(f.string());
        CHECK(text.find("renderWeatherOverlay") == std::string::npos);
        // Wherever the weather is read, no strokes or dots drawn within the
        // next 120 lines' worth of text (the old overlay drew its first line
        // some 60 lines after reading the weather).
        for (const char* ask : {"getWeatherType()", "getWeatherIntensity()", "isRaining()", "isSnowing()"}) {
            for (size_t at = text.find(ask); at != std::string::npos; at = text.find(ask, at + 1)) {
                const std::string after = text.substr(at, 6000);
                CHECK(after.find("AddLine(") == std::string::npos);
                CHECK(after.find("AddCircleFilled(") == std::string::npos);
                CHECK(after.find("AddRectFilledMultiColor(") == std::string::npos);
            }
        }
    }
}
