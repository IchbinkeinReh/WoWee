// game_handler_corpse.cpp - Where the player's corpse lies when it lies on a
// transport (GameUI.cpp 0x0051f430, 0x00523db0, 0x00512bd0).
#include "game/game_handler.hpp"
#include "game/spell_target_point.hpp"
#include "game/transport_manager.hpp"
#include "network/packet.hpp"
#include "network/world_socket.hpp"

#include <chrono>
#include <cmath>

namespace wowee::game {

namespace {

/// A transport's guid from the low part MSG_CORPSE_QUERY gives (0x00523db0:
/// high 0x1fc00000, HIGHGUID_MO_TRANSPORT).
constexpr uint64_t transportGuidOf(uint32_t low) { return (uint64_t{0x1FC00000u} << 32) | low; }

int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

glm::vec3 GameHandler::corpseServerPos() const {
    const glm::vec3 kept(corpseX_, corpseY_, corpseZ_);
    if (corpseTransportLow_ == 0) return kept;
    const uint64_t transport = transportGuidOf(corpseTransportLow_);
    // The transport in sight: the place carried through its world matrix
    // (0x0074b4c0).
    if (TransportManager* transports = transportManager_.get(); transports && transports->getTransport(transport)) {
        return core::coords::canonicalToServer(
            transports->getPlayerWorldPosition(transport, transports->serverToTransportLocal(transport, kept)));
    }
    if (const auto object = entityController_ ? entityController_->getEntityManager().getEntity(transport) : nullptr) {
        return core::coords::canonicalToServer(spell_target_point::inObjectFrame(
            glm::vec3(object->getX(), object->getY(), object->getZ()), object->getOrientation(), kept));
    }
    // Out of sight: asked where it is, at most every thirty seconds, and the
    // place carried through what the last answer said meanwhile.
    if (nowSeconds() >= corpseMapQueryAllowedAt_ && socket) {
        network::Packet packet(wireOpcode(Opcode::CMSG_CORPSE_MAP_POSITION_QUERY));
        packet.writeUInt32(corpseTransportLow_);
        socket->send(packet);
        corpseMapQueryAllowedAt_ = nowSeconds() + 30;
    }
    if (!corpseTransportFrame_) return kept;
    const float c = std::cos(corpseTransportFrame_->facing);
    const float s = std::sin(corpseTransportFrame_->facing);
    const glm::vec3& at = corpseTransportFrame_->position;
    return {at.x + kept.x * c - kept.y * s, at.y + kept.x * s + kept.y * c, at.z + kept.z};
}

void GameHandler::registerCorpseOpcodes() {
    // 0x00526530 case 0x4b7: three floats and the facing (0x00512bd0).
    dispatchTable_[Opcode::SMSG_CORPSE_MAP_POSITION_QUERY_RESPONSE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(16)) return;
        CorpseTransportFrame frame;
        frame.position.x = packet.readFloat();
        frame.position.y = packet.readFloat();
        frame.position.z = packet.readFloat();
        frame.facing = packet.readFloat();
        corpseTransportFrame_ = frame;
    };
}

}  // namespace wowee::game
