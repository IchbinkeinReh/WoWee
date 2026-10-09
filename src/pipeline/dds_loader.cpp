#include "pipeline/dds_loader.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace wowee {
namespace pipeline {

namespace {

constexpr uint32_t kMagic = 0x20534444u;  // "DDS "

// The file's first 128 bytes: the magic, then the 124-byte DDS_HEADER with
// its DDS_PIXELFORMAT inside it.
struct DdsPixelFormatDisk {
    uint32_t size;
    uint32_t flags;               // DDPF_*
    uint32_t fourCC;
    uint32_t rgbBitCount;
    uint32_t rBitMask;
    uint32_t gBitMask;
    uint32_t bBitMask;
    uint32_t aBitMask;
};
struct DdsFileHeaderDisk {
    uint32_t magic;               // "DDS "
    uint32_t headerSize;          // DDS_HEADER's own size, 124
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth;
    uint32_t mipMapCount;
    uint32_t reserved1[11];
    DdsPixelFormatDisk pixelFormat;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};
static_assert(sizeof(DdsFileHeaderDisk) == 128,
              "DdsFileHeaderDisk is the magic plus the 124-byte DDS_HEADER, no padding");
static_assert(offsetof(DdsFileHeaderDisk, mipMapCount) == 28 &&
              offsetof(DdsFileHeaderDisk, pixelFormat) == 76,
              "DdsFileHeaderDisk fields must sit where DDS puts them");
constexpr uint32_t kDdsHeaderSize = sizeof(DdsFileHeaderDisk) - sizeof(uint32_t);

// DDS_HEADER_DXT10, which follows when the four-character code is "DX10".
struct DdsDx10HeaderDisk {
    uint32_t dxgiFormat;
    uint32_t resourceDimension;
    uint32_t miscFlag;
    uint32_t arraySize;
    uint32_t miscFlags2;
};
static_assert(sizeof(DdsDx10HeaderDisk) == 20,
              "DdsDx10HeaderDisk is read straight from the file: 20 bytes, no padding");

constexpr uint32_t kFourCC(char a, char b, char c, char d) {
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}

// A disk struct at `offset`; the caller has checked the file holds it.
template <typename T>
T readDisk(const std::vector<uint8_t>& data, size_t offset) {
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

/// The four-character code as it reads in a log, so a refusal names the format
/// the file actually claims rather than a number.
std::string fourCCText(uint32_t fourCC) {
    std::string out(4, ' ');
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((fourCC >> (i * 8)) & 0xFF);
        out[static_cast<size_t>(i)] = (c >= 32 && c < 127) ? c : '?';
    }
    return out;
}

}  // namespace

size_t DdsLoader::levelBytes(BLPCompression compression, int width, int height) {
    const size_t blocksX = static_cast<size_t>(std::max(1, (width + 3) / 4));
    const size_t blocksY = static_cast<size_t>(std::max(1, (height + 3) / 4));
    const size_t blockSize = (compression == BLPCompression::DXT1) ? 8u : 16u;
    return blocksX * blocksY * blockSize;
}

BLPImage DdsLoader::load(const std::vector<uint8_t>& ddsData) {
    BLPImage image;
    if (ddsData.size() < sizeof(DdsFileHeaderDisk)) {
        LOG_WARNING("DDS too small: ", ddsData.size(), " bytes");
        return image;
    }
    const auto header = readDisk<DdsFileHeaderDisk>(ddsData, 0);
    if (header.magic != kMagic || header.headerSize != kDdsHeaderSize) {
        LOG_WARNING("DDS header not recognised");
        return image;
    }

    const int height = static_cast<int>(header.height);
    const int width = static_cast<int>(header.width);
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        LOG_WARNING("DDS dimensions out of range: ", width, "x", height);
        return image;
    }

    constexpr uint32_t kDdpfFourCC = 0x4u;
    if ((header.pixelFormat.flags & kDdpfFourCC) == 0) {
        // An uncompressed DDS has nothing this path wants: the PNG sidecar is
        // the way to hand over RGBA8, and it is already read.
        LOG_WARNING("DDS is not block compressed; use a .png sidecar instead");
        return image;
    }

    const uint32_t fourCC = header.pixelFormat.fourCC;
    size_t dataOffset = sizeof(DdsFileHeaderDisk);
    BLPCompression compression = BLPCompression::NONE;
    if (fourCC == kFourCC('D', 'X', 'T', '1')) {
        compression = BLPCompression::DXT1;
    } else if (fourCC == kFourCC('D', 'X', 'T', '3')) {
        compression = BLPCompression::DXT3;
    } else if (fourCC == kFourCC('D', 'X', 'T', '5')) {
        compression = BLPCompression::DXT5;
    } else if (fourCC == kFourCC('D', 'X', '1', '0')) {
        // Written by texconv and by Pillow's "BC3" spelling. The extra header
        // carries a DXGI format where the legacy one carried a four-character
        // code; the sRGB variants are the same blocks, and the renderer
        // uploads UNORM either way.
        if (ddsData.size() < sizeof(DdsFileHeaderDisk) + sizeof(DdsDx10HeaderDisk)) {
            LOG_WARNING("DDS claims a DX10 header but is too small to hold one");
            return image;
        }
        const auto dx10 = readDisk<DdsDx10HeaderDisk>(ddsData, sizeof(DdsFileHeaderDisk));
        dataOffset = sizeof(DdsFileHeaderDisk) + sizeof(DdsDx10HeaderDisk);
        switch (dx10.dxgiFormat) {
            case 71: case 72: compression = BLPCompression::DXT1; break;  // BC1_UNORM(_SRGB)
            case 74: case 75: compression = BLPCompression::DXT3; break;  // BC2_UNORM(_SRGB)
            case 77: case 78: compression = BLPCompression::DXT5; break;  // BC3_UNORM(_SRGB)
            default:
                LOG_WARNING("DDS DXGI format ", dx10.dxgiFormat,
                            " is not BC1/BC2/BC3");
                return image;
        }
    } else {
        LOG_WARNING("DDS format '", fourCCText(fourCC), "' is not DXT1/DXT3/DXT5");
        return image;
    }

    const uint32_t declaredMips = header.mipMapCount;
    const uint32_t mipCount = std::max(1u, declaredMips);

    size_t at = dataOffset;
    int w = width;
    int h = height;
    for (uint32_t level = 0; level < mipCount; ++level) {
        const size_t bytes = levelBytes(compression, w, h);
        if (at + bytes > ddsData.size()) {
            // A chain that stops early is still usable as far as it goes; one
            // that has no level zero is not.
            if (level == 0) {
                LOG_WARNING("DDS is shorter than its own level 0 (", width, "x", height, ")");
                return image;
            }
            LOG_WARNING("DDS mip chain ends at level ", level, " of ", mipCount,
                        "; the rest of the file is missing");
            break;
        }
        image.mipmaps.emplace_back(ddsData.begin() + static_cast<long>(at),
                                   ddsData.begin() + static_cast<long>(at + bytes));
        at += bytes;
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }

    image.width = width;
    image.height = height;
    image.channels = 4;
    image.format = BLPFormat::BLP2;
    image.compression = compression;
    image.mipLevels = static_cast<int>(image.mipmaps.size());
    return image;
}

}  // namespace pipeline
}  // namespace wowee
