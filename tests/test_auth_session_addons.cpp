// CMSG_AUTH_SESSION's addon block as the client writes it (0x005f5440,
// decided D5): a count, each addon's name, a signed byte, its key's CRC and
// its .url's CRC, then the ban list's time - compressed after a size.
#include <catch_amalgamated.hpp>
#include <zlib.h>
#include "game/world_packets.hpp"
#include "core/application.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}

using namespace wowee::game;

namespace {
uint32_t u32(const std::vector<uint8_t>& b, size_t at) {
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}
}

TEST_CASE("the WotLK addon block names every addon", "[auth_session]") {
    const std::vector<uint8_t> key(40, 0x11);
    const std::vector<AuthSessionPacket::Addon> addons = {
        {"Blizzard_TalentUI", true, 0},
        {"MyAddon", false, 0xDEADBEEF},
    };
    const std::string account = "ACC";
    auto packet = AuthSessionPacket::build(12340, account, 1, key, 2, 1, addons, 77);
    const auto& bytes = packet.getData();
    // build, login server id, account, login server type, seed, region,
    // battlegroup, realm, dos response, digest - then the block.
    const size_t at = 4 + 4 + account.size() + 1 + 4 + 4 + 4 + 4 + 4 + 8 + 20;
    REQUIRE(bytes.size() > at + 4);
    uLongf size = u32(bytes, at);
    std::vector<uint8_t> block(size);
    REQUIRE(uncompress(block.data(), &size, bytes.data() + at + 4,
                       static_cast<uLong>(bytes.size() - at - 4)) == Z_OK);
    size_t p = 0;
    REQUIRE(u32(block, p) == 2); p += 4;
    const std::string first(reinterpret_cast<const char*>(block.data() + p));
    CHECK(first == "Blizzard_TalentUI"); p += first.size() + 1;
    CHECK(block[p] == 1); p += 1;
    CHECK(u32(block, p) == 0x4C1C776Du); p += 4;
    CHECK(u32(block, p) == 0u); p += 4;
    const std::string second(reinterpret_cast<const char*>(block.data() + p));
    CHECK(second == "MyAddon"); p += second.size() + 1;
    CHECK(block[p] == 0); p += 1;
    CHECK(u32(block, p) == 0u); p += 4;
    CHECK(u32(block, p) == 0xDEADBEEFu); p += 4;
    CHECK(u32(block, p) == 77u); p += 4;
    CHECK(p == block.size());
}
