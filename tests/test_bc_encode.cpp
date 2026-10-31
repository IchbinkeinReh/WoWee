// The block encoder behind "Texture compression": what it writes decodes,
// through the BLP loader's own DXT decoder, back close to what went in.
#include <catch_amalgamated.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "pipeline/bc_encode.hpp"
#include "pipeline/blp_loader.hpp"

using namespace wowee::pipeline;

namespace {

std::vector<uint8_t> decode(const std::vector<uint8_t>& blocks, int w, int h, bool alpha) {
    BLPImage img;
    img.width = w;
    img.height = h;
    img.format = BLPFormat::BLP2;
    img.compression = alpha ? BLPCompression::DXT5 : BLPCompression::DXT1;
    img.mipmaps.push_back(blocks);
    return BLPLoader::decodeBaseLevel(img);
}

/// Peak signal to noise over the colour channels (and alpha, when asked).
double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, bool withAlpha) {
    double se = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        for (int c = 0; c < (withAlpha ? 4 : 3); ++c) {
            const double d = double(a[i + c]) - double(b[i + c]);
            se += d * d;
            ++n;
        }
    }
    if (se == 0.0) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / (se / double(n)));
}

/// A skin-like image: smooth gradients with some texture.
std::vector<uint8_t> testImage(int w, int h, bool alpha) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(150 + 60 * std::sin(x * 0.05));
            p[1] = static_cast<uint8_t>(100 + 40 * std::cos(y * 0.07));
            p[2] = static_cast<uint8_t>(80 + 30 * std::sin((x + y) * 0.03) + ((x * 7 + y * 13) % 9));
            p[3] = alpha ? static_cast<uint8_t>((x * 255) / (w - 1)) : 255;
        }
    }
    return px;
}

}  // namespace

TEST_CASE("BC1 decodes back close to the source", "[bc_encode]") {
    const int w = 64, h = 64;
    const auto src = testImage(w, h, false);
    const auto blocks = encodeBC(src.data(), w, h, false);
    REQUIRE(blocks.size() == static_cast<size_t>(16 * 16 * 8));
    const auto back = decode(blocks, w, h, false);
    REQUIRE(back.size() == src.size());
    CHECK(psnr(src, back, false) > 36.0);
}

TEST_CASE("BC3 keeps the alpha and the colour", "[bc_encode]") {
    const int w = 64, h = 32;
    const auto src = testImage(w, h, true);
    const auto blocks = encodeBC(src.data(), w, h, true);
    REQUIRE(blocks.size() == static_cast<size_t>(16 * 8 * 16));
    const auto back = decode(blocks, w, h, true);
    REQUIRE(back.size() == src.size());
    CHECK(psnr(src, back, false) > 36.0);
    CHECK(psnr(src, back, true) > 36.0);
}

TEST_CASE("A flat block is exact up to 5:6:5", "[bc_encode]") {
    std::vector<uint8_t> src(16 * 4);
    for (size_t i = 0; i < src.size(); i += 4) {
        src[i] = 255;  // 255 survives 5 bits exactly
        src[i + 1] = 0;
        src[i + 2] = 255;
        src[i + 3] = 255;
    }
    const auto back = decode(encodeBC(src.data(), 4, 4, false), 4, 4, false);
    CHECK(back == src);
}

TEST_CASE("The mip chain runs down to 1x1, odd sizes included", "[bc_encode]") {
    const auto src = testImage(20, 12, false);
    const auto levels = encodeBCWithMips(src.data(), 20, 12, false);
    // 20x12, 10x6, 5x3, 2x1, 1x1
    REQUIRE(levels.mips.size() == 5);
    CHECK(levels.mips[0].size() == static_cast<size_t>(5 * 3 * 8));
    CHECK(levels.mips[1].size() == static_cast<size_t>(3 * 2 * 8));
    CHECK(levels.mips[4].size() == 8u);
}
