// BLP loader tests: isValid, format names, invalid data handling
#include <catch_amalgamated.hpp>
#include "pipeline/blp_loader.hpp"

using wowee::pipeline::BLPLoader;
using wowee::pipeline::BLPImage;
using wowee::pipeline::BLPFormat;
using wowee::pipeline::BLPCompression;

TEST_CASE("BLPImage default is invalid", "[blp]") {
    BLPImage img;
    REQUIRE_FALSE(img.isValid());
    REQUIRE(img.width == 0);
    REQUIRE(img.height == 0);
    REQUIRE(img.format == BLPFormat::UNKNOWN);
    REQUIRE(img.compression == BLPCompression::NONE);
}

TEST_CASE("BLPImage isValid with data", "[blp]") {
    BLPImage img;
    img.width = 64;
    img.height = 64;
    img.data.resize(64 * 64 * 4, 0xFF);  // RGBA
    REQUIRE(img.isValid());
}

TEST_CASE("BLPImage isValid requires non-empty data", "[blp]") {
    BLPImage img;
    img.width = 64;
    img.height = 64;
    // data is empty
    REQUIRE_FALSE(img.isValid());
}

TEST_CASE("BLPLoader::load empty data returns invalid", "[blp]") {
    std::vector<uint8_t> empty;
    auto img = BLPLoader::load(empty);
    REQUIRE_FALSE(img.isValid());
}

TEST_CASE("BLPLoader::load too small data returns invalid", "[blp]") {
    std::vector<uint8_t> tiny = {0x42, 0x4C, 0x50}; // BLP but truncated
    auto img = BLPLoader::load(tiny);
    REQUIRE_FALSE(img.isValid());
}

TEST_CASE("BLPLoader::load invalid magic returns invalid", "[blp]") {
    // Provide enough bytes but with wrong magic
    std::vector<uint8_t> bad(256, 0);
    bad[0] = 'N'; bad[1] = 'O'; bad[2] = 'T'; bad[3] = '!';
    auto img = BLPLoader::load(bad);
    REQUIRE_FALSE(img.isValid());
}

TEST_CASE("BLPLoader getFormatName returns non-null", "[blp]") {
    REQUIRE(BLPLoader::getFormatName(BLPFormat::UNKNOWN) != nullptr);
    REQUIRE(BLPLoader::getFormatName(BLPFormat::BLP1) != nullptr);
    REQUIRE(BLPLoader::getFormatName(BLPFormat::BLP2) != nullptr);

    // Check that names are distinct
    REQUIRE(std::string(BLPLoader::getFormatName(BLPFormat::BLP1)) !=
            std::string(BLPLoader::getFormatName(BLPFormat::BLP2)));
}

TEST_CASE("BLPLoader getCompressionName returns non-null", "[blp]") {
    REQUIRE(BLPLoader::getCompressionName(BLPCompression::NONE) != nullptr);
    REQUIRE(BLPLoader::getCompressionName(BLPCompression::DXT1) != nullptr);
    REQUIRE(BLPLoader::getCompressionName(BLPCompression::DXT3) != nullptr);
    REQUIRE(BLPLoader::getCompressionName(BLPCompression::DXT5) != nullptr);
    REQUIRE(BLPLoader::getCompressionName(BLPCompression::PALETTE) != nullptr);
    REQUIRE(BLPLoader::getCompressionName(BLPCompression::ARGB8888) != nullptr);

    // Check that DXT names differ
    REQUIRE(std::string(BLPLoader::getCompressionName(BLPCompression::DXT1)) !=
            std::string(BLPLoader::getCompressionName(BLPCompression::DXT5)));
}

TEST_CASE("BLPImage mipmap storage", "[blp]") {
    BLPImage img;
    img.width = 128;
    img.height = 128;
    img.data.resize(128 * 128 * 4, 0);
    img.mipLevels = 3;

    // Add mipmap data
    img.mipmaps.push_back(std::vector<uint8_t>(64 * 64 * 4, 0));
    img.mipmaps.push_back(std::vector<uint8_t>(32 * 32 * 4, 0));
    img.mipmaps.push_back(std::vector<uint8_t>(16 * 16 * 4, 0));

    REQUIRE(img.isValid());
    REQUIRE(img.mipmaps.size() == 3);
}

// The ray tracer's cutout of a leaf card (BLPImage::alphaMask): which of a
// 64x64 grid is visible, as the shadow map's alpha test at half sees it.
TEST_CASE("alphaMask marks the visible texels of a decoded image", "[blp]") {
    BLPImage img;
    img.width = 128;
    img.height = 32;
    img.data.assign(size_t(128) * 32 * 4, 255);
    for (int y = 0; y < 32; ++y) {
        for (int x = 64; x < 128; ++x) img.data[(size_t(y) * 128 + x) * 4 + 3] = 0;  // right half clear
    }
    std::array<uint64_t, 64> mask{};
    REQUIRE(img.alphaMask(mask));
    for (uint64_t row : mask) CHECK(row == 0x00000000FFFFFFFFull);
}

TEST_CASE("alphaMask reads DXT1's punch-through", "[blp]") {
    BLPImage img;
    img.width = 64;
    img.height = 64;
    img.compression = BLPCompression::DXT1;
    // 16x16 blocks. colour0 <= colour1 is the three-colour mode, index 3
    // transparent: the left half's blocks index 0, the right half's 3.
    std::vector<uint8_t> blocks;
    for (int by = 0; by < 16; ++by) {
        for (int bx = 0; bx < 16; ++bx) {
            const uint8_t idx = bx < 8 ? 0x00 : 0xFF;
            for (uint8_t b : {uint8_t(0x00), uint8_t(0x00), uint8_t(0xFF), uint8_t(0xFF), idx, idx, idx, idx}) {
                blocks.push_back(b);
            }
        }
    }
    img.mipmaps.push_back(blocks);
    std::array<uint64_t, 64> mask{};
    REQUIRE(img.alphaMask(mask));
    for (uint64_t row : mask) CHECK(row == 0x00000000FFFFFFFFull);
}
