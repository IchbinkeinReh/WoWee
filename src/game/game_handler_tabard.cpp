// game_handler_tabard.cpp - The tabard vendor and the guild emblem it saves
// (TabardModelFrame.cpp 0x00598e60-0x00598ec0, Player_C 0x006db320,
// 0x006de1c0, 0x006d9230).
#include "game/game_handler.hpp"
#include "game/interface_text.hpp"
#include "game/social_handler.hpp"
#include "game/tabard_design.hpp"
#include "game/update_field_table.hpp"
#include "pipeline/asset_manager.hpp"
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

void GameHandler::tabardFromGuildRegistrar() {
    // The registrar kept at 0xc0e570 is the guild one only; the arena
    // registrar (0x005a22a0) keeps its own.
    if (!socialHandler_ || !socialHandler_->hasPetitionShowlist() ||
        !socialHandler_->isGuildRegistrar())
        return;
    activateTabardVendor(socialHandler_->getPetitionNpcGuid());
    // CloseGuildRegistrar (0x00599f00): the npc let go, GUILD_REGISTRAR_CLOSED.
    closePetitionVendor();
}

bool GameHandler::canShowTabard() const {
    // 0x007202c0 for the active player: a form on its way counts as able;
    // so does a model whose extra display (CreatureDisplayInfoExtra Flags 1)
    // is of a race that shows tabards (ChrRaces Flags 4); else only the
    // player's own model - the display its native one.
    if (shapeshiftFormPending_) return true;
    const auto me = getEntityManager().getEntity(playerGuid);
    if (!me) return false;
    const auto field = [&](UF f) {
        const uint16_t i = fieldIndex(f);
        return i != 0xFFFF ? me->getField(i) : 0u;
    };
    const uint32_t display = field(UF::UNIT_FIELD_DISPLAYID);
    if (display == field(UF::UNIT_FIELD_NATIVEDISPLAYID)) return true;
    auto* am = services_.assetManager;
    if (!am || !am->isInitialized()) return false;
    const auto displays = am->loadDBCOptional("CreatureDisplayInfo.dbc");
    const auto extras = am->loadDBCOptional("CreatureDisplayInfoExtra.dbc");
    const auto races = am->loadDBCOptional("ChrRaces.dbc");
    if (!displays || !extras || !races) return false;
    const int32_t drow = displays->findRecordById(display);
    if (drow < 0 || displays->getFieldCount() <= 3) return false;
    const int32_t erow = extras->findRecordById(displays->getUInt32(static_cast<uint32_t>(drow), 3));
    if (erow < 0 || extras->getFieldCount() <= 19) return false;
    if ((extras->getUInt32(static_cast<uint32_t>(erow), 19) & 0x1u) == 0) return false;
    const int32_t rrow = races->findRecordById(extras->getUInt32(static_cast<uint32_t>(erow), 1));
    return rrow >= 0 && races->getFieldCount() > 1 &&
           (races->getUInt32(static_cast<uint32_t>(rrow), 1) & 0x4u) != 0;
}

void GameHandler::activateTabardVendor(uint64_t npc) {
    // 0x006d2500: asked only when the designer is not already open with this
    // npc, and refused where the player's model cannot show a tabard.
    if (npc == 0 || npc == tabardVendorGuid_ || !socket) return;
    if (!canShowTabard()) {
        playGameErrorSound("ERR_EMBLEMERROR_NOTABARDGEOSET");
        raiseUiError(interfaceString("ERR_EMBLEMERROR_NOTABARDGEOSET",
                                     "Your current form can't wear a tabard."));
        return;
    }
    network::Packet packet(wireOpcode(Opcode::MSG_TABARDVENDOR_ACTIVATE));
    packet.writeUInt64(npc);
    socket->send(packet);
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
