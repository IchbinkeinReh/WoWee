// CMSG_CANCEL_CAST layout (0x00806200): one byte of cast count, then the
// spell id. The server skips the byte and reads the id; with four bytes of
// count in front it read a different spell and the cast was not cancelled.
#include <catch_amalgamated.hpp>

#include "test_support.hpp"
#include "game/world_packets.hpp"
#include "core/application.hpp"

// The builders live in translation units that inline isActiveExpansion(), which
// reaches through the Application singleton. The builder under test never calls
// it, so a null instance satisfies the linker.
namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}

using namespace wowee::game;

TEST_CASE("CMSG_CANCEL_CAST carries a one-byte cast count before the spell", "[spell][packet]") {
    SECTION("WotLK: count byte, then the spell id") {
        auto p = CancelCastPacket::build(133, 7);
        const auto& b = p.getData();
        REQUIRE(b.size() == 5u);
        CHECK(b[0] == 7u);
        CHECK(wowee::test::readU32(b, 1) == 133u);
    }
    SECTION("before WotLK the old layout stays") {
        auto p = CancelCastPacket::build(133, 7, false);
        const auto& b = p.getData();
        REQUIRE(b.size() == 8u);
        CHECK(wowee::test::readU32(b, 4) == 133u);
    }
}
