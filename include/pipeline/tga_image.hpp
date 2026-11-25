#pragma once

#include "pipeline/blp_loader.hpp"

#include <cstdint>
#include <vector>

namespace wowee::pipeline {

/// A TGA's picture as the client takes it (0x006aafb0): true colour of 24 or 32
/// bits, plain (type 2) or run-length encoded (10), from its lower left corner
/// up or from the upper left down (descriptor 0x20), and at least 8 pixels
/// either way (0x004b95b0). Without an alpha channel it is opaque. An image that
/// is not valid for anything else.
BLPImage loadTga(const std::vector<uint8_t>& data);

/// A picture of the interface: a BLP's or a TGA's.
BLPImage loadInterfaceImage(const std::vector<uint8_t>& data);

}  // namespace wowee::pipeline
