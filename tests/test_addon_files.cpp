// The files of addons (pipeline/addon_files.hpp) and their pictures
// (pipeline/tga_image.hpp).
#include <catch_amalgamated.hpp>

#include "pipeline/addon_files.hpp"
#include "pipeline/tga_image.hpp"

#include <fstream>

using namespace wowee::pipeline;
namespace fs = std::filesystem;

TEST_CASE("a path under Interface\\AddOns", "[addon_files]") {
    CHECK(addonRelativePath("Interface\\AddOns\\TstM\\kreis.tga") == "TstM/kreis.tga");
    CHECK(addonRelativePath("interface/addons/TstM/a/b.blp") == "TstM/a/b.blp");
    CHECK(addonRelativePath("Interface\\Icons\\a.blp").empty());
    CHECK(addonRelativePath("Interface\\AddOns\\").empty());
    CHECK(addonRelativePath("Interface\\AddOns\\..\\..\\login.cfg").empty());
    CHECK(addonRelativePath("Interface\\AddOns\\C:\\a").empty());
}

TEST_CASE("an addon's file is found in one of the folders", "[addon_files]") {
    const fs::path root = fs::temp_directory_path() / "wowee_addon_files";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "one" / "A", ec);
    fs::create_directories(root / "two" / "TstM" / "Media", ec);
    { std::ofstream(root / "two" / "TstM" / "Media" / "Ton.wav") << "x"; }
    const std::vector<std::string> roots{(root / "nowhere").string(), (root / "one").string(), (root / "two").string()};
    CHECK_FALSE(addonFile(roots, "interface\\addons\\tstm\\media\\ton.wav").empty());
    CHECK(addonFile(roots, "interface\\addons\\tstm\\media").empty());
    CHECK(addonFile(roots, "interface\\addons\\tstm\\media\\other.wav").empty());
    CHECK(addonFile({}, "interface\\addons\\tstm\\media\\ton.wav").empty());
    fs::remove_all(root, ec);
}

namespace {
/// A picture of 8 x 8 whose first pixels are the ones given (BGR or BGRA).
std::vector<uint8_t> tga(uint8_t type, uint8_t bits, uint8_t descriptor, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out(18, 0);
    out[0] = 2;   // two bytes of a name
    out[2] = type;
    out[12] = 8;
    out[14] = 8;
    out[16] = bits;
    out[17] = descriptor;
    out.push_back('a');
    out.push_back('b');
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
std::vector<uint8_t> pixel(const BLPImage& image, int x, int y) {
    const size_t at = static_cast<size_t>(y * image.width + x) * 4;
    return {image.data[at], image.data[at + 1], image.data[at + 2], image.data[at + 3]};
}
}  // namespace

// 0x006aafb0, 0x004b95b0
TEST_CASE("a TGA of 24 and of 32 bits", "[addon_files]") {
    std::vector<uint8_t> body{1, 2, 3};
    body.resize(8 * 8 * 3, 9);
    auto image = loadTga(tga(2, 24, 0, body));
    REQUIRE(image.isValid());
    // From its lower left corner up.
    CHECK(pixel(image, 0, 7) == std::vector<uint8_t>{3, 2, 1, 255});
    CHECK(pixel(image, 0, 0) == std::vector<uint8_t>{9, 9, 9, 255});
    body = {1, 2, 3, 40};
    body.resize(8 * 8 * 4, 7);
    image = loadTga(tga(2, 32, 0x20, body));
    REQUIRE(image.isValid());
    CHECK(pixel(image, 0, 0) == std::vector<uint8_t>{3, 2, 1, 40});
    CHECK(pixel(image, 1, 0) == std::vector<uint8_t>{7, 7, 7, 7});
}

TEST_CASE("a run-length encoded TGA", "[addon_files]") {
    // Five of one pixel, two of their own, and the rest in one.
    std::vector<uint8_t> body{0x84, 1, 2, 3, 0x01, 4, 5, 6, 7, 8, 9};
    for (int left = 8 * 8 - 7; left > 0; left -= 57) {
        body.push_back(static_cast<uint8_t>(0x80 | (left - 1)));
        body.insert(body.end(), {10, 11, 12});
    }
    const auto image = loadTga(tga(10, 24, 0x20, body));
    REQUIRE(image.isValid());
    CHECK(pixel(image, 4, 0) == std::vector<uint8_t>{3, 2, 1, 255});
    CHECK(pixel(image, 5, 0) == std::vector<uint8_t>{6, 5, 4, 255});
    CHECK(pixel(image, 6, 0) == std::vector<uint8_t>{9, 8, 7, 255});
    CHECK(pixel(image, 7, 7) == std::vector<uint8_t>{12, 11, 10, 255});
}

TEST_CASE("what is no picture", "[addon_files]") {
    CHECK_FALSE(loadTga({}).isValid());
    CHECK_FALSE(loadTga(tga(2, 16, 0, std::vector<uint8_t>(128, 0))).isValid());
    CHECK_FALSE(loadTga(tga(1, 24, 0, std::vector<uint8_t>(192, 0))).isValid());
    CHECK_FALSE(loadTga(tga(2, 24, 0, std::vector<uint8_t>(191, 0))).isValid());
    CHECK_FALSE(loadTga(tga(10, 24, 0, {0x87, 1, 2, 3})).isValid());
    auto small = tga(2, 24, 0, std::vector<uint8_t>(192, 0));
    small[12] = 4;
    CHECK_FALSE(loadTga(small).isValid());
    CHECK_FALSE(loadInterfaceImage({'B', 'L', 'P', '2'}).isValid());
}
