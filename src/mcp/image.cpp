#include "mcp/image.hpp"

#include <algorithm>

namespace wowee::mcp {

Picture cutFrame(const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height, uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h) {
    Picture out;
    if (width == 0 || height == 0 || bgra.size() < static_cast<size_t>(width) * height * 4) return out;
    if (w == 0 || h == 0) {
        x = y = 0;
        w = width;
        h = height;
    }
    x = std::min(x, width - 1);
    y = std::min(y, height - 1);
    out.width = std::min(w, width - x);
    out.height = std::min(h, height - y);
    out.rgb.resize(static_cast<size_t>(out.width) * out.height * 3);
    for (uint32_t row = 0; row < out.height; ++row) {
        const uint8_t* from = bgra.data() + (static_cast<size_t>(y + row) * width + x) * 4;
        uint8_t* to = out.rgb.data() + static_cast<size_t>(row) * out.width * 3;
        for (uint32_t column = 0; column < out.width; ++column, from += 4, to += 3) {
            to[0] = from[2];
            to[1] = from[1];
            to[2] = from[0];
        }
    }
    return out;
}

Picture shrink(const Picture& picture, uint32_t most) {
    if (most == 0 || picture.width <= most) return picture;
    Picture out;
    out.width = most;
    out.height = std::max<uint32_t>(
        1, static_cast<uint32_t>((static_cast<uint64_t>(picture.height) * most + picture.width / 2) / picture.width));
    out.rgb.resize(static_cast<size_t>(out.width) * out.height * 3);
    for (uint32_t y = 0; y < out.height; ++y) {
        const auto y0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * picture.height / out.height);
        const auto y1 = std::max(y0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * picture.height / out.height));
        for (uint32_t x = 0; x < out.width; ++x) {
            const auto x0 = static_cast<uint32_t>(static_cast<uint64_t>(x) * picture.width / out.width);
            const auto x1 =
                std::max(x0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * picture.width / out.width));
            uint32_t sum[3] = {0, 0, 0};
            for (uint32_t sy = y0; sy < y1; ++sy) {
                const uint8_t* p = picture.rgb.data() + (static_cast<size_t>(sy) * picture.width + x0) * 3;
                for (uint32_t sx = x0; sx < x1; ++sx, p += 3) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }
            const uint32_t n = (y1 - y0) * (x1 - x0);
            uint8_t* to = out.rgb.data() + (static_cast<size_t>(y) * out.width + x) * 3;
            for (int c = 0; c < 3; ++c) to[c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
        }
    }
    return out;
}

std::string base64(const std::vector<uint8_t>& bytes) {
    constexpr const char* kDigits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t b0 = bytes[i];
        const uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
        out += kDigits[(v >> 18) & 63];
        out += kDigits[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? kDigits[(v >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? kDigits[v & 63] : '=';
    }
    return out;
}

}  // namespace wowee::mcp
