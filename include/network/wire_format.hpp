#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// The byte layouts the world protocol frames its packets with, in one place.
//
// The socket, SMSG_MULTIPLE_PACKETS, SMSG_COMPRESSED_MOVES and the zlib
// wrapped packets all used to spell these out as bare offsets - `+ 4`,
// `[pos + 2] << 8` - each in its own copy, and the copies had begun to read
// the same header in slightly different words. The loads here are plain
// shifts rather than casts over the buffer: the fields sit at any alignment,
// and the size field is big-endian on every host.
//
// None of the loads check bounds. Every caller has already checked that the
// header is there, and the socket's receive loop should not pay for it twice.
namespace wowee::network::wire {

[[nodiscard]] inline uint16_t loadLE16(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

[[nodiscard]] inline uint16_t loadBE16(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
}

[[nodiscard]] inline uint32_t loadLE32(std::span<const uint8_t> bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

// ---------------------------------------------------------------------------
// World packet header.
//
// Both directions open with a 16-bit big-endian size that counts the opcode
// and the payload but not itself. The opcode follows, little-endian: two bytes
// from the server, four from the client (the client's high two are always
// zero). Once the session key is in, the session cipher covers the header
// and only the header.
// ---------------------------------------------------------------------------

inline constexpr size_t kSizeFieldBytes = 2;
inline constexpr size_t kServerOpcodeBytes = 2;
inline constexpr size_t kClientOpcodeBytes = 4;
inline constexpr size_t kServerHeaderBytes = kSizeFieldBytes + kServerOpcodeBytes;
inline constexpr size_t kClientHeaderBytes = kSizeFieldBytes + kClientOpcodeBytes;
static_assert(kServerHeaderBytes == 4 && kClientHeaderBytes == 6);

/// A server header as it reads once decrypted.
struct ServerHeader {
    uint16_t size = 0;    ///< opcode + payload bytes
    uint16_t opcode = 0;
};

/// The header at `offset` in `bytes`; kServerHeaderBytes must be there.
[[nodiscard]] inline ServerHeader decodeServerHeader(std::span<const uint8_t> bytes, size_t offset = 0) {
    return {.size = loadBE16(bytes, offset),
            .opcode = loadLE16(bytes, offset + kSizeFieldBytes)};
}

/// The header that goes in front of a client packet's payload, in the clear.
[[nodiscard]] inline std::array<uint8_t, kClientHeaderBytes> encodeClientHeader(uint16_t opcode,
                                                                               uint16_t payloadLen) {
    const auto sizeField = static_cast<uint16_t>(payloadLen + kClientOpcodeBytes);
    return {static_cast<uint8_t>((sizeField >> 8) & 0xFF),
            static_cast<uint8_t>(sizeField & 0xFF),
            static_cast<uint8_t>(opcode & 0xFF),
            static_cast<uint8_t>((opcode >> 8) & 0xFF),
            0, 0};
}

// ---------------------------------------------------------------------------
// SMSG_COMPRESSED_MOVES / SMSG_MULTIPLE_MOVES sub-packet: a one-byte size,
// a little-endian two-byte opcode, then the payload. Most cores count the
// opcode in the size the way the world header does; some count the payload
// alone, which is why the size is left to the caller to interpret.
// ---------------------------------------------------------------------------

inline constexpr size_t kMoveSubSizeBytes = 1;
inline constexpr size_t kMoveSubOpcodeBytes = 2;
inline constexpr size_t kMoveSubHeaderBytes = kMoveSubSizeBytes + kMoveSubOpcodeBytes;

[[nodiscard]] inline uint16_t moveSubOpcodeAt(std::span<const uint8_t> bytes, size_t subPacketStart) {
    return loadLE16(bytes, subPacketStart + kMoveSubSizeBytes);
}

// ---------------------------------------------------------------------------
// A zlib-wrapped payload: the uncompressed length as a little-endian uint32,
// then the zlib stream itself.
// ---------------------------------------------------------------------------

inline constexpr size_t kInflatedSizeBytes = 4;

/// Whether a zlib stream starts at `offset`: CMF 0x78 (deflate, 32K window)
/// and one of the four FLG bytes the levels a server would use produce.
[[nodiscard]] inline bool isZlibStreamAt(std::span<const uint8_t> bytes, size_t offset) {
    if (bytes.size() < offset + 2) return false;
    const uint8_t cmf = bytes[offset];
    const uint8_t flg = bytes[offset + 1];
    return cmf == 0x78 && (flg == 0x01 || flg == 0x9C || flg == 0xDA || flg == 0x5E);
}

}  // namespace wowee::network::wire
