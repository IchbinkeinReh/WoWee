// World packet framing: the header layouts the socket, SMSG_MULTIPLE_PACKETS
// and the compressed-moves handlers read through network/wire_format.hpp.
#include <catch_amalgamated.hpp>
#include "network/wire_format.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace wire = wowee::network::wire;

TEST_CASE("a server header is a big-endian size and a little-endian opcode", "[wire_format]") {
    // size 0x0102 (counting the opcode), opcode 0x01EE (SMSG_AUTH_RESPONSE)
    const std::vector<uint8_t> bytes = {0xAA, 0x01, 0x02, 0xEE, 0x01};
    const wire::ServerHeader header = wire::decodeServerHeader(bytes, 1);
    CHECK(header.size == 0x0102);
    CHECK(header.opcode == 0x01EE);
    CHECK(wire::kServerHeaderBytes == 4);
}

TEST_CASE("a client header counts its four opcode bytes in the size", "[wire_format]") {
    // What WorldSocket::send wrote byte by byte before the header had a name.
    const uint16_t opcode = 0x1ED;  // CMSG_AUTH_SESSION
    const uint16_t payloadLen = 0x0130;
    const uint16_t sizeField = payloadLen + 4;
    const std::array<uint8_t, 6> expected = {
        static_cast<uint8_t>((sizeField >> 8) & 0xFF), static_cast<uint8_t>(sizeField & 0xFF),
        static_cast<uint8_t>(opcode & 0xFF), static_cast<uint8_t>((opcode >> 8) & 0xFF), 0, 0};
    CHECK(wire::encodeClientHeader(opcode, payloadLen) == expected);
    CHECK(wire::kClientHeaderBytes == expected.size());
    // The size wraps as the uint16 arithmetic it replaced did.
    CHECK(wire::encodeClientHeader(1, 0xFFFE)[0] == 0x00);
    CHECK(wire::encodeClientHeader(1, 0xFFFE)[1] == 0x02);
}

TEST_CASE("little- and big-endian loads read the bytes they name", "[wire_format]") {
    const std::vector<uint8_t> bytes = {0x78, 0x56, 0x34, 0x12, 0xFF};
    CHECK(wire::loadLE32(bytes, 0) == 0x12345678u);
    CHECK(wire::loadLE16(bytes, 1) == 0x3456u);
    CHECK(wire::loadBE16(bytes, 3) == 0x12FFu);
}

TEST_CASE("a compressed-moves sub-packet's opcode follows its one-byte size", "[wire_format]") {
    const std::vector<uint8_t> bytes = {0x00, 0x05, 0xDD, 0x00, 0x11, 0x22, 0x33};
    CHECK(wire::moveSubOpcodeAt(bytes, 1) == 0x00DD);  // SMSG_MONSTER_MOVE
    CHECK(wire::kMoveSubHeaderBytes == 3);
}

TEST_CASE("a zlib wrapper is a uint32 length and a deflate stream header", "[wire_format]") {
    for (uint8_t flg : {0x01, 0x5E, 0x9C, 0xDA}) {
        const std::vector<uint8_t> bytes = {0x10, 0, 0, 0, 0x78, flg};
        CHECK(wire::isZlibStreamAt(bytes, wire::kInflatedSizeBytes));
    }
    CHECK_FALSE(wire::isZlibStreamAt(std::vector<uint8_t>{0x10, 0, 0, 0, 0x78, 0x02}, 4));
    CHECK_FALSE(wire::isZlibStreamAt(std::vector<uint8_t>{0x10, 0, 0, 0, 0x79, 0x9C}, 4));
    // Too short to hold both header bytes.
    CHECK_FALSE(wire::isZlibStreamAt(std::vector<uint8_t>{0x10, 0, 0, 0, 0x78}, 4));
}
