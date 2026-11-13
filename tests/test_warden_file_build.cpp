// A WoW.exe says which build it is.
//
// Memory checks are answered from the executable on disk; one of another
// build answers every one with the wrong bytes, which a server takes as a
// tampered client and disconnects without a reason. The build is read from
// the version resource so the wrong file can be named.
#include <catch_amalgamated.hpp>
#include "game/warden_memory.hpp"

using wowee::game::WardenMemory;

namespace {
// [signature][struct version][file version MS][file version LS]
std::vector<uint8_t> fixedFileInfo(uint16_t major, uint16_t minor, uint16_t patch, uint16_t build) {
    auto le32 = [](std::vector<uint8_t>& v, uint32_t x) {
        for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
    };
    std::vector<uint8_t> v(37, 0x11);  // something ahead of it, as a resource has
    le32(v, 0xFEEF04BD);
    le32(v, 0x00010000);
    le32(v, (uint32_t(major) << 16) | minor);
    le32(v, (uint32_t(patch) << 16) | build);
    return v;
}
}  // namespace

TEST_CASE("the build comes from the low word of the file version", "[wardenmemory]") {
    const auto wotlk = fixedFileInfo(3, 3, 5, 12340);
    CHECK(WardenMemory::fileBuildOf(wotlk.data(), wotlk.size()) == 12340);

    const auto beta = fixedFileInfo(3, 0, 1, 8874);
    CHECK(WardenMemory::fileBuildOf(beta.data(), beta.size()) == 8874);
}

TEST_CASE("an executable with no version resource has no build", "[wardenmemory]") {
    const std::vector<uint8_t> none(512, 0x90);
    CHECK(WardenMemory::fileBuildOf(none.data(), none.size()) == 0);
    CHECK(WardenMemory::fileBuildOf(nullptr, 0) == 0);
}
