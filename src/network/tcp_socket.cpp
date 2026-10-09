#include "network/tcp_socket.hpp"
#include "network/packet.hpp"
#include "network/net_platform.hpp"
#include "network/wire_format.hpp"
#include "auth/auth_opcodes.hpp"
#include "core/logger.hpp"

namespace wowee {
namespace network {

namespace {
// Auth server packet framing: the offsets and lengths getExpectedPacketSize
// walks to find where one response ends. The parsers in auth_packets.cpp read
// the same fields by name once the whole packet is in.

// LOGON_CHALLENGE: opcode(1) + unk(1) + status(1) + B(32), then g's length.
constexpr size_t kChallengeStatusOffset = 2;
constexpr size_t kChallengeGLenOffset = 1 + 1 + 1 + 32;
constexpr size_t kChallengeFailureBytes = kChallengeStatusOffset + 1;
constexpr size_t kChallengeSaltBytes = 32;
constexpr size_t kChallengeCrcSaltBytes = 16;
constexpr size_t kSecurityFlagsBytes = 1;
// What each security flag (auth_opcodes.hpp) appends after the flags byte.
using auth::kSecurityFlagAuthenticator;
using auth::kSecurityFlagMatrixCard;
using auth::kSecurityFlagPin;
constexpr size_t kPinExtensionBytes = 4 + 16;              // seed + salt
constexpr size_t kMatrixExtensionBytes = 1 + 1 + 1 + 1 + 8; // w, h, digits, challenges, seed
constexpr size_t kAuthenticatorExtensionBytes = 1;         // required

// LOGON_PROOF success, by the server build the challenge named (see below).
constexpr size_t kProofSuccessBytesTbcWotlk = 32;
constexpr size_t kProofSuccessBytesLateVanilla = 28;
constexpr size_t kProofSuccessBytesEarlyVanilla = 26;

// REALM_LIST: opcode(1) + payload size(2).
constexpr size_t kRealmListHeaderBytes = 3;
}  // namespace

TCPSocket::TCPSocket() {
    net::ensureInit();
}

TCPSocket::~TCPSocket() {
    TCPSocket::disconnect();  // qualified call: virtual dispatch is bypassed in destructors
}

bool TCPSocket::connect(const std::string& host, uint16_t port) {
    LOG_INFO("Connecting to ", host, ":", port);

    // Socket open, non-blocking, and the address resolved.
    struct sockaddr_in serverAddr;
    sockfd = net::openResolvedSocket(host, port, serverAddr);
    if (sockfd == INVALID_SOCK) return false;

    int result = ::connect(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
    if (result < 0) {
        int err = net::lastError();
        if (!net::isInProgress(err)) {
            LOG_ERROR("Failed to connect: ", net::errorString(err));
            net::closeSocket(sockfd);
            sockfd = INVALID_SOCK;
            return false;
        }

        // Non-blocking connect in progress - wait for it to complete
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(sockfd, &writefds);

        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;

        int selectResult = ::select(static_cast<int>(sockfd) + 1, nullptr, &writefds, nullptr, &tv);
        if (selectResult <= 0) {
            LOG_ERROR("Connection timed out to ", host, ":", port);
            net::closeSocket(sockfd);
            sockfd = INVALID_SOCK;
            return false;
        }

        // Check if the connection actually succeeded
        int sockErr = 0;
        socklen_t errLen = sizeof(sockErr);
        getsockopt(sockfd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&sockErr), &errLen);
        if (sockErr != 0) {
            LOG_ERROR("Connection failed: ", net::errorString(sockErr));
            net::closeSocket(sockfd);
            sockfd = INVALID_SOCK;
            return false;
        }
    }

    // Disable Nagle's algorithm - send small packets immediately.
    int one = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&one), sizeof(one));

    connected = true;
    LOG_INFO("Connected to ", host, ":", port);
    return true;
}

void TCPSocket::disconnect() {
    if (sockfd != INVALID_SOCK) {
        net::closeSocket(sockfd);
        sockfd = INVALID_SOCK;
    }
    connected = false;
    receiveBuffer.clear();
}

void TCPSocket::send(const Packet& packet) {
    if (!connected) return;

    // Build complete packet with opcode
    std::vector<uint8_t> sendData;

    // Add opcode (1 byte) - always little-endian, but it's just 1 byte so doesn't matter
    sendData.push_back(static_cast<uint8_t>(packet.getOpcode() & 0xFF));

    // Add packet data
    const auto& data = packet.getData();
    sendData.insert(sendData.end(), data.begin(), data.end());

    LOG_DEBUG("Sending packet: opcode=0x", std::hex, packet.getOpcode(), std::dec,
              " size=", sendData.size(), " bytes");

    // Send complete packet
    ssize_t sent = net::portableSend(sockfd, sendData.data(), sendData.size());
    if (sent < 0) {
        LOG_ERROR("Send failed: ", net::errorString(net::lastError()));
    } else if (static_cast<size_t>(sent) != sendData.size()) {
        LOG_WARNING("Partial send: ", sent, " of ", sendData.size(), " bytes");
    }
}

void TCPSocket::update() {
    if (!connected) return;

    // Drain the socket. Some servers send small packets and close immediately; a single recv()
    // can return a partial packet, and the next recv() may return 0 (FIN) which would otherwise
    // make us drop the buffered bytes without parsing.
    bool sawClose = false;
    bool receivedAny = false;
    for (;;) {
        // 4 KB per recv() call - large enough for any single game packet while keeping
        // stack usage reasonable. Typical WoW packets are 20-500 bytes; UPDATE_OBJECT
        // can reach ~2 KB in crowded zones.
        uint8_t buffer[4096];
        ssize_t received = net::portableRecv(sockfd, buffer, sizeof(buffer));

        if (received > 0) {
            receivedAny = true;
            LOG_DEBUG("Received ", received, " bytes from server");
            receiveBuffer.insert(receiveBuffer.end(), buffer, buffer + received);
            continue; // keep draining
        }

        if (received == 0) {
            sawClose = true;
            break;
        }

        int err = net::lastError();
        if (net::isWouldBlock(err)) {
            break;
        }
        if (net::isConnectionClosed(err)) {
            // Peer closed the connection - treat the same as recv() returning 0
            sawClose = true;
            break;
        }

        LOG_ERROR("Receive failed: ", net::errorString(err));
        disconnect();
        return;
    }

    if (receivedAny) {
        tryParsePackets();
    }

    if (sawClose) {
        LOG_INFO("Connection closed by server");
        disconnect();
    }
}

void TCPSocket::tryParsePackets() {
    // For auth packets, we need at least 1 byte (opcode)
    while (!receiveBuffer.empty()) {
        uint8_t opcode = receiveBuffer[0];

        // Determine expected packet size based on opcode
        // This is specific to authentication protocol
        size_t expectedSize = getExpectedPacketSize(opcode);

        if (expectedSize == 0) {
            // Unknown opcode or need more data to determine size
            LOG_WARNING("Unknown opcode or indeterminate size: 0x", std::hex, static_cast<int>(opcode), std::dec);
            break;
        }

        if (receiveBuffer.size() < expectedSize) {
            // Not enough data yet
            LOG_DEBUG("Waiting for more data: have ", receiveBuffer.size(),
                     " bytes, need ", expectedSize);
            break;
        }

        // We have a complete packet!
        LOG_DEBUG("Parsing packet: opcode=0x", std::hex, static_cast<int>(opcode), std::dec,
                 " size=", expectedSize, " bytes");

        // Create packet from buffer data
        std::vector<uint8_t> packetData(receiveBuffer.begin(),
                                        receiveBuffer.begin() + expectedSize);

        Packet packet(opcode, packetData);

        // Remove parsed data from buffer
        receiveBuffer.erase(receiveBuffer.begin(), receiveBuffer.begin() + expectedSize);

        // Call callback if set
        if (packetCallback) {
            packetCallback(packet);
        }
    }
}

size_t TCPSocket::getExpectedPacketSize(uint8_t opcode) {
    // Authentication packet sizes (WoW 3.3.5a)
    // Note: These are minimum sizes. Some packets are variable length.

    switch (opcode) {
        case 0x00:  // LOGON_CHALLENGE response
            // Need to read status byte to determine success/failure
            if (receiveBuffer.size() > kChallengeStatusOffset) {
                uint8_t status = receiveBuffer[kChallengeStatusOffset];
                if (status == 0x00) {
                    // Success: opcode(1) + unk(1) + status(1) + B(32) + gLen(1) + g(gLen) +
                    //          nLen(1) + N(nLen) + salt(32) + crcHash(16) + securityFlags(1)
                    //          + optional security flag data
                    if (receiveBuffer.size() > kChallengeGLenOffset) {  // enough to read g_len
                        uint8_t gLen = receiveBuffer[kChallengeGLenOffset];
                        const size_t nLenOffset = kChallengeGLenOffset + 1 + gLen;
                        if (receiveBuffer.size() > nLenOffset) {
                            uint8_t nLen = receiveBuffer[nLenOffset];
                            size_t baseSize = nLenOffset + 1 + nLen + kChallengeSaltBytes +
                                              kChallengeCrcSaltBytes + kSecurityFlagsBytes;
                            // Need to read securityFlags to account for extra data
                            if (receiveBuffer.size() >= baseSize) {
                                uint8_t secFlags = receiveBuffer[baseSize - kSecurityFlagsBytes];
                                size_t extra = 0;
                                if (secFlags & kSecurityFlagPin) extra += kPinExtensionBytes;
                                if (secFlags & kSecurityFlagMatrixCard) extra += kMatrixExtensionBytes;
                                if (secFlags & kSecurityFlagAuthenticator) extra += kAuthenticatorExtensionBytes;
                                return baseSize + extra;
                            }
                        }
                    }
                    return 0;  // Need more data
                }                     // Failure - just opcode + unknown + status
                    return kChallengeFailureBytes;
               
            }
            return 0;  // Need more data to determine

        case 0x01:  // LOGON_PROOF response
            // Success response varies by server build (determined by client build sent in challenge):
            //   Build >= 8089: cmd(1)+error(1)+M2(20)+accountFlags(4)+surveyId(4)+loginFlags(2) = 32
            //   Build 6299-8088: cmd(1)+error(1)+M2(20)+surveyId(4)+loginFlags(2) = 28
            //   Build < 6299: cmd(1)+error(1)+M2(20)+surveyId(4) = 26
            // Failure: varies by server - minimum 2 bytes (opcode + status), some send 4
            if (receiveBuffer.size() >= 2) {
                uint8_t status = receiveBuffer[1];
                if (status == 0x00) {
                    if (receiveBuffer.size() >= kProofSuccessBytesTbcWotlk) return kProofSuccessBytesTbcWotlk;
                    if (receiveBuffer.size() >= kProofSuccessBytesLateVanilla) return kProofSuccessBytesLateVanilla;
                    if (receiveBuffer.size() >= kProofSuccessBytesEarlyVanilla) return kProofSuccessBytesEarlyVanilla;
                    return 0;
                }                     // Consume up to 4 bytes if available, minimum 2
                    return (receiveBuffer.size() >= 4) ? 4 : 2;
               
            }
            return 0;  // Need more data

        case 0x10:  // REALM_LIST response
            // Variable length - format: opcode(1) + size(2, little-endian) + payload(size)
            if (receiveBuffer.size() >= kRealmListHeaderBytes) {
                const uint16_t size = wire::loadLE16(receiveBuffer, 1);
                return kRealmListHeaderBytes + size;
            }
            return 0;  // Need more data to read size field

        default:
            LOG_WARNING("Unknown auth packet opcode: 0x", std::hex, static_cast<int>(opcode), std::dec);
            return 0;
    }
}

} // namespace network
} // namespace wowee
