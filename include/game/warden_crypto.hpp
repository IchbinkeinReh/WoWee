#pragma once

#include <vector>
#include <cstdint>
#include <memory>

namespace wowee {
namespace game {

/**
 * Warden anti-cheat crypto handler.
 * Derives RC4 keys from the 40-byte SRP session key using SHA1Randx,
 * then encrypts/decrypts Warden packet payloads.
 */
class WardenCrypto {
public:
    WardenCrypto();
    ~WardenCrypto();

    /**
     * Initialize Warden crypto from the 40-byte SRP session key.
     * Derives encrypt (client->server) and decrypt (server->client) RC4 keys
     * using the SHA1Randx / WardenKeyGenerator algorithm.
     */
    bool initFromSessionKey(const std::vector<uint8_t>& sessionKey);

    /**
     * Replace RC4 keys (called after module hash challenge succeeds).
     * @param newEncryptKey 16-byte key for encrypting outgoing packets
     * @param newDecryptKey 16-byte key for decrypting incoming packets
     */
    void replaceKeys(const std::vector<uint8_t>& newEncryptKey,
                     const std::vector<uint8_t>& newDecryptKey);

    /**
     * Replace the RC4 state outright, as a Warden module hands it over: the
     * client->server state then the server->client state, each 256 S-box
     * bytes then i then j. False, leaving the state alone, when the buffer
     * is not exactly 516 bytes.
     */
    bool replaceState(const std::vector<uint8_t>& statePair);

    /** Decrypt an incoming SMSG_WARDEN_DATA payload. */
    std::vector<uint8_t> decrypt(const std::vector<uint8_t>& data);

    /** Encrypt an outgoing CMSG_WARDEN_DATA payload. */
    std::vector<uint8_t> encrypt(const std::vector<uint8_t>& data);

    [[nodiscard]] bool isInitialized() const { return initialized_; }

private:
    bool initialized_ = false;

    // RC4 state for decrypting incoming packets (server->client)
    std::vector<uint8_t> decryptRC4State_;
    uint8_t decryptRC4_i_ = 0;
    uint8_t decryptRC4_j_ = 0;

    // RC4 state for encrypting outgoing packets (client->server)
    std::vector<uint8_t> encryptRC4State_;
    uint8_t encryptRC4_i_ = 0;
    uint8_t encryptRC4_j_ = 0;

    void initRC4(const std::vector<uint8_t>& key,
                 std::vector<uint8_t>& state,
                 uint8_t& i, uint8_t& j);

    void processRC4(const uint8_t* input, uint8_t* output, size_t length,
                    std::vector<uint8_t>& state, uint8_t& i, uint8_t& j);

public:
    /**
     * SHA1Randx / WardenKeyGenerator: generates pseudo-random bytes from a seed.
     * Used to derive the 16-byte encrypt and decrypt keys from a seed.
     * Public so GameHandler can use it for module hash key derivation.
     */
    static void sha1RandxGenerate(const std::vector<uint8_t>& seed,
                                  uint8_t* outputEncryptKey,
                                  uint8_t* outputDecryptKey);
};

} // namespace game
} // namespace wowee
