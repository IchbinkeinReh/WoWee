#include "pipeline/tga_image.hpp"

#include <cstddef>

namespace wowee::pipeline {

BLPImage loadTga(const std::vector<uint8_t>& data) {
    BLPImage image;
    if (data.size() < 18) return image;
    const auto u16 = [&](size_t at) { return static_cast<size_t>(data[at] | (data[at + 1] << 8)); };
    const uint8_t type = data[2], bits = data[16], descriptor = data[17];
    const size_t width = u16(12), height = u16(14);
    if (data[1] != 0 || (type != 2 && type != 10) || (bits != 24 && bits != 32) || width < 8 || height < 8) return image;
    const size_t bytes = bits / 8;
    size_t at = size_t{18} + data[0];
    std::vector<uint8_t> out(width * height * 4);
    size_t pixel = 0;
    const auto put = [&](size_t from) {
        const size_t x = pixel % width, line = pixel / width;
        const size_t y = (descriptor & 0x20) != 0 ? line : height - 1 - line;
        const size_t column = (descriptor & 0x10) != 0 ? width - 1 - x : x;
        uint8_t* p = out.data() + (y * width + column) * 4;
        p[0] = data[from + 2];
        p[1] = data[from + 1];
        p[2] = data[from];
        p[3] = bytes == 4 ? data[from + 3] : 255;
        ++pixel;
    };
    const size_t pixels = width * height;
    while (pixel < pixels) {
        size_t run = 1;
        bool repeated = false;
        if (type == 10) {
            if (at >= data.size()) return image;
            run = size_t{1} + (data[at] & 0x7F);
            repeated = (data[at] & 0x80) != 0;
            ++at;
        }
        for (size_t i = 0; i < run && pixel < pixels; ++i) {
            if (at + bytes > data.size()) return image;
            put(at);
            if (!repeated) at += bytes;
        }
        if (repeated) at += bytes;
    }
    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    image.channels = 4;
    image.alphaDepth = bytes == 4 ? 8 : 0;
    image.data = std::move(out);
    return image;
}

BLPImage loadInterfaceImage(const std::vector<uint8_t>& data) {
    const bool blp = data.size() >= 4 && data[0] == 'B' && data[1] == 'L' && data[2] == 'P';
    return blp ? BLPLoader::load(data) : loadTga(data);
}

}  // namespace wowee::pipeline
