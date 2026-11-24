// The encoders, apart from what needs none (image.cpp) so that a test of that
// does not link them.

#include "mcp/image.hpp"

#include <algorithm>

#include "stb_image_write.h"

namespace wowee::mcp {

namespace {

void append(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

}  // namespace

std::vector<uint8_t> encodeJpeg(const Picture& picture, int quality) {
    std::vector<uint8_t> out;
    if (picture.rgb.empty() ||
        stbi_write_jpg_to_func(append, &out, static_cast<int>(picture.width), static_cast<int>(picture.height), 3,
                               picture.rgb.data(), std::clamp(quality, 1, 100)) == 0) {
        return {};
    }
    return out;
}

std::vector<uint8_t> encodePng(const Picture& picture) {
    std::vector<uint8_t> out;
    if (picture.rgb.empty() ||
        stbi_write_png_to_func(append, &out, static_cast<int>(picture.width), static_cast<int>(picture.height), 3,
                               picture.rgb.data(), static_cast<int>(picture.width * 3)) == 0) {
        return {};
    }
    return out;
}

}  // namespace wowee::mcp
