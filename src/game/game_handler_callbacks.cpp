#include "game/ground_target.hpp"
#include <array>
#include <string_view>
#include "game/interface_text.hpp"
#include "game/pet_action.hpp"
#include "game/vehicle_mover.hpp"
#include "game/unit_name_rules.hpp"
#include "rendering/mount_transition.hpp"
#include "game/game_handler.hpp"
#include "game/chat_profanity.hpp"
#include "game/inventory_alerts.hpp"
#include "game/reputation_standing.hpp"
#include "addons/lua_api_registrations.hpp"
#include "game/spell_description_eval.hpp"
#include "game/spell_target_kind.hpp"
#include "game/spell_visual_choice.hpp"
#include "game/packed_time.hpp"
#include "game/inventory_slots.hpp"
#include "game/game_utils.hpp"
#include "game/chat_handler.hpp"
#include "game/movement_handler.hpp"
#include "game/combat_handler.hpp"
#include "game/spell_handler.hpp"
#include "game/aura_vision.hpp"
#include "game/inventory_handler.hpp"
#include "game/social_handler.hpp"
#include "game/quest_handler.hpp"
#include "game/npc_interaction.hpp"
#include "game/pet_action_bar.hpp"
#include "game/warden_handler.hpp"
#include "game/warden_crypto.hpp"
#include "game/warden_memory.hpp"
#include "game/warden_module.hpp"
#include "game/packet_parsers.hpp"
#include "game/transport_manager.hpp"
#include "game/opcodes.hpp"
#include "game/update_field_table.hpp"
#include "game/expansion_profile.hpp"
#include "ui/framexml_takeover.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/spell_visual_system.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/player_voice_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "pipeline/dbc_layout.hpp"
#include "network/world_socket.hpp"
#include "network/packet.hpp"
#include "auth/crypto.hpp"
#include "core/coordinates.hpp"
#include "core/application.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/animation_data.hpp"
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <ctime>
#include <vector>
#include <random>
#include <zlib.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <climits>
#include <cstring>
#include <limits>
#include <openssl/sha.h>
#include <openssl/hmac.h>


namespace wowee {
namespace game {

namespace {



} // end anonymous namespace

void GameHandler::handleAuthChallenge(network::Packet& packet) {
    LOG_INFO("Handling SMSG_AUTH_CHALLENGE");

    AuthChallengeData challenge;
    if (!AuthChallengeParser::parse(packet, challenge)) {
        fail("Failed to parse SMSG_AUTH_CHALLENGE");
        return;
    }

    if (!challenge.isValid()) {
        fail("Invalid auth challenge data");
        return;
    }

    // Store server seed
    serverSeed = challenge.serverSeed;
    LOG_DEBUG("Server seed: 0x", std::hex, serverSeed, std::dec);

    setState(WorldState::CHALLENGE_RECEIVED);

    // Send authentication session
    sendAuthSession();
}

void GameHandler::handleAddonInfo(network::Packet& packet) {
    // 0x005f79a0 (decided D5: read, as the client does, rather than skipped):
    // for each addon CMSG_AUTH_SESSION named, in that order, its state - 0
    // banned, 2 secure-capable - whether a key and a revision follow, and
    // whether a url does; then the server's ban list, each entry the MD5 of
    // an addon's name and version and whether it is banned.
    addonVerdicts_.clear();
    for (const auto& sent : sentAuthAddons_) {
        if (!packet.hasRemaining(2)) break;
        AddonVerdict v;
        const uint8_t state = packet.readUInt8();
        const uint8_t hasKeyInfo = packet.readUInt8();
        if (hasKeyInfo) {
            if (!packet.hasRemaining(1)) break;
            if (packet.readUInt8()) {          // the key itself, 256 bytes
                if (!packet.hasRemaining(256)) break;
                for (int i = 0; i < 256; ++i) packet.readUInt8();
            }
            if (!packet.hasRemaining(4)) break;
            v.revision = packet.readUInt32();
        }
        if (!packet.hasRemaining(1)) break;
        if (packet.readUInt8()) v.url = packet.readString();
        v.secureCapable = state == 2;
        v.banned = state == 0;
        addonVerdicts_[sent.name] = v;
    }
    if (packet.hasRemaining(4)) {
        const uint32_t count = packet.readUInt32();
        for (uint32_t i = 0; i < count && packet.hasRemaining(4 + 16 + 16 + 4 + 4); ++i) {
            packet.readUInt32();                               // the entry's id
            std::vector<uint8_t> nameHash(16);
            for (auto& b : nameHash) b = packet.readUInt8();
            for (int k = 0; k < 16; ++k) packet.readUInt8();   // version hash
            packet.readUInt32();                               // timestamp
            const uint32_t isBanned = packet.readUInt32();
            if (!isBanned) continue;
            for (const auto& sent : sentAuthAddons_) {
                if (auth::Crypto::md5(sent.name) == nameHash) addonVerdicts_[sent.name].banned = true;
            }
        }
    }
    LOG_INFO("SMSG_ADDON_INFO: ", addonVerdicts_.size(), " addons answered");
    if (addonInfoCallback_) addonInfoCallback_();
}

void GameHandler::sendAuthSession() {
    LOG_INFO("Sending CMSG_AUTH_SESSION");

    // Build authentication packet
    auto packet = AuthSessionPacket::build(
        build,
        accountName,
        clientSeed,
        sessionKey,
        serverSeed,
        realmId_,
        authAddons_
    );
    // SMSG_ADDON_INFO answers these, in this order.
    sentAuthAddons_ = authAddons_;

    LOG_DEBUG("CMSG_AUTH_SESSION packet size: ", packet.getSize(), " bytes");

    // Send packet (unencrypted - this is the last unencrypted packet)
    socket->send(packet);

    // Enable encryption IMMEDIATELY after sending AUTH_SESSION
    // AzerothCore enables encryption before sending AUTH_RESPONSE,
    // so we need to be ready to decrypt the response
    LOG_INFO("Enabling encryption immediately after AUTH_SESSION");
    socket->initEncryption(sessionKey, build);

    setState(WorldState::AUTH_SENT);
    LOG_INFO("CMSG_AUTH_SESSION sent, encryption enabled, waiting for AUTH_RESPONSE...");
}

void GameHandler::handleAuthResponse(network::Packet& packet) {
    LOG_DEBUG("Handling SMSG_AUTH_RESPONSE, size=", packet.getSize());

    AuthResponseData response;
    if (!AuthResponseParser::parse(packet, response)) {
        fail("Failed to parse SMSG_AUTH_RESPONSE");
        return;
    }

    // Kept for the whole session, as the client keeps it: 0x00464640 stores
    // the block on the connection object and the billing bindings read it back
    // there long after the response itself has been dealt with.
    accountStateRef() = response.account;

    if (!response.isSuccess()) {
        std::string reason = std::string("Authentication failed: ") +
                           getAuthResultString(response.result);
        fail(reason);
        return;
    }

    // Encryption was already enabled after sending AUTH_SESSION
    LOG_INFO("AUTH_RESPONSE OK - world authentication successful");

    setState(WorldState::AUTHENTICATED);

    LOG_INFO("========================================");
    LOG_INFO("   WORLD AUTHENTICATION SUCCESSFUL!");
    LOG_INFO("========================================");
    LOG_INFO("Connected to world server");
    LOG_INFO("Ready for character operations");

    setState(WorldState::READY);

    // The account's copies of its settings first (0x004de390), then the
    // character list.
    accountDataKnown_.fill(false);
    accountDataAwaited_.fill(false);
    sendReadyForAccountDataTimes();
    requestCharacterList();

    // Call success callback
    if (onSuccess) {
        onSuccess();
    }
}

void GameHandler::requestCharacterList() {
    if (state == WorldState::FAILED || !socket || !socket->isConnected()) {
        return;
    }

    if (state != WorldState::READY && state != WorldState::AUTHENTICATED &&
        state != WorldState::CHAR_LIST_RECEIVED) {
        LOG_WARNING("Cannot request character list in state: ", worldStateName(state));
        return;
    }

    LOG_INFO("Requesting character list from server...");

    // Prevent the UI from showing/selecting stale characters while we wait for the new SMSG_CHAR_ENUM.
    // This matters after character create/delete where the old list can linger for a few frames.
    characters.clear();

    // Build CMSG_CHAR_ENUM packet (no body, just opcode)
    auto packet = CharEnumPacket::build();

    // Send packet
    socket->send(packet);

    setState(WorldState::CHAR_LIST_REQUESTED);
    LOG_INFO("CMSG_CHAR_ENUM sent, waiting for character list...");
}

void GameHandler::handleCharEnum(network::Packet& packet) {
    LOG_INFO("Handling SMSG_CHAR_ENUM");

    CharEnumResponse response;
    // IMPORTANT: Do not infer packet formats from numeric build alone.
    // Turtle WoW uses a "high" build but classic-era world packet formats.
    bool parsed = packetParsers_ ? packetParsers_->parseCharEnum(packet, response)
                                 : CharEnumParser::parse(packet, response);
    if (!parsed) {
        fail("Failed to parse SMSG_CHAR_ENUM");
        return;
    }

    // Store characters
    characters = response.characters;

    setState(WorldState::CHAR_LIST_RECEIVED);

    std::vector<uint32_t> queriedGuildIds;
    for (const auto& character : characters) {
        if (!character.hasGuild()) continue;
        if (std::find(queriedGuildIds.begin(), queriedGuildIds.end(), character.guildId) != queriedGuildIds.end())
            continue;
        queriedGuildIds.push_back(character.guildId);
        queryGuildInfo(character.guildId);
    }
    if (!queriedGuildIds.empty()) {
        LOG_INFO("Queued guild name queries for ", queriedGuildIds.size(), " guild(s) from character list");
    }

    LOG_INFO("========================================");
    LOG_INFO("   CHARACTER LIST RECEIVED");
    LOG_INFO("========================================");
    LOG_INFO("Found ", characters.size(), " character(s)");
    // The test harness's watchdog waits for this line: a run that has not
    // reached the character list soon after its start is hung (login, realm
    // list or the world server's connect) and is killed and tried again.
    LOG_INFO("CHARLIST reached");

    if (characters.empty()) {
        LOG_INFO("No characters on this account");
    } else {
        LOG_INFO("Characters:");
        for (size_t i = 0; i < characters.size(); ++i) {
            const auto& character = characters[i];
            LOG_INFO("  [", i + 1, "] ", character.name);
            LOG_INFO("      GUID: 0x", std::hex, character.guid, std::dec);
            LOG_INFO("      ", getRaceName(character.race), " ",
                     getClassName(character.characterClass));
            LOG_INFO("      Level ", static_cast<int>(character.level));
        }
    }

    LOG_INFO("Ready to select character");
}

void GameHandler::createCharacter(const CharCreateData& data) {

    // Online mode: send packet to server
    if (!socket) {
        LOG_WARNING("Cannot create character: not connected");
        if (charCreateCallback_) {
            charCreateCallback_(false, "Not connected to server");
        }
        return;
    }

    if (state != WorldState::CHAR_LIST_RECEIVED) {
        std::string msg = "Character list not ready yet. Wait for SMSG_CHAR_ENUM.";
        LOG_WARNING("Blocking CMSG_CHAR_CREATE in state=", worldStateName(state),
                    " (awaiting CHAR_LIST_RECEIVED)");
        if (charCreateCallback_) {
            charCreateCallback_(false, msg);
        }
        return;
    }

    auto packet = CharCreatePacket::build(data);
    socket->send(packet);
    LOG_INFO("CMSG_CHAR_CREATE sent for: ", data.name);
}

void GameHandler::handleCharCreateResponse(network::Packet& packet) {
    CharCreateResponseData data;
    if (!CharCreateResponseParser::parse(packet, data)) {
        LOG_ERROR("Failed to parse SMSG_CHAR_CREATE");
        return;
    }

    if (data.result == CharCreateResult::SUCCESS || data.result == CharCreateResult::IN_PROGRESS) {
        LOG_INFO("Character created successfully (code=", static_cast<int>(data.result), ")");
        requestCharacterList();
        if (charCreateCallback_) {
            charCreateCallback_(true, "Character created!");
        }
    } else {
        std::string msg;
        switch (data.result) {
            case CharCreateResult::CHAR_ERROR: msg = "Server error"; break;
            case CharCreateResult::FAILED: msg = "Creation failed"; break;
            case CharCreateResult::NAME_IN_USE: msg = "Name already in use"; break;
            case CharCreateResult::DISABLED: msg = "Character creation disabled"; break;
            case CharCreateResult::PVP_TEAMS_VIOLATION: msg = "PvP faction violation"; break;
            case CharCreateResult::SERVER_LIMIT: msg = "Server character limit reached"; break;
            case CharCreateResult::ACCOUNT_LIMIT: msg = "Account character limit reached"; break;
            case CharCreateResult::SERVER_QUEUE: msg = "Server is queued"; break;
            case CharCreateResult::ONLY_EXISTING: msg = "Only existing characters allowed"; break;
            case CharCreateResult::EXPANSION: msg = "Expansion required"; break;
            case CharCreateResult::EXPANSION_CLASS: msg = "Expansion required for this class"; break;
            case CharCreateResult::LEVEL_REQUIREMENT: msg = "Level requirement not met"; break;
            case CharCreateResult::UNIQUE_CLASS_LIMIT: msg = "Unique class limit reached"; break;
            case CharCreateResult::RESTRICTED_RACECLASS: msg = "Race/class combination not allowed"; break;
            case CharCreateResult::IN_PROGRESS: msg = "Character creation in progress..."; break;
            case CharCreateResult::CHARACTER_CHOOSE_RACE: msg = "Please choose a different race"; break;
            case CharCreateResult::CHARACTER_ARENA_LEADER: msg = "Arena team leader restriction"; break;
            case CharCreateResult::CHARACTER_DELETE_MAIL: msg = "Character has mail"; break;
            case CharCreateResult::CHARACTER_SWAP_FACTION: msg = "Faction swap restriction"; break;
            case CharCreateResult::CHARACTER_RACE_ONLY: msg = "Race-only restriction"; break;
            case CharCreateResult::CHARACTER_GOLD_LIMIT: msg = "Gold limit reached"; break;
            case CharCreateResult::FORCE_LOGIN: msg = "Force login required"; break;
            case CharCreateResult::CHARACTER_IN_GUILD: msg = "Character is in a guild"; break;
            // Name validation errors
            case CharCreateResult::NAME_FAILURE: msg = "Invalid name"; break;
            case CharCreateResult::NAME_NO_NAME: msg = "Please enter a name"; break;
            case CharCreateResult::NAME_TOO_SHORT: msg = "Name is too short"; break;
            case CharCreateResult::NAME_TOO_LONG: msg = "Name is too long"; break;
            case CharCreateResult::NAME_INVALID_CHARACTER: msg = "Name contains invalid characters"; break;
            case CharCreateResult::NAME_MIXED_LANGUAGES: msg = "Name mixes languages"; break;
            case CharCreateResult::NAME_PROFANE: msg = "Name contains profanity"; break;
            case CharCreateResult::NAME_RESERVED: msg = "Name is reserved"; break;
            case CharCreateResult::NAME_INVALID_APOSTROPHE: msg = "Invalid apostrophe in name"; break;
            case CharCreateResult::NAME_MULTIPLE_APOSTROPHES: msg = "Name has multiple apostrophes"; break;
            case CharCreateResult::NAME_THREE_CONSECUTIVE: msg = "Name has 3+ consecutive same letters"; break;
            case CharCreateResult::NAME_INVALID_SPACE: msg = "Invalid space in name"; break;
            case CharCreateResult::NAME_CONSECUTIVE_SPACES: msg = "Name has consecutive spaces"; break;
            default: msg = "Unknown error (code " + std::to_string(static_cast<int>(data.result)) + ")"; break;
        }
        LOG_WARNING("Character creation failed: ", msg, " (code=", static_cast<int>(data.result), ")");
        if (charCreateCallback_) {
            charCreateCallback_(false, msg);
        }
    }
}

void GameHandler::deleteCharacter(uint64_t characterGuid) {
    if (!socket) {
        if (charDeleteCallback_) charDeleteCallback_(false, "Delete failed: not connected to server.");
        return;
    }

    network::Packet packet(wireOpcode(Opcode::CMSG_CHAR_DELETE));
    packet.writeUInt64(characterGuid);
    socket->send(packet);
    pendingCharDeleteResponse_ = true;
    pendingDeleteGuid_ = characterGuid;
    pendingDeleteTimer_ = 0.0f;
    LOG_INFO("CMSG_CHAR_DELETE sent for GUID: 0x", std::hex, characterGuid, std::dec);
}

const Character* GameHandler::getActiveCharacter() const {
    if (activeCharacterGuid_ == 0) return nullptr;
    for (const auto& ch : characters) {
        if (ch.guid == activeCharacterGuid_) return &ch;
    }
    return nullptr;
}
void GameHandler::playErrorSpeech(audio::PlayerErrorSpeech type) {
    auto* ac = services_.audioCoordinator;
    if (!ac) return;
    auto* voice = ac->getPlayerVoiceManager();
    if (!voice) return;
    const Character* ch = getActiveCharacter();
    if (!ch) return;
    voice->playError(type, static_cast<uint8_t>(ch->race),
                     static_cast<uint8_t>(toServerGender(ch->gender)),
                     static_cast<uint8_t>(ch->characterClass));
}

void GameHandler::playGameErrorSound(std::string_view gameErrorKey) {
    // 0x005216f0: the game error ERR_BAG_FULL (11) triggers tutorial 0x39.
    if (gameErrorKey == "ERR_BAG_FULL") triggerTutorial(0x39);
    if (const auto speech = audio::vocal_error::forGameError(gameErrorKey)) {
        playErrorSpeech(*speech);
        return;
    }
    if (const auto sound = audio::vocal_error::namedSound(gameErrorKey)) {
        if (auto* ac = services_.audioCoordinator)
            if (auto* ui = ac->getUiSoundManager()) ui->playByName(std::string(*sound));
    }
}

void GameHandler::handleCharLoginFailed(network::Packet& packet) {
    uint8_t reason = packet.readUInt8();

    static const char* reasonNames[] = {
        "Login failed",          // 0
        "World server is down",  // 1
        "Duplicate character",   // 2 (session still active)
        "No instance servers",   // 3
        "Login disabled",        // 4
        "Character not found",   // 5
        "Locked for transfer",   // 6
        "Locked by billing",     // 7
        "Using remote",          // 8
    };
    const char* msg = (reason < 9) ? reasonNames[reason] : "Unknown reason";

    LOG_ERROR("SMSG_CHARACTER_LOGIN_FAILED: reason=", static_cast<int>(reason), " (", msg, ")");

    // Allow the player to re-select a character
    setState(WorldState::CHAR_LIST_RECEIVED);

    if (charLoginFailCallback_) {
        charLoginFailCallback_(msg);
    }
}

void GameHandler::selectCharacter(uint64_t characterGuid) {
    if (state != WorldState::CHAR_LIST_RECEIVED) {
        LOG_WARNING("Cannot select character in state: ", static_cast<int>(state));
        return;
    }

    // Make the selected character authoritative in GameHandler.
    // This avoids relying on UI/Application ordering for appearance-dependent logic.
    activeCharacterGuid_ = characterGuid;

    // The last character's guild is not this one's.
    //
    // The guild name is kept for the character being played and was cleared
    // only when that character left the guild or it disbanded - so logging out
    // of a guilded character and into a guildless one left the name behind,
    // and everything that asks "am I in a guild" answered yes. The guild vault
    // is where that showed: the click passed the check, the server refused it
    // with error 9, and an empty vault window was already on screen.
    if (socialHandler_) socialHandler_->clearGuildMembership();
    if (socialHandler_) socialHandler_->resetForWorldEntry();
    // A new combat log, timed from now (0x007538a0).
    if (combatLogFeed_) combatLogFeed_->enterWorld();
    // The player in the first phase again (0x0052b550: 0xac80a4 = 1).
    {
        world_state_ui::Place place = worldStateUiPlace_;
        place.phaseMask = 1;
        setWorldStateUiPlace(place);
    }
    arenaRoster_.clear();

    LOG_INFO("========================================");
    LOG_INFO("   ENTERING WORLD");
    LOG_INFO("========================================");
    LOG_INFO("Character GUID: 0x", std::hex, characterGuid, std::dec);

    // Find character name for logging
    for (const auto& character : characters) {
        if (character.guid == characterGuid) {
            LOG_INFO("Character: ", character.name);
            LOG_INFO("Level ", static_cast<int>(character.level), " ",
                     getRaceName(character.race), " ",
                     getClassName(character.characterClass));
            playerRace_ = character.race;
            break;
        }
    }

    // Store player GUID
    playerGuid = characterGuid;
    // Entering the world clears a difficulty change's cooldown (0x0052b550
    // -> 0x0052d0e0).
    playerDifficultyCooldownEnd_ = 0;

    // Reset per-character state so previous character data doesn't bleed through
    inventory = Inventory();
    onlineItems_.clear();
    itemInfoCache_.clear();
    pendingItemQueries_.clear();
    equipSlotGuids_ = {};
    backpackSlotGuids_ = {};
    keyringSlotGuids_ = {};
    invSlotBase_ = -1;
    packSlotBase_ = -1;
    lastPlayerFields_.clear();
    onlineEquipDirty_ = false;
    playerMoneyCopper_ = 0;
    playerArmorRating_ = 0;
    std::fill(std::begin(playerResistances_), std::end(playerResistances_), 0);
    std::fill(std::begin(playerStats_), std::end(playerStats_), -1);
    playerMeleeAP_ = -1;
    playerRangedAP_ = -1;
    std::fill(std::begin(playerSpellDmgBonus_), std::end(playerSpellDmgBonus_), -1);
    playerHealBonus_ = -1;
    playerDodgePct_ = -1.0f;
    playerParryPct_ = -1.0f;
    playerBlockPct_ = -1.0f;
    playerExpertise_ = 0;
    playerOffhandExpertise_ = 0;
    playerCritPct_  = -1.0f;
    playerRangedCritPct_ = -1.0f;
    std::fill(std::begin(playerSpellCritPct_), std::end(playerSpellCritPct_), -1.0f);
    std::fill(std::begin(playerCombatRatings_), std::end(playerCombatRatings_), -1);
    if (spellHandler_) spellHandler_->resetAllState();
    // The trade partner and who asked (0x007048d0, from the world's setup
    // 0x00405540): one left from the last world - logged out or cut off
    // mid-request - answered every request with ERR_ALREADY_TRADING.
    if (inventoryHandler_) inventoryHandler_->resetTradeState();
    if (auto* renderer = services_.renderer) {
        if (auto* camera = renderer->getCameraController()) camera->setIntoxication(0.0f);
    }
    spellFlatMods_.clear();
    spellPctMods_.clear();
    actionBar = {};
    // The interface's own start (0x0052b550): no combo points, on nobody
    // (0xbd084d, 0xbd08a8) - the server says them again only when they
    // change, so the last character's stayed on the next one's frame.
    comboPoints_ = 0;
    comboTarget_ = 0;
    // And the pet's information (0x005d6d90): no pet, its spell list emptied
    // (0x005d64f0(0)), its bar and its combo points with it. The server
    // sends SMSG_PET_SPELLS only for a pet there is.
    setActivePet(0);
    petSpellList_.clear();
    petAutocastSpells_.clear();
    petAutocastableSpells_.clear();
    std::fill(std::begin(petActionSlots_), std::end(petActionSlots_), 0u);
    petComboPoints_ = 0;
    petComboTarget_ = 0;
    stableWindowOpen_  = false;
    stableMasterGuid_  = 0;
    stableNumSlots_    = 0;
    stabledPets_.clear();
    playerXp_ = 0;
    playerNextLevelXp_ = 0;
    serverPlayerLevel_ = 1;
    std::fill(playerExploredZones_.begin(), playerExploredZones_.end(), 0u);
    hasPlayerExploredZones_ = false;
    playerSkills_.clear();
    // The handler's, not this class's same-named members - every reader
    // forwards there, so clearing the copies here cleared nothing anyone
    // looks at and the previous character's quests stayed in the log.
    if (questHandler_) questHandler_->clearQuestStateForCharacterSwitch();
    pendingLoginQuestResync_ = false;
    pendingLoginQuestResyncTimeout_ = 0.0f;
    if (questHandler_) {
        questHandler_->pendingQuestAcceptTimeoutsRef().clear();
        questHandler_->pendingQuestAcceptNpcGuidsRef().clear();
    }
    if (combatHandler_) combatHandler_->resetAllCombatState();
    // resetCastState() already called inside resetAllState() above
    playerDead_ = false;
    releasedSpirit_ = false;
    // Back to "nothing has been decided", which is what the original's own
    // start-up does with the same pair (00520000.c:8083 zeroes a stamp and the
    // byte beside it): a character that has not died answers nought, not the
    // -1 a death would have stamped.
    releaseStampMs_ = 0;
    releaseNoTimer_ = false;
    corpseGuid_ = 0;
    corpsePositionValid_ = false;
    corpseTransportLow_ = 0;
    corpseTransportFrame_.reset();
    corpseReclaimAvailableMs_ = 0;
    targetGuid = 0;
    focusGuid = 0;
    lastTargetGuid = 0;
    tabCycleStale = true;
    entityController_->clearAll();
    // A new character's session starts with no transports.
    if (transportManager_) transportManager_->clearTransports("character login");

    // Build CMSG_PLAYER_LOGIN packet
    auto packet = PlayerLoginPacket::build(characterGuid);

    // Send packet
    socket->send(packet);

    setState(WorldState::ENTERING_WORLD);
    LOG_INFO("CMSG_PLAYER_LOGIN sent, entering world...");
}

void GameHandler::handleLoginSetTimeSpeed(network::Packet& packet) {
    // SMSG_LOGIN_SETTIMESPEED (0x042)
    // Structure: PackedTime gameTime, float timeScale
    //
    // gameTime is a packed bitfield, not a count of seconds - Player.cpp writes
    // it with AppendPackedTime. It was being stored raw and then divided by
    // 86400 to get a time of day, which made the sky's clock a number with no
    // relation to the hour the server sent.
    //
    // timeScale: game minutes per real second. Servers send 1/60, so a game day
    // lasts a real day; the clock runs on from here (getGameTime, 0x0076cff0).

    if (packet.getSize() < 8) {
        LOG_WARNING("SMSG_LOGIN_SETTIMESPEED: packet too small (", packet.getSize(), " bytes)");
        return;
    }

    const WowDate now = unpackWowPackedTime(packet.readUInt32());
    float timeScale = packet.readFloat();

    // Hours since midnight, which is what GetGameTime already assumed and what
    // the sky wants. One meaning for this field, written down here.
    gameTime_ = static_cast<float>(now.hour) + static_cast<float>(now.minute) / 60.0f;
    gameTimeSetAt_ = std::chrono::steady_clock::now();
    gameDateSet_ = now;
    timeSpeed_ = clampGameTimeSpeed(timeScale);

    LOG_INFO("Server time: ", now.hour, ":", now.minute, " (gameTime=", gameTime_,
             "h), timeSpeed=", timeSpeed_, " game minutes per second");
}

// SMSG_LOGIN_VERIFY_WORLD: the client's handler is 0x00403de0.
void GameHandler::handleLoginVerifyWorld(network::Packet& packet) {
    LOG_INFO("Handling SMSG_LOGIN_VERIFY_WORLD");
    const bool initialWorldEntry = (state == WorldState::ENTERING_WORLD);

    LoginVerifyWorldData data;
    if (!LoginVerifyWorldParser::parse(packet, data)) {
        fail("Failed to parse SMSG_LOGIN_VERIFY_WORLD");
        return;
    }

    if (!data.isValid()) {
        fail("Invalid world entry data");
        return;
    }

    glm::vec3 canonical = core::coords::serverToCanonical(glm::vec3(data.x, data.y, data.z));
    const bool alreadyInWorld = (state == WorldState::IN_WORLD);
    const bool sameMap = alreadyInWorld && (currentMapId_ == data.mapId);
    // 0x00403de0: on the map the world already shows (0x004d37e0) the
    // client does nothing with it at all, whatever position it carries -
    // only another map moves the player into a world (0x00403b70).
    if (!initialWorldEntry && sameMap) {
        LOG_INFO("Ignoring SMSG_LOGIN_VERIFY_WORLD on the current map: mapId=", data.mapId);
        return;
    }
    if (!initialWorldEntry) {
        LOG_INFO("SMSG_LOGIN_VERIFY_WORLD in-world for another map: mapId=", data.mapId);
    }

    // Successfully entered the world (or teleported)
    currentMapId_ = data.mapId;
    setState(WorldState::IN_WORLD);
    if (socket) {
        socket->tracePacketsFor(std::chrono::seconds(12), "login_verify_world");
    }

    LOG_INFO("========================================");
    LOG_INFO("   SUCCESSFULLY ENTERED WORLD!");
    LOG_INFO("========================================");
    LOG_INFO("Map ID: ", data.mapId);
    LOG_INFO("Position: (", data.x, ", ", data.y, ", ", data.z, ")");
    LOG_INFO("Orientation: ", data.orientation, " radians");
    LOG_INFO("Player is now in the game world");

    // Initialize movement info with world entry position (server → canonical)
    LOG_DEBUG("LOGIN_VERIFY_WORLD: server=(", data.x, ", ", data.y, ", ", data.z,
             ") canonical=(", canonical.x, ", ", canonical.y, ", ", canonical.z, ") mapId=", data.mapId);
    movementInfo.x = canonical.x;
    movementInfo.y = canonical.y;
    movementInfo.z = canonical.z;
    movementInfo.orientation = core::coords::serverToCanonicalYaw(data.orientation);
    movementInfo.flags = 0;
    movementInfo.flags2 = 0;
    if (movementHandler_) {
        movementHandler_->resetMovementClock();
    }
    movementInfo.time = nextMovementTimestampMs();
    if (movementHandler_) {
        movementHandler_->setFalling(false);
        movementHandler_->setFallStartMs(0);
    }
    movementInfo.fallTime = 0;
    movementInfo.jumpVelocity = 0.0f;
    movementInfo.jumpSinAngle = 0.0f;
    movementInfo.jumpCosAngle = 0.0f;
    movementInfo.jumpXYSpeed = 0.0f;
    resurrectPending_ = false;
    resurrectRequestPending_ = false;
    selfResAvailable_ = false;
    onTaxiFlight_ = false;
    taxiMountActive_ = false;
    taxiActivatePending_ = false;
    taxiClientActive_ = false;
    taxiClientPath_.clear();
    // taxiRecoverPending_ is NOT cleared here - it must survive the general
    // state reset so the recovery check below can detect a mid-flight reconnect.
    taxiStartGrace_ = 0.0f;
    currentMountDisplayId_ = 0;
    vehicleId_ = 0;
    vehicleRoster_.clear();
    castVisuals_.clear();
    if (mountCallback_) {
        mountCallback_(0);
    }

    // Clear boss encounter unit slots and raid marks on world transfer
    if (socialHandler_) socialHandler_->resetTransferState();

    // Suppress area triggers on initial login - prevents exit portals from
    // immediately firing when spawning inside a dungeon/instance. Deeprun Tram
    // (map 369) needs a shorter window because exits are close to the spawn.
    const bool deeprunTram = data.mapId == 369;
    activeAreaTriggers_.clear();
    areaTriggerCheckTimer_ = deeprunTram ? -1.0f : -5.0f;
    areaTriggerSuppressFirst_ = true;
    areaTriggerCooldown_ = deeprunTram ? 1.5f : 10.0f;

    // Notify application to load terrain for this map/position (online mode)
    if (worldEntryCallback_) {
        worldEntryCallback_(data.mapId, data.x, data.y, data.z, initialWorldEntry);
    }

    // Send CMSG_SET_ACTIVE_MOVER on initial world entry and world transfers:
    // the player moves itself again (0x00717c50).
    if (movementHandler_) movementHandler_->resetActiveMover();
    if (playerGuid != 0 && socket) {
        auto activeMoverPacket = SetActiveMoverPacket::build(playerGuid);
        socket->send(activeMoverPacket);
        LOG_INFO("Sent CMSG_SET_ACTIVE_MOVER for player 0x", std::hex, playerGuid, std::dec);
        // A new mover: VEHICLE_UPDATE and its aim (0x00747f40).
        fireAddonEvent("VEHICLE_UPDATE", {});
        announceVehicleAim();
    }

    // Immediately sync our position so the server doesn't keep stale coordinates
    // from a previous session or character. Without this, the server can think
    // the player is elsewhere and issue a corrective teleport.
    if (socket) {
        sendMovement(game::Opcode::MSG_MOVE_STOP);
        sendMovement(game::Opcode::MSG_MOVE_HEARTBEAT);
    }

    // Kick the first keepalive immediately on world entry. Classic-like realms
    // can close the session before our default 30s ping cadence fires.
    timeSinceLastPing = 0.0f;
    if (socket) {
        LOG_DEBUG("World entry keepalive: sending immediate ping after LOGIN_VERIFY_WORLD");
        sendPing();
    }

    // If we disconnected mid-taxi, attempt to recover to destination after login.
    if (taxiRecoverPending_ && taxiRecoverMapId_ == data.mapId) {
        float dx = movementInfo.x - taxiRecoverPos_.x;
        float dy = movementInfo.y - taxiRecoverPos_.y;
        float dz = movementInfo.z - taxiRecoverPos_.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > 5.0f) {
            // Keep pending until player entity exists; update() will apply.
            LOG_INFO("Taxi recovery pending: dist=", dist);
        } else {
            taxiRecoverPending_ = false;
        }
    }

    if (initialWorldEntry) {
        // Clear inspect caches on world entry to avoid showing stale data.
        inspectedPlayerAchievements_.clear();

        // Buyback mirror persists across vendor windows within a session but
        // must not leak into another character's session.
        if (inventoryHandler_) inventoryHandler_->clearBuybackState();

        // Reset talent initialization so the first SMSG_TALENTS_INFO after login
        // correctly sets the active spec (static locals don't reset across logins).
        if (spellHandler_) spellHandler_->resetTalentState();

        // Auto-join default chat channels only on first world entry.
        autoJoinDefaultChannels();

        // Auto-query guild info on login.
        const Character* activeChar = getActiveCharacter();
        if (activeChar && activeChar->hasGuild() && socket) {
            auto gqPacket = GuildQueryPacket::build(activeChar->guildId);
            socket->send(gqPacket);
            auto grPacket = GuildRosterPacket::build();
            socket->send(grPacket);
            LOG_INFO("Auto-queried guild info (guildId=", activeChar->guildId, ")");
        }

        if (questHandler_) {
            questHandler_->pendingQuestAcceptTimeoutsRef().clear();
            questHandler_->pendingQuestAcceptNpcGuidsRef().clear();
        }
        pendingQuestQueryIds_.clear();
        pendingLoginQuestResync_ = true;
        pendingLoginQuestResyncTimeout_ = 10.0f;
        completedQuests_.clear();
        LOG_INFO("Queued quest log resync for login (from server quest slots)");

        // Not asked for here: the client sends CMSG_QUERY_QUESTS_COMPLETED
        // only from QueryQuestsCompleted (0x005b1bb0), the one place 0x500 is
        // written, so the list stays empty until an addon asks for it.

        // Auto-request played time on login so the character Stats tab is
        // populated immediately without requiring /played.
        if (socket) {
            auto ptPkt = RequestPlayedTimePacket::build(false);  // false = don't show in chat
            socket->send(ptPkt);
            LOG_INFO("Auto-requested played time on login");
        }
    }

    // Pre-load DBC name caches during world entry so the first packet that
    // needs spell/title/achievement data doesn't stall mid-gameplay (the
    // Spell.dbc cache alone is ~170ms on a cold load).
    if (initialWorldEntry) {
        preloadDBCCaches();
        // Asked once, read all session: the reply carries how long until the
        // daily quests reset, which is the only place that figure comes from.
        // Silently - the announcement belongs to /time.
        queryServerTime(false);
        // No calendar request is pending in a new session (0x00528010 ->
        // 0x005be2a0), and the interface is told so.
        setCalendarActionPending(false);
        // And the guild money the player may take today is asked for
        // (0x00528010 -> 0x005a56f0).
        requestGuildBankWithdrawMoney();
    }
    // 0x00528010 -> 0x005d6e60: with a pet - the charm, else the summon -
    // its bar and spells asked for again, a bare CMSG_REQUEST_PET_INFO.
    if (socket && wireOpcode(Opcode::CMSG_REQUEST_PET_INFO) != 0xFFFF) {
        if (auto me = getEntityManager().getEntity(playerGuid)) {
            const auto guidAt = [&](UF low) -> uint64_t {
                const uint16_t i = fieldIndex(low);
                return i != 0xFFFF ? me->getField(i) | (static_cast<uint64_t>(me->getField(static_cast<uint16_t>(i + 1))) << 32)
                                   : 0ull;
            };
            const uint64_t charm = guidAt(UF::UNIT_FIELD_CHARM);
            const uint64_t pet = charm != 0 ? charm : guidAt(UF::UNIT_FIELD_SUMMON);
            if (pet != 0) {
                network::Packet pkt(wireOpcode(Opcode::CMSG_REQUEST_PET_INFO));
                socket->send(pkt);
            }
        }
    }
    // 0x00528010's tutorials, each shown once: 0x29, 1, 2 and 0 in that order.
    // The client runs this once the world is loaded, by when the server's
    // flags are always in; here world entry can come first, so the four wait
    // for SMSG_TUTORIAL_FLAGS when it has not arrived.
    if (tutorials_.loaded()) triggerEntryTutorials();
    else entryTutorialsPending_ = true;
    // Every way into the world (0x00528010, once per entry - 0x00528c30
    // clears it on the way out): the battleground queues' status asked for
    // (0x0054ca50, CMSG_BATTLEFIELD_STATUS), then the dungeon finder's
    // (0x00557240: CMSG_LFG_GET_STATUS, 0x00555920, and the player's lock
    // list, 0x00555990). All three are bare opcodes.
    if (socket) {
        for (Opcode op : {Opcode::CMSG_BATTLEFIELD_STATUS, Opcode::CMSG_LFG_GET_STATUS,
                          Opcode::CMSG_LFD_PLAYER_LOCK_INFO_REQUEST}) {
            if (wireOpcode(op) == 0xFFFF) continue;
            network::Packet pkt(wireOpcode(op));
            socket->send(pkt);
        }
    }

    // Fire PLAYER_ENTERING_WORLD - THE most important event for addon initialization.
    // Fires on initial login, teleports, instance transitions, and zone changes.
    if (addonEventCallback_) {
        fireAddonEvent("PLAYER_ENTERING_WORLD", {initialWorldEntry ? "1" : "0"});
        // FrameXML arranges itself from this event, so the interface diagnostics
        // wait for it rather than describing a layout nobody ever sees.
        ui::frameXmlNoteWorldEntry();
        // UPDATE_WORLD_STATES so map and battleground addons refresh. Not
        // ZONE_CHANGED_NEW_AREA: the client raises that from the player's
        // area (0x005204c0), which Renderer::updatePlayerArea follows and
        // which says it for the first area on a map.
        fireAddonEvent("UPDATE_WORLD_STATES", {});
        // Entering a battleground, told apart by the map itself rather than by
        // anything the server says about it - BattlemasterList.dbc names the
        // maps and this client already reads it for the queue list.
        //
        // The battleground frame answers this by filtering chat: a battleground
        // announces every player joining and leaving, and the filter collapses
        // that into one line that counts them. Without the event the filter is
        // never installed and all of it goes to the chat window one at a time.
        if (isBattlegroundMap(getCurrentMapId())) {
            fireAddonEvent("PLAYER_ENTERING_BATTLEGROUND", {});
        }
        // The currencies are known now - CurrencyTypes.dbc is read on demand
        // and the amounts are item stacks that have just arrived with the
        // inventory. The main bar builds its token frame on this and had no
        // reason to, so the frame stayed empty however much was held.
        //
        // CURRENCY_DISPLAY_UPDATE (605) is the event the client raises here,
        // from the pass it runs once the player object is live: 0x006e6150
        // installs the player's field watchers (0x006e5180) and then calls
        // 0x00528010, a refresh list guarded to run once per player object,
        // whose currency entry is a call to 0x005b0660 - rebuild the list
        // (0x005b0030), raise 605. KNOWN_CURRENCY_TYPES_UPDATE (604) is not
        // raised on the way in at all: it has exactly one site, the callback
        // watching the known-currency pair (+0x790, eight bytes), and that
        // belongs to a mask that moves rather than to a character arriving.
        //
        // The main bar answers 604 and 605 with one branch
        // (mainmenubar.lua:176-177,193), so either event fills the row - which
        // is why this is a matter of which name is right, not of whether the
        // frame comes up.
        fireAddonEvent("CURRENCY_DISPLAY_UPDATE", {});
        // PLAYER_LOGIN fires only on initial login (not teleports)
        if (initialWorldEntry) {
            fireAddonEvent("PLAYER_LOGIN", {});
        }

        // The player's own values, said again now.
        //
        // Every UNIT_ event this client sends is a change notice: it fires when
        // an update block carries a different number. That is right for keeping
        // a frame current and useless for filling one in, because a frame built
        // before the player existed has missed every one of them and nothing
        // will change health or level just because someone is looking. The
        // original interface fills its unit frames on PLAYER_ENTERING_WORLD for
        // exactly this reason, and it reads the current values rather than the
        // event's - so the event alone is enough, and this is where it belongs.
        for (const char* what : {"UNIT_HEALTH", "UNIT_MAXHEALTH", "UNIT_LEVEL",
                                 "UNIT_DISPLAYPOWER", "UNIT_MANA", "UNIT_MAXMANA",
                                 "UNIT_RAGE", "UNIT_MAXRAGE", "UNIT_ENERGY",
                                 "UNIT_MAXENERGY", "UNIT_FOCUS", "UNIT_MAXFOCUS",
                                 "UNIT_NAME_UPDATE", "UNIT_FACTION"}) {
            fireAddonEvent(what, {"player"});
        }
        fireAddonEvent("PLAYER_XP_UPDATE", {"player"});
        // Dead on arrival, said now.
        //
        // Logging in as a corpse is the one time death is not a transition the
        // client watches happen - it is the state the player already has when
        // the world loads, so no health-drop and no flag-flip fires PLAYER_DEAD
        // and the interface never puts up the release-spirit popup. The player
        // sits dead, unable to act, until the server tires of waiting and
        // pulls them to the graveyard. Same reason every UNIT_ value above is
        // re-sent here: a frame built after the fact has missed the event that
        // would have filled it in.
        if (isPlayerDead()) {
            // Stamped first, and here rather than at the fields: the original
            // decides this countdown at the death (FUN_006dc070 beside the rest
            // of 0x006dc0f0) and at the forced-death packet, and never again
            // while the player lives, so a death nobody watched needs the one
            // place it can be decided at. The interface asks as soon as it hears
            // PLAYER_DEAD, so an unstamped arrival would answer nought - the one
            // answer that shows no release dialog at all - and the player would
            // sit in a corpse with nothing to click.
            decideReleaseTimer();
            fireAddonEvent("PLAYER_DEAD", {});
        }
        // The durability warnings hide themselves when nothing is damaged, and
        // this is the pass that asks them to look (0x006e1e10 -> 0x005e90d0).
        // Without it the frame keeps the state its XML was written with,
        // which is shown.
        updateInventoryAlerts();
    }
}

void GameHandler::handleClientCacheVersion(network::Packet& packet) {
    if (packet.getSize() < 4) {
        LOG_WARNING("SMSG_CLIENTCACHE_VERSION too short: ", packet.getSize(), " bytes");
        return;
    }

    uint32_t version = packet.readUInt32();
    LOG_INFO("SMSG_CLIENTCACHE_VERSION: ", version);
}

void GameHandler::handleTutorialFlags(network::Packet& packet) {
    if (packet.getSize() < 32) {
        LOG_WARNING("SMSG_TUTORIAL_FLAGS too short: ", packet.getSize(), " bytes");
        return;
    }

    std::array<uint32_t, 8> flags{};
    for (uint32_t& v : flags) {
        v = packet.readUInt32();
    }

    tutorials_.loadFlags(flags);  // 0x00530920
    if (entryTutorialsPending_) {
        entryTutorialsPending_ = false;
        triggerEntryTutorials();
    }
    LOG_INFO("SMSG_TUTORIAL_FLAGS: [",
             flags[0], ", ", flags[1], ", ", flags[2], ", ", flags[3], ", ",
             flags[4], ", ", flags[5], ", ", flags[6], ", ", flags[7], "]");
}

void GameHandler::handleMotd(network::Packet& packet) {
    if (chatHandler_) chatHandler_->handleMotd(packet);
}

void GameHandler::handleNotification(network::Packet& packet) {
    // SMSG_NOTIFICATION: single null-terminated string
    std::string message = packet.readString();
    if (message.empty()) return;
    LOG_INFO("Server notification: ", message);
    // 0x00401280: the text goes on the red error line (UI_ERROR_MESSAGE,
    // through 0x00513290), not into chat - this printed it as a system line.
    // A "DEBUGACTION: " prefix is dropped. A leading '~' carries switches:
    // '>' keeps it off the screen, '#' puts it in chat as well, and '^n' makes
    // n the chat type, with anything but 3 going to the yellow info line.
    std::string_view text = message;
    if (text.rfind("DEBUGACTION: ", 0) == 0) text.remove_prefix(13);
    if (text.empty() || text.front() != '~') {
        addUIError(std::string(text));
        return;
    }
    text.remove_prefix(1);
    bool onScreen = true, inChat = false;
    int level = 3, chatType = 0;
    if (!text.empty() && text.front() == '>') { onScreen = false; text.remove_prefix(1); }
    if (!text.empty() && text.front() == '#') { inChat = true; text.remove_prefix(1); }
    if (text.size() >= 2 && text.front() == '^') {
        level = text[1] - '0';
        chatType = level;
        text.remove_prefix(2);
    }
    const std::string line(text);
    if (onScreen && !line.empty()) {
        if (level == 3) addUIError(line);
        else addUIInfoMessage(line);
    }
    if (inChat) addLocalChatLine(static_cast<ChatType>(chatType), line);
}

void GameHandler::sendPing() {
    if (state != WorldState::IN_WORLD) {
        return;
    }

    // Increment sequence number
    pingSequence++;

    LOG_DEBUG("Sending CMSG_PING: sequence=", pingSequence,
              " latencyHintMs=", lastLatency);

    // Record send time for RTT measurement
    pingTimestamp_ = std::chrono::steady_clock::now();

    // Build and send ping packet
    auto packet = PingPacket::build(pingSequence, lastLatency);
    socket->send(packet);
}

// The vehicle interface's requests. A passenger that does not drive asks
// the server (0x0074c7f0, 0x0074c8b0, 0x0074c9a0, 0x0074ca90); the driver -
// its vehicle the active mover, the player on it - sends the vehicle's own
// with its movement.
namespace {
/// Whether the player drives the vehicle it rides: that vehicle is the
/// active mover (0x0074c7f0's test).
bool drivesItsVehicle(const vehicle::VehicleRoster& roster, uint64_t player, uint64_t mover) {
    const auto p = roster.passenger(player);
    return p && mover != 0 && mover != player && p->vehicleGuid == mover;
}
}  // namespace

MovementSpeeds GameHandler::moverSpeeds() const {
    return movementHandler_ ? movementHandler_->moverSpeeds() : MovementSpeeds{};
}

uint64_t GameHandler::activeMover() const {
    return movementHandler_ ? movementHandler_->activeMover() : playerGuid;
}

uint64_t GameHandler::cameraTarget() const {
    return movementHandler_ ? movementHandler_->cameraTarget() : playerGuid;
}

bool GameHandler::onMoveSpline(uint64_t guid) {
    return movementHandler_ && movementHandler_->onMoveSpline(guid);
}

std::optional<uint32_t> GameHandler::moveSplineFlags(uint64_t guid) {
    return movementHandler_ ? movementHandler_->moveSplineFlags(guid) : std::nullopt;
}

uint32_t GameHandler::getSpellAttributesEx2(uint32_t spellId) const {
    if (spellId == 0) return 0;
    loadSpellNameCache();
    auto it = spellNameCache_.find(spellId);
    return it != spellNameCache_.end() ? it->second.attrEx2 : 0u;
}

void GameHandler::sendRequestVehicleExit() {
    if (state != WorldState::IN_WORLD || !socket) return;
    if (drivesItsVehicle(vehicleRoster_, playerGuid, activeMover())) {
        // The driver's exit is PetDismiss (0x0074c7f0 -> 0x005d46f0): the
        // vehicle as its pet leaves by CMSG_DISMISS_CONTROLLED_VEHICLE, any
        // other falls through to sending the pet away.
        dismissPet();
        return;
    }
    socket->send(VehiclePackets::requestExit());
}

void GameHandler::sendRequestVehicleSeatStep(bool next) {
    if (state != WorldState::IN_WORLD || !socket) return;
    if (drivesItsVehicle(vehicleRoster_, playerGuid, activeMover())) {
        // 0x00747fa0, then 0x006ef5a0 with no vehicle and seat 1 (next) or
        // 0xff (previous).
        askVehicleSeatChange();
        if (movementHandler_) movementHandler_->sendChangeSeatsOnControlledVehicle(0, next ? 1 : 0xff);
        return;
    }
    socket->send(next ? VehiclePackets::requestNextSeat() : VehiclePackets::requestPrevSeat());
}

void GameHandler::sendRequestVehicleSwitchSeat(uint64_t vehicleGuid, uint8_t seat) {
    if (state != WorldState::IN_WORLD || !socket) return;
    if (drivesItsVehicle(vehicleRoster_, playerGuid, activeMover())) {
        // 0x0074ca90: 0x00747fa0, then 0x0074bb30 with the vehicle and the
        // seat.
        askVehicleSeatChange();
        if (movementHandler_) movementHandler_->sendChangeSeatsOnControlledVehicle(vehicleGuid, seat);
        return;
    }
    socket->send(VehiclePackets::requestSwitchSeat(vehicleGuid, seat));
}

uint64_t GameHandler::vehicleAimUnit() const {
    // 0x005f9d20: the mover - or, where it sits in a control seat (Flags
    // 0x800), the vehicle under it.
    const uint64_t mover = activeMover();
    if (mover == 0 || !entityController_ || !getEntityManager().getEntity(mover)) return 0;
    if (vehicleRoster_.seatState(mover) == 3) {
        const auto p = vehicleRoster_.passenger(mover);
        const vehicle::SeatRecord* seat = vehicleRoster_.seatOf(mover);
        if (p && seat && (seat->flags & 0x800) != 0 && getEntityManager().getEntity(p->vehicleGuid) &&
            vehicleRoster_.vehicleId(p->vehicleGuid) != 0)
            return p->vehicleGuid;
    }
    return mover;
}

float GameHandler::unitPitch(uint64_t guid) const {
    return guid == activeMover() ? movementInfo.pitch : vehicleRoster_.pitch(guid);
}

float GameHandler::vehicleAimBasePitch() const {
    // The mover's pitch is movementInfo's, which the movement handler shares.
    if (!movementHandler_) return movementInfo.pitch;
    return movementHandler_->aimBasePitch();
}

const vehicle::VehicleRecord* GameHandler::vehicleRowOf(uint64_t guid) const {
    const auto* tables = vehicleRoster_.tables();
    return tables ? tables->vehicle(vehicleRoster_.vehicleId(guid)) : nullptr;
}

void GameHandler::fireVehicleAngleUpdate(float pitch) {
    // 0x005fa910: by the mover's row; nothing without one.
    if (const vehicle::VehicleRecord* moverRow = vehicleRowOf(activeMover())) {
        fireAddonEvent("VEHICLE_ANGLE_UPDATE",
                       {std::to_string(vehicle::normalizedAim(moverRow, pitch)), std::to_string(pitch)});
    }
}

void GameHandler::setVehicleAimKey(bool up, bool held) {
    if (movementHandler_) movementHandler_->setAimKey(up, held);
}

void GameHandler::requestVehicleAim(uint64_t unit, float pitch) {
    // 0x005fb3a0: held in the unit's row's limits, then set on it - at once
    // and sent (0x0072ead0 -> 0x006ee460, MSG_MOVE_SET_PITCH), or over time
    // for movement flags2 0x10 (0x0072d470). Only the mover's own movement
    // goes out.
    const vehicle::VehicleRecord* row = vehicleRowOf(unit);
    pitch = vehicle::clampAim(row, pitch);
    if (unit == 0 || unit != activeMover() || !movementHandler_) return;
    const bool overTime = (movementInfo.flags2 & 0x0010) != 0;
    movementHandler_->requestMoverPitch(pitch);
    if (!overTime && row && (row->flags & vehicle::kVehicleFlagAimAngle)) fireVehicleAngleUpdate(pitch);
}

namespace {
uint32_t seatClockMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
}  // namespace

void GameHandler::askVehicleSeatChange() {
    // 0x00747fa0: the seated player's movement keys are let go and pressed
    // again (0x005fbf10), and the seat change is marked asked (+0x10 0x8,
    // +0x4c), which holds its mover (0x0074ba40).
    if (vehicleRoster_.seatState(playerGuid) != 3) return;
    vehicleRoster_.markSeatChangeAsked(playerGuid, seatClockMs());
}

bool GameHandler::heldBySeat(uint64_t unit) const {
    // 0x0074ba40: the unit's own seat, and the driver's - the player, where
    // the unit is the vehicle it drives. A move held for the vehicle's
    // animation is the passenger's +0x10 0x200.
    if (unit == 0) return false;
    const bool driven = drivesItsVehicle(vehicleRoster_, playerGuid, unit);
    const uint8_t driverState = driven ? vehicleRoster_.seatState(playerGuid) : 0;
    const auto animHeld = [this](uint64_t guid) {
        return movementHandler_ && movementHandler_->vehicleMoveHeld(guid);
    };
    return vehicle::moverHeldBySeat(vehicleRoster_.seatState(unit), animHeld(unit), driverState,
                                    driven && animHeld(playerGuid),
                                    driven && vehicleRoster_.seatChangeAsked(playerGuid, seatClockMs()));
}

void GameHandler::announceVehicleAim() {
    // 0x0074c5a0 for the mover, by its own row: whether the aim's angle
    // (Flags 0x400) and power (0x800) are shown - neither while it may not
    // move for its seat (0x0074ba40) - then the angle.
    const uint64_t mover = activeMover();
    const vehicle::VehicleRecord* row = vehicleRowOf(mover);
    const bool moving = moverHeldBySeat();
    const bool angle = !moving && row && (row->flags & vehicle::kVehicleFlagAimAngle);
    const bool power = !moving && row && (row->flags & vehicle::kVehicleFlagAimPower);
    fireAddonEvent("VEHICLE_ANGLE_SHOW", angle ? std::vector<std::string>{"1"} : std::vector<std::string>{});
    fireAddonEvent("VEHICLE_POWER_SHOW", power ? std::vector<std::string>{"1"} : std::vector<std::string>{});
    if (row) {
        const float pitch = movementInfo.pitch;
        fireAddonEvent("VEHICLE_ANGLE_UPDATE",
                       {std::to_string(vehicle::normalizedAim(row, pitch)), std::to_string(pitch)});
    }
}

void GameHandler::releaseVehicleMove(uint64_t mover) {
    if (movementHandler_) movementHandler_->releaseHeldVehicleMove(mover);
}

void GameHandler::sendEjectPassenger(uint64_t passengerGuid) {
    if (state != WorldState::IN_WORLD || !socket) return;
    socket->send(VehiclePackets::ejectPassenger(passengerGuid));
}

const std::vector<GameHandler::EquipmentSetInfo>& GameHandler::getEquipmentSets() const {
    if (inventoryHandler_) return inventoryHandler_->getEquipmentSets();
    static const std::vector<EquipmentSetInfo> empty;
    return empty;
}

const std::array<uint64_t, 19>* GameHandler::getEquipmentSetItems(uint32_t setId) const {
    return inventoryHandler_ ? inventoryHandler_->getEquipmentSetItems(setId) : nullptr;
}

uint32_t GameHandler::getEquipmentSetIgnoreMask(uint32_t setId) const {
    return inventoryHandler_ ? inventoryHandler_->getEquipmentSetIgnoreMask(setId) : 0u;
}

// Trade state delegation to InventoryHandler (which owns the canonical trade state)
GameHandler::TradeStatus GameHandler::getTradeStatus() const {
    if (inventoryHandler_) return static_cast<TradeStatus>(inventoryHandler_->getTradeStatus());
    return tradeStatus_;
}
bool GameHandler::hasPendingTradeRequest() const {
    return inventoryHandler_ ? inventoryHandler_->hasPendingTradeRequest() : false;
}
bool GameHandler::isTradeOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isTradeOpen() : false;
}
uint64_t GameHandler::getTradePeerGuid() const {
    return inventoryHandler_ ? inventoryHandler_->getTradePeerGuid() : 0;
}

bool GameHandler::feedPetWithItem(uint64_t itemGuid) {
    if (!spellHandler_ || itemGuid == 0) return false;
    for (uint32_t spellId : spellHandler_->getKnownSpells()) {
        const auto it = spellNameCache_.find(spellId);
        if (it == spellNameCache_.end()) continue;
        for (uint32_t effect : it->second.effectIds) {
            if (effect == 101) {
                spellHandler_->castSpellAtItem(spellId, itemGuid);
                return true;
            }
        }
    }
    return false;
}

const std::string& GameHandler::getTradePeerName() const {
    if (inventoryHandler_) return inventoryHandler_->getTradePeerName();
    return tradePeerName_;
}
const std::array<GameHandler::TradeSlot, GameHandler::TRADE_SLOT_COUNT>& GameHandler::getMyTradeSlots() const {
    if (inventoryHandler_) {
        // Convert InventoryHandler::TradeSlot → GameHandler::TradeSlot (different struct layouts)
        static std::array<TradeSlot, TRADE_SLOT_COUNT> converted{};
        const auto& src = inventoryHandler_->getMyTradeSlots();
        for (size_t i = 0; i < TRADE_SLOT_COUNT; i++) {
            converted[i].itemId = src[i].itemId;
            converted[i].displayId = src[i].displayId;
            converted[i].stackCount = src[i].stackCount;
            converted[i].itemGuid = src[i].itemGuid;
            converted[i].inst = src[i].inst;
            converted[i].occupied = (src[i].itemId != 0);
        }
        return converted;
    }
    return myTradeSlots_;
}
const std::array<GameHandler::TradeSlot, GameHandler::TRADE_SLOT_COUNT>& GameHandler::getPeerTradeSlots() const {
    if (inventoryHandler_) {
        static std::array<TradeSlot, TRADE_SLOT_COUNT> converted{};
        const auto& src = inventoryHandler_->getPeerTradeSlots();
        for (size_t i = 0; i < TRADE_SLOT_COUNT; i++) {
            converted[i].itemId = src[i].itemId;
            converted[i].displayId = src[i].displayId;
            converted[i].stackCount = src[i].stackCount;
            converted[i].itemGuid = src[i].itemGuid;
            converted[i].inst = src[i].inst;
            converted[i].occupied = (src[i].itemId != 0);
        }
        return converted;
    }
    return peerTradeSlots_;
}
uint64_t GameHandler::getMyTradeGold() const {
    return inventoryHandler_ ? inventoryHandler_->getMyTradeGold() : myTradeGold_;
}
uint64_t GameHandler::getPeerTradeGold() const {
    return inventoryHandler_ ? inventoryHandler_->getPeerTradeGold() : peerTradeGold_;
}
uint32_t GameHandler::getTradeEnchantSpell(bool ownSide) const {
    return inventoryHandler_ ? inventoryHandler_->getTradeEnchantSpell(ownSide) : 0;
}

std::string GameHandler::itemNameWithProperty(const std::string& name, int32_t randomPropertyId) const {
    if (randomPropertyId == 0 || name.empty()) return name;
    const std::string property = getRandomPropertyName(randomPropertyId);
    if (property.empty()) return name;
    return interfaceFormat("ITEM_SUFFIX_TEMPLATE", "%s %s", {name, property});
}

bool GameHandler::supportsEquipmentSets() const {
    return inventoryHandler_ && inventoryHandler_->supportsEquipmentSets();
}

void GameHandler::useEquipmentSet(uint32_t setId) {
    if (inventoryHandler_) inventoryHandler_->useEquipmentSet(setId);
}

void GameHandler::saveEquipmentSet(const std::string& name, const std::string& iconName,
                                    uint64_t existingGuid, uint32_t setIndex,
                                    uint32_t ignoredSlotMask) {
    if (inventoryHandler_)
        inventoryHandler_->saveEquipmentSet(name, iconName, existingGuid, setIndex, ignoredSlotMask);
}

void GameHandler::deleteEquipmentSet(uint64_t setGuid) {
    if (inventoryHandler_) inventoryHandler_->deleteEquipmentSet(setGuid);
}

// --- Inventory state delegation (canonical state lives in InventoryHandler) ---

// Item text
bool GameHandler::isItemTextOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isItemTextOpen() : itemTextOpen_;
}
const std::string& GameHandler::getItemText() const {
    if (inventoryHandler_) return inventoryHandler_->getItemText();
    return itemText_;
}
void GameHandler::closeItemText() {
    if (inventoryHandler_) inventoryHandler_->closeItemText();
    else itemTextOpen_ = false;
}

// Loot
bool GameHandler::isLootWindowOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isLootWindowOpen() : lootWindowOpen;
}
const LootResponseData& GameHandler::getCurrentLoot() const {
    if (inventoryHandler_) return inventoryHandler_->getCurrentLoot();
    return currentLoot;
}
void GameHandler::setAutoLoot(bool enabled) {
    if (inventoryHandler_) inventoryHandler_->setAutoLoot(enabled);
    else autoLoot_ = enabled;
}
bool GameHandler::clickAutoLoots() const {
    const bool byDefault = addons::storedCVarValue("autoLootDefault", "0") != "0";
    return byDefault != addons::modifiedClickHeld("AUTOLOOTTOGGLE");
}
bool GameHandler::isAutoLoot() const {
    return inventoryHandler_ ? inventoryHandler_->isAutoLoot() : autoLoot_;
}
void GameHandler::setAutoSellGrey(bool enabled) {
    if (inventoryHandler_) inventoryHandler_->setAutoSellGrey(enabled);
    else autoSellGrey_ = enabled;
}
bool GameHandler::isAutoSellGrey() const {
    return inventoryHandler_ ? inventoryHandler_->isAutoSellGrey() : autoSellGrey_;
}
void GameHandler::setAutoRepair(bool enabled) {
    if (inventoryHandler_) inventoryHandler_->setAutoRepair(enabled);
    else autoRepair_ = enabled;
}
bool GameHandler::isAutoRepair() const {
    return inventoryHandler_ ? inventoryHandler_->isAutoRepair() : autoRepair_;
}
const std::vector<uint64_t>& GameHandler::getMasterLootCandidates() const {
    if (inventoryHandler_) return inventoryHandler_->getMasterLootCandidates();
    return masterLootCandidates_;
}
bool GameHandler::hasMasterLootCandidates() const {
    return inventoryHandler_ ? inventoryHandler_->hasMasterLootCandidates() : !masterLootCandidates_.empty();
}
const LootRollEntry* GameHandler::getLootRoll(int rollId) const {
    return inventoryHandler_ ? inventoryHandler_->getLootRoll(rollId) : nullptr;
}

// Vendor
bool GameHandler::isVendorWindowOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isVendorWindowOpen() : vendorWindowOpen;
}
const ListInventoryData& GameHandler::getVendorItems() const {
    if (inventoryHandler_) return inventoryHandler_->getVendorItems();
    return currentVendorItems;
}
void GameHandler::updateInventoryAlerts() {
    namespace ia = inventory_alerts;
    bool moved = false;
    for (int i = 0; i < ia::kCount; ++i) {
        const int slot = ia::kSlots[static_cast<size_t>(i)];
        int status = 0;
        if (slot < 0) {
            const uint32_t ammo = getAmmoId();
            status = ia::ammoAlert(ammo, ammo ? static_cast<uint32_t>(getInventory().countItem(ammo)) : 0);
        } else {
            const auto& s = getInventory().getEquipSlot(static_cast<EquipSlot>(slot));
            ia::Item item;
            item.present = !s.empty();
            item.durability = s.item.curDurability;
            item.maxDurability = s.item.maxDurability;
            item.fieldFlags = getItemFieldFlags(getEquipSlotGuid(slot));
            const ia::Result r = ia::itemAlert(item);
            status = r.status;
            if (r.tutorial >= 0) triggerTutorial(static_cast<uint32_t>(r.tutorial));
        }
        if (inventoryAlerts_[static_cast<size_t>(i)] != status) {
            inventoryAlerts_[static_cast<size_t>(i)] = status;
            moved = true;
        }
    }
    if (moved) fireAddonEvent("UPDATE_INVENTORY_ALERTS", {});
    fireAddonEvent("UPDATE_INVENTORY_DURABILITY", {});
}

void GameHandler::setVendorCanRepair(bool v) {
    if (inventoryHandler_) inventoryHandler_->setVendorCanRepair(v);
    else currentVendorItems.canRepair = v;
}
const std::deque<GameHandler::BuybackItem>& GameHandler::getBuybackItems() const {
    if (inventoryHandler_) {
        // Layout-identical structs (InventoryHandler::BuybackItem == GameHandler::BuybackItem)
        return reinterpret_cast<const std::deque<BuybackItem>&>(inventoryHandler_->getBuybackItems());
    }
    return buybackItems_;
}
uint64_t GameHandler::getVendorGuid() const {
    if (inventoryHandler_) return inventoryHandler_->getVendorGuid();
    return currentVendorItems.vendorGuid;
}
uint32_t GameHandler::buybackFieldPrice(uint32_t wireSlot) const {
    return inventoryHandler_ ? inventoryHandler_->buybackFieldPrice(wireSlot) : 0u;
}
uint32_t GameHandler::buybackRowCount() const {
    if (inventoryHandler_) return inventoryHandler_->buybackRowCount();
    // No handler of its own: this deque is a leftover of the time the vendor
    // state lived here, and nothing ever filled ring slots into it.
    return static_cast<uint32_t>(buybackItems_.size());
}
const GameHandler::BuybackItem* GameHandler::buybackRowAt(uint32_t row) const {
    if (inventoryHandler_) {
        // Layout-identical structs; see getBuybackItems.  nullptr stays nullptr.
        return reinterpret_cast<const BuybackItem*>(inventoryHandler_->buybackRow(row));
    }
    if (row >= buybackItems_.size()) return nullptr;
    return &buybackItems_[row];
}

// Mail
bool GameHandler::isSocketingOpen() const {
    return inventoryHandler_ && inventoryHandler_->getSocketSession().open;
}

uint64_t GameHandler::getSocketItemGuid() const {
    return inventoryHandler_ ? inventoryHandler_->getSocketSession().itemGuid : 0;
}

uint32_t GameHandler::getSocketItemId() const {
    return inventoryHandler_ ? inventoryHandler_->getSocketSession().itemId : 0;
}
uint32_t GameHandler::getSocketPendingGemItemId(int index) const {
    if (!inventoryHandler_ || index < 0 || index > 2) return 0;
    return inventoryHandler_->getSocketSession().newGemItemId[index];
}

void GameHandler::openSocketing(uint64_t itemGuid) {
    if (inventoryHandler_) inventoryHandler_->openSocketing(itemGuid);
}

void GameHandler::closeSocketing() {
    if (inventoryHandler_) inventoryHandler_->closeSocketing();
}

bool GameHandler::setSocketGem(int index, uint64_t gemGuid, uint32_t gemItemId) {
    return inventoryHandler_ && inventoryHandler_->setSocketGem(index, gemGuid, gemItemId);
}

void GameHandler::acceptSockets() {
    if (inventoryHandler_) inventoryHandler_->acceptSockets();
}

bool GameHandler::isMailboxOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isMailboxOpen() : mailboxOpen_;
}
const std::vector<MailMessage>& GameHandler::getMailInbox() const {
    if (inventoryHandler_) return inventoryHandler_->getMailInbox();
    return mailInbox_;
}
std::vector<MailMessage>& GameHandler::mailInboxRef() {
    if (inventoryHandler_) return inventoryHandler_->mailInboxRef();
    return mailInbox_;
}
std::string GameHandler::getMailDisplaySubject(const MailMessage& mail) {
    if (mail.messageType != 2) return mail.subject;

    AuctionMailSubject auction;
    if (!parseAuctionMailSubject(mail.subject, auction)) return mail.subject;

    ensureItemInfo(auction.itemEntry);
    const auto* info = getItemInfo(auction.itemEntry);
    const std::string itemName = info && !info->name.empty()
        ? info->name
        : "Item #" + std::to_string(auction.itemEntry);
    return formatAuctionMailSubject(auction, itemName);
}
int GameHandler::getSelectedMailIndex() const {
    return inventoryHandler_ ? inventoryHandler_->getSelectedMailIndex() : selectedMailIndex_;
}
void GameHandler::setSelectedMailIndex(int idx) {
    if (inventoryHandler_) inventoryHandler_->setSelectedMailIndex(idx);
    else selectedMailIndex_ = idx;
}
bool GameHandler::isMailComposeOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isMailComposeOpen() : showMailCompose_;
}
void GameHandler::openMailCompose() {
    if (inventoryHandler_) inventoryHandler_->openMailCompose();
    else { showMailCompose_ = true; clearMailAttachments(); }
}
void GameHandler::closeMailCompose() {
    if (inventoryHandler_) inventoryHandler_->closeMailCompose();
    else { showMailCompose_ = false; clearMailAttachments(); }
}
void GameHandler::setMailComposeShowing(bool showing) {
    if (inventoryHandler_) inventoryHandler_->setMailComposeShowing(showing);
    else showMailCompose_ = showing;
}
bool GameHandler::hasNewMail() const {
    return inventoryHandler_ ? inventoryHandler_->hasNewMail() : hasNewMail_;
}
const std::array<GameHandler::MailAttachSlot, 12>& GameHandler::getMailAttachments() const {
    if (inventoryHandler_) {
        // Layout-identical structs (InventoryHandler::MailAttachSlot == GameHandler::MailAttachSlot)
        return reinterpret_cast<const std::array<MailAttachSlot, 12>&>(inventoryHandler_->getMailAttachments());
    }
    return mailAttachments_;
}


// Bank
bool GameHandler::isBankOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isBankOpen() : bankOpen_;
}
uint64_t GameHandler::getBankerGuid() const {
    return inventoryHandler_ ? inventoryHandler_->getBankerGuid() : bankerGuid_;
}
int GameHandler::getEffectiveBankSlots() const {
    return inventoryHandler_ ? inventoryHandler_->getEffectiveBankSlots() : effectiveBankSlots_;
}
int GameHandler::getEffectiveBankBagSlots() const {
    return inventoryHandler_ ? inventoryHandler_->getEffectiveBankBagSlots() : effectiveBankBagSlots_;
}

// Guild Bank
bool GameHandler::isGuildBankOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isGuildBankOpen() : guildBankOpen_;
}
const GuildBankData& GameHandler::getGuildBankData() const {
    if (inventoryHandler_) return inventoryHandler_->getGuildBankData();
    return guildBankData_;
}
void GameHandler::requestGuildBankWithdrawMoney() {
    if (inventoryHandler_) inventoryHandler_->requestGuildBankWithdrawMoney();
}

int32_t GameHandler::getGuildBankWithdrawMoney() const {
    return inventoryHandler_ ? inventoryHandler_->getGuildBankWithdrawMoney() : 0;
}

void GameHandler::requestGuildPermissions() {
    if (socialHandler_) socialHandler_->requestGuildPermissions();
    requestGuildBankWithdrawMoney();
}

uint8_t GameHandler::getGuildBankTabCount() const {
    return inventoryHandler_ ? inventoryHandler_->getGuildBankTabCount() : 0;
}

void GameHandler::setGuildBankTabCount(uint8_t count) {
    if (inventoryHandler_) inventoryHandler_->setGuildBankTabCount(count);
}

void GameHandler::setGuildBankMoney(uint64_t money) {
    if (inventoryHandler_) inventoryHandler_->setGuildBankMoney(money);
    else guildBankData_.money = money;
}
uint8_t GameHandler::getGuildBankActiveTab() const {
    return inventoryHandler_ ? inventoryHandler_->getGuildBankActiveTab() : guildBankActiveTab_;
}
void GameHandler::setGuildBankActiveTab(uint8_t tab) {
    if (inventoryHandler_) inventoryHandler_->setGuildBankActiveTab(tab);
    else guildBankActiveTab_ = tab;
}

// Auction House
uint32_t GameHandler::getAuctionHouseId() const {
    return inventoryHandler_ ? inventoryHandler_->getAuctionHouseId() : 0;
}
bool GameHandler::isAuctionHouseOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isAuctionHouseOpen() : auctionOpen_;
}
uint64_t GameHandler::getAuctioneerGuid() const {
    return inventoryHandler_ ? inventoryHandler_->getAuctioneerGuid() : auctioneerGuid_;
}
const AuctionListResult& GameHandler::getAuctionBrowseResults() const {
    if (inventoryHandler_) return inventoryHandler_->getAuctionBrowseResults();
    return auctionBrowseResults_;
}
AuctionListResult& GameHandler::auctionBrowseResultsRef() {
    if (inventoryHandler_) return inventoryHandler_->auctionBrowseResultsRef();
    return auctionBrowseResults_;
}
const AuctionListResult& GameHandler::getAuctionOwnerResults() const {
    if (inventoryHandler_) return inventoryHandler_->getAuctionOwnerResults();
    return auctionOwnerResults_;
}
AuctionListResult& GameHandler::auctionOwnerResultsRef() {
    if (inventoryHandler_) return inventoryHandler_->auctionOwnerResultsRef();
    return auctionOwnerResults_;
}
const AuctionListResult& GameHandler::getAuctionBidderResults() const {
    if (inventoryHandler_) return inventoryHandler_->getAuctionBidderResults();
    return auctionBidderResults_;
}
AuctionListResult& GameHandler::auctionBidderResultsRef() {
    if (inventoryHandler_) return inventoryHandler_->auctionBidderResultsRef();
    return auctionBidderResults_;
}
int GameHandler::getAuctionActiveTab() const {
    return inventoryHandler_ ? inventoryHandler_->getAuctionActiveTab() : auctionActiveTab_;
}
void GameHandler::setAuctionActiveTab(int tab) {
    if (inventoryHandler_) inventoryHandler_->setAuctionActiveTab(tab);
    else auctionActiveTab_ = tab;
}
float GameHandler::getAuctionSearchDelay() const {
    return inventoryHandler_ ? inventoryHandler_->getAuctionSearchDelay() : 0.0f;
}

// Trainer
bool GameHandler::isTrainerWindowOpen() const {
    return inventoryHandler_ ? inventoryHandler_->isTrainerWindowOpen() : trainerWindowOpen_;
}
const TrainerListData& GameHandler::getTrainerSpells() const {
    if (inventoryHandler_) return inventoryHandler_->getTrainerSpells();
    // inventoryHandler_ is always constructed; this is the empty answer for the
    // theoretical null case, replacing a GameHandler-owned copy nothing wrote.
    static const TrainerListData kEmpty;
    return kEmpty;
}
const std::vector<GameHandler::TrainerTab>& GameHandler::getTrainerTabs() const {
    if (inventoryHandler_) {
        // Layout-identical structs (InventoryHandler::TrainerTab == GameHandler::TrainerTab)
        return reinterpret_cast<const std::vector<TrainerTab>&>(inventoryHandler_->getTrainerTabs());
    }
    static const std::vector<TrainerTab> kEmpty;
    return kEmpty;
}

void GameHandler::sendMinimapPing(float wowX, float wowY) {
    if (socialHandler_) socialHandler_->sendMinimapPing(wowX, wowY);
}

void GameHandler::handlePong(network::Packet& packet) {
    LOG_DEBUG("Handling SMSG_PONG");

    PongData data;
    if (!PongParser::parse(packet, data)) {
        LOG_WARNING("Failed to parse SMSG_PONG");
        return;
    }

    // Verify sequence matches
    if (data.sequence != pingSequence) {
        LOG_WARNING("SMSG_PONG sequence mismatch: expected ", pingSequence,
                    ", got ", data.sequence);
        return;
    }

    // Measure round-trip time
    auto rtt = std::chrono::steady_clock::now() - pingTimestamp_;
    lastLatency = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(rtt).count());

    LOG_DEBUG("SMSG_PONG acknowledged: sequence=", data.sequence,
              " latencyMs=", lastLatency);
}

bool GameHandler::isServerMovementAllowed() const {
    return movementHandler_ ? movementHandler_->isServerMovementAllowed() : true;
}

uint32_t GameHandler::nextMovementTimestampMs() {
    if (movementHandler_) return movementHandler_->nextMovementTimestampMs();
    return 0;
}

void GameHandler::sendMovement(Opcode opcode) {
    if (movementHandler_) movementHandler_->sendMovement(opcode);
}

// ToggleRun (0x005faae0): the mover's walk mode. Refused while a server
// spline carries the mover (its +0xbc without 0x400), while its seat holds it
// (0x0074ba40), and unless it is alive, standing, unrooted and off a transport
// (0x005fa9e0: health, movement flags 0x100a00, stand state 7). Otherwise the
// mode flips by the move event 0x11/0x12 (0x0071ae50 -> 0x006ecf10), which
// sets or clears WALKING (0x0098b0e0) and sends MSG_MOVE_SET_RUN_MODE or
// MSG_MOVE_SET_WALK_MODE (0x006ef860, 0xc2/0xc3).
void GameHandler::toggleRun() {
    if (state != WorldState::IN_WORLD) return;
    const uint64_t mover = activeMover();
    auto unit = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(mover));
    if (!unit || onMoveSpline(mover) || moverHeldBySeat()) return;
    constexpr uint32_t kRefused = static_cast<uint32_t>(MovementFlags::ROOT) |
                                  static_cast<uint32_t>(MovementFlags::ONTRANSPORT) | 0x00100000u;
    constexpr uint8_t kStandStateDead = 7;
    if (unit->getHealth() == 0 || (movementInfo.flags & kRefused) != 0 || isOnTransport() ||
        unit->getStandState() == kStandStateDead) {
        return;
    }
    sendMovement(movementInfo.hasFlag(MovementFlags::WALKING) ? Opcode::MSG_MOVE_SET_RUN_MODE
                                                               : Opcode::MSG_MOVE_SET_WALK_MODE);
}

bool GameHandler::isWalkMode() const {
    return movementInfo.hasFlag(MovementFlags::WALKING);
}

void GameHandler::sanitizeMovementForTaxi() {
    if (movementHandler_) movementHandler_->sanitizeMovementForTaxi();
}

void GameHandler::forceClearTaxiAndMovementState() {
    if (movementHandler_) movementHandler_->forceClearTaxiAndMovementState();
}

void GameHandler::setPosition(float x, float y, float z) {
    if (movementHandler_) movementHandler_->setPosition(x, y, z);
}

void GameHandler::setOrientation(float orientation) {
    if (movementHandler_) movementHandler_->setOrientation(orientation);
}

// Entity lifecycle methods (handleUpdateObject, processOutOfRangeObjects,
// applyUpdateObjectBlock, finalizeUpdateObjectBatch, handleCompressedUpdateObject,
// handleDestroyObject) moved to EntityController - see entity_controller.cpp

void GameHandler::sendChatMessage(ChatType type, const std::string& message, const std::string& target,
                                  uint32_t language) {
    if (chatHandler_) chatHandler_->sendChatMessage(type, message, target, language);
}

void GameHandler::sendAddonMessage(ChatType type, const std::string& message, const std::string& target) {
    if (chatHandler_) chatHandler_->sendAddonMessage(type, message, target);
}

bool GameHandler::sendTextEmote(uint32_t textEmoteId, uint64_t targetGuid) {
    // 0x006dd9e0: an emote the player may not do now is not sent; one
    // refused for moving says so (ERR_NOEMOTEWHILERUNNING, 0x005216f0 with
    // 0x14c). One that is sent puts the weapons away first.
    if (textEmoteGateCallback_ && state == WorldState::IN_WORLD) {
        switch (textEmoteGateCallback_(textEmoteId)) {
            case TextEmoteGate::Send: break;
            case TextEmoteGate::RefuseWhileMoving:
                raiseUiError(interfaceString("ERR_NOEMOTEWHILERUNNING", "You can't do that while moving!"));
                return false;
            case TextEmoteGate::Refuse: return false;
        }
    }
    // Aimed at the player itself, it is aimed at nobody.
    if (targetGuid == playerGuid) targetGuid = 0;
    if (chatHandler_) chatHandler_->sendTextEmote(textEmoteId, targetGuid);
    return true;
}

void GameHandler::joinChannel(const std::string& channelName, const std::string& password) {
    if (chatHandler_) chatHandler_->joinChannel(channelName, password);
}

void GameHandler::leaveChannel(const std::string& channelName) {
    if (chatHandler_) chatHandler_->leaveChannel(channelName);
}

std::string GameHandler::getChannelByIndex(int index) const {
    return chatHandler_ ? chatHandler_->getChannelByIndex(index) : "";
}

int GameHandler::getChannelIndex(const std::string& channelName) const {
    return chatHandler_ ? chatHandler_->getChannelIndex(channelName) : 0;
}

void GameHandler::autoJoinDefaultChannels() {
    if (chatHandler_) {
        chatHandler_->chatAutoJoin.general = chatAutoJoin.general;
        chatHandler_->chatAutoJoin.trade = chatAutoJoin.trade;
        chatHandler_->chatAutoJoin.localDefense = chatAutoJoin.localDefense;
        chatHandler_->chatAutoJoin.lfg = chatAutoJoin.lfg;
        chatHandler_->chatAutoJoin.local = chatAutoJoin.local;
        chatHandler_->autoJoinDefaultChannels();
    }
}

void GameHandler::setTarget(uint64_t guid) {
    if (combatHandler_) combatHandler_->setTarget(guid);
}

bool GameHandler::targetUnit(uint64_t guid) {
    return combatHandler_ ? combatHandler_->targetUnit(guid) : false;
}

void GameHandler::clearTarget() {
    if (combatHandler_) combatHandler_->clearTarget();
}

std::shared_ptr<Entity> GameHandler::getTarget() const {
    return combatHandler_ ? combatHandler_->getTarget() : nullptr;
}

void GameHandler::setFocus(uint64_t guid) {
    if (combatHandler_) combatHandler_->setFocus(guid);
}

void GameHandler::clearFocus() {
    if (combatHandler_) combatHandler_->clearFocus();
}

void GameHandler::setMouseoverGuid(uint64_t guid) {
    if (combatHandler_) combatHandler_->setMouseoverGuid(guid);
}

std::shared_ptr<Entity> GameHandler::getFocus() const {
    return combatHandler_ ? combatHandler_->getFocus() : nullptr;
}

void GameHandler::stopAutoRepeat(bool tellServer) {
    if (!autoRepeating_) return;
    if (tellServer && socket && isInWorld()) {
        network::Packet packet(wireOpcode(Opcode::CMSG_CANCEL_AUTO_REPEAT_SPELL));
        socket->send(packet);
    }
    autoRepeating_ = false;
    autoRepeatSpellId_ = 0;
    fireAddonEvent("STOP_AUTOREPEAT_SPELL", {});
}

namespace {
std::string questRewardLine(const ItemQueryResponseData& info, uint32_t count, uint32_t level) {
    const std::string link = buildItemLink(info.entry, info.quality, info.name, level);
    if (count > 1)
        return interfaceFormat("ERR_QUEST_REWARD_ITEM_MULT_IS", "Received %d of item: %s.",
                               {std::to_string(count), link});
    return interfaceFormat("ERR_QUEST_REWARD_ITEM_S", "Received item: %s.", {link});
}
}  // namespace

void GameHandler::announceQuestRewardItem(uint32_t itemId, uint32_t count) {
    if (itemId == 0) return;
    if (const auto* info = getItemInfo(itemId); info && info->valid) {
        addSystemChatMessage(questRewardLine(*info, count, getPlayerLevel()));
        return;
    }
    pendingQuestRewardItems_.emplace_back(itemId, count);
    ensureItemInfo(itemId);
}

void GameHandler::questRewardItemArrived(uint32_t itemId) {
    const auto* info = getItemInfo(itemId);
    for (auto it = pendingQuestRewardItems_.begin(); it != pendingQuestRewardItems_.end();) {
        if (it->first != itemId) { ++it; continue; }
        if (info && info->valid) addSystemChatMessage(questRewardLine(*info, it->second, getPlayerLevel()));
        it = pendingQuestRewardItems_.erase(it);
    }
}

void GameHandler::targetLastTarget() {
    if (combatHandler_) combatHandler_->targetLastTarget();
}

void GameHandler::targetEnemy(bool reverse) {
    if (combatHandler_) combatHandler_->targetEnemy(reverse);
}

void GameHandler::targetFriend(bool reverse) {
    if (combatHandler_) combatHandler_->targetFriend(reverse);
}
void GameHandler::targetNearestEnemyPlayer(bool reverse) {
    if (combatHandler_) combatHandler_->targetNearestEnemyPlayer(reverse);
}
void GameHandler::targetNearestFriendPlayer(bool reverse) {
    if (combatHandler_) combatHandler_->targetNearestFriendPlayer(reverse);
}
void GameHandler::targetNearestPartyMember(bool reverse) {
    if (combatHandler_) combatHandler_->targetNearestPartyMember(reverse);
}
void GameHandler::targetNearestRaidMember(bool reverse) {
    if (combatHandler_) combatHandler_->targetNearestRaidMember(reverse);
}
void GameHandler::targetNearest(bool reverse) {
    if (combatHandler_) combatHandler_->targetNearest(reverse);
}
void GameHandler::attackTarget() {
    // 0x0072c2b0: a running swing (the player's +0x1858 0x2, or a unit it is
    // aimed at) is stopped while the player has control (0x00bcfb8c);
    // otherwise one is started at the target, where there is one.
    if (isAutoAttacking()) {
        stopAutoAttack();
        return;
    }
    if (targetGuid != 0) startAutoAttack(targetGuid);
}
void GameHandler::targetLastEnemy() {
    if (combatHandler_) combatHandler_->targetLastEnemy();
}
void GameHandler::targetLastFriend() {
    if (combatHandler_) combatHandler_->targetLastFriend();
}

void GameHandler::inspectTarget() {
    if (socialHandler_) socialHandler_->inspectTarget();
}

void GameHandler::inspectUnit(uint64_t guid) {
    if (socialHandler_) socialHandler_->inspectUnit(guid);
}

void GameHandler::requestInspectHonorData(uint64_t guid) {
    if (socialHandler_) socialHandler_->requestInspectHonorData(guid);
}

const GameHandler::InspectResult* GameHandler::getInspectResult() const {
    return socialHandler_ ? socialHandler_->getInspectResult() : nullptr;
}

const ArenaTeamQueryData* GameHandler::arenaTeamQuery(uint32_t teamId) {
    return socialHandler_ ? socialHandler_->arenaTeamQuery(teamId) : nullptr;
}

void GameHandler::sendZoneUpdate(uint32_t zoneId) {
    if (zoneId == 0 || state != WorldState::IN_WORLD || !socket) return;
    if (wireOpcode(Opcode::CMSG_ZONEUPDATE) == 0xFFFF) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_ZONEUPDATE));
    packet.writeUInt32(zoneId);
    socket->send(packet);
}

void GameHandler::queryServerTime(bool announce) {
    if (socialHandler_) socialHandler_->queryServerTime(announce);
}

uint64_t GameHandler::getTalentWipeNpcGuid() const {
    return spellHandler_ ? spellHandler_->getTalentWipeNpcGuid() : 0u;
}

uint32_t GameHandler::getSecondsUntilDailyReset() const {
    return socialHandler_ ? socialHandler_->getSecondsUntilDailyReset() : 0u;
}

void GameHandler::requestPlayedTime() {
    if (socialHandler_) socialHandler_->requestPlayedTime();
}

void GameHandler::queryWho(const std::string& playerName) {
    if (socialHandler_) socialHandler_->queryWho(playerName);
}

void GameHandler::addFriend(const std::string& playerName, const std::string& note) {
    if (socialHandler_) socialHandler_->addFriend(playerName, note);
}

void GameHandler::removeFriend(const std::string& playerName) {
    if (socialHandler_) socialHandler_->removeFriend(playerName);
}

void GameHandler::setFriendNote(const std::string& playerName, const std::string& note) {
    if (socialHandler_) socialHandler_->setFriendNote(playerName, note);
}

void GameHandler::randomRoll(uint32_t minRoll, uint32_t maxRoll) {
    if (socialHandler_) socialHandler_->randomRoll(minRoll, maxRoll);
}

void GameHandler::addIgnore(const std::string& playerName) {
    if (socialHandler_) socialHandler_->addIgnore(playerName);
}

void GameHandler::removeIgnore(const std::string& playerName) {
    if (socialHandler_) socialHandler_->removeIgnore(playerName);
}

void GameHandler::addVoiceIgnore(const std::string& playerName) {
    if (socialHandler_) socialHandler_->addVoiceIgnore(playerName);
}

void GameHandler::removeVoiceIgnore(const std::string& playerName) {
    if (socialHandler_) socialHandler_->removeVoiceIgnore(playerName);
}

void GameHandler::faceCanonicalYaw(float canonicalYaw) {
    movementInfo.orientation = canonicalYaw;

    // One-shot check that the renderer's facing round-trips to the same
    // canonical yaw we just computed from positions. If it does not, the
    // per-frame resync replaces this with a different heading and the server
    // re-checks the arc against that instead.
    if (auto* r = services_.renderer) {
        const float fromRender = core::coords::characterYawDegToCanonical(r->getCharacterYaw());
        const float delta = core::coords::normalizeAngleRad(fromRender - canonicalYaw);
        if (std::abs(delta) > 0.2f) {
            LOG_WARNING("Facing mismatch: computed canonical=", canonicalYaw,
                        " but the character's ", r->getCharacterYaw(),
                        " deg maps to ", fromRender, " (off by ",
                        delta * 57.2957795f, " deg) - the resync will undo this");
        }
    }

    // The renderer owns facing; the game side is downstream of it every frame.
    if (auto* renderer = services_.renderer) {
        const float facingDeg = core::coords::canonicalToCharacterYawDeg(canonicalYaw);
        renderer->setCharacterYaw(facingDeg);
        if (auto* cc = renderer->getCameraController()) {
            cc->setFacingYaw(facingDeg);
        }
    }

    sendMovement(Opcode::MSG_MOVE_SET_FACING);
}

void GameHandler::requestLogout(bool exitAfterLogout) {
    if (socialHandler_) socialHandler_->requestLogout(exitAfterLogout);
}

void GameHandler::cancelLogout() {
    if (socialHandler_) socialHandler_->cancelLogout();
}

void GameHandler::sendSetDifficulty(uint32_t difficulty, bool raid) {
    if (socialHandler_) socialHandler_->sendSetDifficulty(difficulty, raid);
}

void GameHandler::setStandState(uint8_t standState) {
    if (socialHandler_) socialHandler_->setStandState(standState);
}

void GameHandler::requestSheathState(uint8_t state) {
    if (!isInWorld()) return;
    auto packet = SetSheathedPacket::build(state);
    socket->send(packet);
    LOG_INFO("CMSG_SET_SHEATHED ", static_cast<int>(state));
}

void GameHandler::toggleHelm() {
    if (socialHandler_) socialHandler_->toggleHelm();
}

void GameHandler::toggleCloak() {
    if (socialHandler_) socialHandler_->toggleCloak();
}

void GameHandler::followTarget() {
    if (movementHandler_) movementHandler_->followTarget();
}

void GameHandler::followGuid(uint64_t guid) {
    if (movementHandler_) movementHandler_->followGuid(guid);
}

void GameHandler::cancelFollow() {
    if (movementHandler_) movementHandler_->cancelFollow();
}

void GameHandler::assistTarget() {
    if (combatHandler_) combatHandler_->assistTarget();
}

void GameHandler::togglePvp() {
    if (combatHandler_) combatHandler_->togglePvp();
}

void GameHandler::setPvpState(uint8_t state) {
    if (combatHandler_) combatHandler_->setPvpState(state);
}

void GameHandler::requestGuildInfo() {
    if (socialHandler_) socialHandler_->requestGuildInfo();
}

void GameHandler::requestGuildRoster() {
    if (socialHandler_) socialHandler_->requestGuildRoster();
}

void GameHandler::setGuildMotd(const std::string& motd) {
    if (socialHandler_) socialHandler_->setGuildMotd(motd);
}

void GameHandler::promoteGuildMember(const std::string& playerName) {
    if (socialHandler_) socialHandler_->promoteGuildMember(playerName);
}

void GameHandler::demoteGuildMember(const std::string& playerName) {
    if (socialHandler_) socialHandler_->demoteGuildMember(playerName);
}

void GameHandler::leaveGuild() {
    if (socialHandler_) socialHandler_->leaveGuild();
}

void GameHandler::inviteToGuild(const std::string& playerName) {
    if (socialHandler_) socialHandler_->inviteToGuild(playerName);
}

void GameHandler::initiateReadyCheck() {
    if (socialHandler_) socialHandler_->initiateReadyCheck();
}

void GameHandler::respondToReadyCheck(bool ready) {
    if (socialHandler_) socialHandler_->respondToReadyCheck(ready);
}

void GameHandler::acceptDuel() {
    if (socialHandler_) socialHandler_->acceptDuel();
}

void GameHandler::forfeitDuel() {
    if (socialHandler_) socialHandler_->forfeitDuel();
}

void GameHandler::toggleAfk(const std::string& message) {
    if (chatHandler_) chatHandler_->toggleAfk(message);
}

void GameHandler::toggleDnd(const std::string& message) {
    if (chatHandler_) chatHandler_->toggleDnd(message);
}

void GameHandler::replyToLastWhisper(const std::string& message) {
    if (chatHandler_) chatHandler_->replyToLastWhisper(message);
}

void GameHandler::setGuildBankTabText(uint8_t tab, const std::string& text) {
    if (socialHandler_) socialHandler_->setGuildBankTabText(tab, text);
}

void GameHandler::channelModeration(Opcode op, const std::string& channelName,
                                    const std::string& targetName,
                                    bool allowEmptyTarget) {
    if (socialHandler_) socialHandler_->channelModeration(op, channelName, targetName,
                                                         allowEmptyTarget);
}

void GameHandler::promoteToLeader(uint64_t guid) {
    if (socialHandler_) socialHandler_->promoteToLeader(guid);
}

void GameHandler::uninvitePlayer(const std::string& playerName, const std::string& reason) {
    if (socialHandler_) socialHandler_->uninvitePlayer(playerName, reason);
}

void GameHandler::leaveParty() {
    if (socialHandler_) socialHandler_->leaveParty();
}

void GameHandler::setMainTank(uint64_t targetGuid) {
    if (socialHandler_) socialHandler_->setMainTank(targetGuid);
}

void GameHandler::setMainAssist(uint64_t targetGuid) {
    if (socialHandler_) socialHandler_->setMainAssist(targetGuid);
}

void GameHandler::clearMainTank() {
    if (socialHandler_) socialHandler_->clearMainTank();
}

void GameHandler::clearMainAssist() {
    if (socialHandler_) socialHandler_->clearMainAssist();
}

void GameHandler::setRaidMark(uint64_t guid, uint8_t icon) {
    if (socialHandler_) socialHandler_->setRaidMark(guid, icon);
}

// The marks live in SocialHandler - MSG_RAID_TARGET_UPDATE writes them there.
// GameHandler kept a second, identical array that nothing ever wrote, so every
// caller of these (target frame, nameplates, minimap, social panel) read an
// array of zeros and no marker icon was ever drawn.
uint64_t GameHandler::getRaidMarkGuid(uint32_t icon) const {
    return socialHandler_ ? socialHandler_->getRaidMarkGuid(icon) : 0;
}

uint8_t GameHandler::getEntityRaidMark(uint64_t guid) const {
    return socialHandler_ ? socialHandler_->getEntityRaidMark(guid) : 0xFF;
}

void GameHandler::requestRaidInfo() {
    if (socialHandler_) socialHandler_->requestRaidInfo();
}

void GameHandler::setSavedInstanceExtend(uint32_t mapId, uint32_t difficulty, bool extend) {
    if (socialHandler_) socialHandler_->setSavedInstanceExtend(mapId, difficulty, extend);
}

void GameHandler::proposeDuel(uint64_t targetGuid) {
    if (socialHandler_) socialHandler_->proposeDuel(targetGuid);
}

void GameHandler::initiateTrade(uint64_t targetGuid) {
    if (inventoryHandler_) inventoryHandler_->initiateTrade(targetGuid);
}

void GameHandler::reportPlayer(uint64_t targetGuid, const std::string& reason) {
    if (socialHandler_) socialHandler_->reportPlayer(targetGuid, reason);
}

void GameHandler::stopCasting() {
    if (spellHandler_) spellHandler_->stopCasting();
}

void GameHandler::resetCastState() {
    if (spellHandler_) spellHandler_->resetCastState();
}

void GameHandler::clearUnitCaches() {
    if (spellHandler_) spellHandler_->clearUnitCaches();
}

void GameHandler::releaseSpirit() {
    if (combatHandler_) combatHandler_->releaseSpirit();
}

bool GameHandler::canReclaimCorpse() const {
    return combatHandler_ ? combatHandler_->canReclaimCorpse() : false;
}

float GameHandler::getCorpseReclaimDelaySec() const {
    return combatHandler_ ? combatHandler_->getCorpseReclaimDelaySec() : 0.0f;
}

void GameHandler::reclaimCorpse() {
    if (combatHandler_) combatHandler_->reclaimCorpse();
}

void GameHandler::useSelfRes() {
    if (combatHandler_) combatHandler_->useSelfRes();
}

void GameHandler::activateSpiritHealer(uint64_t npcGuid) {
    if (combatHandler_) combatHandler_->activateSpiritHealer(npcGuid);
}

void GameHandler::acceptResurrect() {
    if (combatHandler_) combatHandler_->acceptResurrect();
}

void GameHandler::declineResurrect() {
    if (combatHandler_) combatHandler_->declineResurrect();
}

void GameHandler::tabTarget(float playerX, float playerY, float playerZ) {
    if (combatHandler_) combatHandler_->tabTarget(playerX, playerY, playerZ);
}

void GameHandler::addLocalChatMessage(const MessageChatData& msg) {
    if (chatHandler_) chatHandler_->addLocalChatMessage(msg);
}

const std::deque<MessageChatData>& GameHandler::getChatHistory() const {
    if (chatHandler_) return chatHandler_->getChatHistory();
    static const std::deque<MessageChatData> kEmpty;
    return kEmpty;
}

void GameHandler::clearChatHistory() {
    if (chatHandler_) chatHandler_->getChatHistory().clear();
}

bool GameHandler::ownsChatChannel(const std::string& name) const {
    return chatHandler_ && chatHandler_->ownsChannel(name);
}

uint64_t GameHandler::chatLineSender(uint32_t lineId) const {
    return chatHandler_ ? chatHandler_->chatLineSender(lineId) : 0;
}

const std::vector<std::string>& GameHandler::getJoinedChannels() const {
    if (chatHandler_) return chatHandler_->getJoinedChannels();
    static const std::vector<std::string> kEmpty;
    return kEmpty;
}

// ============================================================
// Name Queries (delegated to EntityController)
// ============================================================

// Who a piece of mail is from. The wire gives a player GUID for player mail and
// an entry for everything else, and nothing resolved either - the inbox showed
// an empty From line for every message.
//
// Resolved on demand rather than stored, so a name that arrives after the inbox
// was parsed shows up on the next frame instead of staying blank.
std::string GameHandler::getMailSenderName(const MailMessage& mail) const {
    switch (mail.messageType) {
        case 0: {  // from another player
            // The name-query response backfills senderName, and Reply uses that
            // field, so prefer it and fall back to the cache for the window
            // between the inbox arriving and the query returning.
            if (!mail.senderName.empty()) return mail.senderName;
            if (mail.senderGuid == 0) break;
            const std::string& name = lookupName(mail.senderGuid);
            if (!name.empty()) return name;
            return "Unknown";
        }
        case 2:    // the auction house, which has no creature behind it
            return "Auction House";
        case 3: {  // a creature
            std::string name = getCachedCreatureName(mail.senderEntry);
            if (!name.empty()) return name;
            break;
        }
        case 4: {  // a game object
            if (const auto* info = getCachedGameObjectInfo(mail.senderEntry);
                info && !info->name.empty()) {
                return info->name;
            }
            break;
        }
        default:
            break;
    }
    return "Unknown";
}

void GameHandler::queryPlayerName(uint64_t guid) {
    if (entityController_) entityController_->queryPlayerName(guid);
}

void GameHandler::queryCreatureInfo(uint32_t entry, uint64_t guid) {
    if (entityController_) entityController_->queryCreatureInfo(entry, guid);
}

uint32_t GameHandler::getCreatureDisplayIdForEntry(uint32_t entry) {
    if (entry == 0) return 0;
    const auto& cache = getCreatureInfoCache();
    if (auto it = cache.find(entry); it != cache.end()) {
        // The first non-zero of the four. A template may carry several models
        // and the server picks between them per spawn; for a preview any of
        // them is the creature, and the first is what it looks like by default.
        for (uint32_t id : it->second.displayId)
            if (id != 0) return id;
        return 0;
    }
    // Guid zero: this is not about any one spawn. queryCreatureInfo drops a
    // repeat while one is in flight, so calling every frame sends one query.
    queryCreatureInfo(entry, 0);
    return 0;
}

void GameHandler::queryGameObjectInfo(uint32_t entry, uint64_t guid) {
    if (entityController_) entityController_->queryGameObjectInfo(entry, guid);
}

std::string GameHandler::getCachedPlayerName(uint64_t guid) const {
    return entityController_ ? entityController_->getCachedPlayerName(guid) : "";
}

std::string GameHandler::getCachedCreatureName(uint32_t entry) const {
    return entityController_ ? entityController_->getCachedCreatureName(entry) : "";
}

// ============================================================
// Item Query (forwarded to InventoryHandler)
// ============================================================

void GameHandler::queryItemInfo(uint32_t entry, uint64_t guid) {
    if (inventoryHandler_) inventoryHandler_->queryItemInfo(entry, guid);
}

uint64_t GameHandler::resolveOnlineItemGuid(uint32_t itemId) const {
    return inventoryHandler_ ? inventoryHandler_->resolveOnlineItemGuid(itemId) : 0;
}

void GameHandler::detectInventorySlotBases(const FlatFieldMap& fields) {
    if (inventoryHandler_) inventoryHandler_->detectInventorySlotBases(fields);
}

bool GameHandler::applyInventoryFields(const FlatFieldMap& fields, bool initial) {
    return inventoryHandler_ ? inventoryHandler_->applyInventoryFields(fields, initial) : false;
}

uint64_t GameHandler::getInteractNpcGuid() const {
    if (inventoryHandler_) {
        if (const uint64_t peer = inventoryHandler_->getTradePeerGuid()) return peer;
        if (const uint64_t vendor = getVendorGuid()) return vendor;
    }
    // The trainer, which the list above this function has always named and
    // this function never asked.
    //
    // Nothing else answers for it either: SMSG_TRAINER_LIST closes the gossip
    // that led to the trainer, so by the time the panel is up the gossip row
    // below returns 0 as well. With no unit to render, the portrait kept the
    // stand-in SetPortraitTexture stamps when it cannot get a face - which is
    // a class circle, and a tailoring trainer was drawn as a warrior's sword.
    //
    // Above the gossip for the same reason the vendor is: a trainer is a window
    // opened from a gossip and covering it.
    if (isTrainerWindowOpen()) {
        if (const uint64_t trainer = getTrainerSpells().trainerGuid) return trainer;
    }
    // From the movement handler, which owns both. GameHandler kept members of
    // the same two names that the decomposition left behind unwritten, so this
    // read false forever and the flight master had no portrait in the taxi
    // frame.
    if (movementHandler_ && movementHandler_->isTaxiWindowOpen()) {
        if (const uint64_t taxiNpc = movementHandler_->getTaxiNpcGuid()) return taxiNpc;
    }
    if (questHandler_) {
        if (const uint64_t gossip = questHandler_->getCurrentGossip().npcGuid) return gossip;
    }
    return 0;
}

bool GameHandler::getOtherPlayerEquipment(uint64_t guid,
                                          std::array<uint32_t, 19>& displayIds,
                                          std::array<uint8_t, 19>& invTypes) const {
    return inventoryHandler_
        ? inventoryHandler_->resolveOtherPlayerEquipment(guid, displayIds, invTypes)
        : false;
}

void GameHandler::extractContainerFields(uint64_t containerGuid, const FlatFieldMap& fields) {
    if (inventoryHandler_) inventoryHandler_->extractContainerFields(containerGuid, fields);
}

void GameHandler::rebuildOnlineInventory() {
    if (inventoryHandler_) inventoryHandler_->rebuildOnlineInventory();
}

void GameHandler::maybeDetectVisibleItemLayout() {
    if (inventoryHandler_) inventoryHandler_->maybeDetectVisibleItemLayout();
}

void GameHandler::updateOtherPlayerVisibleItems(uint64_t guid, const FlatFieldMap& fields) {
    if (inventoryHandler_) inventoryHandler_->updateOtherPlayerVisibleItems(guid, fields);
}

void GameHandler::cacheInspectedPlayerEquipment(uint64_t guid, const std::array<uint32_t, 19>& itemEntries,
                                                const std::array<ItemLinkInstance, 19>& instances) {
    if (inventoryHandler_) inventoryHandler_->cacheInspectedPlayerEquipment(guid, itemEntries, instances);
}

void GameHandler::emitOtherPlayerEquipment(uint64_t guid) {
    if (inventoryHandler_) inventoryHandler_->emitOtherPlayerEquipment(guid);
}

void GameHandler::emitAllOtherPlayerEquipment() {
    if (inventoryHandler_) inventoryHandler_->emitAllOtherPlayerEquipment();
}

void GameHandler::respawnKnownEntities() {
    if (entityController_) entityController_->respawnKnownEntities();
    // Their equipment reaches the spawner by its own callback, not with them.
    emitAllOtherPlayerEquipment();
    // The player's own mount is the player's state rather than an entity's,
    // and the spawner forgot it along with every instance.
    if (currentMountDisplayId_ != 0 && mountCallback_) mountCallback_(currentMountDisplayId_);
}

// ============================================================
// Combat (delegated to CombatHandler)
// ============================================================

void GameHandler::startAutoAttack(uint64_t targetGuid) {
    if (combatHandler_) combatHandler_->startAutoAttack(targetGuid);
}

void GameHandler::stopAutoAttack() {
    if (combatHandler_) combatHandler_->stopAutoAttack();
}

void GameHandler::addCombatText(CombatTextEntry::Type type, int32_t amount, uint32_t spellId, bool isPlayerSource, uint8_t powerType,
                                uint64_t srcGuid, uint64_t dstGuid) {
    if (combatHandler_) combatHandler_->addCombatText(type, amount, spellId, isPlayerSource, powerType, srcGuid, dstGuid);
}

bool GameHandler::shouldLogSpellstealAura(uint64_t casterGuid, uint64_t victimGuid, uint32_t spellId) {
    return combatHandler_ ? combatHandler_->shouldLogSpellstealAura(casterGuid, victimGuid, spellId) : false;
}

void GameHandler::updateCombatText(float deltaTime) {
    if (combatHandler_) combatHandler_->updateCombatText(deltaTime);
}

bool GameHandler::isAutoAttacking() const {
    return combatHandler_ ? combatHandler_->isAutoAttacking() : false;
}

bool GameHandler::hasAutoAttackIntent() const {
    return combatHandler_ ? combatHandler_->hasAutoAttackIntent() : false;
}

bool GameHandler::isInCombat() const {
    return combatHandler_ ? combatHandler_->isInCombat() : false;
}

bool GameHandler::isInCombatWith(uint64_t guid) const {
    return combatHandler_ ? combatHandler_->isInCombatWith(guid) : false;
}

uint64_t GameHandler::getAutoAttackTargetGuid() const {
    return combatHandler_ ? combatHandler_->getAutoAttackTargetGuid() : 0;
}

uint64_t GameHandler::getUnitMeleeTarget(uint64_t guid) const {
    return combatHandler_ ? combatHandler_->getUnitMeleeTarget(guid) : 0;
}

uint64_t GameHandler::getLastMeleeSwingMs() const {
    return combatHandler_ ? combatHandler_->getLastMeleeSwingMs() : 0;
}

uint32_t GameHandler::numPlayerDamageHits() const {
    return combatHandler_ ? combatHandler_->playerDamageHits() : 0u;
}

const std::vector<CombatTextEntry>& GameHandler::getCombatText() const {
    static const std::vector<CombatTextEntry> empty;
    return combatHandler_ ? combatHandler_->getCombatText() : empty;
}

const std::deque<CombatLogEntry>& GameHandler::getCombatLog() const {
    static const std::deque<CombatLogEntry> empty;
    return combatHandler_ ? combatHandler_->getCombatLog() : empty;
}

void GameHandler::clearCombatLog() {
    if (combatHandler_) combatHandler_->clearCombatLog();
}

void GameHandler::clearCombatText() {
    if (combatHandler_) combatHandler_->clearCombatText();
}

void GameHandler::clearHostileAttackers() {
    if (combatHandler_) combatHandler_->clearHostileAttackers();
}

const std::vector<GameHandler::ThreatEntry>* GameHandler::getThreatList(uint64_t unitGuid) const {
    return combatHandler_ ? combatHandler_->getThreatList(unitGuid) : nullptr;
}

const std::vector<GameHandler::ThreatEntry>* GameHandler::getTargetThreatList() const {
    return targetGuid ? getThreatList(targetGuid) : nullptr;
}

bool GameHandler::isHostileAttacker(uint64_t guid) const {
    return combatHandler_ ? combatHandler_->isHostileAttacker(guid) : false;
}

void GameHandler::askBinder(uint64_t guid) {
    // 0x006d2120: the innkeeper remembered, and CONFIRM_BINDER asks with the
    // name of the area the player stands in - the subzone's, else the
    // zone's (0x00519ca0) - or HOME_INN where there is none.
    binderGuid_ = guid;
    std::string where;
    if (const auto* r = services_.renderer) {
        if (const auto* t = r->getZoneTexts()) where = !t->subzone.empty() ? t->subzone : t->zone;
    }
    if (where.empty()) where = interfaceString("HOME_INN", "your inn");
    fireAddonEvent("CONFIRM_BINDER", {where});
}

void GameHandler::confirmBinder() {
    if (!isInWorld() || !socket || binderGuid_ == 0) return;
    auto pkt = BinderActivatePacket::build(binderGuid_);
    socket->send(pkt);
    LOG_INFO("Sent CMSG_BINDER_ACTIVATE (confirmed) for npc=0x",
             std::hex, binderGuid_, std::dec);
}

void GameHandler::setFlightAirborne(bool airborne) {
    if (movementHandler_) movementHandler_->setFlightAirborne(airborne);
}

void GameHandler::dismount() {
    if (movementHandler_) movementHandler_->dismount();
}

// ============================================================
// Arena / Battleground Handlers
// ============================================================

void GameHandler::declineBattlefield(uint32_t queueSlot) {
    if (socialHandler_) socialHandler_->declineBattlefield(queueSlot);
}

bool GameHandler::hasPendingBgInvite() const {
    return socialHandler_ && socialHandler_->hasPendingBgInvite();
}

// Seeds the staging copy from the rank as it stands, so a field the panel never
// touches goes back to the server unchanged instead of as a zero.
void GameHandler::setSelectedGuildRank(int index) {
    selectedGuildRank_ = index;
    pendingGuildRank_ = PendingGuildRank{};
    // Off the rank table, as 0x005ca090 reads it for the rank chosen.
    if (index < 1) return;
    const GuildRankInfo* row = getGuildRankRow(static_cast<uint32_t>(index) - 1);
    if (!row) return;
    const auto& r = *row;
    pendingGuildRank_.rights    = r.rights;
    pendingGuildRank_.goldLimit = r.goldLimit;
    pendingGuildRank_.tabRights = r.bankTabRights;
    pendingGuildRank_.tabSlots  = r.bankTabSlotsPerDay;
}

void GameHandler::saveGuildRank(const std::string& rankName) {
    if (!socialHandler_) return;
    if (selectedGuildRank_ < 1) return;
    const auto& p = pendingGuildRank_;
    // Ranks are zero-based on the wire and one-based in the panel. The gold a
    // day goes as the row holds it times 10000: SetGuildBankWithdrawLimit
    // writes gold, the roster copper (0x005cb170).
    socialHandler_->saveGuildRank(static_cast<uint32_t>(selectedGuildRank_ - 1),
                                  p.rights, rankName, p.goldLimit * 10000u,
                                  p.tabRights.data(), p.tabSlots.data());
}

void GameHandler::setGuildInfoText(const std::string& text) {
    if (socialHandler_) socialHandler_->setGuildInfoText(text);
}

void GameHandler::takeInboxTextItem(uint32_t mailId) {
    if (socialHandler_) socialHandler_->takeInboxTextItem(mailId);
}

uint32_t GameHandler::getPlayerGuildRankRights() const {
    return socialHandler_ ? socialHandler_->getPlayerGuildRankRights() : 0u;
}

void GameHandler::delGuildRank() {
    if (socialHandler_) socialHandler_->delGuildRank();
}

void GameHandler::setLootMethod(uint8_t method, uint64_t masterGuid, uint8_t threshold) {
    if (socialHandler_) socialHandler_->setLootMethod(method, masterGuid, threshold);
}

void GameHandler::setPartyAssignment(uint8_t assignment, uint64_t guid, bool apply) {
    if (socialHandler_) socialHandler_->setPartyAssignment(assignment, guid, apply);
}

void GameHandler::setRaidSubgroup(const std::string& playerName, uint8_t group) {
    if (socialHandler_) socialHandler_->setRaidSubgroup(playerName, group);
}

void GameHandler::swapRaidSubgroup(const std::string& a, const std::string& b) {
    if (socialHandler_) socialHandler_->swapRaidSubgroup(a, b);
}

void GameHandler::joinBattlefield(uint64_t battlemasterGuid, uint32_t bgTypeId,
                                  uint32_t instanceId, bool asGroup) {
    if (socialHandler_) socialHandler_->joinBattlefield(battlemasterGuid, bgTypeId, instanceId, asGroup);
}

void GameHandler::leaveBattlefield() {
    if (socialHandler_) socialHandler_->leaveBattlefield();
}

void GameHandler::requestBattlefieldList(uint32_t bgTypeId) {
    if (socialHandler_) socialHandler_->requestBattlefieldList(bgTypeId);
}

void GameHandler::reportPvpAfk(uint64_t playerGuid) {
    if (socialHandler_) socialHandler_->reportPvpAfk(playerGuid);
}

void GameHandler::acceptBattlefield(uint32_t queueSlot) {
    if (socialHandler_) socialHandler_->acceptBattlefield(queueSlot);
}

// ---------------------------------------------------------------------------
// LFG / Dungeon Finder handlers (WotLK 3.3.5a)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// LFG outgoing packets
// ---------------------------------------------------------------------------

void GameHandler::lfgJoin(const std::vector<uint32_t>& dungeonIds, uint8_t roles) {
    if (socialHandler_) socialHandler_->lfgJoin(dungeonIds, roles);
}

const LfgLocalJoin* GameHandler::getLfgLocalJoin() const {
    return socialHandler_ ? &socialHandler_->getLfgLocalJoin() : nullptr;
}

const std::vector<uint32_t>* GameHandler::getLfgQueuedSlots() const {
    return socialHandler_ ? &socialHandler_->getLfgQueuedSlots(socialHandler_->isInGroup()) : nullptr;
}

void GameHandler::lfgAddSlot(uint32_t slot) {
    if (!socialHandler_) return;
    // 0x0052bd10: how many others the party holds.
    const size_t others = socialHandler_->isInGroup()
        ? std::min<size_t>(socialHandler_->getPartyData().members.size(), 4) : 0;
    socialHandler_->lfgAddSlot(slot, static_cast<int>(others));
}

void GameHandler::lfgRemoveSlot(uint32_t slot) {
    if (socialHandler_) socialHandler_->lfgRemoveSlot(slot);
}

void GameHandler::lfgClearSlots() {
    if (socialHandler_) socialHandler_->lfgClearSlots();
}

void GameHandler::lfgJoinLocal(uint8_t roles) {
    if (socialHandler_) socialHandler_->lfgJoinLocal(roles);
}

int GameHandler::lfgCompleteRoleCheck(bool accept) {
    return socialHandler_ ? socialHandler_->lfgCompleteRoleCheck(accept) : -1;
}

void GameHandler::lfgLeaveLocal() {
    if (socialHandler_) socialHandler_->lfgLeaveLocal();
}

void GameHandler::lfgSetComment(const std::string& comment) {
    if (socialHandler_) socialHandler_->lfgSetComment(comment);
}

void GameHandler::lfgLeave() {
    if (socialHandler_) socialHandler_->lfgLeave();
}

void GameHandler::requestLfgPlayerLockInfo() {
    if (state != WorldState::IN_WORLD || !socket) return;
    // Payload-free requests. The dungeon finder's ready popup asks for both in
    // its OnShow, so the rewards it is about to draw are the current ones.
    network::Packet pkt(wireOpcode(Opcode::CMSG_LFD_PLAYER_LOCK_INFO_REQUEST));
    socket->send(pkt);
}

void GameHandler::requestLfgPartyLockInfo() {
    if (state != WorldState::IN_WORLD || !socket) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_LFD_PARTY_LOCK_INFO_REQUEST));
    socket->send(pkt);
}

void GameHandler::lfgSetRoles(uint8_t roles) {
    if (socialHandler_) socialHandler_->lfgSetRoles(roles);
}

void GameHandler::lfgAcceptProposal(uint32_t proposalId, bool accept) {
    if (socialHandler_) socialHandler_->lfgAcceptProposal(proposalId, accept);
}

void GameHandler::lfgTeleport(bool toLfgDungeon) {
    if (socialHandler_) socialHandler_->lfgTeleport(toLfgDungeon);
}

void GameHandler::lfgSetBootVote(bool vote) {
    if (socialHandler_) socialHandler_->lfgSetBootVote(vote);
}

void GameHandler::loadAreaTriggerDbc() {
    if (movementHandler_) movementHandler_->loadAreaTriggerDbc();
}

void GameHandler::checkAreaTriggers() {
    if (movementHandler_) movementHandler_->checkAreaTriggers();
}

void GameHandler::requestArenaTeamRoster(uint32_t teamId) {
    if (socialHandler_) socialHandler_->requestArenaTeamRoster(teamId);
}

void GameHandler::requestPvpLog() {
    if (socialHandler_) socialHandler_->requestPvpLog();
}

// ============================================================
// Spells
// ============================================================

void GameHandler::castSpell(uint32_t spellId, uint64_t targetGuid) {
    if (spellHandler_) spellHandler_->castSpell(spellId, targetGuid);
}

void GameHandler::cancelCast() {
    if (spellHandler_) spellHandler_->cancelCast();
}

bool GameHandler::isGroundTargeting() const { return spellHandler_ && spellHandler_->isGroundTargeting(); }
uint32_t GameHandler::groundTargetSpellId() const {
    return spellHandler_ ? spellHandler_->groundTargetSpellId() : 0;
}
spell_mods::Sum GameHandler::getSpellModifiers(uint32_t spellId, SpellModOp op) const {
    return spellHandler_ ? spellHandler_->spellModifiers(spellId, static_cast<uint8_t>(op)) : spell_mods::Sum{};
}
uint64_t GameHandler::getCloneCasterGuid(const Unit& unit) const {
    const auto* auras = getUnitAuras(unit.getGuid());
    if (!auras || auras->empty()) return 0;
    return unit_names::cloneCasterGuid(*auras, [this](uint32_t spellId, int k) -> uint32_t {
        getSpellName(spellId);  // fills the cache
        auto it = spellNameCache_.find(spellId);
        return it != spellNameCache_.end() ? it->second.effectAuraIds[k] : 0u;
    });
}

std::string GameHandler::shownUnitName(const Unit& unit) {
    const uint64_t caster = getCloneCasterGuid(unit);
    if (caster == 0) return unit.getName();
    // The caster's own name while it is about; else what is cached for it
    // (0x0074d750), which a name query fills for a player.
    if (auto e = entityController_->getEntityManager().getEntity(caster); e && e->isUnit()) {
        const auto& u = static_cast<const Unit&>(*e);
        if (!u.getName().empty()) return u.getName();
    }
    const std::string& cached = lookupName(caster);
    if (cached.empty() && (caster >> 48) == 0) queryPlayerName(caster);
    return cached;
}

bool GameHandler::cancelGroundTargeting() { return spellHandler_ && spellHandler_->cancelGroundTargeting(); }
bool GameHandler::isUnitTargeting() const { return spellHandler_ && spellHandler_->isUnitTargeting(); }
void GameHandler::spellTargetUnit(uint64_t unitGuid) {
    if (spellHandler_) spellHandler_->spellTargetUnit(unitGuid);
}
bool GameHandler::spellCanTargetUnit(uint64_t unitGuid) const {
    return spellHandler_ && spellHandler_->spellCanTargetUnit(unitGuid);
}
bool GameHandler::turnOrCancelGroundTargeting() {
    return spellHandler_ && spellHandler_->turnOrCancelGroundTargeting();
}
void GameHandler::placeGroundTarget(const ground_target::Place& place) {
    if (spellHandler_) spellHandler_->placeGroundTarget(place);
}
std::optional<GameHandler::GroundTargetAim> GameHandler::aimGroundTarget(std::optional<ground_target::Place> place) {
    if (!spellHandler_) return std::nullopt;
    spellHandler_->setGroundTargetCursor(place);
    if (!place || !spellHandler_->isGroundTargeting()) return std::nullopt;
    GroundTargetAim aim;
    aim.placement = spellHandler_->groundTargetPlacement(place->canonical);
    aim.circleRadius = ground_target::circleRadius(static_cast<ground_target::Placement>(aim.placement),
                                                   spellHandler_->groundTargetSpellArea());
    return aim;
}
uint32_t GameHandler::groundTargetObjectEntry() const {
    return spellHandler_ ? spellHandler_->groundTargetObjectEntry() : 0;
}
float GameHandler::groundTargetObjectFacing() const {
    // Effect 81's starts at 0 (0x0080cce0 clears 0x00d3f4dc) and turns by
    // right clicks (0x007fd800).
    if (spellHandler_ && spellHandler_->groundTargetObjectFixedFacing()) return spellHandler_->groundTargetObjectFacing();
    return movementInfo.orientation;
}
std::optional<ground_target::Place> GameHandler::groundTargetCursor() const {
    return spellHandler_ ? spellHandler_->groundTargetCursor() : std::nullopt;
}


void GameHandler::startCraftQueue(uint32_t spellId, int count) {
    if (spellHandler_) spellHandler_->startCraftQueue(spellId, count);
}

void GameHandler::cancelCraftQueue() {
    if (spellHandler_) spellHandler_->cancelCraftQueue();
}

void GameHandler::cancelAura(uint32_t spellId) {
    if (spellHandler_) spellHandler_->cancelAura(spellId);
}

uint32_t GameHandler::getTempEnchantRemainingMs(uint32_t slot) const {
    return inventoryHandler_ ? inventoryHandler_->getTempEnchantRemainingMs(slot) : 0u;
}

void GameHandler::handlePetSpells(network::Packet& packet) {
    if (spellHandler_) spellHandler_->handlePetSpells(packet);
}

void GameHandler::petStopAttack() {
    if (!petAttacking_) return;
    if (socket && state == WorldState::IN_WORLD && wireOpcode(Opcode::CMSG_PET_STOP_ATTACK) != 0xFFFF) {
        network::Packet packet(wireOpcode(Opcode::CMSG_PET_STOP_ATTACK));
        packet.writeUInt64(petGuid_);
        socket->send(packet);
    }
    petAttacking_ = false;
}

void GameHandler::sendCommentatorRequest(network::Packet& packet) {
    if (socket && state == WorldState::IN_WORLD) socket->send(packet);
}

void GameHandler::sendInterfaceRequest(network::Packet& packet) {
    if (socket && state == WorldState::IN_WORLD && packet.getOpcode() != 0xFFFF) socket->send(packet);
}

void GameHandler::sendActionBarToggles(uint8_t mask) {
    if (!socket || state != WorldState::IN_WORLD || wireOpcode(Opcode::CMSG_SET_ACTIONBAR_TOGGLES) == 0xFFFF) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_SET_ACTIONBAR_TOGGLES));
    packet.writeUInt8(mask);
    socket->send(packet);
}

void GameHandler::flagTutorial(uint32_t id) {
    if (!tutorials_.flag(id)) return;
    if (!socket || state != WorldState::IN_WORLD || wireOpcode(Opcode::CMSG_TUTORIAL_FLAG) == 0xFFFF) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_TUTORIAL_FLAG));
    packet.writeUInt32(id);
    socket->send(packet);
}

void GameHandler::triggerTutorial(uint32_t id) {
    const bool show = addons::storedCVarValue("showTutorials", "1") != "0";
    switch (tutorials_.trigger(id, show)) {
    case TutorialState::Trigger::Nothing:
        break;
    case TutorialState::Trigger::Flag:
        flagTutorial(id);
        break;
    case TutorialState::Trigger::Popup:
        if (auto* ac = services().audioCoordinator) {
            if (auto* sfx = ac->getUiSoundManager()) sfx->playByName("TutorialPopup");
        }
        fireAddonEvent("TUTORIAL_TRIGGER", {std::to_string(id + 1)});
        break;
    }
}

void GameHandler::triggerEntryTutorials() {
    for (uint32_t id : {0x29u, 1u, 2u, 0u}) triggerTutorial(id);
}

void GameHandler::clearTutorials() {
    tutorials_.setAll();
    sendTutorialClear();
}

void GameHandler::resetTutorials() {
    tutorials_.reset();
    if (!socket || state != WorldState::IN_WORLD || wireOpcode(Opcode::CMSG_TUTORIAL_RESET) == 0xFFFF) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_TUTORIAL_RESET));
    socket->send(packet);
}

void GameHandler::sendTutorialClear() {
    if (!socket || state != WorldState::IN_WORLD || wireOpcode(Opcode::CMSG_TUTORIAL_CLEAR) == 0xFFFF) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_TUTORIAL_CLEAR));
    socket->send(packet);
}

void GameHandler::sendPetAction(uint32_t action, uint64_t targetGuid) {
    if (spellHandler_) spellHandler_->sendPetAction(action, targetGuid);
}

namespace {
uint64_t guidFieldOf(const Entity& e, UF low) {
    const uint16_t idx = fieldIndex(low);
    if (idx == 0xFFFF) return 0;
    return e.getField(idx) | (static_cast<uint64_t>(e.getField(static_cast<uint16_t>(idx + 1))) << 32);
}
}  // namespace

void GameHandler::setPossessSpell(uint32_t spellId) {
    if (possessSpellId_ == spellId) return;
    possessSpellId_ = spellId;
    fireAddonEvent("UPDATE_BONUS_ACTIONBAR", {});  // event 0xb4
}

void GameHandler::updatePossessSpell() {
    // 0x006e4fd0, on PLAYER_FARSIGHT.
    auto player = getEntityManager().getEntity(playerGuid);
    if (!player) return;
    const uint64_t farsight = guidFieldOf(*player, UF::PLAYER_FARSIGHT);
    if (farsight == 0 || !getEntityManager().getEntity(farsight)) {
        setPossessSpell(0);  // 0x005d30e0(0)
        return;
    }
    // 0x005d62a0: the player rides its charm, or its summon when it has no
    // charm (+0xd0's first guid, else its second, against the vehicle the
    // player sits in).
    uint64_t controlled = guidFieldOf(*player, UF::UNIT_FIELD_CHARM);
    if (controlled == 0) controlled = guidFieldOf(*player, UF::UNIT_FIELD_SUMMON);
    const auto passenger = vehicleRoster_.passenger(playerGuid);
    const bool ridesItsCharm = passenger && passenger->vehicleGuid != 0 && passenger->vehicleGuid == controlled;
    std::shared_ptr<pipeline::DBCFile> props;
    std::vector<pet::PossessAura> auras;
    for (const AuraSlot& slot : getPlayerAuras()) {
        pet::PossessAura a;
        if (!slot.isEmpty()) {
            getSpellName(slot.spellId);  // fills the cache
            const auto it = spellNameCache_.find(slot.spellId);
            if (it != spellNameCache_.end()) {
                a.spellId = slot.spellId;
                a.flags = slot.flags;
                for (int i = 0; i < 3; ++i) {
                    a.effects[i] = it->second.effectIds[i];
                    a.auraTypes[i] = it->second.effectAuraIds[i];
                    if (a.effects[i] != pet::kEffectSummon) continue;
                    if (!props) {
                        auto* am = services_.assetManager;
                        props = am && am->isInitialized() ? am->loadDBCOptional("SummonProperties.dbc") : nullptr;
                    }
                    const int32_t row = props && props->getFieldCount() > 1
                        ? props->findRecordById(it->second.effectMiscValuesB[i]) : -1;
                    if (row >= 0) a.summonControl[i] = static_cast<int32_t>(props->getUInt32(static_cast<uint32_t>(row), 1));
                }
            }
        }
        auras.push_back(a);
    }
    if (const auto spell = pet::possessBarSpell(auras, ridesItsCharm)) setPossessSpell(*spell);
}

void GameHandler::usePetSlot(uint32_t packed, uint64_t targetGuid) {
    // 0x005d4210: a spell the pet casts itself - AttributesEx4 0x20, or the
    // pet is the active mover (0x006dd060) - goes as the player's casts do
    // (0x0080cce0) at the unit asked; anything else is CMSG_PET_ACTION.
    if (const uint32_t spell = pet::petSlotSpell(packed); spell != 0 && petGuid_ != 0) {
        loadSpellNameCache();
        const auto it = spellNameCache_.find(spell);
        const uint32_t attrEx4 = it != spellNameCache_.end() ? it->second.attrEx4 : 0;
        // Nothing for a pet out of view (0x005d4210 finds none).
        auto petEntity = getEntityManager().getEntity(petGuid_);
        auto petUnit = std::dynamic_pointer_cast<Unit>(petEntity);
        if (!petUnit) return;
        // A spell in effect on the pet is taken off it (0x00802cb0 ->
        // 0x00802f80).
        if (it != spellNameCache_.end()) {
            pet::ToggleSpell toggle;
            toggle.id = spell;
            toggle.activeIconId = it->second.activeIconId;
            for (int k = 0; k < 3; ++k) {
                toggle.effects[k] = it->second.effectIds[k];
                toggle.auras[k] = it->second.effectAuraIds[k];
                toggle.misc[k] = it->second.effectMiscValues[k];
            }
            toggle.miscB0 = it->second.effectMiscValuesB[0];
            pet::ToggleUnit unit;
            const uint64_t critter = guidFieldOf(*petUnit, UF::UNIT_FIELD_CRITTER);
            unit.critterEntry = static_cast<uint32_t>((critter >> 24) & 0xFFFFFFFu);  // its entry bits
            if (const auto p = vehicleRoster_.passenger(petGuid_)) {
                if (auto v = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(p->vehicleGuid))) {
                    unit.vehicleEntry = v->getEntry();
                    unit.vehicleMadeByPlayer = guidFieldOf(*v, UF::UNIT_FIELD_CREATEDBY) == playerGuid;
                }
            }
            auto* am = services_.assetManager;
            auto props = am && am->isInitialized() ? am->loadDBCOptional("SummonProperties.dbc") : nullptr;
            const auto slotOf = [&](uint32_t row) -> uint32_t {
                const int32_t r = props && props->getFieldCount() > 4 ? props->findRecordById(row) : -1;
                return r >= 0 ? props->getUInt32(static_cast<uint32_t>(r), 4) : 0u;
            };
            const auto mountOf = [&](uint32_t auraSpell) -> std::pair<uint32_t, uint32_t> {
                const auto m = spellNameCache_.find(auraSpell);
                if (m == spellNameCache_.end()) return {0u, 0u};
                return {m->second.effectAuraIds[0], m->second.effectMiscValues[0]};
            };
            static const std::vector<AuraSlot> kNoAuras;
            const auto* auras = getUnitAuras(petGuid_);
            if (pet::spellInEffectOn(toggle, unit, slotOf, auras ? *auras : kNoAuras, mountOf)) {
                // Not within 1.5 s of its cooldown beginning, with
                // secureAbilityToggle (0x00809100 on the pet's history).
                if (spellHandler_ && spellHandler_->petToggleHeld(spell)) return;
                if (pet::spellAppliesAura(toggle.effects) && socket) {
                    if (const uint16_t wire = wireOpcode(Opcode::CMSG_PET_CANCEL_AURA); wire != 0xFFFF) {
                        network::Packet pkt(wire);
                        pkt.writeUInt64(petGuid_);
                        pkt.writeUInt32(spell);
                        socket->send(pkt);
                    }
                }
                return;
            }
        }
        // None cast by a pet stunned, fleeing or confused where the spell
        // does not allow it; nothing said (0x005d4210).
        const uint32_t attrEx5 = it != spellNameCache_.end() ? it->second.attrEx5 : 0;
        if (pet::petsCannotCast(std::array<uint32_t, 1>{petUnit->getUnitFlags()}, attrEx5)) return;
        if (pet::petCastsItself(attrEx4, petGuid_ == activeMover())) {
            if (spellHandler_) spellHandler_->castPetSpell(petGuid_, spell, targetGuid);
            return;
        }
        // 0x00805d70: its global cooldown into the pet's history.
        if (spellHandler_) spellHandler_->notePetActionCast(spell, petGuid_);
    }
    sendPetAction(packed, targetGuid != 0 ? targetGuid : petGuid_);
}

void GameHandler::dismissCritter() {
    if (activeCritterGuid_ == 0 || !getSocket() || getState() != WorldState::IN_WORLD) return;
    const uint16_t wire = wireOpcode(Opcode::CMSG_DISMISS_CRITTER);
    if (wire == 0xFFFF) return;   // expansion has no such opcode
    auto pkt = DismissCritterPacket::build(activeCritterGuid_);
    getSocket()->send(pkt);
    LOG_INFO("Dismissing companion: guid=0x", std::hex, activeCritterGuid_, std::dec,
             " summonedBy=", activeCritterSpellId_);
    // The server confirms by clearing the critter field; leave the local state
    // alone so a refused dismiss does not leave the client thinking it worked.
}

uint32_t GameHandler::summonPropertiesFlagsOf(uint64_t unitGuid) {
    // 0x0071d1f0: the creating spell's first SUMMON effect's (28)
    // EffectMiscValueB, as a SummonProperties row; its Flags (+0x14).
    auto unit = getEntityManager().getEntity(unitGuid);
    const uint16_t bySpell = fieldIndex(UF::UNIT_CREATED_BY_SPELL);
    const uint32_t spellId = unit && bySpell != 0xFFFF ? unit->getField(bySpell) : 0;
    if (spellId == 0) return 0;
    getSpellName(spellId);  // fills the cache
    const auto it = spellNameCache_.find(spellId);
    if (it == spellNameCache_.end()) return 0;
    for (int k = 0; k < 3; ++k) {
        if (it->second.effectIds[k] != 28) continue;
        auto* am = services_.assetManager;
        auto props = am && am->isInitialized() ? am->loadDBCOptional("SummonProperties.dbc") : nullptr;
        const int32_t row = props && props->getFieldCount() > 5 ? props->findRecordById(it->second.effectMiscValuesB[k]) : -1;
        return row >= 0 ? props->getUInt32(static_cast<uint32_t>(row), 5) : 0;
    }
    return 0;
}

pet::DismissContext GameHandler::petDismissContext() const {
    pet::DismissContext c;
    auto player = getEntityManager().getEntity(playerGuid);
    if (player) {
        c.player = playerGuid;
        c.playerCharmedBy = guidFieldOf(*player, UF::UNIT_FIELD_CHARMEDBY);
        const uint16_t farsightIdx = fieldIndex(UF::PLAYER_FARSIGHT);
        if (farsightIdx != 0xFFFF) c.farsight = guidFieldOf(*player, UF::PLAYER_FARSIGHT);
    }
    c.pet = petGuid_;
    if (auto pet = petGuid_ ? getEntityManager().getEntity(petGuid_) : nullptr) {
        c.petInView = true;
        const uint64_t charmedBy = guidFieldOf(*pet, UF::UNIT_FIELD_CHARMEDBY);
        c.petOwner = charmedBy != 0 ? charmedBy : guidFieldOf(*pet, UF::UNIT_FIELD_SUMMONEDBY_LO);
        if (auto u = std::dynamic_pointer_cast<Unit>(pet)) c.petUnitFlags = u->getUnitFlags();
    }
    c.mover = activeMover();
    if (const auto p = vehicleRoster_.passenger(playerGuid)) c.playerTransport = p->vehicleGuid;
    c.moverIsVehicle = c.mover != 0 && vehicleRoster_.vehicleId(c.mover) != 0;
    if (const vehicle::SeatRecord* seat = vehicleRoster_.seatOf(playerGuid)) {
        c.seatKnown = true;
        c.seatFlags = seat->flags;
    }
    return c;
}

bool GameHandler::petActionsUsable() const {
    // GetPetActionsUsable (0x005d53c0) -> 0x005d3e80.
    return pet::canDismissPet(petDismissContext());
}

void GameHandler::dismissPet() {
    // CGPetInfo::PetDismiss (0x005d46f0).
    if (state != WorldState::IN_WORLD || !socket) return;
    pet::DismissContext c = petDismissContext();
    if (pet::canDismissPet(c) && !(c.pet == c.mover && c.playerTransport == c.pet))
        c.summonPropertiesFlags = summonPropertiesFlagsOf(c.pet);
    switch (pet::dismissAction(c)) {
        case pet::DismissAction::DismissControlledVehicle:
            // The seat change asked first (0x00747fa0), then the vehicle's
            // own request (0x0074bb20 -> 0x006ef540).
            askVehicleSeatChange();
            if (movementHandler_) movementHandler_->sendDismissControlledVehicle();
            break;
        case pet::DismissAction::PetAction:
            // CMSG_PET_ACTION, COMMAND_ABANDON at no target, and
            // PET_DISMISS_START with 10000 (event 0x176).
            socket->send(PetActionPacket::build(petGuid_, pet::packPetAction(pet::ActionType::Command, pet::kAbandon), 0));
            fireAddonEvent("PET_DISMISS_START", {"10000"});
            break;
        case pet::DismissAction::None:
            break;
    }
}

void GameHandler::togglePetSpellAutocast(uint32_t spellId) {
    if (spellHandler_) spellHandler_->togglePetSpellAutocast(spellId);
}

void GameHandler::renamePet(const std::string& newName) {
    if (spellHandler_) spellHandler_->renamePet(newName);
}

void GameHandler::requestStabledPetList() {
    if (spellHandler_) spellHandler_->requestStabledPetList();
}

void GameHandler::stablePet() {
    if (spellHandler_) spellHandler_->stablePet();
}

void GameHandler::buyStableSlot() {
    if (spellHandler_) spellHandler_->buyStableSlot();
}

void GameHandler::abandonPet() {
    if (spellHandler_) spellHandler_->abandonPet();
}

// CMSG_UNLEARN_SKILL carries the skill line and nothing else.
// HandleUnlearnSkillOpcode drops anything that is not a primary profession, so
// a mistaken call costs nothing, but the confirmation dialog in front of it is
// there because forgetting one is expensive to undo.
void GameHandler::unlearnSkill(uint32_t skillId) {
    if (skillId == 0 || getState() != WorldState::IN_WORLD || !getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_UNLEARN_SKILL));
    pkt.writeUInt32(skillId);
    getSocket()->send(pkt);
    LOG_INFO("Sent CMSG_UNLEARN_SKILL: skill=", skillId);
}

// An empty message. The server resets everything the player is saved to, or
// everything the group is if they lead one, and reads no body at all.
void GameHandler::resetInstances() {
    if (getState() != WorldState::IN_WORLD || !getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_RESET_INSTANCES));
    getSocket()->send(pkt);
    LOG_INFO("Sent CMSG_RESET_INSTANCES");
}

void GameHandler::unstablePet(uint32_t petNumber) {
    if (spellHandler_) spellHandler_->unstablePet(petNumber);
}

void GameHandler::swapStablePet(uint32_t petNumber) {
    if (spellHandler_) spellHandler_->swapStablePet(petNumber);
}

void GameHandler::handleListStabledPets(network::Packet& packet) {
    if (spellHandler_) spellHandler_->handleListStabledPets(packet);
}

void GameHandler::setActionBarSlot(int slot, ActionBarSlot::Type type, uint32_t id) {
    if (slot < 0 || slot >= ACTION_BAR_SLOTS) return;
    actionBar[slot].type = type;
    actionBar[slot].id = id;
    // Pre-query item information so action bar displays item name instead of "Item" placeholder
    if (type == ActionBarSlot::ITEM && id != 0) {
        queryItemInfo(id, 0);
    }
    saveCharacterConfig();
    // Notify Lua addons that the action bar changed
        fireAddonEvent("ACTIONBAR_SLOT_CHANGED", {std::to_string(slot + 1)});
        fireAddonEvent("ACTIONBAR_UPDATE_STATE", {});
    // Notify the server so the action bar persists across relogs.
    if (isInWorld()) {
        const bool classic = isClassicLikeExpansion();
        auto pkt = SetActionButtonPacket::build(
            static_cast<uint8_t>(slot),
            static_cast<uint8_t>(type),
            id,
            classic);
        socket->send(pkt);
    }
}

void GameHandler::setGroupAssistant(uint64_t guid, bool apply) {
    if (socialHandler_) socialHandler_->setGroupAssistant(guid, apply);
}

void GameHandler::requestBattlefieldPositions() {
    if (socialHandler_) socialHandler_->requestBattlefieldPositions();
}

void GameHandler::requestContactList() {
    if (socialHandler_) socialHandler_->requestContactList();
}

uint32_t GameHandler::getPlayerGuildRankIndex() const {
    return socialHandler_ ? socialHandler_->getPlayerGuildRankIndex() : 0xFFFFFFFFu;
}

uint32_t GameHandler::getPlayerGuildRankField() const {
    return socialHandler_ ? socialHandler_->getPlayerGuildRankField() : 0xFFFFFFFFu;
}

const GuildRankInfo* GameHandler::getGuildRankRow(uint32_t rank) const {
    return socialHandler_ ? socialHandler_->getGuildRankRow(rank) : nullptr;
}

void GameHandler::onPlayerEnteredWorldGuild() {
    if (socialHandler_) socialHandler_->onPlayerEnteredWorld();
}

// ============================================================
// Talents
// ============================================================

uint8_t GameHandler::getPetTalentRank(uint32_t talentId) const {
    return spellHandler_ ? spellHandler_->getPetTalentRank(talentId) : 0;
}

uint32_t GameHandler::getPetUnspentTalentPoints() const {
    return spellHandler_ ? spellHandler_->getPetUnspentTalentPoints() : 0;
}

const std::unordered_map<uint32_t, uint8_t>& GameHandler::getPetTalents() const {
    static const std::unordered_map<uint32_t, uint8_t> none;
    return spellHandler_ ? spellHandler_->getPetTalents() : none;
}

const TalentTabEntry* GameHandler::getPetTalentTab() const {
    return spellHandler_ ? spellHandler_->getPetTalentTab() : nullptr;
}

void GameHandler::learnPreviewPetTalents(const std::vector<std::pair<uint32_t, uint32_t>>& talentRanks) {
    if (spellHandler_) spellHandler_->learnPreviewPetTalents(talentRanks);
}

void GameHandler::learnPetTalent(uint32_t talentId, uint32_t requestedRank) {
    if (spellHandler_) spellHandler_->learnPetTalent(talentId, requestedRank);
}

void GameHandler::learnTalent(uint32_t talentId, uint32_t requestedRank) {
    if (spellHandler_) spellHandler_->learnTalent(talentId, requestedRank);
}

void GameHandler::learnPreviewTalents(const std::vector<std::pair<uint32_t, uint32_t>>& talentRanks) {
    if (spellHandler_) spellHandler_->learnPreviewTalents(talentRanks);
}

void GameHandler::switchTalentSpec(uint8_t newSpec) {
    if (spellHandler_) spellHandler_->switchTalentSpec(newSpec);
}

void GameHandler::confirmPetUnlearn() {
    if (spellHandler_) spellHandler_->confirmPetUnlearn();
}

void GameHandler::confirmTalentWipe() {
    if (spellHandler_) spellHandler_->confirmTalentWipe();
}

void GameHandler::sendAlterAppearance(uint32_t hairStyleEntry, uint32_t hairColor,
                                      uint32_t facialHairEntry, uint32_t skinColorEntry) {
    if (socialHandler_) {
        socialHandler_->sendAlterAppearance(hairStyleEntry, hairColor,
                                            facialHairEntry, skinColorEntry);
    }
}

// ============================================================
// Group/Party
// ============================================================

void GameHandler::inviteToGroup(const std::string& playerName, uint32_t roles) {
    if (socialHandler_) socialHandler_->inviteToGroup(playerName, roles);
}

void GameHandler::acceptGroupInvite(uint32_t roles) {
    if (socialHandler_) socialHandler_->acceptGroupInvite(roles);
}

void GameHandler::declineGroupInvite() {
    if (socialHandler_) socialHandler_->declineGroupInvite();
}

void GameHandler::leaveGroup() {
    if (socialHandler_) socialHandler_->leaveGroup();
}

void GameHandler::forgetLfgDungeonOnWorldport(uint32_t mapId) {
    if (socialHandler_) socialHandler_->forgetLfgDungeonOnWorldport(mapId);
}

void GameHandler::convertToRaid() {
    if (socialHandler_) socialHandler_->convertToRaid();
}

void GameHandler::sendSetLootMethod(uint32_t method, uint32_t threshold, uint64_t masterLooterGuid) {
    if (socialHandler_) socialHandler_->sendSetLootMethod(method, threshold, masterLooterGuid);
}

// ============================================================
// Guild Handlers
// ============================================================

void GameHandler::kickGuildMember(const std::string& playerName) {
    if (socialHandler_) socialHandler_->kickGuildMember(playerName);
}

void GameHandler::disbandGuild() {
    if (socialHandler_) socialHandler_->disbandGuild();
}

void GameHandler::setGuildLeader(const std::string& name) {
    if (socialHandler_) socialHandler_->setGuildLeader(name);
}

void GameHandler::setGuildPublicNote(const std::string& name, const std::string& note) {
    if (socialHandler_) socialHandler_->setGuildPublicNote(name, note);
}

void GameHandler::setGuildOfficerNote(const std::string& name, const std::string& note) {
    if (socialHandler_) socialHandler_->setGuildOfficerNote(name, note);
}

void GameHandler::acceptGuildInvite() {
    if (socialHandler_) socialHandler_->acceptGuildInvite();
}

void GameHandler::declineGuildInvite() {
    if (socialHandler_) socialHandler_->declineGuildInvite();
}

// GMSurveyAnswerSubmit(question, rank, comment) (0x005ac390 -> 0x005ac260):
// question 0-9's rank and comment, kept for the submit.
void GameHandler::setGmSurveyAnswer(uint32_t question, uint8_t rank, const std::string& comment) {
    if (question >= gmSurveyRanks_.size()) return;
    gmSurveyRanks_[question] = rank;
    gmSurveyComments_[question] = comment;
}

// GMSurveyCommentSubmit(comment) (0x005ac480 -> 0x005ac2d0).
void GameHandler::setGmSurveyComment(const std::string& comment) { gmSurveyComment_ = comment; }

GameHandler::GmSurvey GameHandler::currentGmSurvey() const {
    // The client's locale index (0x00405dd0, the table at 0x00ad2fe0); enGB
    // reads as enUS.
    const auto* profile = services_.expansionRegistry ? services_.expansionRegistry->getActive() : nullptr;
    const uint32_t localeIndex =
        static_cast<uint32_t>(chat_profanity::localeIndex(profile ? profile->locale : std::string("enUS")));
    GmSurvey survey;
    if (auto* am = services_.assetManager; am && am->isInitialized()) {
        auto current = am->loadDBCOptional("GMSurveyCurrentSurvey.dbc");
        if (current && current->isLoaded()) {
            const int32_t row = current->findRecordById(localeIndex);
            if (row >= 0) survey.id = current->getUInt32(static_cast<uint32_t>(row), 1);
        }
        auto surveys = am->loadDBCOptional("GMSurveySurveys.dbc");
        if (surveys && surveys->isLoaded() && survey.id != 0) {
            const int32_t row = surveys->findRecordById(survey.id);
            if (row >= 0 && surveys->getFieldCount() >= 11) {
                survey.present = true;
                for (uint32_t i = 0; i < survey.questions.size(); ++i)
                    survey.questions[i] = surveys->getUInt32(static_cast<uint32_t>(row), 1 + i);
            }
        }
    }
    return survey;
}

// GMSurveyQuestion(index) (0x005ac750 -> 0x005ac530): the current survey's
// question at 1-10, looked up in GMSurveyQuestions; its text (+4).
std::optional<std::string> GameHandler::gmSurveyQuestion(int32_t question) const {
    if (question < 1 || question > 10) return std::nullopt;
    const GmSurvey survey = currentGmSurvey();
    const auto id = static_cast<int32_t>(survey.questions[question - 1]);
    if (!survey.present || id <= 0) return std::nullopt;
    auto* am = services_.assetManager;
    auto questions = am ? am->loadDBCOptional("GMSurveyQuestions.dbc") : nullptr;
    if (!questions || !questions->isLoaded() || questions->getFieldCount() < 2) return std::nullopt;
    const int32_t row = questions->findRecordById(static_cast<uint32_t>(id));
    if (row < 0) return std::nullopt;
    return questions->getString(static_cast<uint32_t>(row), 1);
}

// GMSurveyAnswer(question, answer) (0x005ac7d0 -> 0x005ac5b0): the first
// GMSurveyAnswers row, in file order, of that question (+8) whose sort index
// (+4) is answer - 1; its text (+0xc).
std::optional<std::string> GameHandler::gmSurveyAnswer(int32_t question, int32_t answer) const {
    if (question < 1 || question > 10) return std::nullopt;
    const GmSurvey survey = currentGmSurvey();
    const auto id = static_cast<int32_t>(survey.questions[question - 1]);
    if (!survey.present || id < 1) return std::nullopt;
    auto* am = services_.assetManager;
    auto questions = am ? am->loadDBCOptional("GMSurveyQuestions.dbc") : nullptr;
    if (!questions || !questions->isLoaded() || questions->findRecordById(static_cast<uint32_t>(id)) < 0)
        return std::nullopt;
    auto answers = am->loadDBCOptional("GMSurveyAnswers.dbc");
    if (!answers || !answers->isLoaded() || answers->getFieldCount() < 4) return std::nullopt;
    for (uint32_t r = 0; r < answers->getRecordCount(); ++r) {
        if (answers->getInt32(r, 2) == id && answers->getInt32(r, 1) == answer - 1)
            return answers->getString(r, 3);
    }
    return std::nullopt;
}

// GMSurveyNumAnswers(question) (0x005ac870 -> 0x005ac680). Not what the name
// says: with a current survey, it takes question - 1 itself as a
// GMSurveyQuestions id (0x005ac6e2), not the survey's question at that index,
// and counts the GMSurveyAnswers rows of that question among ids 0 to the
// row count - 1 (the loop at 0x005ac720). Blizzard_GMSurveyUI never calls it.
int32_t GameHandler::gmSurveyNumAnswers(int32_t question) const {
    if (question < 1 || question > 10) return 0;
    if (!currentGmSurvey().present) return 0;
    const int32_t id = question - 1;
    if (id <= 0) return 0;
    auto* am = services_.assetManager;
    auto questions = am ? am->loadDBCOptional("GMSurveyQuestions.dbc") : nullptr;
    if (!questions || !questions->isLoaded() || questions->findRecordById(static_cast<uint32_t>(id)) < 0)
        return 0;
    auto answers = am->loadDBCOptional("GMSurveyAnswers.dbc");
    if (!answers || !answers->isLoaded() || answers->getFieldCount() < 4) return 0;
    int32_t count = 0;
    for (uint32_t answerId = 0; answerId < answers->getRecordCount(); ++answerId) {
        const int32_t row = answers->findRecordById(answerId);
        if (row >= 0 && answers->getInt32(static_cast<uint32_t>(row), 2) == id) ++count;
    }
    return count;
}

// GMSurveySubmit (0x005ad230 -> 0x005acd50): CMSG_GMSURVEY_SUBMIT. The survey
// is GMSurveyCurrentSurvey's for the client's locale index (DAT_00c5de9c);
// each of its ten questions goes as its id, then - while there is one - the
// rank and comment given it, the first missing id ending the list; then the
// comment. What was kept is let go after (0x005ac1f0).
void GameHandler::submitGmSurvey() {
    const GmSurvey survey = currentGmSurvey();
    const uint32_t surveyId = survey.id;
    const auto& questions = survey.questions;
    if (isInWorld()) {
        network::Packet packet(wireOpcode(Opcode::CMSG_GMSURVEY_SUBMIT));
        packet.writeUInt32(surveyId);
        for (uint32_t i = 0; i < questions.size(); ++i) {
            packet.writeUInt32(questions[i]);
            if (questions[i] == 0) break;
            packet.writeUInt8(gmSurveyRanks_[i]);
            packet.writeString(gmSurveyComments_[i]);
        }
        packet.writeString(gmSurveyComment_);
        socket->send(packet);
    }
    gmSurveyRanks_.fill(0);
    for (auto& comment : gmSurveyComments_) comment.clear();
    gmSurveyComment_.clear();
}

void GameHandler::submitGmTicket(const std::string& text, bool needResponse, bool needMoreHelp) {
    if (chatHandler_) chatHandler_->submitGmTicket(text, needResponse, needMoreHelp);
}

void GameHandler::updateGmTicket(const std::string& text) {
    if (chatHandler_) chatHandler_->updateGmTicket(text);
}

void GameHandler::destroyTotem(int slot) {
    // CMSG_TOTEM_DESTROYED carries the slot and nothing else on these
    // expansions; the guid form arrived a expansion later than any of them.
    if (slot < 0 || slot > 3 || !isInWorld() || !getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_TOTEM_DESTROYED));
    pkt.writeUInt8(static_cast<uint8_t>(slot));
    getSocket()->send(pkt);
}

void GameHandler::deleteGmTicket() {
    if (socialHandler_) socialHandler_->deleteGmTicket();
}

void GameHandler::requestGmTicket() {
    if (socialHandler_) socialHandler_->requestGmTicket();
}

void GameHandler::requestGmSystemStatus() {
    if (socialHandler_) socialHandler_->requestGmSystemStatus();
}

void GameHandler::requestGuildEventLog() {
    if (socialHandler_) socialHandler_->requestGuildEventLog();
}

const std::vector<GuildEventLogEntry>& GameHandler::getGuildEventLog() const {
    static const std::vector<GuildEventLogEntry> empty;
    return socialHandler_ ? socialHandler_->getGuildEventLog() : empty;
}

void GameHandler::queryGuildInfo(uint32_t guildId) {
    if (socialHandler_) socialHandler_->queryGuildInfo(guildId);
}

std::optional<GuildEmblem> GameHandler::lookupGuildEmblem(uint32_t guildId) {
    return socialHandler_ ? socialHandler_->lookupGuildEmblem(guildId) : std::nullopt;
}

void GameHandler::noteGuildTimestamp(uint32_t guildId, uint32_t timestamp) {
    if (socialHandler_) socialHandler_->noteGuildTimestamp(guildId, timestamp);
}

uint32_t GameHandler::guildEmblemGeneration() const {
    return socialHandler_ ? socialHandler_->guildEmblemGeneration() : 0;
}

const std::string& GameHandler::lookupGuildName(uint32_t guildId) {
    static const std::string kEmpty;
    if (socialHandler_) return socialHandler_->lookupGuildName(guildId);
    return kEmpty;
}

uint32_t GameHandler::getEntityGuildId(uint64_t guid) const {
    if (socialHandler_) return socialHandler_->getEntityGuildId(guid);
    return 0;
}

void GameHandler::createGuild(const std::string& guildName) {
    if (socialHandler_) socialHandler_->createGuild(guildName);
}

void GameHandler::addGuildRank(const std::string& rankName) {
    if (socialHandler_) socialHandler_->addGuildRank(rankName);
}

void GameHandler::deleteGuildRank() {
    if (socialHandler_) socialHandler_->deleteGuildRank();
}

void GameHandler::requestPetitionShowlist(uint64_t npcGuid) {
    if (socialHandler_) socialHandler_->requestPetitionShowlist(npcGuid);
}

void GameHandler::buyPetition(uint64_t npcGuid, const std::string& guildName,
                              uint32_t clientIndex) {
    if (socialHandler_) socialHandler_->buyPetition(npcGuid, guildName, clientIndex);
}

void GameHandler::signPetition(uint64_t petitionGuid) {
    if (socialHandler_) socialHandler_->signPetition(petitionGuid);
}

void GameHandler::turnInArenaPetition(uint64_t petitionGuid, const std::array<uint32_t, 5>& design) {
    if (socialHandler_) socialHandler_->turnInArenaPetition(petitionGuid, design);
}

void GameHandler::turnInPetition(uint64_t petitionGuid, bool blankDesign) {
    if (socialHandler_) socialHandler_->turnInPetition(petitionGuid, blankDesign);
}

void GameHandler::offerPetition(uint64_t petitionGuid, uint64_t targetGuid) {
    if (socialHandler_) socialHandler_->offerPetition(petitionGuid, targetGuid);
}

// ============================================================
// Loot, Gossip, Vendor
// ============================================================

void GameHandler::lootTarget(uint64_t guid) {
    if (inventoryHandler_) inventoryHandler_->lootTarget(guid);
}

void GameHandler::lootCorpse(uint64_t guid) {
    if (!isInWorld()) return;
    // 0x006dad90: a corpse is looted standing still - while the player moves
    // (any of the movement flags 0xc010ff) the click does nothing at all.
    if ((movementInfo.flags & 0xc010ffu) != 0) return;
    // A mounted player gets down first where Auto Dismount (and, in the air,
    // Auto Dismount in Flight) lets it (0x0071b0c0), else is told so and
    // nothing is sent.
    if (isMounted() && !isTaxiMountActive()) {
        auto me = std::dynamic_pointer_cast<Unit>(getEntityManager().getEntity(getPlayerGuid()));
        const uint32_t unitFlags = me ? me->getUnitFlags() : 0u;
        const bool getsDown = (unitFlags & 0x100000u) == 0 &&
                              addons::storedCVarValue("autoDismount", "1") != "0" &&
                              (!isPlayerFlying() ||
                               addons::storedCVarValue("autoDismountFlying", "0") != "0");
        if (!getsDown) {
            playGameErrorSound("ERR_NOT_WHILE_MOUNTED");
            raiseUiError(interfaceString("ERR_NOT_WHILE_MOUNTED", "You can't do that while mounted."));
            return;
        }
        dismount();
    }
    // 0x00706010: whether this loot is taken at once (0x006cde50 with
    // 0x00513700).
    setAutoLoot(clickAutoLoots());
    lootTarget(guid);
}

void GameHandler::lootItem(uint8_t slotIndex, bool confirmed) {
    if (inventoryHandler_) inventoryHandler_->lootItem(slotIndex, confirmed);
}

void GameHandler::confirmPendingLoot() {
    if (inventoryHandler_) inventoryHandler_->confirmPendingLoot();
}

void GameHandler::confirmBindOnUse() {
    if (inventoryHandler_) inventoryHandler_->confirmBindOnUse();
}

void GameHandler::lootMoney() {
    if (inventoryHandler_) inventoryHandler_->lootMoney();
}

void GameHandler::cancelTempEnchantment(uint8_t handIndex) {
    if (inventoryHandler_) inventoryHandler_->cancelTempEnchantment(handIndex);
}

void GameHandler::closeLoot() {
    if (inventoryHandler_) inventoryHandler_->closeLoot();
}

void GameHandler::lootMasterGive(uint8_t lootSlot, uint64_t targetGuid) {
    if (inventoryHandler_) inventoryHandler_->lootMasterGive(lootSlot, targetGuid);
}

void GameHandler::interactWithNpc(uint64_t guid) {
    if (!isInWorld()) return;
    // 0x006ddbb0 first: in a form that takes no items (0x00721ca0) whose row
    // is missing or lacks flag 8, the player leaves it where autoUnshift lets
    // it (0x0071b130, 0x00726ce0) and is refused ERR_CANT_INTERACT_SHAPESHIFTED
    // where it does not.
    if (const uint8_t form = getShapeshiftFormId(); form != 0) {
        const auto* row = shapeshiftFormRow(form);
        if ((!row || (row->flags & 0x1u) == 0) && (!row || (row->flags & 0x8u) == 0)) {
            if (!wouldAutoUnshift()) {
                playGameErrorSound("ERR_CANT_INTERACT_SHAPESHIFTED");
                raiseUiError(interfaceString("ERR_CANT_INTERACT_SHAPESHIFTED",
                                             "Can't speak while shapeshifted."));
                return;
            }
            leaveShapeshiftForm();
        }
    }
    // 0x006ddbb0: nothing past the NPC's combat reach plus four yards (a
    // straight-line distance, height included) is spoken to. With
    // click-to-move interaction the client walks there instead; without it -
    // and there is none here - it says so and sends nothing.
    if (auto entity = getEntityManager().getEntity(guid)) {
        if (auto* unit = dynamic_cast<Unit*>(entity.get())) {
            const float dx = movementInfo.x - entity->getX();
            const float dy = movementInfo.y - entity->getY();
            const float dz = movementInfo.z - entity->getZ();
            const float reach = unit->getCombatReach() + 4.0f;
            if (dx * dx + dy * dy + dz * dz > reach * reach) {
                playGameErrorSound("ERR_TOO_FAR_TO_INTERACT");
                raiseUiError(interfaceString("ERR_TOO_FAR_TO_INTERACT",
                                             "You need to be closer to interact with that target."));
                return;
            }
        }
    }
    // Only to a unit the server says has something to say. UNIT_NPC_FLAGS is
    // zero for a creature that is not a questgiver, vendor, trainer, healer or
    // any of the rest, and a hello sent to one of those is answered with the
    // default gossip text - which is how a dog came to greet the player by
    // name through a full conversation window with nothing in it but Goodbye.
    //
    // Zero is a real answer rather than a missing one: an update block carries
    // only the fields that are set, so a creature with no NPC flags and one
    // whose flags have not arrived are the same case and both mean there is
    // nothing here to talk to.
    if (auto entity = getEntityManager().getEntity(guid)) {
        if (auto* unit = dynamic_cast<Unit*>(entity.get())) {
            if (!unit->isInteractable()) {
                LOG_DEBUG("interactWithNpc: 0x", std::hex, guid, std::dec,
                          " has no NPC flags; no gossip hello sent");
                return;
            }
            // Sent anyway - the server is the judge - but said, because the
            // server says nothing: an NPC whose faction regards the player as
            // Unfriendly or worse is refused the conversation in silence.
            if (const int reaction = unitReactionToPlayer(*unit); reaction <= 3) {
                static std::unordered_set<uint64_t> said;
                if (said.insert(guid).second) {
                    LOG_WARNING("interactWithNpc: ", unit->getName(), " (faction template ",
                                unit->getFactionTemplate(), ") regards you as ",
                                kReputationStandings[std::max(reaction, 1) - 1].name,
                                " - the server will not talk to you below Neutral");
                }
            }
        }
    }

    // 0x006ddbb0: a questgiver with nothing for the player yet (status 1)
    // opens its tutorial.
    if (getQuestGiverStatus(guid) == QuestGiverStatus::UNAVAILABLE) triggerTutorial(0x2a);

    const npc_interaction::Unit u = npcInteractionFacts(guid);
    // A flight master whose node is not yet learned is told so first
    // (0x006d62a0, CMSG_ENABLETAXI); only a questgiver with something to say
    // is spoken to past it.
    if (u.taxiNodeUnknown) {
        network::Packet enable(wireOpcode(Opcode::CMSG_ENABLETAXI));
        enable.writeUInt64(guid);
        socket->send(enable);
        if (!npc_interaction::questGiverHas(u)) return;
    }
    switch (npc_interaction::decide(u)) {
        case npc_interaction::Action::SpellClick:
        case npc_interaction::Action::VehicleEnter:
            clickUnitBeyondHellos(guid);
            return;
        case npc_interaction::Action::Mailbox:
            openMailbox(guid);  // 0x0056da60
            return;
        case npc_interaction::Action::Nothing:
            return;
        case npc_interaction::Action::Hello:
            break;
    }
    // The hello its flags ask for, each with its own opcode.
    namespace ni = npc_interaction;
    Opcode op = Opcode::CMSG_GOSSIP_HELLO;
    switch (ni::hello(u)) {
        case ni::Hello::Gossip: op = Opcode::CMSG_GOSSIP_HELLO; break;
        case ni::Hello::QuestGiver: op = Opcode::CMSG_QUESTGIVER_HELLO; break;
        case ni::Hello::FlightMaster: op = Opcode::CMSG_TAXIQUERYAVAILABLENODES; break;
        case ni::Hello::Vendor: op = Opcode::CMSG_LIST_INVENTORY; break;
        case ni::Hello::Trainer: op = Opcode::CMSG_TRAINER_LIST; break;
        case ni::Hello::SpiritHealer:
            // 0x006db010: only a ghost, and nothing sent yet - the healer is
            // remembered and CONFIRM_XP_LOSS asks; its accept activates it.
            if (isPlayerGhost()) {
                resurrectCasterGuid_ = guid;
                resurrectCasterName_.clear();
                resurrectIsSpiritHealer_ = true;
                resurrectRequestPending_ = true;
                fireAddonEvent("CONFIRM_XP_LOSS", {});
            }
            return;
        case ni::Hello::SpiritGuide:
            // 0x006db180: a ghost asks the battleground's spirit healer when
            // it next resurrects (CMSG_AREA_SPIRIT_HEALER_QUERY).
            if (isPlayerGhost()) queryAreaSpiritHealer(guid);
            return;
        case ni::Hello::Innkeeper:
            // 0x006d2120: remembered and asked, nothing sent until the accept.
            askBinder(guid);
            return;
        case ni::Hello::Banker: op = Opcode::CMSG_BANKER_ACTIVATE; break;
        case ni::Hello::Petitioner: op = Opcode::CMSG_PETITION_SHOWLIST; break;
        case ni::Hello::TabardDesigner:
            activateTabardVendor(guid);
            return;
        case ni::Hello::Battlemaster: op = Opcode::CMSG_BATTLEMASTER_HELLO; break;
        case ni::Hello::Auctioneer: op = Opcode::MSG_AUCTION_HELLO; break;
        case ni::Hello::StableMaster: op = Opcode::MSG_LIST_STABLED_PETS; break;
        case ni::Hello::GuildBanker:
            // 0x006d23c0: with the byte that asks for the slots too.
            socket->send(GuildBankerActivatePacket::build(guid));
            return;
        case ni::Hello::None: return;
    }
    if (wireOpcode(op) == 0xFFFF) return;
    network::Packet packet(wireOpcode(op));
    packet.writeUInt64(guid);
    socket->send(packet);

    pendingNpcHello_ = PendingNpcHello{};
    pendingNpcHello_.guid = guid;
    pendingNpcHello_.sentAt = std::chrono::steady_clock::now();
    if (auto entity = getEntityManager().getEntity(guid)) {
        const float dx = movementInfo.x - entity->getX();
        const float dy = movementInfo.y - entity->getY();
        const float dz = movementInfo.z - entity->getZ();
        pendingNpcHello_.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (auto* unit = dynamic_cast<Unit*>(entity.get())) {
            pendingNpcHello_.name = unit->getName();
            pendingNpcHello_.reaction = unitReactionToPlayer(*unit);
            pendingNpcHello_.npcFlags = unit->getNpcFlags();
        }
    }
    LOG_DEBUG("interactWithNpc: hello to ", pendingNpcHello_.name, " at ",
              pendingNpcHello_.distance, " yards");
}

npc_interaction::Unit GameHandler::npcInteractionFacts(uint64_t guid) const {
    npc_interaction::Unit u;
    auto entity = getEntityManager().getEntity(guid);
    auto* unit = dynamic_cast<Unit*>(entity.get());
    if (!unit) return u;
    u.npcFlags = unit->getNpcFlags();
    if (const uint16_t f = fieldIndex(UF::UNIT_FIELD_FLAGS_2); f != 0xFFFF) u.flags2 = unit->getField(f);
    u.questStatus = static_cast<uint8_t>(getQuestGiverStatus(guid));
    u.isPlayer = entity->getType() == ObjectType::PLAYER;
    u.isSelf = guid == playerGuid;
    for (const auto& m : getPartyData().members) {
        if (m.guid == guid) { u.inGroup = true; break; }
    }
    u.emptySeats = vehicleRoster_.emptySeats(guid);
    u.taxiNodeUnknown = isTaxiNodeUnknown(guid);
    return u;
}

bool GameHandler::clickUnitBeyondHellos(uint64_t guid) {
    if (!isInWorld() || !socket || guid == 0) return false;
    if (!dynamic_cast<Unit*>(getEntityManager().getEntity(guid).get())) return false;
    const npc_interaction::Unit u = npcInteractionFacts(guid);
    const auto action = npc_interaction::decide(u);
    if (action != npc_interaction::Action::SpellClick &&
        action != npc_interaction::Action::VehicleEnter) {
        return false;
    }
    const Opcode op = action == npc_interaction::Action::SpellClick
                          ? Opcode::CMSG_SPELLCLICK
                          : Opcode::CMSG_PLAYER_VEHICLE_ENTER;
    if (wireOpcode(op) == 0xFFFF) return false;
    network::Packet packet(wireOpcode(op));
    packet.writeUInt64(guid);
    socket->send(packet);
    LOG_INFO(action == npc_interaction::Action::SpellClick ? "CMSG_SPELLCLICK" : "CMSG_PLAYER_VEHICLE_ENTER",
             ": 0x", std::hex, guid, std::dec);
    return true;
}

void GameHandler::loadingKeepAlive(bool starting) {
    const auto now = std::chrono::steady_clock::now();
    if (!starting && now < nextLoadingKeepAlive_) return;
    if (!starting && socket && wireOpcode(Opcode::CMSG_KEEP_ALIVE) != 0xFFFF) {
        network::Packet packet(wireOpcode(Opcode::CMSG_KEEP_ALIVE));
        socket->send(packet);
    }
    nextLoadingKeepAlive_ = now + std::chrono::seconds(30);
}

void GameHandler::queryAreaSpiritHealer(uint64_t guid) {
    if (!isInWorld() || !socket) return;
    areaSpiritHealerGuid_ = guid;
    network::Packet packet(wireOpcode(Opcode::CMSG_AREA_SPIRIT_HEALER_QUERY));
    packet.writeUInt64(guid);
    socket->send(packet);
    LOG_INFO("CMSG_AREA_SPIRIT_HEALER_QUERY: 0x", std::hex, guid, std::dec);
}

void GameHandler::queueAreaSpiritHeal() {
    // Joining the queue is what resurrects; the query only asks when the next
    // one is. AzerothCore has no way to be told to leave it again, which is
    // why CancelAreaSpiritHeal answers with nothing.
    if (!isInWorld() || !socket || !areaSpiritHealerGuid_) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_AREA_SPIRIT_HEALER_QUEUE));
    packet.writeUInt64(areaSpiritHealerGuid_);
    socket->send(packet);
    LOG_INFO("CMSG_AREA_SPIRIT_HEALER_QUEUE: 0x", std::hex, areaSpiritHealerGuid_, std::dec);
}

void GameHandler::interactWithGameObject(uint64_t guid) {
    if (guid == 0 || !isInWorld()) return;
    performGameObjectInteractionNow(guid);
}

bool GameHandler::meetingStoneAllowsUse(const GameObjectQueryResponseData* goInfo) {
    // 0x00712270, a summoning stone, in its order: a group to summon for; a
    // target; the player's own level in the stone's range (0x007121e0, its
    // first two data fields); the target in the party or raid; and the
    // target's level in the range too - read off the unit, or off its group
    // row when it is online and out of sight, and ERR_MEETING_STONE_NOT_FOUND
    // when neither says.
    auto refuse = [this](const char* key, const char* enUS) {
        playGameErrorSound(key);
        addUIError(interfaceString(key, enUS));
        return false;
    };
    if (!isInGroup())
        return refuse("ERR_MEETING_STONE_NEED_PARTY", "You must be in a party to use this meeting stone.");
    const uint64_t target = getTargetGuid();
    if (target == 0) return refuse("ERR_GENERIC_NO_TARGET", "You have no target.");
    const uint32_t minLevel = goInfo && goInfo->hasData ? goInfo->data[0] : 0;
    const uint32_t maxLevel = goInfo && goInfo->hasData ? goInfo->data[1] : 0;
    auto inRange = [&](uint32_t level) { return level >= minLevel && level <= maxLevel; };
    if (!inRange(getPlayerLevel()))
        return refuse("ERR_MEETING_STONE_INVALID_LEVEL", "You are not the correct level to use this meeting stone.");
    const GroupMember* row = nullptr;
    for (const auto& m : getPartyData().members) {
        if (m.guid == target) { row = &m; break; }
    }
    if (!row)
        return refuse("ERR_MEETING_STONE_TARGET_NOT_IN_PARTY", "Your target is not in your party.");
    uint32_t targetLevel = 0;
    if (auto* unit = dynamic_cast<Unit*>(getEntityManager().getEntity(target).get())) {
        targetLevel = unit->getLevel();
    } else if (row->isOnline && row->level != 0) {
        targetLevel = row->level;
    } else {
        return refuse("ERR_MEETING_STONE_NOT_FOUND", "Your target could not be found.");
    }
    if (!inRange(targetLevel))
        return refuse("ERR_MEETING_STONE_TARGET_INVALID_LEVEL",
                      "Your target is not the correct level to use this meeting stone.");
    return true;
}

void GameHandler::performGameObjectInteractionNow(uint64_t guid) {
    if (guid == 0) return;
    if (!isInWorld()) return;
    auto entity = entityController_->getEntityManager().getEntity(guid);
    if (!entity || entity->getType() != ObjectType::GAMEOBJECT) return;
    auto go = std::static_pointer_cast<GameObject>(entity);
    const uint32_t goEntry = go->getEntry();
    const std::string goName = go->getName();
    const GameObjectQueryResponseData* goInfo = getCachedGameObjectInfo(goEntry);
    const GameObjectFacts facts = gameObjectFacts(*go, goInfo);
    const uint32_t goType = facts.type;
    // 0x00711140: an object its handler says cannot be clicked - a fishing
    // school, a transport, someone else's bobber, an unselectable or busy
    // one - takes no click at all. One that can lets go of the loot open
    // (0x00523640, CMSG_LOOT_RELEASE) before it is used.
    if (!gameObjectClickable(*go, facts)) return;
    closeLoot();
    setAutoLoot(clickAutoLoots());  // 0x006cde50(0x00513700())

    // 0x0070f680: the type's handler is asked whether the object may be used
    // (0x00711470) and its refusal said when not - out of reach is
    // ERR_USE_TOO_FAR, by a box round the object's model rather than a
    // distance from its middle.
    {
        std::string arg;
        if (const uint16_t refusal = gameObjectRefusal(*go, facts, arg); refusal != 0) {
            if (refusal != go_use::kRefusedSilently)
                sayGameObjectError(refusal, arg.empty() ? std::vector<std::string>{} : std::vector<std::string>{arg});
            return;
        }
    }
    if (goType == 23 && !meetingStoneAllowsUse(goInfo)) return;
    // The types whose handler has a use of its own (0x0070c850, 0x0070c9d0,
    // 0x0070ecb0): the sign reads its page into the item text frame, the
    // mailbox opens the mail frame and the guild vault asks the server
    // (CMSG_GUILD_BANKER_ACTIVATE) - none sends CMSG_GAMEOBJ_USE. Every other
    // type uses 0x007116c0.
    LOG_INFO("GO interaction: guid=0x", std::hex, guid, std::dec, " entry=", goEntry, " type=", goType,
             " name='", goName, "' lockId=", gameObjectData(facts, go_use::Field::Lock));
    if (goType == 9) {
        if (entityController_) entityController_->showGameObjectPageText(guid);
    } else if (goType == 19) {
        openMailbox(guid);
    } else if (goType == 34) {
        openGuildBank(guid);
    } else if (useGameObjectByLock(*go, facts)) {
        // A GOOBER's page comes back from the server (SMSG_GAMEOBJECT_PAGETEXT).
        if (guid == hookedFishingBobberGuid_) hookedFishingBobberGuid_ = 0;
    }

    // 0x0070f680 reports every use it let through, whatever the use did
    // (CMSG_GAMEOBJ_REPORT_USE, 0x481) - which is what GO scripts give quest
    // credit from.
    const auto* table = getActiveOpcodeTable();
    if (table && table->hasOpcode(Opcode::CMSG_GAMEOBJ_REPORT_USE)) {
        network::Packet reportUse(wireOpcode(Opcode::CMSG_GAMEOBJ_REPORT_USE));
        reportUse.writeUInt64(guid);
        socket->send(reportUse);
    }
}

void GameHandler::selectGossipOption(uint32_t optionId, const std::string& code) {
    if (questHandler_) questHandler_->selectGossipOption(optionId, code);
}

void GameHandler::selectGossipQuest(uint32_t questId) {
    if (questHandler_) questHandler_->selectGossipQuest(questId);
}

bool GameHandler::requestQuestQuery(uint32_t questId, bool force) {
    return questHandler_ && questHandler_->requestQuestQuery(questId, force);
}

bool GameHandler::hasQuestInLog(uint32_t questId) const {
    return questHandler_ && questHandler_->hasQuestInLog(questId);
}

Unit* GameHandler::getUnitByGuid(uint64_t guid) {
    auto entity = entityController_->getEntityManager().getEntity(guid);
    // Use the type tag to skip RTTI - both UNIT and PLAYER object types derive from Unit.
    if (!entity || !entity->isUnit()) return nullptr;
    return static_cast<Unit*>(entity.get());
}

std::vector<std::string> GameHandler::unitTokensForGuid(uint64_t guid) const {
    std::vector<std::string> out;
    if (guid == 0) return out;
    if (guid == playerGuid) out.emplace_back("player");
    if (guid == petGuid_) out.emplace_back("pet");
    if (isInGroup()) {
        const auto& group = getPartyData();
        for (int slot = 1; slot <= 4; ++slot)
            if (partySlotGuid(group, playerGuid, slot) == guid) out.push_back("party" + std::to_string(slot));
        if (groupIsRaid(group.groupType)) {
            for (size_t i = 0; i < group.members.size() && i < 40; ++i)
                if (group.members[i].guid == guid) out.push_back("raid" + std::to_string(i + 1));
        }
    }
    if (guid == targetGuid) out.emplace_back("target");
    if (guid == focusGuid) out.emplace_back("focus");
    if (guid == mouseoverGuid_) out.emplace_back("mouseover");
    for (size_t i = 0; i < arena::Roster::kSlots; ++i) {
        if (arenaRoster_.opponent(i) == guid) out.push_back("arena" + std::to_string(i + 1));
        if (arenaRoster_.pet(i) == guid) out.push_back("arenapet" + std::to_string(i + 1));
    }
    for (uint32_t i = 0; i < kMaxEncounterSlots; ++i)
        if (getEncounterUnitGuid(i) == guid) out.push_back("boss" + std::to_string(i + 1));
    return out;
}

std::string GameHandler::guidToUnitId(uint64_t guid) const {
    if (guid == playerGuid)      return "player";
    if (guid == targetGuid)      return "target";
    if (guid == focusGuid)       return "focus";
    if (guid == petGuid_)        return "pet";
    return {};
}
int GameHandler::findQuestLogSlotIndexFromServer(uint32_t questId) const {
    if (questHandler_) return questHandler_->findQuestLogSlotIndexFromServer(questId);
    return 0;
}

void GameHandler::addQuestToLocalLogIfMissing(uint32_t questId, const std::string& title, const std::string& objectives) {
    if (questHandler_) questHandler_->addQuestToLocalLogIfMissing(questId, title, objectives);
}

bool GameHandler::resyncQuestLogFromServerSlots(bool forceQueryMetadata) {
    return questHandler_ && questHandler_->resyncQuestLogFromServerSlots(forceQueryMetadata);
}

// Apply quest completion state from player update fields to already-tracked local quests.
// Called from VALUES update handler so quests that complete mid-session (or that were
// complete on login) get quest.complete=true without waiting for SMSG_QUESTUPDATE_COMPLETE.
void GameHandler::applyQuestStateFromFields(const FlatFieldMap& fields) {
    if (questHandler_) questHandler_->applyQuestStateFromFields(fields);
}

// Extract expansion-specific kill/objective counters from quest-log update fields
// and populate quest.killCounts using the structured objectives obtained from a prior
// SMSG_QUEST_QUERY_RESPONSE. Silently does nothing if objectives are absent.
void GameHandler::applyPackedKillCountsFromFields(QuestLogEntry& quest) {
    if (questHandler_) questHandler_->applyPackedKillCountsFromFields(quest);
}

void GameHandler::clearPendingQuestAccept(uint32_t questId) {
    if (questHandler_) questHandler_->clearPendingQuestAccept(questId);
}

void GameHandler::triggerQuestAcceptResync(uint32_t questId, uint64_t npcGuid, const char* reason) {
    if (questHandler_) questHandler_->triggerQuestAcceptResync(questId, npcGuid, reason);
}

void GameHandler::acceptQuest() {
    if (questHandler_) questHandler_->acceptQuest();
}

void GameHandler::declineQuest(bool announce) {
    if (questHandler_) questHandler_->declineQuest(announce);
}

void GameHandler::abandonQuest(uint32_t questId) {
    if (questHandler_) questHandler_->abandonQuest(questId);
    setQuestTracked(questId, false);
}

void GameHandler::shareQuestWithParty(uint32_t questId) {
    if (questHandler_) questHandler_->shareQuestWithParty(questId);
}

void GameHandler::completeQuest() {
    if (questHandler_) questHandler_->completeQuest();
}

void GameHandler::closeQuestRequestItems(bool announce) {
    if (questHandler_) questHandler_->closeQuestRequestItems(announce);
}

void GameHandler::chooseQuestReward(uint32_t rewardIndex) {
    if (questHandler_) questHandler_->chooseQuestReward(rewardIndex);
}

void GameHandler::closeQuestOfferReward(bool announce) {
    if (questHandler_) questHandler_->closeQuestOfferReward(announce);
}

void GameHandler::closeGossip() {
    if (questHandler_) questHandler_->closeGossip();
}

void GameHandler::offerQuestFromItem(uint64_t itemGuid, uint32_t questId) {
    if (questHandler_) questHandler_->offerQuestFromItem(itemGuid, questId);
}

uint64_t GameHandler::getBankSlotGuid(int slot) const {
    return inventoryHandler_ ? inventoryHandler_->getBankSlotGuid(slot) : 0;
}

uint64_t GameHandler::getBankBagItemGuid(int bag, int slot) const {
    return inventoryHandler_ ? inventoryHandler_->getBankBagItemGuid(bag, slot) : 0;
}

uint64_t GameHandler::getBagItemGuid(int bagIndex, int slotIndex) const {
    if (bagIndex < 0 || bagIndex >= inventory.NUM_BAG_SLOTS) return 0;
    if (slotIndex < 0) return 0;
    uint64_t bagGuid = equipSlotGuids_[Inventory::FIRST_BAG_EQUIP_SLOT + bagIndex];
    if (bagGuid == 0) return 0;
    auto it = containerContents_.find(bagGuid);
    if (it == containerContents_.end()) return 0;
    if (slotIndex >= static_cast<int>(it->second.numSlots)) return 0;
    return it->second.slotGuids[slotIndex];
}

void GameHandler::openVendor(uint64_t npcGuid) {
    if (inventoryHandler_) inventoryHandler_->openVendor(npcGuid);
}

void GameHandler::closeVendor() {
    if (inventoryHandler_) inventoryHandler_->closeVendor();
}

void GameHandler::buyItem(uint64_t vendorGuid, uint32_t itemId, uint32_t slot, uint32_t count) {
    if (inventoryHandler_) inventoryHandler_->buyItem(vendorGuid, itemId, slot, count);
}

void GameHandler::buyItemInSlot(uint64_t vendorGuid, uint32_t itemId, uint32_t vendorSlot,
                                uint8_t wireBag, uint8_t wireSlot, uint32_t count) {
    if (inventoryHandler_) inventoryHandler_->buyItemInSlot(vendorGuid, itemId, vendorSlot, wireBag, wireSlot, count);
}

bool GameHandler::petBarEditable() const {
    if (petGuid_ == 0) return false;
    auto pet = getEntityManager().getEntity(petGuid_);
    auto* unit = dynamic_cast<const Unit*>(pet.get());
    return unit && (unit->getUnitFlags() & 0x01000000u) == 0;
}

namespace {
/// CMSG_PET_SET_ACTION (0x005d6020): the pet, the other slot that moved and
/// its action when there is one, then the slot and its action.
void sendPetSetAction(network::WorldSocket* socket, uint64_t pet, int moved, uint32_t movedAction,
                      int slot, uint32_t action) {
    if (!socket || wireOpcode(Opcode::CMSG_PET_SET_ACTION) == 0xFFFF) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_PET_SET_ACTION));
    packet.writeUInt64(pet);
    if (moved >= 0) {
        packet.writeUInt32(static_cast<uint32_t>(moved));
        packet.writeUInt32(movedAction);
    }
    packet.writeUInt32(static_cast<uint32_t>(slot));
    packet.writeUInt32(action);
    socket->send(packet);
}
}  // namespace

uint32_t GameHandler::placePetAction(int idx, uint32_t action) {
    namespace pab = pet_action_bar;
    if (!petBarEditable() || idx < 0 || idx >= pab::kSlots) return 0;
    // A passive spell is not put on the bar (Attributes 0x40).
    if (pab::typeOf(action) == pab::kTypeSpell && pab::valueOf(action) != 0 &&
        isSpellPassive(pab::valueOf(action))) {
        return 0;
    }
    pab::Slots slots{};
    for (int i = 0; i < pab::kSlots; ++i) slots[static_cast<size_t>(i)] = petActionSlots_[i];
    const auto placed = pab::place(slots, idx, action);
    if (!placed.changed) return 0;
    for (int i = 0; i < pab::kSlots; ++i) petActionSlots_[i] = slots[static_cast<size_t>(i)];
    if (state == WorldState::IN_WORLD)
        sendPetSetAction(socket.get(), petGuid_, placed.moved,
                         placed.moved >= 0 ? petActionSlots_[placed.moved] : 0, idx, petActionSlots_[idx]);
    fireAddonEvent("PET_BAR_UPDATE", {});
    return placed.displaced;
}

void GameHandler::clearPetActionSlot(int idx) {
    namespace pab = pet_action_bar;
    if (idx < 0 || idx >= pab::kSlots) return;
    placePetAction(idx, petActionSlots_[idx] & 0xff000000u);
}

bool GameHandler::wrappingItem() const {
    return inventoryHandler_ && inventoryHandler_->wrapping();
}

void GameHandler::wrapItemAt(uint8_t wireBag, uint8_t wireSlot) {
    if (inventoryHandler_) inventoryHandler_->wrapItemAt(wireBag, wireSlot);
}

void GameHandler::cancelWrapping() {
    if (inventoryHandler_) inventoryHandler_->cancelWrapping();
}

void GameHandler::buyBackItem(uint32_t row) {
    if (inventoryHandler_) inventoryHandler_->buyBackItem(row);
}

void GameHandler::repairItem(uint64_t vendorGuid, uint64_t itemGuid) {
    if (inventoryHandler_) inventoryHandler_->repairItem(vendorGuid, itemGuid);
}

void GameHandler::repairAll(uint64_t vendorGuid, bool useGuildBank) {
    if (inventoryHandler_) inventoryHandler_->repairAll(vendorGuid, useGuildBank);
}

uint32_t GameHandler::estimateRepairAllCost() const {
    if (inventoryHandler_) return inventoryHandler_->estimateRepairAllCost();
    return 0;
}

uint32_t GameHandler::itemSellPriceShown(uint64_t itemGuid, uint32_t itemId, uint32_t stackCount) const {
    const auto* info = getItemInfo(itemId);
    if (!info || !info->valid || info->sellPrice == 0) return 0;
    uint32_t price = info->sellPrice;
    const uint32_t repair = inventoryHandler_ && itemGuid ? inventoryHandler_->estimateItemRepairCost(itemGuid) : 0;
    if (repair >= price) price = 1;
    else price -= repair;
    return stackCount > 1 ? price * stackCount : price;
}

uint32_t GameHandler::itemRepairCostShown(uint64_t itemGuid) const {
    if (!inventoryHandler_ || itemGuid == 0) return 0;
    const uint32_t cost = inventoryHandler_->estimateItemRepairCost(itemGuid);
    float discount = 0.0f;
    const auto player = getEntityManager().getEntity(playerGuid);
    const auto merchant = isVendorWindowOpen() ? getEntityManager().getEntity(getVendorItems().vendorGuid) : nullptr;
    if (player && merchant) {
        const uint16_t ft = fieldIndex(UF::UNIT_FIELD_FACTIONTEMPLATE);
        auto* am = services_.assetManager;
        auto templates = am && am->isInitialized() ? am->loadDBCOptional("FactionTemplate.dbc") : nullptr;
        auto factions = am && am->isInitialized() ? am->loadDBCOptional("Faction.dbc") : nullptr;
        const int32_t tr = templates && templates->isLoaded() && ft != 0xFFFF
            ? templates->findRecordById(merchant->getField(ft)) : -1;
        const int32_t fr = tr >= 0 && factions && factions->isLoaded()
            ? factions->findRecordById(templates->getUInt32(static_cast<uint32_t>(tr), 1)) : -1;
        if (fr >= 0 && static_cast<int32_t>(factions->getUInt32(static_cast<uint32_t>(fr), 1)) >= 0) {
            switch (unitReaction(*merchant, *player)) {
                case 4: discount = 0.05f; break;
                case 5: discount = 0.10f; break;
                case 6: discount = 0.15f; break;
                case 7: discount = 0.20f; break;
                default: break;
            }
        }
    }
    return static_cast<uint32_t>(std::lround(static_cast<float>(cost) * (1.0f - discount)));
}

void GameHandler::sellItem(uint64_t vendorGuid, uint64_t itemGuid, uint32_t count) {
    if (inventoryHandler_) inventoryHandler_->sellItem(vendorGuid, itemGuid, count);
}

void GameHandler::sellItemBySlot(int backpackIndex) {
    if (inventoryHandler_) inventoryHandler_->sellItemBySlot(backpackIndex);
}

void GameHandler::equipItemToSlot(uint64_t itemGuid, uint8_t equipSlot, bool confirmed) {
    if (inventoryHandler_) inventoryHandler_->equipItemToSlot(itemGuid, equipSlot, confirmed);
}

void GameHandler::autoEquipItemBySlot(int backpackIndex, bool confirmed) {
    if (inventoryHandler_) inventoryHandler_->autoEquipItemBySlot(backpackIndex, confirmed);
}

void GameHandler::setAmmo(uint32_t itemId) {
    if (inventoryHandler_) inventoryHandler_->setAmmo(itemId);
}

uint32_t GameHandler::getAmmoId() const {
    const uint16_t idx = fieldIndex(UF::PLAYER_AMMO_ID);
    const auto player = getEntityManager().getEntity(playerGuid);
    return player && idx != 0xFFFF ? player->getField(idx) : 0u;
}

bool GameHandler::equipWouldBindFromBackpack(int backpackIndex) const {
    return inventoryHandler_ && inventoryHandler_->equipWouldBindFromBackpack(backpackIndex);
}

bool GameHandler::equipWouldBindFromBag(int bagIndex, int slotIndex) const {
    return inventoryHandler_ && inventoryHandler_->equipWouldBindFromBag(bagIndex, slotIndex);
}

void GameHandler::equipPendingItem() {
    if (inventoryHandler_) inventoryHandler_->equipPendingItem();
}

void GameHandler::cancelPendingEquip() {
    if (inventoryHandler_) inventoryHandler_->cancelPendingEquip();
}

void GameHandler::autoEquipItemInBag(int bagIndex, int slotIndex, bool confirmed) {
    if (inventoryHandler_) inventoryHandler_->autoEquipItemInBag(bagIndex, slotIndex, confirmed);
}

void GameHandler::sellItemInBag(int bagIndex, int slotIndex) {
    if (inventoryHandler_) inventoryHandler_->sellItemInBag(bagIndex, slotIndex);
}

void GameHandler::unequipToBackpack(EquipSlot equipSlot) {
    if (inventoryHandler_) inventoryHandler_->unequipToBackpack(equipSlot);
}

void GameHandler::swapContainerItems(uint8_t srcBag, uint8_t srcSlot, uint8_t dstBag,
                                     uint8_t dstSlot, bool applyLocally, bool confirmed) {
    if (inventoryHandler_) {
        inventoryHandler_->swapContainerItems(srcBag, srcSlot, dstBag, dstSlot, applyLocally,
                                              confirmed);
    }
}

void GameHandler::notifyBagsChanged() {
    if (inventoryHandler_) inventoryHandler_->fireBagUpdates();
}

void GameHandler::swapBagSlots(int srcBagIndex, int dstBagIndex) {
    if (inventoryHandler_) inventoryHandler_->swapBagSlots(srcBagIndex, dstBagIndex);
}

void GameHandler::destroyItem(uint8_t bag, uint8_t slot, uint8_t count) {
    if (inventoryHandler_) inventoryHandler_->destroyItem(bag, slot, count);
}

void GameHandler::splitItem(uint8_t srcBag, uint8_t srcSlot, uint8_t count) {
    if (inventoryHandler_) inventoryHandler_->splitItem(srcBag, srcSlot, count);
}
void GameHandler::splitItemTo(uint8_t srcBag, uint8_t srcSlot,
                              uint8_t dstBag, uint8_t dstSlot, uint8_t count) {
    if (inventoryHandler_) inventoryHandler_->splitItemTo(srcBag, srcSlot, dstBag, dstSlot, count);
}

void GameHandler::useItemBySlot(int backpackIndex, bool confirmed, uint64_t unitTarget) {
    if (inventoryHandler_) inventoryHandler_->useItemBySlot(backpackIndex, confirmed, unitTarget);
}

void GameHandler::useKeyringItem(int index, bool confirmed) {
    if (inventoryHandler_) inventoryHandler_->useKeyringItem(index, confirmed);
}

bool GameHandler::isAwaitingGlyphTarget() const {
    return inventoryHandler_ && inventoryHandler_->isAwaitingGlyphTarget();
}

uint32_t GameHandler::getPendingGlyphSpellId() const {
    return inventoryHandler_ ? inventoryHandler_->getPendingGlyphSpellId() : 0;
}

void GameHandler::cancelGlyphTargeting() {
    if (inventoryHandler_) inventoryHandler_->cancelGlyphTargeting();
}

bool GameHandler::placePendingGlyph(uint32_t socketIndex) {
    return inventoryHandler_ && inventoryHandler_->placePendingGlyph(socketIndex);
}

void GameHandler::useItemInBag(int bagIndex, int slotIndex, bool confirmed,
                               uint64_t unitTarget) {
    if (inventoryHandler_) inventoryHandler_->useItemInBag(bagIndex, slotIndex, confirmed,
                                                           unitTarget);
}

bool GameHandler::isAwaitingItemTarget() const {
    return inventoryHandler_ && inventoryHandler_->isAwaitingItemTarget();
}

bool GameHandler::isAwaitingUnitTarget() const {
    return inventoryHandler_ && inventoryHandler_->isAwaitingUnitTarget();
}

uint32_t GameHandler::getPendingUnitTargetSourceItemId() const {
    return inventoryHandler_ ? inventoryHandler_->getPendingUnitTargetSourceItemId() : 0;
}

void GameHandler::cancelUnitTargeting() {
    if (inventoryHandler_) inventoryHandler_->cancelUnitTargeting();
}

void GameHandler::completeItemUseOnUnit(uint64_t targetUnitGuid) {
    if (inventoryHandler_) inventoryHandler_->completeItemUseOnUnit(targetUnitGuid);
}

void GameHandler::beginSpellItemTargeting(uint32_t spellId, const std::string& spellName) {
    if (inventoryHandler_) inventoryHandler_->beginSpellItemTargeting(spellId, spellName);
}

uint32_t GameHandler::getPendingItemTargetSourceItemId() const {
    return inventoryHandler_ ? inventoryHandler_->getPendingItemTargetSourceItemId() : 0;
}

void GameHandler::cancelItemTargeting() {
    if (inventoryHandler_) inventoryHandler_->cancelItemTargeting();
}

void GameHandler::completeItemUseOnItem(uint64_t targetItemGuid, bool confirmed) {
    if (inventoryHandler_) inventoryHandler_->completeItemUseOnItem(targetItemGuid, confirmed);
}

int GameHandler::getInstanceBootTimeRemaining() const {
    if (!instanceBootDeadline_) return 0;
    const auto left = *instanceBootDeadline_ - std::chrono::steady_clock::now();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(left).count();
    return secs > 0 ? static_cast<int>(secs) : 0;
}

void GameHandler::replaceEnchant() {
    if (inventoryHandler_) inventoryHandler_->replaceEnchant();
}

void GameHandler::openItemBySlot(int backpackIndex) {
    if (inventoryHandler_) inventoryHandler_->openItemBySlot(backpackIndex);
}

void GameHandler::openItemInBag(int bagIndex, int slotIndex) {
    if (inventoryHandler_) inventoryHandler_->openItemInBag(bagIndex, slotIndex);
}

void GameHandler::readItemBySlot(int backpackIndex) {
    if (inventoryHandler_) inventoryHandler_->readItemBySlot(backpackIndex);
}

void GameHandler::readItemInBag(int bagIndex, int slotIndex) {
    if (inventoryHandler_) inventoryHandler_->readItemInBag(bagIndex, slotIndex);
}

void GameHandler::useItemById(uint32_t itemId, uint64_t unitTarget) {
    if (inventoryHandler_) inventoryHandler_->useItemById(itemId, unitTarget);
}

uint32_t GameHandler::getItemIdForSpell(uint32_t spellId) const {
    if (spellId == 0) return 0;
    // Search backpack and bags for an item whose on-use spell matches
    for (int i = 0; i < inventory.getBackpackSize(); i++) {
        const auto& slot = inventory.getBackpackSlot(i);
        if (slot.empty()) continue;
        auto* info = getItemInfo(slot.item.itemId);
        if (!info || !info->valid) continue;
        for (const auto& sp : info->spells) {
            if (sp.spellId == spellId && (sp.spellTrigger == 0 || sp.spellTrigger == 5))
                return slot.item.itemId;
        }
    }
    for (int bag = 0; bag < inventory.NUM_BAG_SLOTS; bag++) {
        for (int s = 0; s < inventory.getBagSize(bag); s++) {
            const auto& slot = inventory.getBagSlot(bag, s);
            if (slot.empty()) continue;
            auto* info = getItemInfo(slot.item.itemId);
            if (!info || !info->valid) continue;
            for (const auto& sp : info->spells) {
                if (sp.spellId == spellId && (sp.spellTrigger == 0 || sp.spellTrigger == 5))
                    return slot.item.itemId;
            }
        }
    }
    return 0;
}

void GameHandler::unstuck() {
    if (unstuckCallback_) {
        unstuckCallback_();
        addSystemChatMessage("Unstuck: snapped upward. Use /unstuckgy for full teleport.");
    }
}

void GameHandler::unstuckGy() {
    if (unstuckGyCallback_) {
        unstuckGyCallback_();
        addSystemChatMessage("Unstuck: teleported to safe location.");
    }
}

void GameHandler::unstuckHearth() {
    if (unstuckHearthCallback_) {
        unstuckHearthCallback_();
        addSystemChatMessage("Unstuck: teleported to hearthstone location.");
    } else {
        addSystemChatMessage("No hearthstone bind point set.");
    }
}

// ============================================================
// Trainer
// ============================================================

void GameHandler::trainSpell(uint32_t spellId) {
    if (inventoryHandler_) inventoryHandler_->trainSpell(spellId);
}

void GameHandler::closeTrainer() {
    if (inventoryHandler_) inventoryHandler_->closeTrainer();
}

void GameHandler::preloadDBCCaches() const {
    LOG_INFO("Pre-loading DBC caches during world entry...");
    auto t0 = std::chrono::steady_clock::now();

    loadSpellNameCache();   // Spell.dbc - largest, ~170ms cold
    loadTitleNameCache();   // CharTitles.dbc
    loadFactionNameCache(); // Faction.dbc
    loadAreaNameCache();    // WorldMapArea.dbc
    loadMapNameCache();     // Map.dbc
    loadLfgDungeonDbc();    // LFGDungeons.dbc

    // Validate animation constants against AnimationData.dbc
    if (auto* am = services_.assetManager) {
        auto animDbc = am->loadDBC("AnimationData.dbc");
        rendering::anim::validateAgainstDBC(animDbc);
        // The rows the renderer walks for what a model lacks (0x00826350,
        // 0x007176f0).
        if (animDbc) rendering::anim::setAnimationData(rendering::anim::loadAnimationData(*animDbc));
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    LOG_INFO("DBC cache pre-load complete in ", elapsed, " ms");
}

void GameHandler::loadSpellNameCache() const {
    if (spellHandler_) spellHandler_->loadSpellNameCache();
}

void GameHandler::loadSkillLineAbilityDbc() {
    if (spellHandler_) spellHandler_->loadSkillLineAbilityDbc();
}

const std::vector<GameHandler::SpellBookTab>& GameHandler::getSpellBookTabs() {
    static const std::vector<SpellBookTab> kEmpty;
    if (spellHandler_) return spellHandler_->getSpellBookTabs();
    return kEmpty;
}


void GameHandler::loadTalentDbc() {
    if (spellHandler_) spellHandler_->loadTalentDbc();
}

static const std::string EMPTY_STRING;

const int32_t* GameHandler::getSpellEffectBasePoints(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellEffectBasePoints(spellId);
    return nullptr;
}

float GameHandler::getSpellDuration(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellDuration(spellId);
    return 0.0f;
}

const std::string& GameHandler::getSpellName(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellName(spellId);
    return EMPTY_STRING;
}

uint32_t GameHandler::getSpellTargetFlags(uint32_t spellId) const {
    return spellHandler_ ? spellHandler_->getSpellTargetFlags(spellId) : 0;
}

uint32_t GameHandler::getSpellImplicitTargetA(uint32_t spellId) const {
    return spellHandler_ ? spellHandler_->getSpellImplicitTargetA(spellId) : 0;
}

bool GameHandler::isSpellKnownToClient(uint32_t spellId) const {
    return spellHandler_ && spellHandler_->isSpellKnownToClient(spellId);
}

uint32_t GameHandler::getSpellTargetAuraState(uint32_t spellId) const {
    return spellHandler_ ? spellHandler_->getSpellTargetAuraState(spellId) : 0;
}

const std::string& GameHandler::getSpellRank(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellRank(spellId);
    return EMPTY_STRING;
}

const std::string& GameHandler::getSpellDescription(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellDescription(spellId);
    return EMPTY_STRING;
}

namespace {
// Format a duration in seconds the way WoW tooltips do: "12 sec", "5 min", "1 hour".
std::string formatSpellDurationSec(float sec) {
    if (sec <= 0.0f) return {};
    auto trim = [](float v) {
        char buf[32];
        if (v == static_cast<float>(static_cast<long>(v)))
            snprintf(buf, sizeof(buf), "%ld", static_cast<long>(v));
        else
            snprintf(buf, sizeof(buf), "%.1f", v);
        return std::string(buf);
    };
    if (sec >= 3600.0f && std::fmod(sec, 3600.0f) == 0.0f) {
        float h = sec / 3600.0f;
        return trim(h) + (h == 1.0f ? " hour" : " hours");
    }
    if (sec >= 60.0f) return trim(sec / 60.0f) + " min";
    return trim(sec) + " sec";
}
} // namespace

/// The evaluator's window onto this client: what a magnitude is, and which
/// spells the player knows.
SpellDescriptionContext GameHandler::descriptionContext(
        uint32_t spellId, const std::string* declarations) const {
    SpellDescriptionContext ctx;
    ctx.declarations = declarations;
    ctx.magnitude = [this, spellId](int index, double& out) {
        const int32_t* bp = getSpellEffectBasePoints(spellId);
        if (!bp || index < 0 || index > 2) return false;
        out = std::abs(static_cast<double>(bp[index]) + 1.0);
        return true;
    };
    ctx.knowsSpell = [this](uint32_t id) { return isSpellKnownToClient(id); };
    return ctx;
}

std::string GameHandler::formatSpellDescription(uint32_t selfSpellId,
                                                const std::string& raw) const {
    if (raw.empty()) return raw;

    std::string out;
    out.reserve(raw.size());
    const size_t n = raw.size();
    long lastValue = -1;  // drives $l plural selection

    // Resolve a base-point magnitude ($s/$o/$m/$M) for a spell + effect index.
    auto basePoints = [&](uint32_t sid, int idx) -> long {
        const int32_t* bp = getSpellEffectBasePoints(sid);
        if (!bp || idx < 0 || idx > 2) return LONG_MIN;
        return std::labs(static_cast<long>(bp[idx]) + 1);  // stored as value-1
    };

    for (size_t i = 0; i < n; ) {
        char c = raw[i];
        if (c != '$') { out += c; ++i; continue; }
        if (i + 1 >= n) { out += c; ++i; continue; }

        char next = raw[i + 1];

        // Plural "$lsingular:plural;" / gender "$gmale:female;" - emit one branch.
        if (next == 'l' || next == 'L' || next == 'g' || next == 'G') {
            size_t semi = raw.find(';', i + 2);
            if (semi != std::string::npos) {
                std::string body = raw.substr(i + 2, semi - (i + 2));
                size_t colon = body.find(':');
                std::string a = (colon == std::string::npos) ? body : body.substr(0, colon);
                std::string b = (colon == std::string::npos) ? body : body.substr(colon + 1);
                bool plural = (next == 'l' || next == 'L') ? (lastValue != 1) : false;
                out += plural ? b : a;
                i = semi + 1;
                continue;
            }
        }

        // A named variable "$<percent>", declared in this spell's row of
        // SpellDescriptionVariables.dbc. Nothing read that table, and the
        // parser had no branch for '<' - so it dropped the "$<" as an unknown
        // token and left "percent>" sitting in the sentence.
        if (next == '<') {
            const size_t close = raw.find('>', i + 2);
            if (close != std::string::npos) {
                const std::string name = raw.substr(i + 2, close - (i + 2));
                i = close + 1;

                const auto& cache = const_cast<GameHandler*>(this)->spellNameCacheRef();
                const auto entry = cache.find(selfSpellId);
                const std::string* decls = nullptr;
                if (entry != cache.end() && entry->second.descriptionVariableId != 0) {
                    auto& table = const_cast<GameHandler*>(this)->spellDescriptionVariablesRef();
                    const auto row = table.find(entry->second.descriptionVariableId);
                    if (row != table.end()) decls = &row->second;
                }

                const SpellDescriptionContext ctx = descriptionContext(selfSpellId, decls);
                std::string decl;
                double v = 0.0;
                if (decls && spellDeclarationOf(ctx, name, decl) &&
                    evaluateSpellExpression(decl, ctx, v)) {
                    const long rounded = std::lround(v);
                    out += std::to_string(rounded);
                    lastValue = rounded;
                } else if (i < n && raw[i] == '%') {
                    // Unresolvable: drop the token and the percent sign it
                    // qualified, rather than leave a bare "%" behind.
                    ++i;
                }
                continue;
            }
        }

        // Bracketed math expression "${...}" - evaluate where it can be, strip
        // it (and a trailing %) where it cannot.
        if (next == '{') {
            size_t close = raw.find('}', i + 2);
            if (close != std::string::npos) {
                const auto& cache = const_cast<GameHandler*>(this)->spellNameCacheRef();
                const auto entry = cache.find(selfSpellId);
                const std::string* decls = nullptr;
                if (entry != cache.end() && entry->second.descriptionVariableId != 0) {
                    auto& table = const_cast<GameHandler*>(this)->spellDescriptionVariablesRef();
                    const auto row = table.find(entry->second.descriptionVariableId);
                    if (row != table.end()) decls = &row->second;
                }
                const SpellDescriptionContext ctx = descriptionContext(selfSpellId, decls);
                double v = 0.0;
                if (evaluateSpellExpression(raw.substr(i + 2, close - (i + 2)), ctx, v)) {
                    const long rounded = std::lround(v);
                    out += std::to_string(rounded);
                    lastValue = rounded;
                    i = close + 1;
                    continue;
                }
                i = close + 1;
                if (i < n && raw[i] == '%') ++i;
                continue;
            }
        }

        // Division token "$/N;<valueToken>" - e.g. "$/5;s1" is "s1 divided by 5"
        // (food eat spells: "Restores $/5;s1 health per second").
        if (next == '/') {
            size_t k = i + 2;
            long divisor = 0;
            while (k < n && raw[k] >= '0' && raw[k] <= '9') { divisor = divisor * 10 + (raw[k] - '0'); ++k; }
            if (k < n && raw[k] == ';' && divisor != 0) {
                ++k;  // past ';'
                uint32_t rid = 0; bool hr = false;
                while (k < n && raw[k] >= '0' && raw[k] <= '9') { rid = rid * 10 + (raw[k] - '0'); hr = true; ++k; }
                if (k < n) {
                    char cd = raw[k++];
                    int ix = 0;
                    if (k < n && raw[k] >= '1' && raw[k] <= '3') { ix = raw[k] - '1'; ++k; }
                    if (cd == 's' || cd == 'S' || cd == 'o' || cd == 'O' || cd == 'm' || cd == 'M') {
                        long v = basePoints(hr ? rid : selfSpellId, ix);
                        if (v != LONG_MIN) {
                            long dv = v / divisor;
                            out += std::to_string(dv);
                            lastValue = dv;
                            i = k;
                            continue;
                        }
                    }
                }
            }
            // Unparseable division token - strip through the terminating ';' if present.
            size_t semi = raw.find(';', i + 2);
            i = (semi != std::string::npos) ? semi + 1 : i + 2;
            if (i < n && raw[i] == '%') ++i;
            continue;
        }

        // General token: $ [spellId] letter [effectIndex]
        size_t j = i + 1;
        uint32_t refId = 0;
        bool hasRef = false;
        while (j < n && raw[j] >= '0' && raw[j] <= '9') {
            refId = refId * 10 + static_cast<uint32_t>(raw[j] - '0');
            hasRef = true;
            ++j;
        }
        if (j >= n) { out += c; ++i; continue; }
        char code = raw[j++];
        int idx = 0;
        if (j < n && raw[j] >= '1' && raw[j] <= '3') { idx = raw[j] - '1'; ++j; }
        uint32_t target = hasRef ? refId : selfSpellId;

        bool resolved = false;
        switch (code) {
            case 's': case 'S': case 'o': case 'O':
            case 'm': case 'M': {
                long v = basePoints(target, idx);
                if (v != LONG_MIN) { out += std::to_string(v); lastValue = v; resolved = true; }
                break;
            }
            case 'd': {
                std::string dur = formatSpellDurationSec(getSpellDuration(target));
                if (!dur.empty()) { out += dur; resolved = true; }
                break;
            }
            case 'z': {
                // Where the hearthstone sends you. Spell.dbc says "Returns you
                // to $z.", and with the token dropped the tooltip read
                // "Returns you to ." - which is the one fact the line exists
                // to carry. The bind zone comes from SMSG_BINDPOINTUPDATE; the
                // continent is a poor second but beats a blank.
                std::string home = getAreaName(getHomeBindZoneId());
                if (home.empty()) {
                    uint32_t bindMap = 0;
                    glm::vec3 bindPos(0.0f);
                    if (getHomeBind(bindMap, bindPos)) home = getMapName(bindMap);
                }
                if (!home.empty()) { out += home; resolved = true; }
                break;
            }
            default: break;  // $h proc chance, $t period, $a radius, ... - not resolvable here
        }

        if (resolved) {
            i = j;
        } else {
            // Drop the whole token; a value slot immediately followed by '%' drops that too,
            // so we don't leave a dangling "%".
            i = j;
            if (i < n && raw[i] == '%') ++i;
        }
    }

    // Collapse whitespace left behind by stripped tokens ("a  chance" -> "a chance").
    std::string cleaned;
    cleaned.reserve(out.size());
    bool prevSpace = false;
    for (char c : out) {
        bool isSpace = (c == ' ');
        if (isSpace && prevSpace) continue;
        // A token that could not be resolved leaves the space in front of it
        // sitting against the punctuation that followed it - "Returns you to ."
        // Drop that space too, so an unresolved token reads as a gap in the
        // sentence rather than as a typo.
        if (!cleaned.empty() && prevSpace &&
            (c == '.' || c == ',' || c == ';' || c == '!' || c == '?' || c == ')')) {
            cleaned.pop_back();
        }
        cleaned += c;
        prevSpace = isSpace;
    }
    return cleaned;
}

const std::string* GameHandler::getSpellMechanicName(uint32_t mechanicId) const {
    return spellHandler_ ? spellHandler_->getSpellMechanicName(mechanicId) : nullptr;
}

uint32_t GameHandler::playerAuraMechanic(std::initializer_list<uint32_t> auraTypes) const {
    return spellHandler_ ? spellHandler_->playerAuraMechanic(auraTypes) : 0;
}

const std::string& GameHandler::getSpellFocusName(uint32_t focusId) const {
    if (spellHandler_) return spellHandler_->getSpellFocusName(focusId);
    return EMPTY_STRING;
}

const std::string& GameHandler::getTotemCategoryName(uint32_t categoryId) const {
    if (spellHandler_) return spellHandler_->getTotemCategoryName(categoryId);
    return EMPTY_STRING;
}

bool GameHandler::enchantSoulbinds(uint32_t enchantId) const {
    return spellHandler_ && spellHandler_->enchantSoulbinds(enchantId);
}

std::string GameHandler::getEnchantName(uint32_t enchantId) const {
    if (spellHandler_) return spellHandler_->getEnchantName(enchantId);
    return {};
}

uint32_t GameHandler::getEnchantGemItem(uint32_t enchantId) const {
    if (spellHandler_) return spellHandler_->getEnchantGemItem(enchantId);
    return 0;
}

uint8_t GameHandler::getSpellDispelType(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellDispelType(spellId);
    return 0;
}

bool GameHandler::isSpellInterruptible(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->isSpellInterruptible(spellId);
    return true;
}

bool GameHandler::isSpellPassive(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->isSpellPassive(spellId);
    return false;
}

std::optional<uint32_t> GameHandler::getSpellAttributes(uint32_t spellId) const {
    if (spellId == 0) return std::nullopt;
    loadSpellNameCache();
    auto it = spellNameCache_.find(spellId);
    if (it == spellNameCache_.end()) return std::nullopt;
    return it->second.attr;
}

uint32_t GameHandler::getSpellVisualId(uint32_t spellId) const {
    if (spellId == 0) return 0;
    loadSpellNameCache();
    auto it = spellNameCache_.find(spellId);
    if (it == spellNameCache_.end()) return 0;
    // 0x007fa290: the second where violenceLevel is under 2.
    return spell_visual::choose(
        it->second.spellVisualId, it->second.spellVisualId2,
        spell_visual::violenceLevel(addons::storedCVarValue("violenceLevel",
                                                            std::to_string(spell_visual::kViolenceLevelLimit))));
}

uint64_t GameHandler::listenerObject() const {
    auto& entities = entityController_->getEntityManager();
    uint64_t object = 0;
    if (const uint64_t mover = activeMover(); mover != 0) {
        if (auto unit = entities.getEntity(mover); unit && unit->isUnit()) object = mover;
    }
    if (object == 0 && entities.getEntity(playerGuid)) object = playerGuid;
    if (object == 0) return 0;
    // 0x004f7310 answers for the active player only.
    if (object == playerGuid && movementHandler_ && movementHandler_->farSightAnnounced()) {
        if (auto player = entities.getEntity(playerGuid)) {
            const uint16_t field = fieldIndex(UF::PLAYER_FARSIGHT);
            if (field != 0xFFFF) {
                const uint64_t farsight =
                    (static_cast<uint64_t>(player->getField(static_cast<uint16_t>(field + 1))) << 32) |
                    player->getField(field);
                if (farsight != 0 && entities.getEntity(farsight)) return farsight;
            }
        }
    }
    return object;
}

cast_visual::Spell GameHandler::castVisualSpell(uint32_t spellId) const {
    cast_visual::Spell spell;
    loadSpellNameCache();
    const auto it = spellNameCache_.find(spellId);
    if (it == spellNameCache_.end()) return spell;
    spell.attributes = it->second.attr;
    spell.attributesEx2 = it->second.attrEx2;
    spell.attributesEx4 = it->second.attrEx4;
    // 0x007fa2e0: the SpellVisual it shows (vfunc +0xbc, 0x007fa290), its
    // PrecastKit (+0x4), the kit's StartAnimID (+0x4) or AnimID (+0x8)
    // above 0.
    const uint32_t visualId = getSpellVisualId(spellId);
    if (visualId == 0) return spell;
    const auto known = precastKitAnimates_.find(visualId);
    if (known != precastKitAnimates_.end()) {
        spell.precastKitAnimates = known->second;
        return spell;
    }
    bool animates = false;
    auto* am = services_.assetManager;
    auto visuals = am && am->isInitialized() ? am->loadDBCOptional("SpellVisual.dbc") : nullptr;
    auto kits = am && am->isInitialized() ? am->loadDBCOptional("SpellVisualKit.dbc") : nullptr;
    const int32_t visualRow = visuals && visuals->getFieldCount() > 1 ? visuals->findRecordById(visualId) : -1;
    if (visualRow >= 0 && kits && kits->getFieldCount() > 2) {
        const uint32_t kitId = visuals->getUInt32(static_cast<uint32_t>(visualRow), 1);
        const int32_t kitRow = kitId != 0 ? kits->findRecordById(kitId) : -1;
        if (kitRow >= 0) {
            animates = kits->getInt32(static_cast<uint32_t>(kitRow), 1) > 0 ||
                       kits->getInt32(static_cast<uint32_t>(kitRow), 2) > 0;
        }
    }
    precastKitAnimates_[visualId] = animates;
    spell.precastKitAnimates = animates;
    return spell;
}

bool GameHandler::isSpellMissileInFlight(uint64_t caster, uint32_t spellId, uint32_t castTime) const {
    return spellHandler_ && spellHandler_->isMissileInFlight(caster, spellId, castTime);
}

uint32_t GameHandler::getSpellMountCreature(uint32_t spellId) const {
    if (spellId == 0) return 0;
    loadSpellNameCache();
    auto it = spellNameCache_.find(spellId);
    if (it == spellNameCache_.end()) return 0;
    for (int i = 0; i < 3; ++i) {
        if (it->second.effectIds[i] == rendering::mount_transition::kEffectApplyAura &&
            it->second.effectAuraIds[i] == rendering::mount_transition::kAuraMounted)
            return it->second.effectMiscValues[i];
    }
    return 0;
}

bool GameHandler::spellAppliesMountedAura(uint32_t spellId) const {
    if (spellId == 0) return false;
    loadSpellNameCache();
    auto it = spellNameCache_.find(spellId);
    if (it == spellNameCache_.end()) return false;
    for (int i = 0; i < 3; ++i) {
        if (it->second.effectAuraIds[i] == rendering::mount_transition::kAuraMounted) return true;
    }
    return false;
}

uint32_t GameHandler::spellRequiredVision(uint32_t spellId) const {
    if (spellId == 0) return 0;
    loadSpellNameCache();
    auto it = spellNameCache_.find(spellId);
    if (it == spellNameCache_.end()) return 0;
    return aura_vision::requiredVision(it->second.requiredAuraVision, it->second.effectAuraIds,
                                       it->second.effectMiscValues);
}

uint32_t GameHandler::playerAuraVision() const {
    const uint16_t i = fieldIndex(UF::PLAYER_FIELD_BYTES2);
    auto player = getEntityManager().getEntity(getPlayerGuid());
    if (i == 0xFFFF || !player) return 0;
    return (player->getField(i) >> 24) & 0xFFu;
}

bool GameHandler::spellAppliesAura(uint32_t spellId, uint32_t auraType) const {
    // 0x007fde50: any of the three effects' EffectApplyAuraName.
    if (spellId == 0) return false;
    loadSpellNameCache();
    auto it = spellNameCache_.find(spellId);
    if (it == spellNameCache_.end()) return false;
    for (int i = 0; i < 3; ++i) {
        if (it->second.effectAuraIds[i] == auraType) return true;
    }
    return false;
}

bool GameHandler::spellShapeshiftsInto(uint32_t spellId, uint8_t form) const {
    if (spellId == 0) return false;
    loadSpellNameCache();
    auto it = spellNameCache_.find(spellId);
    if (it == spellNameCache_.end()) return false;
    for (int i = 0; i < 3; ++i) {
        if (it->second.effectAuraIds[i] == kAuraModShapeshift && it->second.effectMiscValues[i] == form) return true;
    }
    return false;
}

FormAuraCancel GameHandler::formAuraCancelFor(uint32_t spellId) const {
    loadSpellNameCache();
    auto it = spellNameCache_.find(spellId);
    if (it == spellNameCache_.end()) return FormAuraCancel::Send;
    return formAuraCancel(getShapeshiftFormId(), it->second.effectAuraIds, it->second.effectMiscValues,
                          [this](uint8_t form) { return shapeshiftFormRow(form); });
}

uint32_t GameHandler::getSpellTargetKind(uint32_t spellId) const {
    if (spellId == 0) return 0;
    loadSpellNameCache();
    auto it = spellNameCache_.find(spellId);
    if (it == spellNameCache_.end()) return 0;
    const auto& e = it->second;
    return spellTargetKind(e.targetFlags,
                           {e.implicitTargetsA[0], e.implicitTargetsA[1], e.implicitTargetsA[2]},
                           {e.implicitTargetsB[0], e.implicitTargetsB[1], e.implicitTargetsB[2]},
                           {e.effectAuraIds[0], e.effectAuraIds[1], e.effectAuraIds[2]});
}

uint32_t GameHandler::getSpellSchoolMask(uint32_t spellId) const {
    if (spellHandler_) return spellHandler_->getSpellSchoolMask(spellId);
    return 0;
}

const std::string& GameHandler::getSkillLineName(uint32_t skillLineId) const {
    if (spellHandler_) return spellHandler_->getSkillLineName(skillLineId);
    return EMPTY_STRING;
}


void GameHandler::handleCalendarPacket(CalendarPacket which, network::Packet& packet) {
    CalendarContext ctx{calendarData_, calendarEventDetail_};
    ctx.player = playerGuid;
    ctx.newEventOpen = calendarDraftOpen_;
    ctx.playerLevel = static_cast<uint8_t>(serverPlayerLevel_);
    ctx.isIgnored = [this](uint64_t guid) { return ignoreListGuids_.count(guid) != 0; };
    ctx.nameKnown = [this](uint64_t guid) {
        return guid == playerGuid || !lookupName(guid).empty();
    };
    if (!applyCalendarPacket(which, packet, ctx)) {
        LOG_WARNING("Calendar packet ", static_cast<int>(which),
                    " could not be read whole; the calendar is left as it was");
        packet.skipAll();
        return;
    }
    packet.skipAll();
    // An event sent in full is the open one now, in place of a new one
    // (0x005bed20).
    if (which == CalendarPacket::SendEvent) calendarDraftOpen_ = false;
    // Every path that stores an event record re-arms that record's
    // fifteen-minutes-before reminder (0x005b79b0, called by the calendar list,
    // by an event added and by an event updated - and by the last two with the
    // status byte carried over from the record being replaced, which is how an
    // event answered since the calendar was last read still gets one).
    armCalendarAlarms();
    // The list ordered before it is announced, once every name is in: an
    // event sent in full, or a guild's or arena team's list, by status
    // (0x005f2c00, 0x005f2f90: 0x005f1980(3, 0)); an invitee added, by the
    // order kept (0x005f32b0).
    const auto says = [&ctx](const char* event) {
        return std::any_of(ctx.signals.begin(), ctx.signals.end(),
                           [event](const CalendarSignal& s) { return s.event == event; });
    };
    if ((which == CalendarPacket::SendEvent &&
         (says("CALENDAR_OPEN_EVENT") || says("CALENDAR_NEW_EVENT"))) ||
        ((which == CalendarPacket::FilterGuild || which == CalendarPacket::ArenaTeam) &&
         says("CALENDAR_UPDATE_INVITE_LIST"))) {
        calendarSortInvites(calendarEventDetail_, 3, false, calendarSortKeys(*this));
    } else if (which == CalendarPacket::EventInvite && says("CALENDAR_UPDATE_INVITE_LIST")) {
        calendarSortInvites(calendarEventDetail_, calendarEventDetail_.inviteSortCriterion, false,
                            calendarSortKeys(*this));
    }
    for (const auto& signal : ctx.signals) {
        // The dispatcher's ending clears the pending action as it says so.
        if (signal.event == "CALENDAR_ACTION_PENDING") {
            setCalendarActionPending(false);
            continue;
        }
        // A removed invitation that leaves a mail looks for it where 0x005c0ae0
        // does, between its event-list and pending-invite events (0x005704d0).
        if (signal.event == kCalendarCheckMail) {
            if (inventoryHandler_) inventoryHandler_->checkForMail(0.0f);
            continue;
        }
        fireAddonEvent(signal.event, signal.args);
    }
    for (uint64_t guid : ctx.nameQueries) queryPlayerName(guid);
    // 0x005bbbb0: an alert about an event the calendar has not loaded asks
    // for the pending count instead.
    if (ctx.askNumPending && isInWorld() && socket) {
        network::Packet ask(wireOpcode(Opcode::CMSG_CALENDAR_GET_NUM_PENDING));
        socket->send(ask);
    }
}

void GameHandler::armCalendarAlarms() {
    const std::optional<WowDate> now = getGameDate();
    // No game date, nothing to arm against: the times the calendar sends are
    // the server's own, and without a clock of the server's they cannot be
    // placed at all. The same holds for what is still ahead in the calendar
    // (calendarInviteState, the pending invites).
    calendarAlarms_ = calendarArmAlarms(calendarData_, playerGuid, now ? &*now : nullptr);
}

void GameHandler::sayCalendarAlarms(const WowDate& now) {
    const int64_t nowKey = calendarMinuteKey(now);
    for (auto it = calendarAlarms_.begin(); it != calendarAlarms_.end();) {
        const int64_t dueKey = calendarMinuteKey(it->at);
        if (dueKey > nowKey) { ++it; continue; }
        if (dueKey == nowKey) {
            // 0x005b7910. The title is the row's own; the two numbers are the
            // event's hour and minute, which is what the interface calls them
            // and what it ignores - the words, with the fifteen minutes in
            // them, are FrameXML's (CALENDAR_EVENT_ALARM_MESSAGE, printed by
            // gametime.lua:175 into the chat frame in the system colour).
            fireAddonEvent("CALENDAR_EVENT_ALARM",
                           {it->title, std::to_string(it->eventTime.hour),
                            std::to_string(it->eventTime.minute)});
        }
        // Past that minute, it is not said late. The client's scheduler matches
        // the moment rather than passing it (0x0076c890 compares two time
        // patterns field by field, a negative field being a wildcard), so an
        // alarm armed for a moment already gone - a calendar opened ten minutes
        // before its event, say - stays silent for good rather than printing
        // "begins in 15 minutes" about an event already under way.
        it = calendarAlarms_.erase(it);
    }
}

void GameHandler::onCalendarNameKnown(uint64_t guid) {
    std::vector<CalendarSignal> said;
    calendarNameArrived(calendarEventDetail_, guid, said);
    // The last name in orders the list by the order kept (0x005f1a40).
    if (!said.empty()) {
        calendarSortInvites(calendarEventDetail_, calendarEventDetail_.inviteSortCriterion, false,
                            calendarSortKeys(*this));
    }
    for (const auto& signal : said) fireAddonEvent(signal.event, signal.args);
}

bool GameHandler::wouldAutoUnshift() const {
    auto me = getEntityManager().getEntity(getPlayerGuid());
    const auto* unit = dynamic_cast<const Unit*>(me.get());
    if (!unit || (unit->getUnitFlags() & 0x100000u) != 0) return false;
    if (addons::storedCVarValue("autoUnshift", "1") == "0") return false;
    if ((movementInfo.flags & 0x2000000u) != 0 && addons::storedCVarValue("autoDismountFlying", "0") == "0")
        return false;
    const auto* row = shapeshiftFormRow(getShapeshiftFormId());
    return !row || (row->flags & 0x100u) == 0;
}

void GameHandler::leaveShapeshiftForm() {
    if (getShapeshiftFormId() == 0) return;
    std::vector<uint32_t> spells;
    for (const auto& aura : getPlayerAuras()) {
        if ((aura.flags & 0x10) != 0) spells.push_back(aura.spellId);
    }
    // The form is read again for each: the first cancel leaves it.
    for (uint32_t spellId : spells) {
        const uint8_t form = getShapeshiftFormId();
        if (form != 0 && spellShapeshiftsInto(spellId, form)) cancelAura(spellId);
    }
}

} // namespace game
} // namespace wowee
