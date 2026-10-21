// game_handler_tabard.cpp - The tabard vendor and the guild emblem it saves
// (TabardModelFrame.cpp 0x00598e60-0x00598ec0, Player_C 0x006db320,
// 0x006de1c0, 0x006d9230).
#include "game/game_handler.hpp"
#include "game/interface_text.hpp"
#include "game/tabard_design.hpp"
#include "network/packet.hpp"
#include "network/world_socket.hpp"

namespace wowee::game {

void GameHandler::registerTabardOpcodes() {
    // 0x006cc560: the vendor's full guid.
    dispatchTable_[Opcode::MSG_TABARDVENDOR_ACTIVATE] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8)) return;
        openTabardVendor(packet.readUInt64());
    };
    // 0x006d9230: the answer to a save, a u32.
    dispatchTable_[Opcode::MSG_SAVE_GUILD_EMBLEM] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        guildEmblemSaved(packet.readUInt32());
    };
}

void GameHandler::openTabardVendor(uint64_t vendor) {
    // 0x00598e60: the vendor kept (0xc0e518), the interaction object it, and
    // OPEN_TABARD_FRAME (event 0x16c).
    tabardVendorGuid_ = vendor;
    setInteractionObject(vendor, go_interaction::Window::Tabard);
    fireAddonEvent("OPEN_TABARD_FRAME", {});
}

void GameHandler::closeTabardVendor() {
    // 0x00598ec0: the player's own tabard painted again (0x006db5f0), the
    // vendor let go, CLOSE_TABARD_FRAME (event 0x16d).
    if (tabardVendorGuid_ == 0) return;
    tabardPreview_.reset();
    tabardVendorGuid_ = 0;
    fireAddonEvent("CLOSE_TABARD_FRAME", {});
}

void GameHandler::previewTabard(const GuildEmblem& emblem) { tabardPreview_ = emblem; }

uint32_t GameHandler::getPlayerGuildId() const {
    const uint32_t guildId = getEntityGuildId(playerGuid);
    if (guildId != 0) return guildId;
    const Character* self = getActiveCharacter();
    return self ? self->guildId : 0;
}

std::optional<GuildEmblem> GameHandler::getPlayerGuildEmblem() { return lookupGuildEmblem(getPlayerGuildId()); }

std::optional<GuildEmblem> GameHandler::getPlayerTabardEmblem() {
    if (tabardPreview_) return tabardPreview_;
    return getPlayerGuildEmblem();
}

bool GameHandler::canSaveTabardNow() {
    // 0x006de1c0: the player's guild in the cache, and no save waiting
    // (0xc9eacc).
    return getPlayerGuildEmblem().has_value() && !tabardSavePending_;
}

void GameHandler::saveGuildEmblem(const tabard::Design& design) {
    // 0x006db320.
    const tabard::SaveCheck check{.guild = getPlayerGuildEmblem(),
                                  .guildMaster = getPlayerGuildRankField() == 0,
                                  .money = getMoneyCopper()};
    if (const auto refused = tabard::saveRefusal(design, check)) {
        addUIError(interfaceString(refused->key, refused->enUS));
        return;
    }
    if (!socket) return;
    // To the interaction object (0xbd07a8), the five as u32.
    network::Packet packet(wireOpcode(Opcode::MSG_SAVE_GUILD_EMBLEM));
    packet.writeUInt64(interactionObject_);
    for (int32_t v : design.values) packet.writeUInt32(static_cast<uint32_t>(v));
    socket->send(packet);
    tabardSavePending_ = true;
    fireAddonEvent("TABARD_SAVE_PENDING", {});
}

void GameHandler::guildEmblemSaved(uint32_t result) {
    if (!tabard::knownSaveResult(result)) return;
    if (const auto said = tabard::saveResultError(result)) addUIError(interfaceString(said->key, said->enUS));
    tabardSavePending_ = false;
    if (result == 0) {
        // The guild asked for again (0x0067a5f0), and GUILDTABARD_UPDATE
        // (event 0x232).
        if (const uint32_t guildId = getPlayerGuildId()) queryGuildInfo(guildId);
        fireAddonEvent("GUILDTABARD_UPDATE", {});
        return;
    }
    fireAddonEvent("TABARD_SAVE_PENDING", {});
}

}  // namespace wowee::game
