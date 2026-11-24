#pragma once

// A frame's picture on its way to somebody: cut, made smaller, put into text.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wowee::mcp {

struct Picture {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgb;
};

/// A part of a frame as Vulkan hands it over, four bytes a pixel, blue first.
/// The part is held to the frame; an empty one is the whole of it.
Picture cutFrame(const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height, uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h);
/// No wider than `most`, each pixel the mean of the ones it stands for.
Picture shrink(const Picture& picture, uint32_t most);

std::string base64(const std::vector<uint8_t>& bytes);

/// "image/jpeg" or "image/png"; empty when it could not be made.
std::vector<uint8_t> encodeJpeg(const Picture& picture, int quality);
std::vector<uint8_t> encodePng(const Picture& picture);

}  // namespace wowee::mcp
