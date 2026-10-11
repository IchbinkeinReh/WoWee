// The vehicle interface's requests, as UnitVehicle_C.cpp and Vehicle_C.cpp
// write them: CMSG_REQUEST_VEHICLE_EXIT/PREV_SEAT/NEXT_SEAT carry nothing
// (0x0074c7f0, 0x0074c8b0, 0x0074c9a0), SWITCH_SEAT the vehicle's packed
// guid and the seat (0x0074ca90), EJECT_PASSENGER the passenger's whole
// guid (0x00757200).
#include <catch_amalgamated.hpp>
#include "game/world_packets.hpp"
#include "game/packet_parsers.hpp"
#include "core/application.hpp"

// The builders under test never reach the singleton; a null instance satisfies
// the linker for the translation units that inline isActiveExpansion().
namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}  // namespace wowee

using namespace wowee::game;
using Bytes = std::vector<uint8_t>;

TEST_CASE("the seat requests without a payload carry nothing", "[vehicle]") {
    CHECK(VehiclePackets::requestExit().getData().empty());
    CHECK(VehiclePackets::requestPrevSeat().getData().empty());
    CHECK(VehiclePackets::requestNextSeat().getData().empty());
}

TEST_CASE("a switch of seat names the vehicle packed, then the seat", "[vehicle]") {
    const Bytes bytes = VehiclePackets::requestSwitchSeat(0xF150001234000056ull, 3).getData();
    // Mask 0b11011001: bytes 0, 3, 4, 6, 7.
    const Bytes want{0xD9, 0x56, 0x34, 0x12, 0x50, 0xF1, 0x03};
    CHECK(bytes == want);
}

TEST_CASE("an eject names the passenger in eight bytes", "[vehicle]") {
    const Bytes bytes = VehiclePackets::ejectPassenger(0x0000000000ABCDEFull).getData();
    const Bytes want{0xEF, 0xCD, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(bytes == want);
}
