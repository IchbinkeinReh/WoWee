#include "game/group_defines.hpp"
#include "core/local_time.hpp"
#include "game/item_text.hpp"
#include "game/social_handler.hpp"
#include <string_view>
#include "addons/lua_api_registrations.hpp"
#include "ui/framexml_takeover.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/interface_text.hpp"
#include "game/entity.hpp"
#include "game/packet_parsers.hpp"
#include "game/update_field_table.hpp"
#include "game/opcode_table.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/ui_sound_manager.hpp"
#include "network/world_socket.hpp"
#include "rendering/renderer.hpp"
#include "core/logger.hpp"
#include "core/application.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

namespace wowee {
namespace game {

namespace {

std::filesystem::path guildNameCachePath() {
#ifdef _WIN32
    if (const char* appData = std::getenv("APPDATA")) {
        return std::filesystem::path(appData) / "wowee" / "guild_names.tsv";
    }
#else
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".wowee" / "guild_names.tsv";
    }
#endif
    return std::filesystem::path("guild_names.tsv");
}

// The three places 0x005216f0 sends a game error, as its table at 0x00ac8af0
// says for each one: a system line (0x00509dd0, CHAT_MSG_SYSTEM), the yellow
// UI_INFO_MESSAGE (event 0xbc) or the red UI_ERROR_MESSAGE (event 0xbb). The
// words are the interface's ERR_* string with the arguments put in.
enum class ErrTo { Chat, Info, Error };

void gameError(GameHandler& gh, ErrTo to, const char* key, const char* enUS,
               const std::vector<std::string>& args = {}) {
    const std::string text = interfaceFormat(key, enUS, args);
    if (text.empty()) return;
    switch (to) {
        case ErrTo::Chat:  gh.addSystemChatMessage(text); break;
        case ErrTo::Info:  gh.addUIInfoMessage(text); break;
        case ErrTo::Error: gh.addUIError(text); break;
    }
}

/// A duration in milliseconds as 0x0061a9e0 words it with the
/// INT_SPELL_DURATION prefix and rounding up: seconds under a minute, then
/// minutes, hours and days, sixty minutes reading as one hour and twenty-four
/// hours as one day.
std::string spellDurationText(uint64_t ms) {
    constexpr uint64_t kMinute = 60000, kHour = kMinute * 60, kDay = kHour * 24;
    const auto up = [](uint64_t v, uint64_t unit) { return v == 0 ? 0 : (v - 1) / unit + 1; };
    const char* key = "INT_SPELL_DURATION_SEC";
    const char* enUS = "%d sec";
    uint64_t n = ms / 1000;
    if (ms >= kDay) {
        n = up(ms, kDay); key = "INT_SPELL_DURATION_DAYS"; enUS = "%d |4day:days;";
    } else if (ms >= kHour) {
        n = up(ms, kHour);
        if (n == 24) { n = 1; key = "INT_SPELL_DURATION_DAYS"; enUS = "%d |4day:days;"; }
        else { key = "INT_SPELL_DURATION_HOURS"; enUS = "%d |4hour:hours;"; }
    } else if (ms >= kMinute) {
        n = up(ms, kMinute);
        if (n == 60) { n = 1; key = "INT_SPELL_DURATION_HOURS"; enUS = "%d |4hour:hours;"; }
        else { key = "INT_SPELL_DURATION_MIN"; enUS = "%d min"; }
    }
    return interfaceFormat(key, enUS, {std::to_string(n)});
}

/// An instance's name as 0x004fe070 gives it: Map.dbc's, followed by the
/// difficulty (DUNGEON_NAME_WITH_DIFFICULTY) where MapDifficulty.dbc names one
/// for that map and difficulty - "Naxxramas (25 Spieler)". The map's number
/// where Map.dbc has no name, as 0x0050ccd0 prints it.
std::string instanceLabel(GameHandler& gh, uint32_t mapId, uint32_t difficulty) {
    std::string name = gh.getMapName(mapId);
    if (name.empty()) return std::to_string(mapId);
    auto* am = gh.services().assetManager;
    auto dbc = am && am->isInitialized() ? am->loadDBC("MapDifficulty.dbc") : nullptr;
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() <= 22) return name;
    // 0=ID, 1=MapID, 2=Difficulty, 22=Difficultystring (a GlobalStrings key).
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        if (dbc->getUInt32(i, 1) != mapId || dbc->getUInt32(i, 2) != difficulty) continue;
        const std::string key = dbc->getString(i, 22);
        if (key.empty()) break;
        return interfaceFormat("DUNGEON_NAME_WITH_DIFFICULTY", "%1$s (%2$s)",
                               {name, interfaceString(key.c_str(), key.c_str())});
    }
    return name;
}

/// The player's own name, for the handlers that word a line differently when
/// it is about the player.
std::string ownName(GameHandler& gh) {
    std::string name = gh.lookupName(gh.getPlayerGuid());
    if (name.empty())
        if (const Character* ch = gh.getActiveCharacter()) name = ch->name;
    return name;
}

} // namespace

static bool parseInspectEquipmentPayload(network::Packet& packet,
                                         uint64_t& outGuid,
                                         std::array<uint32_t, 19>& outItems,
                                         std::array<uint16_t, 19>& outEnchants) {
    const size_t start = packet.getReadPos();
    constexpr size_t kGearBytes = 19 * sizeof(uint32_t);
    constexpr uint32_t kEquipmentSlotMask = (1u << 19) - 1u;

    auto reset = [&]() {
        packet.setReadPos(start);
        outGuid = 0;
        outItems.fill(0);
        outEnchants.fill(0);
    };

    auto countSlots = [](uint32_t mask) {
        int count = 0;
        for (int slot = 0; slot < 19; ++slot) {
            if (mask & (1u << slot)) ++count;
        }
        return count;
    };

    auto parseMaskedItems = [&](const char* guidEncoding) -> bool {
        if (!packet.hasRemaining(sizeof(uint32_t))) return false;
        const uint32_t slotMask = packet.readUInt32();
        if ((slotMask & ~kEquipmentSlotMask) != 0) return false;

        const int slotCount = countSlots(slotMask);
        if (slotCount <= 0) return false;

        const size_t remaining = packet.getRemainingSize();
        const size_t candidateRecordSizes[] = {24, 20, 16, 12, 8, 4};
        size_t recordSize = 0;
        for (size_t candidate : candidateRecordSizes) {
            if (remaining == static_cast<size_t>(slotCount) * candidate) {
                recordSize = candidate;
                break;
            }
        }
        if (recordSize == 0) {
            // Some cores append a tiny trailer. Prefer the largest plausible
            // record that fits cleanly, but do not consume packets that are
            // clearly not the masked inspect format.
            for (size_t candidate : candidateRecordSizes) {
                const size_t needed = static_cast<size_t>(slotCount) * candidate;
                if (remaining >= needed && remaining - needed <= 8) {
                    recordSize = candidate;
                    break;
                }
            }
        }
        if (recordSize < sizeof(uint32_t)) return false;

        int nonZero = 0;
        for (int slot = 0; slot < 19; ++slot) {
            if ((slotMask & (1u << slot)) == 0) continue;
            if (!packet.hasRemaining(recordSize)) return false;

            const size_t recordStart = packet.getReadPos();
            const uint32_t itemEntry = packet.readUInt32();
            uint16_t enchantId = 0;
            if (recordSize >= sizeof(uint32_t) + sizeof(uint16_t) &&
                packet.hasRemaining(sizeof(uint16_t))) {
                enchantId = packet.readUInt16();
            }
            packet.setReadPos(recordStart + recordSize);

            outItems[slot] = itemEntry;
            outEnchants[slot] = enchantId;
            if (itemEntry != 0) ++nonZero;
        }

        if (nonZero == 0) return false;
        LOG_INFO("SMSG_INSPECT_RESULTS_UPDATE masked gear: guidEncoding=", guidEncoding,
                 " mask=0x", std::hex, slotMask, std::dec,
                 " slots=", slotCount, " recordSize=", recordSize,
                 " nonZero=", nonZero);
        return true;
    };

    auto readItems = [&]() -> bool {
        for (uint32_t& item : outItems) {
            if (!packet.hasRemaining(sizeof(uint32_t))) return false;
            item = packet.readUInt32();
        }
        return true;
    };

    reset();
    if (packet.hasRemaining(sizeof(uint64_t))) {
        outGuid = packet.readUInt64();
        if (outGuid != 0 && parseMaskedItems("uint64")) {
            return true;
        }
    }

    reset();
    if (packet.hasFullPackedGuid()) {
        outGuid = packet.readPackedGuid();
        if (outGuid != 0 && parseMaskedItems("packed")) {
            return true;
        }
    }

    reset();
    if (packet.getRemainingSize() >= sizeof(uint64_t) + kGearBytes) {
        outGuid = packet.readUInt64();
        if (outGuid != 0 && readItems()) {
            LOG_INFO("SMSG_INSPECT_RESULTS_UPDATE flat gear: guidEncoding=uint64 slots=19");
            return true;
        }
    }

    reset();
    if (packet.hasFullPackedGuid()) {
        outGuid = packet.readPackedGuid();
        if (outGuid != 0 && packet.getRemainingSize() >= kGearBytes && readItems()) {
            LOG_INFO("SMSG_INSPECT_RESULTS_UPDATE flat gear: guidEncoding=packed slots=19");
            return true;
        }
    }

    reset();
    return false;
}

static bool isTbcInspectTalentBitSet(const std::vector<uint8_t>& bitfield, uint32_t bitIndex) {
    const uint32_t slot = bitIndex / 7u;
    const uint32_t offset = bitIndex % 7u;
    return slot < bitfield.size() && (bitfield[slot] & (1u << offset)) != 0;
}

static bool decodeTbcInspectTalentBitfield(GameHandler& owner,
                                           uint8_t classId,
                                           const std::vector<uint8_t>& bitfield,
                                           std::array<uint32_t, 3>& outTreePoints,
                                           uint32_t& outSpentTalents) {
    outTreePoints.fill(0);
    outSpentTalents = 0;
    if (classId == 0 || bitfield.empty()) return false;

    owner.loadTalentDbc();

    const uint32_t classMask = 1u << (classId - 1u);
    std::vector<const TalentTabEntry*> classTabs;
    for (const auto& [tabId, tab] : owner.getAllTalentTabs()) {
        if (tab.classMask & classMask) {
            classTabs.push_back(&tab);
        }
    }
    std::sort(classTabs.begin(), classTabs.end(),
              [](const auto* a, const auto* b) { return a->orderIndex < b->orderIndex; });
    if (classTabs.empty()) return false;

    uint32_t tabBitStart = 0;
    for (size_t tabIndex = 0; tabIndex < classTabs.size() && tabIndex < outTreePoints.size(); ++tabIndex) {
        std::vector<const TalentEntry*> talents;
        for (const auto& [talentId, talent] : owner.getAllTalents()) {
            if (talent.tabId == classTabs[tabIndex]->tabId && talent.maxRank > 0) {
                talents.push_back(&talent);
            }
        }
        std::sort(talents.begin(), talents.end(), [](const auto* a, const auto* b) {
            if (a->row != b->row) return a->row < b->row;
            if (a->column != b->column) return a->column < b->column;
            return a->talentId < b->talentId;
        });

        uint32_t tabBits = 0;
        for (const auto* talent : talents) {
            uint8_t rank = 0;
            for (uint8_t r = 1; r <= talent->maxRank; ++r) {
                if (isTbcInspectTalentBitSet(bitfield, tabBitStart + tabBits + (r - 1u))) {
                    rank = r;
                }
            }
            outTreePoints[tabIndex] += rank;
            outSpentTalents += rank;
            tabBits += talent->maxRank;
        }
        tabBitStart += tabBits;
    }

    return true;
}

static bool parseTbcInspectTalentPayload(network::Packet& packet,
                                         GameHandler& owner,
                                         uint8_t classId,
                                         uint32_t& outUnspentTalents,
                                         uint32_t& outSpentTalents,
                                         std::array<uint32_t, 3>& outTreePoints,
                                         bool& outHasTreePoints) {
    const size_t start = packet.getReadPos();
    outUnspentTalents = 0;
    outSpentTalents = 0;
    outTreePoints.fill(0);
    outHasTreePoints = false;

    const size_t payloadSize = packet.getRemainingSize();
    if (payloadSize < sizeof(uint32_t)) {
        packet.setReadPos(start);
        return false;
    }

    auto parseTalentRecords = [](network::Packet& p, size_t count) {
        uint32_t spent = 0;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t talentId = p.readUInt32();
            const uint8_t rank = p.readUInt8();
            if (talentId == 0) continue;
            spent += static_cast<uint32_t>(rank) + 1u;
        }
        return spent;
    };

    const uint32_t firstValue = packet.readUInt32();

    // TBC/CMaNGOS sends SMSG_INSPECT_TALENT as:
    //   uint32 byteCount, byte[byteCount] compact talent bitfield.
    // Older attempts treated byteCount (0x3d) as talent points; keep the
    // fallback list-style parser below for other 2.x cores, but prefer this
    // server-authored bitfield when the size lines up exactly.
    if (firstValue > 0 && firstValue <= 256 && packet.getRemainingSize() == firstValue) {
        std::vector<uint8_t> bitfield;
        bitfield.reserve(firstValue);
        for (uint32_t i = 0; i < firstValue; ++i) {
            bitfield.push_back(packet.readUInt8());
        }

        outHasTreePoints = decodeTbcInspectTalentBitfield(owner, classId, bitfield, outTreePoints, outSpentTalents);
        if (!outHasTreePoints) {
            outSpentTalents = 0;
        }
        LOG_INFO("SMSG_INSPECT_TALENT (TBC): parsed cmangos bitfield bytes=", firstValue,
                 " spent=", outSpentTalents,
                 " trees=", outTreePoints[0], "/", outTreePoints[1], "/", outTreePoints[2],
                 " class=", static_cast<int>(classId),
                 " decoded=", (outHasTreePoints ? "yes" : "no"),
                 " trailingBytes=", packet.getRemainingSize());
        return true;
    }

    // The regular talents-info packet uses uint32 unspent + uint8 count.
    // Accept that shape too so different 2.x cores still render something useful.
    if (packet.hasRemaining(1)) {
        const uint8_t nextByte = packet.readUInt8();
        const size_t afterHeaderBytes = packet.getRemainingSize();

        if (afterHeaderBytes > 0 && (afterHeaderBytes % 5u) == 0u) {
            const size_t recordCount = afterHeaderBytes / 5u;
            const uint32_t rankSpent = parseTalentRecords(packet, recordCount);
            outUnspentTalents = 0;
            outSpentTalents = firstValue != 0 ? firstValue : rankSpent;
            LOG_INFO("SMSG_INSPECT_TALENT (TBC): parsed cmangos talents records=", recordCount,
                     " spent=", outSpentTalents, " rankSpent=", rankSpent,
                     " unk=", static_cast<int>(nextByte),
                     " trailingBytes=", packet.getRemainingSize());
            return true;
        }

        const uint8_t talentCount = nextByte;
        if (packet.hasRemaining(static_cast<size_t>(talentCount) * 5u)) {
            const uint32_t spentTalents = parseTalentRecords(packet, talentCount);
            outUnspentTalents = firstValue;
            outSpentTalents = spentTalents;
            LOG_INFO("SMSG_INSPECT_TALENT (TBC): parsed talents count=", static_cast<int>(talentCount),
                     " spent=", spentTalents, " unspent=", firstValue,
                     " trailingBytes=", packet.getRemainingSize());
            return true;
        }
    }

    if ((payloadSize % 5u) == 0u) {
        packet.setReadPos(start);
        const size_t recordCount = payloadSize / 5u;
        outSpentTalents = parseTalentRecords(packet, recordCount);
        LOG_INFO("SMSG_INSPECT_TALENT (TBC): parsed bare talents records=", recordCount,
                 " spent=", outSpentTalents,
                 " trailingBytes=", packet.getRemainingSize());
        return true;
    }

    packet.setReadPos(start);
    return false;
}

static const std::string kEmptyString;

SocialHandler::SocialHandler(GameHandler& owner)
    : owner_(owner) {
    loadGuildNameCache();
}

void SocialHandler::loadGuildNameCache() {
    std::ifstream in(guildNameCachePath());
    if (!in.is_open()) return;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto separator = line.find('\t');
        if (separator == std::string::npos) continue;

        try {
            const uint32_t guildId = static_cast<uint32_t>(std::stoul(line.substr(0, separator)));
            std::string guildName = line.substr(separator + 1);
            if (guildId != 0 && !guildName.empty()) {
                guildNameCache_[guildId] = std::move(guildName);
            }
        } catch (...) {
            // Ignore stale or hand-edited cache lines.
        }
    }
}

void SocialHandler::saveGuildNameCache() const {
    const std::filesystem::path path = guildNameCachePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path);
    if (!out.is_open()) {
        LOG_WARNING("Could not save guild name cache to ", path.string());
        return;
    }

    for (const auto& [guildId, guildName] : guildNameCache_) {
        if (guildId != 0 && !guildName.empty()) {
            out << guildId << '\t' << guildName << '\n';
        }
    }
}

void SocialHandler::rememberGuildName(uint32_t guildId, const std::string& guildName) {
    if (guildId == 0 || guildName.empty()) return;

    auto it = guildNameCache_.find(guildId);
    if (it != guildNameCache_.end() && it->second == guildName) return;

    guildNameCache_[guildId] = guildName;
    saveGuildNameCache();
}

// ============================================================
// registerOpcodes
// ============================================================

void SocialHandler::registerOpcodes(DispatchTable& table) {
    // ---- Player info queries / social ----
    table[Opcode::SMSG_QUERY_TIME_RESPONSE] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleQueryTimeResponse(packet);
    };
    table[Opcode::SMSG_PLAYED_TIME] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handlePlayedTime(packet);
    };
    table[Opcode::SMSG_WHO] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleWho(packet);
    };
    table[Opcode::SMSG_WHOIS] = [](network::Packet& packet) {
        // The game console's and nobody else's (0x006b3280 hands the text to
        // the console, 0x00765270) - this printed it into chat as "[Whois]".
        if (packet.getReadPos() < packet.getSize()) {
            std::string whoisText = packet.readString();
            if (!whoisText.empty()) LOG_INFO("SMSG_WHOIS: ", whoisText);
        }
    };
    table[Opcode::SMSG_FRIEND_STATUS] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleFriendStatus(packet);
    };
    table[Opcode::SMSG_CONTACT_LIST] = [this](network::Packet& packet) { handleContactList(packet); };
    table[Opcode::SMSG_FRIEND_LIST] = [this](network::Packet& packet) { handleFriendList(packet); };
    table[Opcode::SMSG_IGNORE_LIST] = [this](network::Packet& packet) {
        // Format: uint8 count + count × uint64 guid (no name strings in packet).
        // Names are resolved via SMSG_NAME_QUERY_RESPONSE after the list arrives.
        if (!packet.hasRemaining(1)) return;
        uint8_t ignCount = packet.readUInt8();
        owner_.ignoreListGuidsRef().clear();
        for (uint8_t i = 0; i < ignCount; ++i) {
            if (!packet.hasRemaining(8)) break;
            uint64_t ignGuid = packet.readUInt64();
            if (ignGuid != 0) {
                owner_.ignoreListGuidsRef().insert(ignGuid);
                // Query name so UI can display it later
                owner_.queryPlayerName(ignGuid);
            }
        }
        LOG_DEBUG("SMSG_IGNORE_LIST: loaded ", (int)ignCount, " ignored players");
    };
    table[Opcode::MSG_RANDOM_ROLL] = [this](network::Packet& packet) {
        if (owner_.getState() == WorldState::IN_WORLD) handleRandomRoll(packet);
    };

    // ---- Logout ----
    table[Opcode::SMSG_LOGOUT_RESPONSE] = [this](network::Packet& packet) { handleLogoutResponse(packet); };
    table[Opcode::SMSG_LOGOUT_COMPLETE] = [this](network::Packet& packet) { handleLogoutComplete(packet); };

    // ---- Inspect ----
    table[Opcode::SMSG_INSPECT_TALENT] = [this](network::Packet& packet) { handleInspectResults(packet); };
    // The honour tab's own answer, on the same opcode it was asked with.
    // AzerothCore replies with the target's guid, the honour byte, the packed
    // kill counter and three totals - MiscHandler.cpp, HandleInspectHonorStats.
    table[Opcode::MSG_INSPECT_HONOR_STATS] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(8 + 1 + 16)) return;
        const uint64_t guid = packet.readUInt64();
        // Only for whoever is being inspected. The window reads one result and
        // a stale answer for someone else would be attributed to them.
        if (inspectResult_.guid != 0 && guid != inspectResult_.guid) return;
        inspectResult_.honorRank = packet.readUInt8();
        // One field holds both days: PAIR32_LOPART is today and the high half
        // is yesterday, which is how the server rolls the counter over -
        // MAKE_PAIR32(0, kills_today) at midnight.
        const uint32_t kills = packet.readUInt32();
        inspectResult_.honorTodayKills     = kills & 0xFFFFu;
        inspectResult_.honorYesterdayKills = kills >> 16;
        inspectResult_.honorTodayContribution     = packet.readUInt32();
        inspectResult_.honorYesterdayContribution = packet.readUInt32();
        inspectResult_.honorLifetimeKills         = packet.readUInt32();
        inspectResult_.hasHonorData = true;
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("INSPECT_HONOR_UPDATE", {});
    };
    table[Opcode::SMSG_INSPECT_RESULTS_UPDATE] = [this](network::Packet& packet) { handleInspectResults(packet); };

    // ---- Group ----
    table[Opcode::SMSG_GROUP_INVITE] = [this](network::Packet& packet) { handleGroupInvite(packet); };
    table[Opcode::SMSG_GROUP_DECLINE] = [this](network::Packet& packet) { handleGroupDecline(packet); };
    table[Opcode::SMSG_GROUP_LIST] = [this](network::Packet& packet) { handleGroupList(packet); };
    table[Opcode::SMSG_GROUP_DESTROYED] = [this](network::Packet& /*packet*/) {
        // ERR_GROUP_DISBANDED, a system line, and only to someone who was in
        // the group (0x006cbe40). This said it twice in English, on screen and
        // in chat.
        const bool wasInGroup = !partyData.members.empty();
        partyData.members.clear();
        partyData.memberCount = 0;
        partyData.leaderGuid = 0;
        if (wasInGroup)
            gameError(owner_, ErrTo::Chat, "ERR_GROUP_DISBANDED", "Your group has been disbanded.");
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("GROUP_ROSTER_UPDATE", {});
            owner_.addonEventCallbackRef()("PARTY_MEMBERS_CHANGED", {});
        }
    };
    table[Opcode::SMSG_GROUP_CANCEL] = [this](network::Packet& /*packet*/) {
        // PARTY_INVITE_CANCEL and nothing said (0x006cbd10 -> 0x00512560,
        // event 0x108): UIParent hides the invitation popup on it. This printed
        // "Group invite cancelled." and left the popup up.
        pendingGroupInvite = false;
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("PARTY_INVITE_CANCEL", {});
    };
    table[Opcode::SMSG_GROUP_UNINVITE] = [this](network::Packet& packet) { handleGroupUninvite(packet); };
    table[Opcode::SMSG_PARTY_COMMAND_RESULT] = [this](network::Packet& packet) { handlePartyCommandResult(packet); };
    table[Opcode::SMSG_PARTY_MEMBER_STATS] = [this](network::Packet& packet) { handlePartyMemberStats(packet, false); };
    table[Opcode::SMSG_PARTY_MEMBER_STATS_FULL] = [this](network::Packet& packet) { handlePartyMemberStats(packet, true); };

    // ---- Ready check ----
    table[Opcode::MSG_RAID_READY_CHECK] = [this](network::Packet& packet) {
        pendingReadyCheck_ = true;
        readyCheckReadyCount_ = 0;
        readyCheckNotReadyCount_ = 0;
        readyCheckInitiator_.clear();
        readyCheckResults_.clear();
        bool initiatedHere = false;
        if (packet.hasRemaining(8)) {
            uint64_t initiatorGuid = packet.readUInt64();
            initiatedHere = initiatorGuid == owner_.getPlayerGuid();
            auto entity = owner_.getEntityManager().getEntity(initiatorGuid);
            if (auto* unit = dynamic_cast<Unit*>(entity.get()))
                readyCheckInitiator_ = unit->getName();
        }
        if (readyCheckInitiator_.empty() && partyData.leaderGuid != 0) {
            for (const auto& member : partyData.members) {
                if (member.guid == partyData.leaderGuid) { readyCheckInitiator_ = member.name; break; }
            }
        }
        // READY_CHECK_START, a system line for the one who started it and
        // nobody else (0x00574d50); the others get the popup READY_CHECK opens.
        if (initiatedHere)
            owner_.addSystemChatMessage(interfaceString("READY_CHECK_START", "Starting ready check..."));
        if (owner_.addonEventCallbackRef())
            owner_.addonEventCallbackRef()("READY_CHECK", {readyCheckInitiator_});
    };
    table[Opcode::MSG_RAID_READY_CHECK_CONFIRM] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(9)) { packet.skipAll(); return; }
        uint64_t respGuid = packet.readUInt64();
        uint8_t  isReady  = packet.readUInt8();
        if (isReady) ++readyCheckReadyCount_; else ++readyCheckNotReadyCount_;
        auto nit = owner_.getPlayerNameCache().find(respGuid);
        std::string rname;
        if (nit != owner_.getPlayerNameCache().end()) rname = nit->second;
        else {
            // Only cast to Unit if the entity actually is one - a raw
            // static_pointer_cast on a GameObject would be undefined behavior.
            auto ent = owner_.getEntityManager().getEntity(respGuid);
            if (ent && (ent->getType() == ObjectType::UNIT || ent->getType() == ObjectType::PLAYER))
                rname = std::static_pointer_cast<game::Unit>(ent)->getName();
        }
        if (!rname.empty()) {
            bool found = false;
            for (auto& r : readyCheckResults_) {
                if (r.name == rname) { r.ready = (isReady != 0); found = true; break; }
            }
            if (!found) readyCheckResults_.push_back({ .name = rname, .ready = isReady != 0 });
            // RAID_MEMBER_NOT_READY for a "no" and nothing for a "yes"
            // (0x005744f0) - in a raid only to whoever started the check.
            // This announced every answer either way, in English.
            const bool initiatedHere = !readyCheckInitiator_.empty() &&
                                       readyCheckInitiator_ == ownName(owner_);
            if (!isReady && (!groupIsRaid(partyData.groupType) || initiatedHere))
                owner_.addSystemChatMessage(
                    interfaceFormat("RAID_MEMBER_NOT_READY", "%s is not ready", {rname}));
        }
        if (owner_.addonEventCallbackRef()) {
            char guidBuf[32];
            snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)respGuid);
            owner_.addonEventCallbackRef()("READY_CHECK_CONFIRM", {guidBuf, isReady ? "1" : "0"});
        }
    };
    table[Opcode::MSG_RAID_READY_CHECK_FINISHED] = [this](network::Packet& /*packet*/) {
        // No tally on the server's word. The original's summary -
        // RAID_MEMBERS_AFK, READY_CHECK_ALL_READY or READY_CHECK_FINISHED - comes
        // from its own timer, for the one who started the check (0x00573390);
        // this printed an English count to everybody instead.
        pendingReadyCheck_ = false;
        readyCheckReadyCount_ = 0;
        readyCheckNotReadyCount_ = 0;
        readyCheckResults_.clear();
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("READY_CHECK_FINISHED", {});
    };
    table[Opcode::SMSG_RAID_INSTANCE_INFO] = [this](network::Packet& packet) { handleRaidInstanceInfo(packet); };

    // ---- Duels ----
    table[Opcode::SMSG_DUEL_REQUESTED] = [this](network::Packet& packet) { handleDuelRequested(packet); };
    table[Opcode::SMSG_DUEL_COMPLETE] = [this](network::Packet& packet) { handleDuelComplete(packet); };
    table[Opcode::SMSG_DUEL_WINNER] = [this](network::Packet& packet) { handleDuelWinner(packet); };
    table[Opcode::SMSG_DUEL_OUTOFBOUNDS] = [this](network::Packet& /*packet*/) {
        // The event alone (0x005cf910, event 0x172): UIParent shows the
        // DUEL_OUTOFBOUNDS popup with its ten-second countdown to a forfeit.
        // This put the same English line on screen twice besides.
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("DUEL_OUTOFBOUNDS", {});
    };
    table[Opcode::SMSG_DUEL_INBOUNDS] = [this](network::Packet& /*packet*/) {
        // Stops that countdown. Handled as a no-op before, so a duellist who
        // stepped back inside kept a timer that was no longer true.
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("DUEL_INBOUNDS", {});
    };
    table[Opcode::SMSG_DUEL_COUNTDOWN] = [this](network::Packet& packet) {
        if (packet.hasRemaining(4)) {
            uint32_t ms = packet.readUInt32();
            duelCountdownMs_ = (ms > 0 && ms <= 30000) ? ms : 3000;
            duelCountdownStartedAt_ = std::chrono::steady_clock::now();
        }
    };
    table[Opcode::SMSG_PARTYKILLLOG] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(16)) return;
        uint64_t killerGuid = packet.readUInt64();
        uint64_t victimGuid = packet.readUInt64();
        // A combat log entry and nothing in chat (0x00750c90): the original
        // files it as PARTY_KILL (subevent 0x2b) for the combat log to word -
        // "X tötet Y" is the combat log's line, not a system message. This
        // printed an English "X killed Y." into the chat window instead.
        if (owner_.addonEventCallbackRef()) {
            char src[32], dst[32];
            std::snprintf(src, sizeof(src), "0x%016llX", static_cast<unsigned long long>(killerGuid));
            std::snprintf(dst, sizeof(dst), "0x%016llX", static_cast<unsigned long long>(victimGuid));
            owner_.addonEventCallbackRef()("COMBAT_LOG_EVENT_UNFILTERED", {
                std::to_string(static_cast<double>(std::time(nullptr))), "PARTY_KILL",
                src, owner_.lookupName(killerGuid), "0",
                dst, owner_.lookupName(victimGuid), "0"
            });
        }
    };

    // ---- Guild ----
    table[Opcode::SMSG_GUILD_INFO] = [this](network::Packet& packet) { handleGuildInfo(packet); };
    table[Opcode::SMSG_GUILD_ROSTER] = [this](network::Packet& packet) { handleGuildRoster(packet); };
    table[Opcode::SMSG_GUILD_QUERY_RESPONSE] = [this](network::Packet& packet) { handleGuildQueryResponse(packet); };
    table[Opcode::SMSG_GUILD_EVENT] = [this](network::Packet& packet) { handleGuildEvent(packet); };
    table[Opcode::SMSG_GUILD_INVITE] = [this](network::Packet& packet) { handleGuildInvite(packet); };
    table[Opcode::SMSG_GUILD_COMMAND_RESULT] = [this](network::Packet& packet) { handleGuildCommandResult(packet); };
    table[Opcode::SMSG_PETITION_SHOWLIST] = [this](network::Packet& packet) { handlePetitionShowlist(packet); };
    table[Opcode::SMSG_TURN_IN_PETITION_RESULTS] = [this](network::Packet& packet) { handleTurnInPetitionResults(packet); };
    table[Opcode::SMSG_OFFER_PETITION_ERROR] = [this](network::Packet& packet) {
        // The player the charter was offered to, and ERR_ALREADY_IN_ARENA_TEAM_S
        // naming them as a system line when the name is known (0x005cea00).
        // This read a reason code where the server sends a guid, and answered
        // in English.
        if (packet.hasRemaining(8)) {
            const std::string name = owner_.lookupName(packet.readUInt64());
            if (!name.empty())
                gameError(owner_, ErrTo::Chat, "ERR_ALREADY_IN_ARENA_TEAM_S",
                          "%s is already in an arena team of that size.", {name});
        }
    };
    table[Opcode::MSG_PETITION_DECLINE] = [this](network::Packet& packet) {
        // Someone declined to sign: ERR_PETITION_DECLINED_S naming them, a
        // system line, when the name is known (0x006cf140). Nothing read it.
        if (packet.hasRemaining(8)) {
            const std::string name = owner_.lookupName(packet.readUInt64());
            if (!name.empty())
                gameError(owner_, ErrTo::Chat, "ERR_PETITION_DECLINED_S", "%s has declined to sign your charter.", {name});
        }
    };
    table[Opcode::MSG_GUILD_EVENT_LOG_QUERY] = [this](network::Packet& packet) { handleGuildEventLog(packet); };
    table[Opcode::MSG_GUILD_BANK_LOG_QUERY] = [this](network::Packet& packet) { handleGuildBankLog(packet); };
    table[Opcode::SMSG_PETITION_QUERY_RESPONSE] = [this](network::Packet& packet) { handlePetitionQueryResponse(packet); };
    table[Opcode::SMSG_PETITION_SHOW_SIGNATURES] = [this](network::Packet& packet) { handlePetitionShowSignatures(packet); };
    table[Opcode::SMSG_PETITION_SIGN_RESULTS] = [this](network::Packet& packet) { handlePetitionSignResults(packet); };

    // ---- Battlefield / BG ----
    table[Opcode::SMSG_BATTLEFIELD_STATUS] = [this](network::Packet& packet) { handleBattlefieldStatus(packet); };
    table[Opcode::SMSG_BATTLEFIELD_LIST] = [this](network::Packet& packet) { handleBattlefieldList(packet); };
    table[Opcode::SMSG_BATTLEFIELD_PORT_DENIED] = [this](network::Packet& /*packet*/) {
        // ERR_PLAYER_DEAD on screen, whenever there is a player to say it to
        // (0x0054d4b0). This said "Battlefield port denied." on screen and in
        // chat.
        gameError(owner_, ErrTo::Error, "ERR_PLAYER_DEAD", "You can't do that when you're dead.");
    };
    table[Opcode::MSG_BATTLEGROUND_PLAYER_POSITIONS] = [this](network::Packet& packet) {
        // Canonical, as every reader takes them: the wire's x and y were
        // kept as they came, X and Y the wrong way round.
        bgPlayerPositions_.clear();
        for (const auto& read : BattlegroundPlayerPositionsParser::parse(packet)) {
            BgPlayerPosition pos;
            pos.guid = read.guid;
            pos.wowX = read.canonicalX;
            pos.wowY = read.canonicalY;
            pos.group = read.group;
            bgPlayerPositions_.push_back(pos);
        }
    };
    table[Opcode::SMSG_REMOVED_FROM_PVP_QUEUE] = [this](network::Packet& packet) {
        // The reason, and the refusal on screen that names it (0x00526530 ->
        // 0x00522180); nothing for a reason it has no words for. This printed
        // one English line whatever the reason.
        if (!packet.hasRemaining(4)) return;
        switch (packet.readUInt32()) {
            case 0: gameError(owner_, ErrTo::Error, "ERR_REMOVE_FROM_PVP_QUEUE_GRANT_LEVEL",
                              "You have been removed from a PvP queue because you have been granted a level."); break;
            case 1: gameError(owner_, ErrTo::Error, "ERR_REMOVE_FROM_PVP_QUEUE_XP_GAIN",
                              "You have been removed from a PvP queue because you changed your experience gain settings."); break;
            case 2: gameError(owner_, ErrTo::Error, "ERR_REMOVE_FROM_PVP_QUEUE_FACTION_CHANGE",
                              "You have been removed from a PvP queue because you changed your faction."); break;
            default: break;
        }
    };
    // The answer to a join, and most often a refusal. A positive value is the
    // battleground joined; the negative ones are the reasons a queue was
    // refused. It was read as "joined" whatever it said, so a Deserter or a
    // third queue was reported as success and nothing appeared on the minimap.
    table[Opcode::SMSG_GROUP_JOINED_BATTLEGROUND] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        bgJoinPendingType_ = 0;  // answered
        const int32_t result = static_cast<int32_t>(packet.readUInt32());
        // Each answer as 0x0054b1c0 words it: the ERR_* string for the
        // refusal, a joined queue as yellow ERR_GROUP_JOIN_BATTLEGROUND_S, and
        // the deserter refusal, oddly, yellow too - the table at 0x00ac8af0
        // routes it to UI_INFO_MESSAGE. These were English, each said twice.
        LOG_INFO("SMSG_GROUP_JOINED_BATTLEGROUND: ", result);
        switch (result) {
            case -1: return;  // nothing to say
            case -2:  gameError(owner_, ErrTo::Info, "ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS",
                                "You cannot join the battleground yet because you or one of your party members is flagged as a Deserter."); break;
            case -3:  gameError(owner_, ErrTo::Error, "ERR_ARENA_TEAM_PARTY_SIZE", "Incorrect party size for this arena."); break;
            case -4:  gameError(owner_, ErrTo::Error, "ERR_BATTLEGROUND_TOO_MANY_QUEUES", "You can only be queued for 2 battles at once"); break;
            case -5:  gameError(owner_, ErrTo::Error, "ERR_BATTLEGROUND_CANNOT_QUEUE_FOR_RATED",
                                "You cannot queue for a rated match while queued for other battles"); break;
            case -6:  gameError(owner_, ErrTo::Error, "ERR_BATTLEDGROUND_QUEUED_FOR_RATED",
                                "You cannot queue for another battle while queued for a rated arena match"); break;
            case -7:  gameError(owner_, ErrTo::Error, "ERR_BATTLEGROUND_TEAM_LEFT_QUEUE", "Your team has left the arena queue"); break;
            case -8:  gameError(owner_, ErrTo::Error, "ERR_BATTLEGROUND_NOT_IN_BATTLEGROUND", "You can't do that in a battleground."); break;
            case -9:  gameError(owner_, ErrTo::Error, "ERR_BATTLEGROUND_JOIN_XP_GAIN",
                                "Cannot join as a group unless all the members of your party have the same XP gain setting."); break;
            case -10: gameError(owner_, ErrTo::Error, "ERR_BATTLEGROUND_JOIN_RANGE_INDEX",
                                "Cannot join the queue unless all members of your party are in the same battleground level range."); break;
            case -11:
            case -12: {
                // Both carry the member who held it up; named when known,
                // ERR_BATTLEGROUND_JOIN_FAILED when not.
                std::string who;
                if (packet.hasRemaining(8)) who = owner_.lookupName(packet.readUInt64());
                if (who.empty())
                    gameError(owner_, ErrTo::Error, "ERR_BATTLEGROUND_JOIN_FAILED", "Join as a group failed");
                else
                    gameError(owner_, ErrTo::Error, "ERR_BATTLEGROUND_JOIN_TIMED_OUT",
                              "%s was unavailable to join the queue.", {who});
                break;
            }
            case -13: gameError(owner_, ErrTo::Error, "ERR_LFG_CANT_USE_BATTLEGROUND",
                                "You cannot queue for a battleground or arena while using the dungeon system."); break;
            case -14: gameError(owner_, ErrTo::Error, "ERR_IN_RANDOM_BG", "Can't do that while in a Random Battleground queue."); break;
            case -15: gameError(owner_, ErrTo::Error, "ERR_IN_NON_RANDOM_BG",
                                "Can't queue for Random Battleground while in another Battleground queue."); break;
            default: {
                // A battleground by BattlemasterList.dbc's name, or
                // ERR_GROUP_JOIN_BATTLEGROUND_FAIL where there is none - which
                // is where a zero lands too.
                const GameHandler::BattlemasterEntry* bg =
                    result > 0 ? owner_.getBattlemasterInfo(static_cast<uint32_t>(result)) : nullptr;
                if (bg && !bg->name.empty())
                    gameError(owner_, ErrTo::Info, "ERR_GROUP_JOIN_BATTLEGROUND_S",
                              "Your group has joined the queue for %s", {bg->name});
                else
                    gameError(owner_, ErrTo::Error, "ERR_GROUP_JOIN_BATTLEGROUND_FAIL",
                              "Your group has joined a battleground queue, but you are not eligible");
                break;
            }
        }
    };
    // No handler in the original at all, so nothing to say: this printed
    // "You have joined the battleground queue." The queue itself arrives as
    // SMSG_BATTLEFIELD_STATUS.
    table[Opcode::SMSG_JOINED_BATTLEGROUND_QUEUE] = [](network::Packet& /*packet*/) {};
    // ERR_BG_PLAYER_JOINED_SS and ERR_BG_PLAYER_LEFT_S, system lines naming
    // the player (0x0054b510) - "Name-Realm" for one from another realm,
    // which a name from the cache already is. These were English.
    table[Opcode::SMSG_BATTLEGROUND_PLAYER_JOINED] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            const std::string name = owner_.lookupName(packet.readUInt64());
            if (!name.empty())
                gameError(owner_, ErrTo::Chat, "ERR_BG_PLAYER_JOINED_SS",
                          "|Hplayer:%s|h[%s]|h has joined the battle", {name, name});
        }
    };
    table[Opcode::SMSG_BATTLEGROUND_PLAYER_LEFT] = [this](network::Packet& packet) {
        if (packet.hasRemaining(8)) {
            const std::string name = owner_.lookupName(packet.readUInt64());
            if (!name.empty())
                gameError(owner_, ErrTo::Chat, "ERR_BG_PLAYER_LEFT_S", "%s has left the battle", {name});
        }
    };

    // ---- Instance ----
    for (auto op : { Opcode::SMSG_INSTANCE_DIFFICULTY, Opcode::MSG_SET_DUNGEON_DIFFICULTY }) {
        table[op] = [this](network::Packet& packet) { handleInstanceDifficulty(packet); };
    }

    // ---- Guild / RAF / PvP AFK (moved from GameHandler) ----
    table[Opcode::SMSG_GUILD_DECLINE] = [this](network::Packet& packet) {
        if (packet.hasData()) {
            // ERR_GUILD_DECLINE_S, a system line (0x006cc410).
            std::string name = packet.readString();
            gameError(owner_, ErrTo::Chat, "ERR_GUILD_DECLINE_S", "%s declines your guild invitation.", {name});
        }
    };
    table[Opcode::SMSG_REFER_A_FRIEND_EXPIRED] = [this](network::Packet& packet) {
        // The friend whose link ran out, and a friends-list refresh - nothing
        // said (0x00526530, which fires FRIENDLIST_UPDATE). This printed an
        // English line.
        packet.skipAll();
        if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("FRIENDLIST_UPDATE", {});
    };
    table[Opcode::SMSG_REFER_A_FRIEND_FAILURE] = [this](network::Packet& packet) {
        // The reason, and a name only for reason 9; each said on screen as
        // 0x005219e0 picks the ERR_ string, nothing for the rest. This printed
        // "Recruit-A-Friend: " and an English reason of its own numbering.
        if (packet.hasRemaining(4)) {
            const uint32_t reason = packet.readUInt32();
            std::string name;
            if (reason == 9 && packet.getReadPos() < packet.getSize()) name = packet.readString();
            const std::string maxLevel = "60";  // 0x3c, a constant in the original
            switch (reason) {
                case 1: gameError(owner_, ErrTo::Error, "ERR_REFER_A_FRIEND_NOT_REFERRED_BY", "You were not referred by that player."); break;
                case 2: gameError(owner_, ErrTo::Error, "ERR_REFER_A_FRIEND_TARGET_TOO_HIGH", "That player's level is too high."); break;
                case 3: gameError(owner_, ErrTo::Error, "ERR_REFER_A_FRIEND_INSUFFICIENT_GRANTABLE_LEVELS", "You have not earned enough levels to grant any more."); break;
                case 4: gameError(owner_, ErrTo::Error, "ERR_REFER_A_FRIEND_TOO_FAR", "That player is too far away."); break;
                case 5: gameError(owner_, ErrTo::Error, "ERR_REFER_A_FRIEND_DIFFERENT_FACTION", "You cannot grant a level to a character of the opposing faction."); break;
                case 6: gameError(owner_, ErrTo::Error, "ERR_REFER_A_FRIEND_NOT_NOW", "You cannot grant a level to that player right now."); break;
                case 7: gameError(owner_, ErrTo::Error, "ERR_REFER_A_FRIEND_GRANT_LEVEL_MAX_I", "You cannot grant levels to players level %d or higher.", {maxLevel}); break;
                case 8: gameError(owner_, ErrTo::Error, "ERR_GENERIC_NO_TARGET", "You have no target."); break;
                case 9: gameError(owner_, ErrTo::Chat, "ERR_TARGET_NOT_IN_GROUP_S", "%s is not in your party.", {name}); break;
                case 10: gameError(owner_, ErrTo::Error, "ERR_REFER_A_FRIEND_SUMMON_LEVEL_MAX_I", "You can only summon players of level %d or lower.", {maxLevel}); break;
                case 11: gameError(owner_, ErrTo::Error, "ERR_REFER_A_FRIEND_SUMMON_COOLDOWN", "You can only summon your friend once per hour."); break;
                case 12: gameError(owner_, ErrTo::Error, "ERR_REFER_A_FRIEND_INSUF_EXPAN_LVL", "Your friend's account does not have the expansion required."); break;
                case 13: gameError(owner_, ErrTo::Error, "ERR_REFER_A_FRIEND_SUMMON_OFFLINE_S", "%s is offline and cannot be summoned.", {name}); break;
                default: break;
            }
        }
        packet.skipAll();
    };
    table[Opcode::SMSG_REPORT_PVP_AFK_RESULT] = [this](network::Packet& packet) {
        // Only the reporting system being switched on (5) or off (6) is said,
        // as PVP_REPORT_AFK_SYSTEM_ENABLED / _DISABLED system lines
        // (0x006e2e90); any other result is silent. This announced every
        // report in English, or refused it on screen.
        if (packet.hasRemaining(1)) {
            const uint8_t result = packet.readUInt8();
            if (result == 5)
                owner_.addSystemChatMessage(interfaceString("PVP_REPORT_AFK_SYSTEM_ENABLED", "The notification system has been enabled."));
            else if (result == 6)
                owner_.addSystemChatMessage(interfaceString("PVP_REPORT_AFK_SYSTEM_DISABLED", "The notification system has been disabled."));
        }
        packet.skipAll();
    };
    table[Opcode::SMSG_INSTANCE_SAVE_CREATED] = [this](network::Packet& packet) {
        // INSTANCE_SAVED, a system line, for the zero the server sends
        // (0x0052bb10; a one is a debug-only lock notice). This printed
        // "You are now saved to this instance."
        const uint32_t kind = packet.hasRemaining(4) ? packet.readUInt32() : 0;
        if (kind == 0)
            owner_.addSystemChatMessage(interfaceString("INSTANCE_SAVED", "You are now saved to this instance"));
    };
    table[Opcode::SMSG_RAID_INSTANCE_MESSAGE] = [this](network::Packet& packet) {
        // The type, the map, its difficulty and a time, worded by 0x0050ca80:
        // 1 RAID_INSTANCE_WARNING_HOURS, 2 _MIN, 3 _MIN_SOON, 5
        // RAID_INSTANCE_EXPIRED - system lines - and 4, the welcome, as
        // RAID_INSTANCE_WELCOME for UIParent to word itself. The types were
        // read one place out here and answered in English.
        if (!packet.hasRemaining(12)) return;
        const uint32_t msgType = packet.readUInt32();
        const uint32_t mapId   = packet.readUInt32();
        const uint32_t diff    = packet.readUInt32();
        const uint32_t timeLeft = packet.hasRemaining(4) ? packet.readUInt32() : 0;
        const std::string mapLabel = instanceLabel(owner_, mapId, diff);
        switch (msgType) {
            case 1:
                owner_.addSystemChatMessage(interfaceFormat("RAID_INSTANCE_WARNING_HOURS",
                    "WARNING! Your save for '%s' will expire in %d |4hour:hours;.",
                    {mapLabel, std::to_string(timeLeft / 3600)}));
                break;
            case 2:
                owner_.addSystemChatMessage(interfaceFormat("RAID_INSTANCE_WARNING_MIN",
                    "WARNING! Your save for '%s' will expire in %d |4minute:minutes;!",
                    {mapLabel, std::to_string(timeLeft / 60)}));
                break;
            case 3:
                owner_.addSystemChatMessage(interfaceFormat("RAID_INSTANCE_WARNING_MIN_SOON",
                    "WARNING! Your save for '%s' will expire in %d |4minute:minutes;.",
                    {mapLabel, std::to_string(timeLeft / 60)}));
                break;
            case 4: {
                const bool locked   = packet.hasRemaining(1) && packet.readUInt8() != 0;
                const bool extended = packet.hasRemaining(1) && packet.readUInt8() != 0;
                if (owner_.addonEventCallbackRef())
                    owner_.addonEventCallbackRef()("RAID_INSTANCE_WELCOME", {
                        mapLabel, std::to_string(timeLeft), locked ? "1" : "0", extended ? "1" : "0"});
                break;
            }
            case 5:
                owner_.addSystemChatMessage(interfaceFormat("RAID_INSTANCE_EXPIRED",
                    "Your save for %s has expired.", {mapLabel}));
                break;
            default: break;
        }
    };
    table[Opcode::SMSG_INSTANCE_RESET] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(4)) return;
        uint32_t mapId = packet.readUInt32();
        auto it = std::remove_if(instanceLockouts_.begin(), instanceLockouts_.end(),
            [mapId](const InstanceLockout& lo){ return lo.mapId == mapId; });
        instanceLockouts_.erase(it, instanceLockouts_.end());
        // INSTANCE_RESET_SUCCESS, a system line naming the map from Map.dbc,
        // or its number (0x0050ccd0).
        std::string mapLabel = owner_.getMapName(mapId);
        if (mapLabel.empty()) mapLabel = std::to_string(mapId);
        owner_.addSystemChatMessage(interfaceFormat("INSTANCE_RESET_SUCCESS", "%s has been reset.", {mapLabel}));
    };
    table[Opcode::SMSG_INSTANCE_RESET_FAILED] = [this](network::Packet& packet) {
        // The reason first and then the map - read the other way round here -
        // and a system line for the three reasons 0x0050cda0 has words for:
        // INSTANCE_RESET_FAILED (players inside), _OFFLINE, _ZONING. This said
        // it on screen and in chat, in English.
        if (!packet.hasRemaining(8)) return;
        const uint32_t reason = packet.readUInt32();
        const uint32_t mapId  = packet.readUInt32();
        std::string mapLabel = owner_.getMapName(mapId);
        if (mapLabel.empty()) mapLabel = std::to_string(mapId);
        switch (reason) {
            case 0: owner_.addSystemChatMessage(interfaceFormat("INSTANCE_RESET_FAILED",
                        "Cannot reset %s.  There are players still inside the instance.", {mapLabel})); break;
            case 1: owner_.addSystemChatMessage(interfaceFormat("INSTANCE_RESET_FAILED_OFFLINE",
                        "Cannot reset %s.  There are players offline in your party.", {mapLabel})); break;
            case 2: owner_.addSystemChatMessage(interfaceFormat("INSTANCE_RESET_FAILED_ZONING",
                        "Cannot reset %s.  There are players in your party attempting to zone into an instance.", {mapLabel})); break;
            default: break;
        }
    };
    table[Opcode::SMSG_INSTANCE_LOCK_WARNING_QUERY] = [this](network::Packet& packet) {
        // Nine bytes: a countdown in milliseconds, the mask of encounters
        // already completed in this instance, and whether the player was
        // previously saved to it.
        //
        // What was here read a map id, a difficulty, a time and a flag across
        // seventeen bytes, so the guard alone kept the handler from ever
        // running against the nine the server sends. It also answered on the
        // player's behalf - always accept - which is the one thing this packet
        // is asking them.
        if (!packet.hasRemaining(9)) { packet.skipAll(); return; }
        const uint32_t timerMs = packet.readUInt32();
        const uint32_t completedMask = packet.readUInt32();
        const uint8_t previouslySaved = packet.readUInt8();

        instanceLock_.active = true;
        instanceLock_.secondsLeft = static_cast<float>(timerMs) / 1000.0f;
        instanceLock_.previouslySaved = (previouslySaved != 0);
        instanceLock_.completedEncounterMask = completedMask;

        LOG_INFO("SMSG_INSTANCE_LOCK_WARNING_QUERY: ", timerMs / 1000,
                 "s to decide, completed mask 0x", std::hex, completedMask, std::dec,
                 previouslySaved ? ", previously saved" : "");

        owner_.fireAddonEvent("INSTANCE_LOCK_START", {});
    };

    // ---- LFG ----
    table[Opcode::SMSG_LFG_JOIN_RESULT] = [this](network::Packet& packet) { handleLfgJoinResult(packet); };
    table[Opcode::SMSG_LFG_QUEUE_STATUS] = [this](network::Packet& packet) { handleLfgQueueStatus(packet); };
    table[Opcode::SMSG_LFG_PROPOSAL_UPDATE] = [this](network::Packet& packet) { handleLfgProposalUpdate(packet); };
    table[Opcode::SMSG_LFG_ROLE_CHECK_UPDATE] = [this](network::Packet& packet) { handleLfgRoleCheckUpdate(packet); };
    // Two opcodes, two bodies. They were sharing a handler, and the party one
    // carries a join flag and three more bytes of padding the player one does
    // not - so every party update read the wrong byte as the queue flag.
    table[Opcode::SMSG_LFG_UPDATE_PLAYER] = [this](network::Packet& packet) { handleLfgUpdatePlayer(packet); };
    table[Opcode::SMSG_LFG_UPDATE_PARTY]  = [this](network::Packet& packet) { handleLfgUpdateParty(packet); };
    table[Opcode::SMSG_LFG_PLAYER_REWARD] = [this](network::Packet& packet) { handleLfgPlayerReward(packet); };
    table[Opcode::SMSG_LFG_BOOT_PROPOSAL_UPDATE] = [this](network::Packet& packet) { handleLfgBootProposalUpdate(packet); };
    table[Opcode::SMSG_LFG_TELEPORT_DENIED] = [this](network::Packet& packet) { handleLfgTeleportDenied(packet); };
    table[Opcode::SMSG_LFG_DISABLED] = [this](network::Packet& /*packet*/) {
        // 0x0055bdc0 case 0x398: everything the dungeon finder held is
        // dropped - the proposal, both queue bodies, the locks - then
        // LFG_UPDATE false and ERR_SYSTEM_DISABLED.
        clearLfgProposal();
        lfgPlayerUpdate_ = LfgUpdateBody{};
        lfgPartyUpdate_ = LfgUpdateBody{};
        lfgLocks_.clear();
        lfgPartyLocks_.clear();
        lfgState_ = LfgState::None;
        owner_.fireAddonEvent("LFG_UPDATE", {eventBool(false)});
        gameError(owner_, ErrTo::Error, "ERR_SYSTEM_DISABLED", "This system is currently disabled.");
    };
    table[Opcode::SMSG_LFG_OFFER_CONTINUE] = [this](network::Packet& packet) {
        // uint32 dungeon entry, which the handler used to skip entirely - and
        // then fired the event with nothing. lfgframe.lua opens with
        // NORMAL_FONT_COLOR_CODE..displayName.."|r", and concatenating nil
        // raises, so the offer took its own handler down every time.
        //
        // The entry packs both halves the way LFGDungeonData::Entry builds it:
        // id in the low twenty-four bits, type in the top eight.
        uint32_t dungeonId = 0, typeId = 0;
        if (packet.hasRemaining(4)) {
            const uint32_t entry = packet.readUInt32();
            dungeonId = entry & 0x00FFFFFFu;
            typeId    = (entry >> 24) & 0xFFu;
        }
        // LFG_OFFER_CONTINUE alone; the dialog it opens says the rest
        // (0x0055bdc0). This printed "Dungeon Finder: You may continue ...".
        std::string name = owner_.getLfgDungeonName(dungeonId);
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("LFG_OFFER_CONTINUE",
                                           {name, std::to_string(dungeonId),
                                            std::to_string(typeId)});
        }
    };
    table[Opcode::SMSG_LFG_ROLE_CHOSEN] = [this](network::Packet& packet) {
        if (!packet.hasRemaining(13)) { packet.skipAll(); return; }
        uint64_t roleGuid = packet.readUInt64();
        uint8_t  ready    = packet.readUInt8();
        uint32_t roles    = packet.readUInt32();
        std::string pName = owner_.lookupName(roleGuid);
        if (pName.empty())
            if (auto e = owner_.getEntityManager().getEntity(roleGuid))
                if (auto u = std::dynamic_pointer_cast<Unit>(e))
                    pName = u->getName();
        // Nothing said for a choice; a refusal is
        // ERR_LFG_PLAYER_DECLINED_ROLE_CHECK, a system line naming the player
        // (0x0055bdc0, case 699), and the event is for a choice only. This
        // printed "X has chosen: Tank Healer" in English.
        if (pName.empty()) { packet.skipAll(); return; }
        if (!ready) {
            gameError(owner_, ErrTo::Chat, "ERR_LFG_PLAYER_DECLINED_ROLE_CHECK",
                      "%s has declined the role check.", {pName});
            packet.skipAll();
            return;
        }
        // The role-check window lists who has picked what as they pick it,
        // and it learns that from this event alone; it was never fired, so
        // the list stayed blank.
        //
        // Booleans through eventBool: "0" is a number and true in Lua, so a
        // false argument in front of a true one cannot be spelled as text.
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("LFG_ROLE_CHECK_ROLE_CHOSEN", {
                pName,
                eventBool((roles & 0x02) != 0),
                eventBool((roles & 0x04) != 0),
                eventBool((roles & 0x08) != 0)
            });
        }
        packet.skipAll();
    };
    // The one that carries the locks. Skipped wholesale until now, which is
    // why the dungeon finder listed every dungeon as queueable and left the
    // server to refuse.
    table[Opcode::SMSG_LFG_PLAYER_INFO] = [this](network::Packet& packet) {
        handleLfgPlayerInfo(packet);
    };
    table[Opcode::SMSG_LFG_PARTY_INFO] = [this](network::Packet& packet) {
        handleLfgPartyInfo(packet);
    };
    for (auto op : { Opcode::SMSG_LFG_UPDATE_SEARCH, Opcode::SMSG_UPDATE_LFG_LIST }) {
        table[op] = [](network::Packet& packet) { packet.skipAll(); };
    }
    table[Opcode::SMSG_OPEN_LFG_DUNGEON_FINDER] = [this](network::Packet& packet) {
        packet.skipAll();
        if (owner_.openLfgCallbackRef()) owner_.openLfgCallbackRef()();
    };

    // ---- Arena ----
    table[Opcode::SMSG_ARENA_TEAM_COMMAND_RESULT] = [this](network::Packet& packet) { handleArenaTeamCommandResult(packet); };
    table[Opcode::SMSG_ARENA_TEAM_QUERY_RESPONSE] = [this](network::Packet& packet) { handleArenaTeamQueryResponse(packet); };
    table[Opcode::SMSG_ARENA_TEAM_ROSTER] = [this](network::Packet& packet) { handleArenaTeamRoster(packet); };
    table[Opcode::SMSG_ARENA_TEAM_INVITE] = [this](network::Packet& packet) { handleArenaTeamInvite(packet); };
    table[Opcode::SMSG_ARENA_TEAM_EVENT] = [this](network::Packet& packet) { handleArenaTeamEvent(packet); };
    table[Opcode::SMSG_ARENA_TEAM_STATS] = [this](network::Packet& packet) { handleArenaTeamStats(packet); };
    table[Opcode::SMSG_ARENA_ERROR] = [this](network::Packet& packet) { handleArenaError(packet); };
    table[Opcode::MSG_PVP_LOG_DATA] = [this](network::Packet& packet) { handlePvpLogData(packet); };

    // ---- Factions / group leader ----
    table[Opcode::SMSG_INITIALIZE_FACTIONS] = [this](network::Packet& p) { handleInitializeFactions(p); };
    table[Opcode::SMSG_SET_FACTION_STANDING] = [this](network::Packet& p) { handleSetFactionStanding(p); };
    table[Opcode::SMSG_SET_FACTION_ATWAR] = [this](network::Packet& p) { handleSetFactionAtWar(p); };
    table[Opcode::SMSG_SET_FACTION_VISIBLE] = [this](network::Packet& p) { handleSetFactionVisible(p); };
    table[Opcode::SMSG_GROUP_SET_LEADER] = [this](network::Packet& p) { handleGroupSetLeader(p); };
}

// ============================================================
// Non-inline accessors requiring GameHandler
// ============================================================

std::string SocialHandler::getWhoAreaName(uint32_t zoneId) const {
    return owner_.getAreaName(zoneId);
}

std::string SocialHandler::getCurrentLfgDungeonName() const {
    return owner_.getLfgDungeonName(lfgDungeonId_);
}

uint32_t SocialHandler::ownGuildId() const {
    // The player's own field first: it is what the server keeps current, and
    // the character list's copy was taken at login - before any guild joined
    // or left since.
    if (const uint32_t live = getEntityGuildId(owner_.getPlayerGuid())) return live;
    const Character* ch = owner_.getActiveCharacter();
    return ch ? ch->guildId : 0;
}

bool SocialHandler::isInGuild() const {
    // Three records, and the newest wins: a guild joined this session is in
    // none of the others until the next login.
    if (inGuild_) return true;
    if (!guildName_.empty()) return true;
    return ownGuildId() != 0;
}

uint32_t SocialHandler::getEntityGuildId(uint64_t guid) const {
    auto entity = owner_.getEntityManager().getEntity(guid);
    if (!entity || entity->getType() != ObjectType::PLAYER) return 0;
    const uint16_t ufUnitEnd = fieldIndex(UF::UNIT_END);
    if (ufUnitEnd == 0xFFFF) return 0;
    return entity->getField(ufUnitEnd + 3);
}

std::optional<GuildEmblem> SocialHandler::lookupGuildEmblem(uint32_t guildId) {
    if (guildId == 0) return std::nullopt;
    if (auto it = guildEmblems_.find(guildId); it != guildEmblems_.end()) return it->second;
    const auto now = std::chrono::steady_clock::now();
    auto pendingIt = pendingGuildNameQueries_.find(guildId);
    if (pendingIt == pendingGuildNameQueries_.end() ||
        std::chrono::duration_cast<std::chrono::seconds>(now - pendingIt->second).count() >= 2) {
        pendingGuildNameQueries_[guildId] = now;
        queryGuildInfo(guildId);
    }
    return std::nullopt;
}

const std::string& SocialHandler::lookupGuildName(uint32_t guildId) {
    if (guildId == 0) return kEmptyString;
    auto it = guildNameCache_.find(guildId);
    if (it != guildNameCache_.end()) return it->second;

    const auto now = std::chrono::steady_clock::now();
    auto pendingIt = pendingGuildNameQueries_.find(guildId);
    const bool shouldQuery =
        pendingIt == pendingGuildNameQueries_.end() ||
        std::chrono::duration_cast<std::chrono::seconds>(now - pendingIt->second).count() >= 2;
    if (shouldQuery) {
        pendingGuildNameQueries_[guildId] = now;
        queryGuildInfo(guildId);
    }
    return kEmptyString;
}

// ============================================================
// Inspection
// ============================================================

void SocialHandler::inspectTarget() { inspectUnit(owner_.getTargetGuid()); }

// Any player, not only the targeted one: FrameXML's unit menus name the unit
// the menu was opened on, and on a party frame that is not the target.
void SocialHandler::requestInspectHonorData(uint64_t guid) {
    if (guid == 0) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    const uint32_t wire = wireOpcode(Opcode::MSG_INSPECT_HONOR_STATS);
    if (wire == 0xFFFF) return;
    network::Packet pkt(static_cast<uint16_t>(wire));
    pkt.writeUInt64(guid);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::inspectUnit(uint64_t guid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) {
        LOG_WARNING("Cannot inspect: not in world or not connected");
        return;
    }
    // Nothing is said for a unit that cannot be inspected, nor for the
    // request going out: NotifyInspect (0x00522870) returns quietly when
    // CanInspect fails. These were English lines - "You must target a player
    // to inspect.", "You can only inspect players.", "Inspecting X...".
    if (guid == 0) return;
    auto entity = owner_.getEntityManager().getEntity(guid);
    if (!entity || entity->getType() != ObjectType::PLAYER) return;
    auto packet = InspectPacket::build(guid);
    owner_.getSocket()->send(packet);
    if (isActiveExpansion("wotlk")) {
        auto achPkt = QueryInspectAchievementsPacket::build(guid);
        owner_.getSocket()->send(achPkt);
    }
    auto player = std::static_pointer_cast<Player>(entity);
    LOG_INFO("Sent inspect request for player: ", player->getName(), " (GUID: 0x", std::hex, guid, std::dec, ")");
}

void SocialHandler::handleInspectResults(network::Packet& packet) {
    if (isActiveExpansion("tbc") && packet.getOpcode() == wireOpcode(Opcode::SMSG_INSPECT_RESULTS_UPDATE)) {
        uint64_t guid = 0;
        std::array<uint32_t, 19> items{};
        std::array<uint16_t, 19> enchantIds{};
        if (parseInspectEquipmentPayload(packet, guid, items, enchantIds)) {
            owner_.cacheInspectedPlayerEquipment(guid, items);

            auto entity = owner_.getEntityManager().getEntity(guid);
            std::string playerName = "Target";
            if (entity) {
                auto player = std::dynamic_pointer_cast<Player>(entity);
                if (player && !player->getName().empty()) playerName = player->getName();
            }

            // TBC servers send inspected gear separately from talent data. Make the
            // gear-only response visible to the Inspect window immediately while
            // preserving talents if a matching talent response already arrived.
            if (inspectResult_.guid != guid) {
                inspectResult_ = InspectResult{};
                inspectResult_.guid = guid;
            }
            inspectResult_.playerName = playerName;
            inspectResult_.itemEntries = items;
            inspectResult_.enchantIds = enchantIds;

            LOG_INFO("SMSG_INSPECT_RESULTS_UPDATE (TBC gear): ", playerName, " has gear in ",
                     std::count_if(items.begin(), items.end(),
                                   [](uint32_t e) { return e != 0; }), "/19 slots");
            if (owner_.addonEventCallbackRef()) {
                char guidBuf[32];
                snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)guid);
                owner_.addonEventCallbackRef()("INSPECT_READY", {guidBuf});
                // INSPECT_READY is a later expansion's name and nothing in this
                // interface listens for it. The inspect paperdoll refreshes on
                // UNIT_INVENTORY_CHANGED for the unit it is showing, so the gear
                // arrived and the window was never told - it kept whatever it had
                // when it opened. Fired for the unit token this guid answers to,
                // and only when it answers to one: an inspected player who is not
                // the target has no token for the interface to match against.
                const std::string inspectUnit = owner_.guidToUnitId(guid);
                if (!inspectUnit.empty()) {
                    owner_.addonEventCallbackRef()("UNIT_INVENTORY_CHANGED", {inspectUnit});
                }
            }
            return;
        }
        LOG_DEBUG("SMSG_INSPECT_RESULTS_UPDATE (TBC gear): unrecognized payload size=",
                  packet.getSize());
    }

    if (isActiveExpansion("tbc") &&
        packet.getOpcode() == wireOpcode(Opcode::SMSG_INSPECT_TALENT)) {
        if (!packet.hasFullPackedGuid()) return;
        uint64_t guid = packet.readPackedGuid();
        if (guid == 0) return;

        auto entity = owner_.getEntityManager().getEntity(guid);
        std::string playerName = "Target";
        if (entity) {
            auto player = std::dynamic_pointer_cast<Player>(entity);
            if (player && !player->getName().empty()) playerName = player->getName();
        }

        if (inspectResult_.guid != guid) {
            inspectResult_ = InspectResult{};
            inspectResult_.guid = guid;
        }
        inspectResult_.playerName = playerName;

        uint32_t unspentTalents = 0;
        uint32_t spentTalents = 0;
        std::array<uint32_t, 3> treePoints{};
        bool hasTreePoints = false;
        const uint8_t classId = owner_.lookupPlayerClass(guid);
        const size_t talentPayloadStart = packet.getReadPos();
        if (parseTbcInspectTalentPayload(packet, owner_, classId, unspentTalents, spentTalents,
                                         treePoints, hasTreePoints)) {
            inspectResult_.totalTalents = spentTalents;
            inspectResult_.unspentTalents = unspentTalents;
            inspectResult_.hasTalentData = true;
            inspectResult_.hasTalentTreePoints = hasTreePoints;
            inspectResult_.talentTreePoints = treePoints;
            inspectResult_.talentGroups = 1;
            inspectResult_.activeTalentGroup = 0;
        } else {
            packet.setReadPos(talentPayloadStart);
            LOG_INFO("SMSG_INSPECT_TALENT (TBC): ", playerName,
                     " unparsed payload bytes=", packet.getRemainingSize());
        }

        auto gearIt = owner_.inspectedPlayerItemEntriesRef().find(guid);
        if (gearIt != owner_.inspectedPlayerItemEntriesRef().end()) {
            inspectResult_.itemEntries = gearIt->second;
        }

        LOG_INFO("SMSG_INSPECT_TALENT (TBC): ", playerName,
                 " gearCached=", (gearIt != owner_.inspectedPlayerItemEntriesRef().end() ? "yes" : "no"),
                 " payload bytes remaining=", packet.getRemainingSize());
        if (owner_.addonEventCallbackRef()) {
            char guidBuf[32];
            snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)guid);
            owner_.addonEventCallbackRef()("INSPECT_READY", {guidBuf});
            const std::string inspectUnit = owner_.guidToUnitId(guid);
            if (!inspectUnit.empty()) {
                owner_.addonEventCallbackRef()("UNIT_INVENTORY_CHANGED", {inspectUnit});
            }
        }
        return;
    }

    // SMSG_INSPECT_TALENT (WotLK) is a packed guid, then
    // BuildPlayerTalentsInfoData and BuildEnchantmentsInfoData. There is no
    // leading discriminator, and reading one ate the packed guid's mask byte
    // and put everything after it a byte out.
    //
    // The branch that used to sit behind that byte re-parsed the player's own
    // talents into a second set of members on GameHandler. Nothing read them -
    // SpellHandler keeps the ones the talent frame draws from - and this
    // opcode never carries them anyway.
    const bool talentTbc = isPreWotlk();
    if (packet.getRemainingSize() < (talentTbc ? 8u : 2u)) return;
    uint64_t guid = talentTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    if (guid == 0) return;

    size_t bytesLeft = packet.getRemainingSize();
    if (bytesLeft < 6) {
        // Before WotLK the guid is the whole answer - a 1.12 realm sends eight
        // bytes and nothing after them, which is the shape and not a
        // truncation. Reported as one, every inspect on turtle wrote a warning
        // into a log that is read for faults.
        if (talentTbc) {
            LOG_INFO("Inspect: this realm answers with the guid alone, so there "
                     "are no talents to show");
        } else {
            LOG_WARNING("SMSG_TALENTS_INFO: too short after guid, ", bytesLeft, " bytes");
        }
        // Said in the log only - this printed "Inspecting X (no talent data
        // available)." into chat, which the original has no line for.
        return;
    }

    uint32_t unspentTalents = packet.readUInt32();
    uint8_t talentGroupCount = packet.readUInt8();
    uint8_t activeTalentGroup = packet.readUInt8();

    auto entity = owner_.getEntityManager().getEntity(guid);
    std::string playerName = "Target";
    if (entity) {
        auto player = std::dynamic_pointer_cast<Player>(entity);
        if (player && !player->getName().empty()) playerName = player->getName();
    }

    // Every spec is sent. Counting all of them reported a dual-spec character
    // as having spent roughly twice what they have.
    uint32_t totalTalents = 0;
    std::unordered_map<uint32_t, uint8_t> inspectTalents;
    for (uint8_t g = 0; g < talentGroupCount && g < 2; ++g) {
        bytesLeft = packet.getRemainingSize();
        if (bytesLeft < 1) break;
        uint8_t talentCount = packet.readUInt8();
        for (uint8_t t = 0; t < talentCount; ++t) {
            bytesLeft = packet.getRemainingSize();
            if (bytesLeft < 5) break;
            const uint32_t talentId = packet.readUInt32();
            const uint8_t rank = packet.readUInt8();
            if (g == activeTalentGroup) {
                // Rank is zero-based on the wire, as it is for the player's own
                // talents. The inspect talent tab was counting these and
                // throwing them away, so it drew the viewer's own tree with the
                // target's name over it.
                inspectTalents[talentId] = static_cast<uint8_t>(rank + 1u);
                totalTalents++;
            }
        }
        bytesLeft = packet.getRemainingSize();
        if (bytesLeft < 1) break;
        uint8_t glyphCount = packet.readUInt8();
        for (uint8_t gl = 0; gl < glyphCount; ++gl) {
            bytesLeft = packet.getRemainingSize();
            if (bytesLeft < 2) break;
            packet.readUInt16();
        }
    }

    // BuildEnchantmentsInfoData, as 0x006cdf30 reads it.
    std::array<uint32_t, 19> itemEntries{};
    std::array<uint16_t, 19> enchantIds{};
    const bool sawGear = readInspectEnchantments(packet, itemEntries, enchantIds);

    // A different player means the honour figures in hand are somebody else's.
    // The fields here are assigned one at a time rather than as a whole struct,
    // so anything not named survives - and the honour tab reads
    // HasInspectHonorData to decide whether to ask, so a stale true would show
    // the last player's kills against this one's name and never correct itself.
    if (inspectResult_.guid != guid) {
        inspectResult_.hasHonorData = false;
        inspectResult_.honorTodayKills = 0;
        inspectResult_.honorYesterdayKills = 0;
        inspectResult_.honorTodayContribution = 0;
        inspectResult_.honorYesterdayContribution = 0;
        inspectResult_.honorLifetimeKills = 0;
        inspectResult_.honorRank = 0;
    }
    inspectResult_.guid              = guid;
    inspectResult_.playerName        = playerName;
    inspectResult_.totalTalents      = totalTalents;
    inspectResult_.unspentTalents    = unspentTalents;
    inspectResult_.hasTalentData     = true;
    inspectResult_.hasTalentTreePoints = false;
    inspectResult_.talentTreePoints  = {};
    inspectResult_.talentGroups      = talentGroupCount;
    inspectResult_.activeTalentGroup = activeTalentGroup;
    inspectResult_.enchantIds        = enchantIds;
    inspectResult_.talentRanks       = std::move(inspectTalents);
    inspectResult_.classId           = owner_.lookupPlayerClass(guid);

    // The gear is in this packet. It used to be looked up from the cache that
    // Classic's SMSG_INSPECT and TBC's SMSG_INSPECT_RESULTS_UPDATE fill, and
    // nothing filled it on WotLK, so inspecting anyone showed an empty
    // paperdoll however well equipped they were.
    if (sawGear) {
        inspectResult_.itemEntries = itemEntries;
        owner_.cacheInspectedPlayerEquipment(guid, itemEntries);
    } else {
        auto gearIt = owner_.inspectedPlayerItemEntriesRef().find(guid);
        inspectResult_.itemEntries =
            (gearIt != owner_.inspectedPlayerItemEntriesRef().end()) ? gearIt->second
                                                                     : std::array<uint32_t, 19>{};
    }

    LOG_INFO("Inspect results for ", playerName, ": ", totalTalents, " talents, ",
             unspentTalents, " unspent, ", (int)talentGroupCount, " specs");
    if (owner_.addonEventCallbackRef()) {
        char guidBuf[32];
        snprintf(guidBuf, sizeof(guidBuf), "0x%016llX", (unsigned long long)guid);
        owner_.addonEventCallbackRef()("INSPECT_READY", {guidBuf});
        // The same packet carries the talents, and the talent tab of the
        // inspect window redraws on this event alone. Only INSPECT_READY was
        // fired, which is what the gear tab listens for - so inspecting anyone
        // filled the paperdoll and left the talent tab blank, however
        // completely the ranks above had been parsed.
        owner_.addonEventCallbackRef()("INSPECT_TALENT_READY", {});
        const std::string inspectUnit = owner_.guidToUnitId(guid);
        if (!inspectUnit.empty()) {
            owner_.addonEventCallbackRef()("UNIT_INVENTORY_CHANGED", {inspectUnit});
        }
    }
}

// ============================================================
// Server Info / Who / Social
// ============================================================

void SocialHandler::queryServerTime(bool announce) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    announceServerTime_ = announce;
    auto packet = QueryTimePacket::build();
    owner_.getSocket()->send(packet);
    LOG_INFO("Requested server time");
}

void SocialHandler::requestPlayedTime() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = RequestPlayedTimePacket::build(true);
    owner_.getSocket()->send(packet);
    LOG_INFO("Requested played time");
}

void SocialHandler::queryWho(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = WhoPacket::build(0, 0, playerName);
    owner_.getSocket()->send(packet);
    LOG_INFO("Sent WHO query", playerName.empty() ? "" : " for: " + playerName);
}

void SocialHandler::addFriend(const std::string& playerName, const std::string& note) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // The request goes out without a word (0x006b6e10); the server's answer,
    // SMSG_FRIEND_STATUS, is what is said. This printed "Sending friend
    // request to X..." and refused an empty name in English.
    if (playerName.empty()) return;
    auto packet = AddFriendPacket::build(playerName, note);
    owner_.getSocket()->send(packet);
    LOG_INFO("Sent friend request to: ", playerName);
}

void SocialHandler::removeFriend(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // A name not on the list is ERR_FRIEND_NOT_FOUND, a system line, and the
    // removal itself goes out silently (0x006b6f00). These were English.
    if (playerName.empty()) return;
    auto it = owner_.friendsCacheRef().find(playerName);
    if (it == owner_.friendsCacheRef().end()) {
        gameError(owner_, ErrTo::Chat, "ERR_FRIEND_NOT_FOUND", "Player not found.");
        return;
    }
    auto packet = DelFriendPacket::build(it->second);
    owner_.getSocket()->send(packet);
    LOG_INFO("Sent remove friend request for: ", playerName);
}

void SocialHandler::setFriendNote(const std::string& playerName, const std::string& note) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // As for a removal (0x006b6f70): ERR_FRIEND_NOT_FOUND for a stranger,
    // and nothing for the note being set.
    if (playerName.empty()) return;
    auto it = owner_.friendsCacheRef().find(playerName);
    if (it == owner_.friendsCacheRef().end()) {
        gameError(owner_, ErrTo::Chat, "ERR_FRIEND_NOT_FOUND", "Player not found.");
        return;
    }
    auto packet = SetContactNotesPacket::build(it->second, note);
    owner_.getSocket()->send(packet);
    LOG_INFO("Set friend note for: ", playerName);
}

void SocialHandler::addIgnore(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // Silent until the server answers with SMSG_FRIEND_STATUS.
    if (playerName.empty()) return;
    auto packet = AddIgnorePacket::build(playerName);
    owner_.getSocket()->send(packet);
    LOG_INFO("Sent ignore request for: ", playerName);
}

void SocialHandler::removeIgnore(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // ERR_IGNORE_NOT_FOUND for a name not on the list (0x006b7040), and
    // nothing for the request going out. These were English.
    if (playerName.empty()) return;
    auto it = owner_.ignoreCacheRef().find(playerName);
    if (it == owner_.ignoreCacheRef().end()) {
        gameError(owner_, ErrTo::Chat, "ERR_IGNORE_NOT_FOUND", "Player not found.");
        return;
    }
    auto packet = DelIgnorePacket::build(it->second);
    owner_.getSocket()->send(packet);
    // Don't erase from ignoreCache here - wait for the server's SMSG_IGNORE_LIST
    // response to confirm. Erasing optimistically desyncs the cache if the server
    // rejects the request. (Compare with removeFriend which also waits for
    // SMSG_FRIEND_STATUS before updating its cache.)
    owner_.ignoreListGuidsRef().erase(it->second);
    LOG_INFO("Sent remove ignore request for: ", playerName);
}

void SocialHandler::randomRoll(uint32_t minRoll, uint32_t maxRoll) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (minRoll > maxRoll) std::swap(minRoll, maxRoll);
    if (maxRoll > 10000) maxRoll = 10000;
    auto packet = RandomRollPacket::build(minRoll, maxRoll);
    owner_.getSocket()->send(packet);
    LOG_INFO("Rolled ", minRoll, "-", maxRoll);
}

// ============================================================
// Logout
// ============================================================

void SocialHandler::requestLogout(bool exitAfterLogout) {
    // Exit Game has to close the program. Everything below is about leaving the
    // world tidily first, which is worth doing and is not worth being trapped
    // by: each way the request cannot be made is answered by going anyway.
    //
    // With no connection there is nobody to ask. At the login screen, or after
    // a disconnect, the button did nothing whatsoever.
    if (!owner_.getSocket()) {
        if (exitAfterLogout && owner_.logoutCompleteCallbackRef()) {
            owner_.logoutCompleteCallbackRef()(true);
        }
        return;
    }
    if (loggingOut_) {
        // Asked a second time while a logout is already running. For Exit that
        // is the player saying they would rather not wait, which is exactly
        // what ForceQuit is described as doing and what the countdown popup's
        // own button is for.
        if (exitAfterLogout) {
            LOG_WARNING("Exit Game asked for during a logout - closing now");
            if (owner_.logoutCompleteCallbackRef()) owner_.logoutCompleteCallbackRef()(true);
            return;
        }
        // Nothing said: the countdown popup is already up. This printed
        // "Already logging out."
        return;
    }
    auto packet = LogoutRequestPacket::build();
    owner_.getSocket()->send(packet);
    loggingOut_ = true;
    exitAfterLogout_ = exitAfterLogout;
    LOG_WARNING("Sent logout request (exitAfterLogout=", exitAfterLogout ? "yes" : "no", ")");  // temporary
}

void SocialHandler::cancelLogout() {
    if (!owner_.getSocket()) return;
    // No line either way: the original's cancel answers with LOGOUT_CANCEL
    // (event 0x105, 0x006b0900), which closes the countdown popup. This
    // printed "Not currently logging out." and "Logout cancelled."
    if (!loggingOut_) return;
    auto packet = LogoutCancelPacket::build();
    owner_.getSocket()->send(packet);
    loggingOut_ = false;
    exitAfterLogout_ = false;
    logoutCountdown_ = 0.0f;
    LOG_INFO("Cancelled logout");
}

// ============================================================
// Guild
// ============================================================

void SocialHandler::requestGuildInfo() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildInfoPacket::build();
    owner_.getSocket()->send(packet);
}

void SocialHandler::requestGuildRoster() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildRosterPacket::build();
    owner_.getSocket()->send(packet);
    // Said in the log, not to the player. This is asked for far more often
    // than a person would ask for it: eight places here request a roster after
    // a guild change, and the interface asks whenever the mail frame or the
    // friends frame opens with an empty one. Every one of those put a line in
    // chat that the real client never writes.
    LOG_DEBUG("Requested guild roster");
}

// The longer "guild information" text, which is a different field from the
// message of the day and has its own opcode.
void SocialHandler::setGuildInfoText(const std::string& text) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_INFO_TEXT));
    packet.writeString(text);
    owner_.getSocket()->send(packet);
    requestGuildRoster();
}

// Turn a letter into a readable item, which is what "take" does to a mail whose
// body is the thing being kept.
void SocialHandler::takeInboxTextItem(uint32_t mailId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_MAIL_CREATE_TEXT_ITEM));
    packet.writeUInt64(0);            // mailbox guid - the server uses the open one
    packet.writeUInt32(mailId);
    owner_.getSocket()->send(packet);
}

void SocialHandler::setGuildMotd(const std::string& motd) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // No line of its own: the new message comes back as the GUILD_MOTD event,
    // which the chat frame prints. This said "Guild MOTD updated." besides.
    auto packet = GuildMotdPacket::build(motd);
    owner_.getSocket()->send(packet);
}

void SocialHandler::promoteGuildMember(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // The guild commands go out silently and the server's answer is what is
    // said - SMSG_GUILD_EVENT or SMSG_GUILD_COMMAND_RESULT. These printed
    // "Promoting X...", "Demoting X...", "Leaving guild...", "Inviting X to
    // guild..." and refused an empty name in English.
    if (playerName.empty()) return;
    auto packet = GuildPromotePacket::build(playerName);
    owner_.getSocket()->send(packet);
}

void SocialHandler::demoteGuildMember(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (playerName.empty()) return;
    auto packet = GuildDemotePacket::build(playerName);
    owner_.getSocket()->send(packet);
}

void SocialHandler::leaveGuild() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildLeavePacket::build();
    owner_.getSocket()->send(packet);
}

void SocialHandler::inviteToGuild(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (playerName.empty()) return;
    auto packet = GuildInvitePacket::build(playerName);
    owner_.getSocket()->send(packet);
}

void SocialHandler::kickGuildMember(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildRemovePacket::build(playerName);
    owner_.getSocket()->send(packet);
}

void SocialHandler::disbandGuild() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildDisbandPacket::build();
    owner_.getSocket()->send(packet);
}

void SocialHandler::setGuildLeader(const std::string& name) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildLeaderPacket::build(name);
    owner_.getSocket()->send(packet);
}

void SocialHandler::setGuildPublicNote(const std::string& name, const std::string& note) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildSetPublicNotePacket::build(name, note);
    owner_.getSocket()->send(packet);
}

void SocialHandler::setGuildOfficerNote(const std::string& name, const std::string& note) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildSetOfficerNotePacket::build(name, note);
    owner_.getSocket()->send(packet);
}

void SocialHandler::acceptGuildInvite() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    pendingGuildInvite_ = false;
    // What the invite called it, until the guild names itself: the roster that
    // confirms the join does not carry a name, and the query that does needs
    // an id the player only has once the server has moved them.
    joinedGuildName_ = pendingGuildInviteGuildName_;
    auto packet = GuildAcceptPacket::build();
    owner_.getSocket()->send(packet);
    // ...and the roster, which is what fills the social window's guild tab.
    // The server sends the join event to the guild rather than a roster to the
    // joiner, so nothing arrived unless something asked.
    requestGuildRoster();
}

void SocialHandler::declineGuildInvite() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    pendingGuildInvite_ = false;
    auto packet = GuildDeclineInvitationPacket::build();
    owner_.getSocket()->send(packet);
}

void SocialHandler::noteGuildTimestamp(uint32_t guildId, uint32_t timestamp) {
    if (guildId == 0) return;
    auto [it, fresh] = guildTimestamps_.try_emplace(guildId, timestamp);
    if (!fresh && it->second == timestamp) return;
    it->second = timestamp;
    // The record stays until the answer replaces it; a design that has not
    // changed paints nothing new.
    pendingGuildNameQueries_[guildId] = std::chrono::steady_clock::now();
    queryGuildInfo(guildId);
}

void SocialHandler::queryGuildInfo(uint32_t guildId) {
    // Allow guild queries at the character screen too - the socket is
    // connected and the server accepts CMSG_GUILD_QUERY before login.
    if (!owner_.getSocket()) return;
    auto packet = GuildQueryPacket::build(guildId);
    owner_.getSocket()->send(packet);
}

void SocialHandler::createGuild(const std::string& guildName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildCreatePacket::build(guildName);
    owner_.getSocket()->send(packet);
}

void SocialHandler::addGuildRank(const std::string& rankName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildAddRankPacket::build(rankName);
    owner_.getSocket()->send(packet);
    requestGuildRoster();
}

void SocialHandler::deleteGuildRank() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildDelRankPacket::build();
    owner_.getSocket()->send(packet);
    requestGuildRoster();
}

void SocialHandler::requestPetitionShowlist(uint64_t npcGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = PetitionShowlistPacket::build(npcGuid);
    owner_.getSocket()->send(packet);
}

void SocialHandler::buyPetition(uint64_t npcGuid, const std::string& guildName,
                                uint32_t clientIndex) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = PetitionBuyPacket::build(npcGuid, guildName, clientIndex);
    owner_.getSocket()->send(packet);
}

void SocialHandler::signPetition(uint64_t petitionGuid) {
    if (!owner_.getSocket() || owner_.getState() != WorldState::IN_WORLD) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_PETITION_SIGN));
    pkt.writeUInt64(petitionGuid);
    pkt.writeUInt8(0);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::turnInPetition(uint64_t petitionGuid) {
    if (!owner_.getSocket() || owner_.getState() != WorldState::IN_WORLD) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_TURN_IN_PETITION));
    pkt.writeUInt64(petitionGuid);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::offerPetition(uint64_t petitionGuid, uint64_t targetGuid) {
    if (!owner_.getSocket() || owner_.getState() != WorldState::IN_WORLD) return;
    if (petitionGuid == 0 || targetGuid == 0) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_OFFER_PETITION));
    // The leading dword is not the petition type despite where it sits - the
    // server reads it and throws it away. Every expansion this client speaks
    // has it, so it is written unconditionally rather than gated.
    pkt.writeUInt32(0);
    pkt.writeUInt64(petitionGuid);
    pkt.writeUInt64(targetGuid);
    owner_.getSocket()->send(pkt);
}

// ============================================================
// Ready Check
// ============================================================

void SocialHandler::initiateReadyCheck() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // DoReadyCheck (0x005732b0) says nothing either way: without the lead
    // it returns, and READY_CHECK_START comes back with the server's
    // MSG_RAID_READY_CHECK. These were English lines.
    if (!isInGroup()) return;
    auto packet = ReadyCheckPacket::build();
    owner_.getSocket()->send(packet);
}

void SocialHandler::respondToReadyCheck(bool ready) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // ConfirmReadyCheck (0x005740c0) sends the answer and says nothing. This
    // printed "You are ready." / "You are not ready."
    auto packet = ReadyCheckConfirmPacket::build(ready);
    owner_.getSocket()->send(packet);
}

// ============================================================
// Duel
// ============================================================

void SocialHandler::acceptDuel() {
    if (!pendingDuelRequest_ || owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    pendingDuelRequest_ = false;
    // AcceptDuel and CancelDuel send and say nothing; these printed "You
    // accept the duel." and "You have forfeited the duel."
    auto pkt = DuelAcceptPacket::build(duelArbiterGuid_);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::forfeitDuel() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    pendingDuelRequest_ = false;
    auto packet = DuelCancelPacket::build(duelArbiterGuid_);
    owner_.getSocket()->send(packet);
}

void SocialHandler::proposeDuel(uint64_t targetGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // StartDuel (0x005cf950) sends the challenge without a word, and with
    // no unit nothing at all; ERR_DUEL_REQUESTED comes back with the
    // server's SMSG_DUEL_REQUESTED. These were English lines.
    if (targetGuid == 0) return;
    auto packet = DuelProposedPacket::build(targetGuid);
    owner_.getSocket()->send(packet);
    duelOpponentGuid_ = targetGuid;
}

void SocialHandler::reportPlayer(uint64_t targetGuid, const std::string& reason) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // Nothing said here: the answer is the server's SMSG_COMPLAIN_RESULT
    // (COMPLAINT_ADDED). These printed English lines.
    if (targetGuid == 0) return;
    auto packet = ComplainPacket::build(targetGuid, reason);
    owner_.getSocket()->send(packet);
    LOG_INFO("Reported player: 0x", std::hex, targetGuid, std::dec, " reason=", reason);
}

void SocialHandler::handleDuelRequested(network::Packet& packet) {
    if (!packet.hasRemaining(16)) { packet.skipAll(); return; }
    // The flag first, then the challenger. Read the other way round, the
    // challenger was the flag - a game object no name lookup could find, so
    // the challenge came from a hex number - and the flag, which the accept
    // and the refusal both have to name, was thrown away.
    duelArbiterGuid_ = packet.readUInt64();
    duelChallengerGuid_ = packet.readUInt64();
    // The server sends this to both players. The one who issued the challenge
    // keeps the flag, to be able to call it off, and is not asked to accept it
    // - but is told ERR_DUEL_REQUESTED, a system line (0x005cfcd0).
    if (duelChallengerGuid_ == owner_.getPlayerGuid()) {
        pendingDuelRequest_ = false;
        gameError(owner_, ErrTo::Chat, "ERR_DUEL_REQUESTED", "You have requested a duel.");
        return;
    }
    duelOpponentGuid_ = duelChallengerGuid_;
    duelChallengerName_.clear();
    auto entity = owner_.getEntityManager().getEntity(duelChallengerGuid_);
    if (auto* unit = dynamic_cast<Unit*>(entity.get()))
        duelChallengerName_ = unit->getName();
    if (duelChallengerName_.empty()) {
        auto nit = owner_.getPlayerNameCache().find(duelChallengerGuid_);
        if (nit != owner_.getPlayerNameCache().end()) duelChallengerName_ = nit->second;
    }
    if (duelChallengerName_.empty()) {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "0x%llX", static_cast<unsigned long long>(duelChallengerGuid_));
        duelChallengerName_ = tmp;
    }
    pendingDuelRequest_ = true;
    // The DUEL_REQUESTED popup says it; no chat line (0x005cfcd0). This
    // printed "X challenges you to a duel!"
    if (auto* ac = owner_.services().audioCoordinator)
        if (auto* sfx = ac->getUiSoundManager()) sfx->playTargetSelect();
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("DUEL_REQUESTED", {duelChallengerName_});
}

void SocialHandler::handleDuelComplete(network::Packet& packet) {
    if (!packet.hasRemaining(1)) return;
    uint8_t started = packet.readUInt8();
    pendingDuelRequest_ = false;
    duelCountdownMs_ = 0;
    duelOpponentGuid_ = 0;
    // ERR_DUEL_CANCELLED for a duel that never began, in yellow on screen as
    // the table routes it (0x005cfa90). This was an English chat line.
    if (!started)
        gameError(owner_, ErrTo::Info, "ERR_DUEL_CANCELLED", "Duel cancelled.");
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("DUEL_FINISHED", {});
}

void SocialHandler::handleDuelWinner(network::Packet& packet) {
    if (!packet.hasRemaining(3)) return;
    uint8_t duelType = packet.readUInt8();
    std::string winner = packet.readString();
    std::string loser  = packet.readString();
    // DUEL_WINNER_KNOCKOUT, or DUEL_WINNER_RETREAT for a duel fled, the two
    // names in the order they came - a system line (0x005cfb20). These were
    // English.
    owner_.addSystemChatMessage(duelType == 0
        ? interfaceFormat("DUEL_WINNER_KNOCKOUT", "%1$s has defeated %2$s in a duel", {winner, loser})
        : interfaceFormat("DUEL_WINNER_RETREAT", "%2$s has fled from %1$s in a duel", {winner, loser}));
}

// ============================================================
// Party / Raid
// ============================================================

void SocialHandler::inviteToGroup(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    LOG_WARNING(">>> Sending CMSG_GROUP_INVITE to '", playerName, "'");
    auto packet = GroupInvitePacket::build(playerName);
    owner_.getSocket()->send(packet);
}

void SocialHandler::acceptGroupInvite() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    LOG_WARNING(">>> Sending CMSG_GROUP_ACCEPT");
    pendingGroupInvite = false;
    auto packet = GroupAcceptPacket::build();
    owner_.getSocket()->send(packet);
}

void SocialHandler::declineGroupInvite() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    pendingGroupInvite = false;
    auto packet = GroupDeclinePacket::build();
    owner_.getSocket()->send(packet);
}

void SocialHandler::acceptArenaTeamInvite() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_ACCEPT));
    owner_.getSocket()->send(packet);
}

void SocialHandler::declineArenaTeamInvite() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_DECLINE));
    owner_.getSocket()->send(packet);
}

// The four arena team commands that had no sender. Every one is the team id
// and, for three of them, a player name - the shape AzerothCore reads in
// ArenaTeamHandler.cpp: `recvData >> arenaTeamId >> name`. Disband and the two
// invite answers below were already built; these four were named by the
// interface's own popups and slash commands and reached bindings with nothing
// behind them, so the buttons closed and nothing happened.
void SocialHandler::arenaTeamInvite(uint32_t teamId, const std::string& name) {
    if (teamId == 0 || name.empty()) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_INVITE));
    packet.writeUInt32(teamId);
    packet.writeString(name);
    owner_.getSocket()->send(packet);
}

void SocialHandler::arenaTeamLeave(uint32_t teamId) {
    if (teamId == 0) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_LEAVE));
    packet.writeUInt32(teamId);
    owner_.getSocket()->send(packet);
}

void SocialHandler::arenaTeamRemove(uint32_t teamId, const std::string& name) {
    if (teamId == 0 || name.empty()) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_REMOVE));
    packet.writeUInt32(teamId);
    packet.writeString(name);
    owner_.getSocket()->send(packet);
}

void SocialHandler::arenaTeamSetLeader(uint32_t teamId, const std::string& name) {
    if (teamId == 0 || name.empty()) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_LEADER));
    packet.writeUInt32(teamId);
    packet.writeString(name);
    owner_.getSocket()->send(packet);
}

void SocialHandler::disbandArenaTeam(uint32_t teamId) {
    if (teamId == 0) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_ARENA_TEAM_DISBAND));
    packet.writeUInt32(teamId);
    owner_.getSocket()->send(packet);
}

void SocialHandler::reportMailSpam(uint64_t senderGuid, uint32_t mailId) {
    if (senderGuid == 0) return;
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_COMPLAIN));
    packet.writeUInt8(0);              // 0 = mail, 1 = chat
    packet.writeUInt64(senderGuid);
    packet.writeUInt32(0);
    packet.writeUInt32(mailId);
    packet.writeUInt32(0);
    owner_.getSocket()->send(packet);
}

void SocialHandler::leaveGroup() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GroupDisbandPacket::build();
    owner_.getSocket()->send(packet);
    partyData = GroupListData{};
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("GROUP_ROSTER_UPDATE", {});
        owner_.addonEventCallbackRef()("PARTY_MEMBERS_CHANGED", {});
    }
}

void SocialHandler::convertToRaid() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // ConvertToRaid (0x00574a00) does nothing at all unless the player
    // leads a party, and says nothing about it. These were English refusals.
    if (!isInGroup()) return;
    if (partyData.leaderGuid != owner_.getPlayerGuid()) return;
    if (groupIsRaid(partyData.groupType)) return;
    auto packet = GroupRaidConvertPacket::build();
    owner_.getSocket()->send(packet);
    LOG_INFO("Sent CMSG_GROUP_RAID_CONVERT");
}

void SocialHandler::sendSetLootMethod(uint32_t method, uint32_t threshold, uint64_t masterLooterGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = SetLootMethodPacket::build(method, threshold, masterLooterGuid);
    owner_.getSocket()->send(packet);
    LOG_INFO("sendSetLootMethod: method=", method, " threshold=", threshold);
}

// The server checks that this player leads the group and that the target is in
// it, so there is nothing to validate here beyond having a guid to send.
void SocialHandler::setGuildBankTabText(uint8_t tab, const std::string& text) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildBankSetTabTextPacket::build(tab, text);
    owner_.getSocket()->send(packet);
}

void SocialHandler::channelModeration(Opcode op, const std::string& channelName,
                                     const std::string& targetName,
                                     bool allowEmptyTarget) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // An empty second string is meaningless for the moderation commands - there
    // is no player called nothing - but it is how a channel password is
    // cleared, so that one caller says it means it.
    if (channelName.empty() || (targetName.empty() && !allowEmptyTarget)) return;
    auto packet = ChannelModerationPacket::build(op, channelName, targetName);
    owner_.getSocket()->send(packet);
}

void SocialHandler::promoteToLeader(uint64_t guid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (guid == 0) return;
    auto packet = GroupSetLeaderPacket::build(guid);
    owner_.getSocket()->send(packet);
}

void SocialHandler::uninvitePlayer(const std::string& playerName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // UninviteUnit (0x0051a7a0) sends and says nothing; the group's own
    // update announces who left. These were English lines.
    if (playerName.empty()) return;
    auto packet = GroupUninvitePacket::build(playerName);
    owner_.getSocket()->send(packet);
}

// CMSG_LOOT_METHOD carries all three settings together, so changing one means
// resending the other two as they stand. The master looter is only meaningful
// for method 2 and is sent as an empty guid otherwise.
// Removes the lowest rank, which is what the server does with this - it takes
// no index, so there is nothing to get wrong about which one goes.
void SocialHandler::delGuildRank() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = GuildDelRankPacket::build();
    owner_.getSocket()->send(packet);
    requestGuildRoster();
}

void SocialHandler::setLootMethod(uint8_t method, uint64_t masterGuid, uint8_t threshold) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_LOOT_METHOD));
    packet.writeUInt32(method);
    packet.writeUInt64(method == 2 ? masterGuid : 0);
    packet.writeUInt32(threshold);
    owner_.getSocket()->send(packet);
}

// MSG_PARTY_ASSIGNMENT: which assignment (0 main tank, 1 main assist), whether
// it is being given or taken away, and who.
void SocialHandler::setPartyAssignment(uint8_t assignment, uint64_t guid, bool apply) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (guid == 0) return;
    network::Packet packet(wireOpcode(Opcode::MSG_PARTY_ASSIGNMENT));
    packet.writeUInt8(assignment);
    packet.writeUInt8(apply ? 1 : 0);
    packet.writeUInt64(guid);
    owner_.getSocket()->send(packet);
}

// Move a raid member into a different group of eight. The server takes the
// player's name and the destination group as a zero-based index, while the raid
// UI counts its groups from one.
void SocialHandler::setRaidSubgroup(const std::string& playerName, uint8_t group) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (playerName.empty() || group < 1 || group > 8) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_GROUP_CHANGE_SUB_GROUP));
    packet.writeString(playerName);
    packet.writeUInt8(static_cast<uint8_t>(group - 1));
    owner_.getSocket()->send(packet);
}

// Exchange two raid members' places. Used when the destination group is already
// full, which is why it is a separate packet rather than two moves.
void SocialHandler::swapRaidSubgroup(const std::string& firstName, const std::string& secondName) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (firstName.empty() || secondName.empty()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_GROUP_SWAP_SUB_GROUP));
    packet.writeString(firstName);
    packet.writeString(secondName);
    owner_.getSocket()->send(packet);
}

void SocialHandler::leaveParty() {
    // Delegates to leaveGroup which handles both the packet send AND local
    // state cleanup (clearing partyData, firing addon events). Previously
    // this method only sent the packet, leaving the client thinking it was
    // still in a group until the server pushed a group list update.
    leaveGroup();
}

void SocialHandler::setMainTank(uint64_t targetGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // The assignments go out without a word (SetPartyAssignment,
    // 0x0052e1b0, speaks only to refuse). These printed "Main tank set." and
    // the like, and an English refusal for want of a target.
    if (targetGuid == 0) return;
    auto packet = RaidTargetUpdatePacket::build(0, targetGuid);
    owner_.getSocket()->send(packet);
}

void SocialHandler::setMainAssist(uint64_t targetGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (targetGuid == 0) return;
    auto packet = RaidTargetUpdatePacket::build(1, targetGuid);
    owner_.getSocket()->send(packet);
}

void SocialHandler::clearMainTank() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = RaidTargetUpdatePacket::build(0, 0);
    owner_.getSocket()->send(packet);
}

void SocialHandler::clearMainAssist() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = RaidTargetUpdatePacket::build(1, 0);
    owner_.getSocket()->send(packet);
}

void SocialHandler::setRaidMarkLocally(uint64_t guid, uint8_t icon) {
    // Mirrors what the server does for a group: an icon is unique, and a unit
    // carries at most one mark, so placing one clears any previous holder.
    if (icon == 0xFF) {
        for (uint32_t i = 0; i < kRaidMarkCount; ++i) {
            if (raidTargetGuids_[i] == guid) raidTargetGuids_[i] = 0;
        }
    } else if (icon < kRaidMarkCount) {
        for (uint32_t i = 0; i < kRaidMarkCount; ++i) {
            if (raidTargetGuids_[i] == guid) raidTargetGuids_[i] = 0;
        }
        raidTargetGuids_[icon] = guid;
    } else {
        return;
    }
    owner_.fireAddonEvent("RAID_TARGET_UPDATE", {});
}

void SocialHandler::setRaidMark(uint64_t guid, uint8_t icon) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // Raid marks are group state on the server: it drops MSG_RAID_TARGET_UPDATE
    // from an ungrouped player and broadcasts nothing back, so a solo mark used
    // to do nothing at all. Apply it locally instead - a deliberate divergence
    // from Blizzlike behaviour, since a solo player marking their own targets
    // harms nobody and the alternative is a feature that cannot be used, or
    // tested, without a second player. Joining a group later is safe: the
    // server's full list is authoritative and replaces these marks wholesale.
    if (!isInGroup()) {
        setRaidMarkLocally(guid, icon);
        return;
    }
    if (icon == 0xFF) {
        for (int i = 0; i < 8; ++i) {
            if (raidTargetGuids_[i] == guid) {
                auto packet = RaidTargetUpdatePacket::build(static_cast<uint8_t>(i), 0);
                owner_.getSocket()->send(packet);
                break;
            }
        }
    } else if (icon < 8) {
        auto packet = RaidTargetUpdatePacket::build(icon, guid);
        owner_.getSocket()->send(packet);
    }
}

void SocialHandler::setSavedInstanceExtend(uint32_t mapId, uint32_t difficulty, bool extend) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    // uint32 mapId, uint32 difficulty, uint8 toggle - HandleSetSavedInstanceExtend.
    // The server ignores it unless the bind is permanent and the flag actually
    // changes, so there is nothing to check here that it does not check better.
    network::Packet packet(wireOpcode(Opcode::CMSG_SET_SAVED_INSTANCE_EXTEND));
    packet.writeUInt32(mapId);
    packet.writeUInt32(difficulty);
    packet.writeUInt8(extend ? 1 : 0);
    owner_.getSocket()->send(packet);
}

void SocialHandler::requestRaidInfo() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    auto packet = RequestRaidInfoPacket::build();
    owner_.getSocket()->send(packet);
    // Not said in chat. The interface asks for this at every login - it is what
    // fills the Raid Info panel - so the line was three words of noise in the
    // first frame of every session, describing a request nobody made. The
    // player-facing answer belongs to whoever asked: /raidinfo prints what came
    // back, and the panel draws it.
    LOG_INFO("CMSG_REQUEST_RAID_INFO: asked for saved instance lockouts");
}

// ============================================================
// Group Handlers
// ============================================================

void SocialHandler::handleGroupInvite(network::Packet& packet) {
    GroupInviteResponseData data;
    const bool hasCanAccept = !isPreWotlk();
    if (!GroupInviteResponseParser::parse(packet, data, hasCanAccept)) return;

    // The leading byte is not decoration: AzerothCore sends this same opcode
    // with a zero in it to say "you were invited and it could not be offered,
    // because you are already in a group" - its own comment beside the write,
    // and the interface carries Blizzard's sentence for exactly that case
    // (ERR_INVITED_ALREADY_IN_GROUP_SS).
    //
    // It was read off the wire and then ignored, so that refusal raised the
    // accept/decline popup, played the invitation sound and left an invite
    // pending that the server would never honour. Answering it is a refusal,
    // not an offer.
    //
    // Both are said as 0x006df1a0 says them, as system lines with the inviter
    // linked: ERR_INVITED_ALREADY_IN_GROUP_SS for that refusal, and
    // ERR_INVITED_TO_GROUP_SS beside PARTY_INVITE_REQUEST for an invitation.
    // These were English, the invitation once in chat and again on screen.
    if (!data.canAccept) {
        gameError(owner_, ErrTo::Chat, "ERR_INVITED_ALREADY_IN_GROUP_SS",
                  "|Hplayer:%s|h[%s]|h has invited you to join a group, but you cannot accept because you are already in a group.",
                  {data.inviterName, data.inviterName});
        return;
    }

    pendingGroupInvite = true;
    pendingInviterName = data.inviterName;
    gameError(owner_, ErrTo::Chat, "ERR_INVITED_TO_GROUP_SS",
              "|Hplayer:%s|h[%s]|h has invited you to join a group.",
              {data.inviterName, data.inviterName});
    if (auto* ac = owner_.services().audioCoordinator)
        if (auto* sfx = ac->getUiSoundManager()) sfx->playQuestActivate();
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("PARTY_INVITE_REQUEST", {data.inviterName});
}

void SocialHandler::handleGroupDecline(network::Packet& packet) {
    GroupDeclineData data;
    if (!GroupDeclineResponseParser::parse(packet, data)) return;
    // ERR_DECLINE_GROUP_S, a system line (0x006cbd40).
    gameError(owner_, ErrTo::Chat, "ERR_DECLINE_GROUP_S", "%s declines your group invitation.",
              {data.playerName});
}

void SocialHandler::handleGroupList(network::Packet& packet) {
    const bool hasRoles = isActiveExpansion("wotlk");
    const bool hasBattleGroupFlag = isActiveExpansion("tbc");
    const uint8_t prevLootMethod = partyData.lootMethod;
    const uint64_t prevLooterGuid = partyData.looterGuid;
    const uint8_t prevLootThreshold = partyData.lootThreshold;
    const bool wasInGroup = !partyData.isEmpty();
    const std::vector<GroupMember> prevMembers = partyData.members;
    partyData = GroupListData{};
    if (!GroupListParser::parse(packet, partyData, hasRoles, hasBattleGroupFlag)) return;
    // 0x006d8870: a group with members sets the player's difficulty.
    if (partyData.hasPlayerDifficulty) owner_.playerDifficultyRef() = partyData.playerDifficulty;

    // The roster carries online state in isOnline; the per-member stat updates
    // carry it in the onlineStatus bitmask. Seed the bitmask from the roster so
    // both readers agree until the first SMSG_PARTY_MEMBER_STATS arrives -
    // partyData was just reset, so every onlineStatus is otherwise zero and
    // every member would read as offline.
    for (auto& m : partyData.members) {
        if (m.isOnline) m.onlineStatus |= 0x0001;
    }

    const bool nowInGroup = !partyData.isEmpty();
    // Who came and went, as 0x006d8870 says it - in a party only, a raid's
    // roster being the raid frame's business: ERR_JOINED_GROUP_S for each
    // member not there before and ERR_LEFT_GROUP_S for each one gone, system
    // lines. A group just formed names its other member to the leader alone.
    // Leaving is said elsewhere (ERR_LEFT_GROUP_YOU, ERR_UNINVITE_YOU,
    // ERR_GROUP_DISBANDED). This printed "You joined a group with: ..." and
    // "You are no longer in a group." in English.
    if (nowInGroup && !groupIsRaid(partyData.groupType)) {
        const auto had = [](const std::vector<GroupMember>& list, uint64_t guid) {
            return std::any_of(list.begin(), list.end(),
                               [guid](const GroupMember& m) { return m.guid == guid; });
        };
        if (wasInGroup) {
            for (const auto& m : partyData.members)
                if (!m.name.empty() && !had(prevMembers, m.guid))
                    gameError(owner_, ErrTo::Chat, "ERR_JOINED_GROUP_S", "%s joins the party.", {m.name});
            for (const auto& m : prevMembers)
                if (!m.name.empty() && !had(partyData.members, m.guid))
                    gameError(owner_, ErrTo::Chat, "ERR_LEFT_GROUP_S", "%s leaves the party.", {m.name});
        } else if (partyData.leaderGuid == owner_.getPlayerGuid() && !partyData.members.empty() &&
                   !partyData.members.back().name.empty()) {
            gameError(owner_, ErrTo::Chat, "ERR_JOINED_GROUP_S", "%s joins the party.",
                      {partyData.members.back().name});
        }
    }
    if (nowInGroup != wasInGroup) LOG_INFO(nowInGroup ? "Joined group" : "Left group");
    // The loot rules, as 0x0052bd90 announces them - on joining a group, and
    // afterwards whenever one changes: ERR_SET_LOOT_* for the method,
    // ERR_NEW_LOOT_MASTER_S for a new master looter and
    // ERR_SET_LOOT_THRESHOLD_S for the threshold, system lines. This printed
    // "Loot method changed to X." in English, and only for the method.
    if (nowInGroup) {
        const bool fresh = !wasInGroup;
        const bool methodChanged = fresh || partyData.lootMethod != prevLootMethod;
        if (methodChanged) {
            switch (partyData.lootMethod) {
                case 0: gameError(owner_, ErrTo::Chat, "ERR_SET_LOOT_FREEFORALL", "Looting changed to free for all."); break;
                case 1: gameError(owner_, ErrTo::Chat, "ERR_SET_LOOT_ROUNDROBIN", "Looting changed to round robin."); break;
                case 2: gameError(owner_, ErrTo::Chat, "ERR_SET_LOOT_MASTER", "Looting changed to master looter."); break;
                case 3: gameError(owner_, ErrTo::Chat, "ERR_SET_LOOT_GROUP", "Looting changed to group loot."); break;
                case 4: gameError(owner_, ErrTo::Chat, "ERR_SET_LOOT_NBG", "Looting changed to need before greed."); break;
                default: break;
            }
        }
        if ((methodChanged || partyData.looterGuid != prevLooterGuid) &&
            partyData.lootMethod == 2 && partyData.looterGuid != 0) {
            const std::string looter = owner_.lookupName(partyData.looterGuid);
            if (!looter.empty())
                gameError(owner_, ErrTo::Chat, "ERR_NEW_LOOT_MASTER_S", "%s is now the loot master.", {looter});
        }
        if (fresh || partyData.lootThreshold != prevLootThreshold) {
            const std::string qualityKey = "ITEM_QUALITY" + std::to_string(partyData.lootThreshold) + "_DESC";
            gameError(owner_, ErrTo::Chat, "ERR_SET_LOOT_THRESHOLD_S", "Loot threshold set to %s.",
                      {interfaceString(qualityKey.c_str(), qualityKey.c_str())});
        }
    }
    // The party frames read the method to decide whether to offer the
    // master-looter menu, and they reread it on this.
    if (wasInGroup && nowInGroup && partyData.lootMethod != prevLootMethod &&
        owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("PARTY_LOOT_METHOD_CHANGED", {});
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("GROUP_ROSTER_UPDATE", {});
        owner_.addonEventCallbackRef()("PARTY_MEMBERS_CHANGED", {});
        // The roles came with this roster and UnitGroupRolesAssigned reads them
        // straight out of it. The player frame draws its tank or healer badge
        // on this event and nothing else, so the roles were parsed, readable,
        // and never shown.
        owner_.addonEventCallbackRef()("PLAYER_ROLES_ASSIGNED", {});
        if (groupIsRaid(partyData.groupType))
            owner_.addonEventCallbackRef()("RAID_ROSTER_UPDATE", {});
    }
}

void SocialHandler::handleGroupUninvite(network::Packet& packet) {
    (void)packet;
    partyData = GroupListData{};
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("GROUP_ROSTER_UPDATE", {});
        owner_.addonEventCallbackRef()("PARTY_MEMBERS_CHANGED", {});
        owner_.addonEventCallbackRef()("RAID_ROSTER_UPDATE", {});
    }
    // ERR_UNINVITE_YOU, a system line (0x006cbe10). This said it in English,
    // on screen and in chat.
    gameError(owner_, ErrTo::Chat, "ERR_UNINVITE_YOU", "You have been removed from the group.");
}

void SocialHandler::handlePartyCommandResult(network::Packet& packet) {
    PartyCommandResultData data;
    if (!PartyCommandResultParser::parse(packet, data)) return;
    // The value after the result, which the vote-kick cooldowns are counted
    // in (seconds).
    const int32_t value = packet.hasRemaining(4) ? static_cast<int32_t>(packet.readUInt32()) : 0;
    const std::vector<std::string> name = {data.name};
    // Each result as 0x006cbec0 words it, from the ERR_* strings and through
    // the channel the error table gives each - most of them system lines, a
    // few on screen. Success is said too, for an invitation sent
    // (ERR_INVITE_PLAYER_S) and for leaving (ERR_LEFT_GROUP_YOU). These were
    // English, each said on screen and in chat.
    switch (static_cast<uint32_t>(data.result)) {
        case 0:
            if (data.command == PartyCommand::INVITE && !data.name.empty())
                gameError(owner_, ErrTo::Chat, "ERR_INVITE_PLAYER_S", "You have invited %s to join your group.", name);
            else if (data.command == PartyCommand::LEAVE) {
                gameError(owner_, ErrTo::Chat, "ERR_LEFT_GROUP_YOU", "You leave the group.");
                if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("CANCEL_SUMMON", {});
            }
            break;
        case 1:  gameError(owner_, ErrTo::Chat, "ERR_BAD_PLAYER_NAME_S", "Cannot find player '%s'.", name); break;
        case 2:  gameError(owner_, ErrTo::Chat, "ERR_TARGET_NOT_IN_GROUP_S", "%s is not in your party.", name); break;
        case 3:  gameError(owner_, ErrTo::Chat, "ERR_TARGET_NOT_IN_INSTANCE_S", "%s is not in your instance.", name); break;
        case 4:  gameError(owner_, ErrTo::Chat, "ERR_GROUP_FULL", "Your party is full."); break;
        case 5:  gameError(owner_, ErrTo::Chat, "ERR_ALREADY_IN_GROUP_S", "%s is already in a group.", name); break;
        case 6:  gameError(owner_, ErrTo::Chat, "ERR_NOT_IN_GROUP", "You aren't in a party."); break;
        case 7:  gameError(owner_, ErrTo::Chat, "ERR_NOT_LEADER", "You are not the party leader."); break;
        case 8:  gameError(owner_, ErrTo::Error, "ERR_PLAYER_WRONG_FACTION", "Target is unfriendly."); break;
        case 9:  gameError(owner_, ErrTo::Chat, "ERR_IGNORING_YOU_S", "%s is ignoring you.", name); break;
        case 12: gameError(owner_, ErrTo::Chat, "ERR_LFG_PENDING", "You cannot do that while in the Dungeon Finder queue."); break;
        case 13: gameError(owner_, ErrTo::Chat, "ERR_INVITE_RESTRICTED", "Trial accounts cannot invite characters into groups."); break;
        case 14:
            if (data.command == PartyCommand::SWAP)
                gameError(owner_, ErrTo::Error, "ERR_GROUP_SWAP_FAILED", "Party member swap failed.");
            else
                gameError(owner_, ErrTo::Error, "ERR_INVITE_IN_COMBAT", "You can't invite players while in combat.");
            break;
        case 15: gameError(owner_, ErrTo::Error, "ERR_INVITE_UNKNOWN_REALM", "You cannot invite players from that realm."); break;
        case 16: gameError(owner_, ErrTo::Error, "ERR_INVITE_NO_PARTY_SERVER", "Unable to begin party server session."); break;
        case 17: gameError(owner_, ErrTo::Error, "ERR_INVITE_PARTY_BUSY", "The party server is busy."); break;
        case 18: gameError(owner_, ErrTo::Error, "ERR_PARTY_TARGET_AMBIGUOUS", "That name is ambiguous."); break;
        case 19: gameError(owner_, ErrTo::Chat, "ERR_PARTY_LFG_INVITE_RAID_LOCKED",
                           "%s is locked to a different instance of this dungeon.", name); break;
        case 20: gameError(owner_, ErrTo::Error, "ERR_PARTY_LFG_BOOT_LIMIT", "You cannot vote to kick anyone else from this group."); break;
        case 21: gameError(owner_, ErrTo::Error, "ERR_PARTY_LFG_BOOT_COOLDOWN_S",
                           "You must wait %s before starting another vote to kick.",
                           {spellDurationText(static_cast<uint64_t>(value < 0 ? 1 : value) * 1000)}); break;
        case 22: gameError(owner_, ErrTo::Error, "ERR_PARTY_LFG_BOOT_IN_PROGRESS", "A vote to kick is already in progress."); break;
        case 23: gameError(owner_, ErrTo::Error, "ERR_PARTY_LFG_BOOT_TOO_FEW_PLAYERS", "There are not enough players in the group to vote to kick."); break;
        case 24: gameError(owner_, ErrTo::Error, "ERR_PARTY_LFG_BOOT_NOT_ELIGIBLE_S",
                           "This player cannot be kicked for %s.",
                           {spellDurationText(static_cast<uint64_t>(value < 0 ? 1 : value) * 1000)}); break;
        case 25: gameError(owner_, ErrTo::Error, "ERR_RAID_DISALLOWED_BY_LEVEL", "You cannot form a raid group at this level."); break;
        case 26: gameError(owner_, ErrTo::Error, "ERR_PARTY_LFG_BOOT_IN_COMBAT", "You cannot vote to kick while in combat."); break;
        case 27:
            // No line: the vote-kick dialog asks for a reason on this.
            if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("VOTE_KICK_REASON_NEEDED", {data.name});
            break;
        case 28: gameError(owner_, ErrTo::Error, "ERR_PARTY_LFG_BOOT_DUNGEON_COMPLETE", "You cannot vote to kick once the dungeon is complete."); break;
        case 29: gameError(owner_, ErrTo::Error, "ERR_PARTY_LFG_BOOT_LOOT_ROLLS", "You cannot vote to kick while a loot roll is in progress."); break;
        case 30: gameError(owner_, ErrTo::Error, "ERR_PARTY_LFG_TELEPORT_IN_COMBAT", "You cannot teleport to the dungeon while in combat."); break;
        default: break;
    }
}

void SocialHandler::handlePartyMemberStats(network::Packet& packet, bool isFull) {
    auto remaining = [&]() { return packet.getRemainingSize(); };
    const bool isWotLK = isActiveExpansion("wotlk");

    if (isFull) { if (remaining() < 1) return; packet.readUInt8(); }

    const bool pmsTbc = isActiveExpansion("tbc");
    if (remaining() < (pmsTbc ? 8u : 1u)) return;
    uint64_t memberGuid = pmsTbc
        ? packet.readUInt64() : packet.readPackedGuid();
    if (remaining() < 4) return;
    uint32_t updateFlags = packet.readUInt32();

    game::GroupMember* member = nullptr;
    for (auto& m : partyData.members) {
        if (m.guid == memberGuid) { member = &m; break; }
    }
    if (!member) { packet.skipAll(); return; }

    // Track the online transition so it can be announced once the packet is
    // fully read. isOnline is what the Lua bindings read; onlineStatus is what
    // this client's own panels read. Both describe the same fact, so keep them
    // in step here - otherwise UnitIsConnected answers with the stale roster
    // snapshot for as long as the group lasts.
    const bool wasOnline = (member->onlineStatus & 0x0001) != 0;
    bool onlineChanged = false;
    if (updateFlags & 0x0001) {
        if (remaining() >= 2) {
            member->onlineStatus = packet.readUInt16();
            const bool nowOnline = (member->onlineStatus & 0x0001) != 0;
            member->isOnline = nowOnline ? 1 : 0;
            onlineChanged = (nowOnline != wasOnline);
        }
    }
    if (updateFlags & 0x0002) {
        if (isWotLK) { if (remaining() >= 4) member->curHealth = packet.readUInt32(); }
        else { if (remaining() >= 2) member->curHealth = packet.readUInt16(); }
    }
    if (updateFlags & 0x0004) {
        if (isWotLK) { if (remaining() >= 4) member->maxHealth = packet.readUInt32(); }
        else { if (remaining() >= 2) member->maxHealth = packet.readUInt16(); }
    }
    if (updateFlags & 0x0008) { if (remaining() >= 1) member->powerType = packet.readUInt8(); }
    if (updateFlags & 0x0010) { if (remaining() >= 2) member->curPower = packet.readUInt16(); }
    if (updateFlags & 0x0020) { if (remaining() >= 2) member->maxPower = packet.readUInt16(); }
    if (updateFlags & 0x0040) { if (remaining() >= 2) member->level = packet.readUInt16(); }
    if (updateFlags & 0x0080) { if (remaining() >= 2) member->zoneId = packet.readUInt16(); }
    if (updateFlags & 0x0100) {
        if (remaining() >= 4) {
            member->posX = static_cast<int16_t>(packet.readUInt16());
            member->posY = static_cast<int16_t>(packet.readUInt16());
        }
    }
    if (updateFlags & 0x0200) {
        if (remaining() >= 8) {
            uint64_t auraMask = packet.readUInt64();
            // 0x006cf9b0: the mask names the slots this packet changes. The
            // full form starts from no auras; the other keeps every slot the
            // mask leaves out, and a spell id of 0 empties a slot. The list
            // is slot-indexed, as SMSG_AURA_UPDATE keeps the same cache.
            // Rebuilt from only the slots named, a party member lost every
            // aura but the one that changed.
            const bool cacheIt = memberGuid != 0 && memberGuid != owner_.getPlayerGuid() &&
                                 memberGuid != owner_.getTargetGuid();
            std::vector<AuraSlot> auras;
            if (!isFull && cacheIt) {
                auto it = owner_.unitAurasCacheRef().find(memberGuid);
                if (it != owner_.unitAurasCacheRef().end()) auras = it->second;
            }
            for (int i = 0; i < 64; ++i) {
                if (auraMask & (uint64_t(1) << i)) {
                    AuraSlot a;
                    if (isWotLK) {
                        if (remaining() < 5) break;
                        a.spellId = packet.readUInt32();
                        a.flags   = packet.readUInt8();
                    } else {
                        if (remaining() < 2) break;
                        a.spellId = packet.readUInt16();
                        uint8_t dt = owner_.getSpellDispelType(a.spellId);
                        if (dt > 0) a.flags = 0x80;
                    }
                    if (auras.size() <= static_cast<size_t>(i)) auras.resize(i + 1);
                    auras[i] = a;
                }
            }
            if (cacheIt) owner_.unitAurasCacheRef()[memberGuid] = std::move(auras);
        }
    }
    // Skip pet fields and vehicle seat
    if (updateFlags & 0x0400) { if (remaining() >= 8) packet.readUInt64(); }
    if (updateFlags & 0x0800) { if (remaining() > 0) packet.readString(); }
    if (updateFlags & 0x1000) { if (remaining() >= 2) packet.readUInt16(); }
    if (updateFlags & 0x2000) { if (isWotLK) { if (remaining() >= 4) packet.readUInt32(); } else { if (remaining() >= 2) packet.readUInt16(); } }
    if (updateFlags & 0x4000) { if (isWotLK) { if (remaining() >= 4) packet.readUInt32(); } else { if (remaining() >= 2) packet.readUInt16(); } }
    if (updateFlags & 0x8000) { if (remaining() >= 1) packet.readUInt8(); }
    if (updateFlags & 0x10000) { if (remaining() >= 2) packet.readUInt16(); }
    if (updateFlags & 0x20000) { if (remaining() >= 2) packet.readUInt16(); }
    if (updateFlags & 0x40000) {
        if (remaining() >= 8) {
            uint64_t petAuraMask = packet.readUInt64();
            for (int i = 0; i < 64; ++i) {
                if (petAuraMask & (uint64_t(1) << i)) {
                    if (isWotLK) { if (remaining() < 5) break; packet.readUInt32(); packet.readUInt8(); }
                    else { if (remaining() < 2) break; packet.readUInt16(); }
                }
            }
        }
    }
    if (isWotLK && (updateFlags & 0x80000)) { if (remaining() >= 4) packet.readUInt32(); }

    member->hasPartyStats = true;

    if (owner_.addonEventCallbackRef()) {
        std::string unitId;
        if (groupIsRaid(partyData.groupType)) {
            for (size_t i = 0; i < partyData.members.size(); ++i) {
                if (partyData.members[i].guid == memberGuid) { unitId = "raid" + std::to_string(i + 1); break; }
            }
        } else {
            int found = 0;
            for (const auto& m : partyData.members) {
                if (m.guid == owner_.getPlayerGuid()) continue;
                ++found;
                if (m.guid == memberGuid) { unitId = "party" + std::to_string(found); break; }
            }
        }
        if (!unitId.empty()) {
            if (updateFlags & (0x0002 | 0x0004)) owner_.addonEventCallbackRef()("UNIT_HEALTH", {unitId});
            if (updateFlags & (0x0010 | 0x0020)) owner_.addonEventCallbackRef()("UNIT_POWER", {unitId});
            if (updateFlags & 0x0200) {
                owner_.addonEventCallbackRef()("UNIT_AURA", {unitId});
                // The 1.12 name, which carries no unit.
                if (unitId == "player") owner_.addonEventCallbackRef()("PLAYER_AURAS_CHANGED", {});
            }
        }
        // Fired without arguments, as the retail client does - the interface
        // re-reads the whole roster rather than acting on one member. Not
        // gated on unitId: the player's own stats resolve to no unit token,
        // and a member dropping is worth announcing either way.
        if (onlineChanged) {
            owner_.addonEventCallbackRef()(
                member->isOnline ? "PARTY_MEMBER_ENABLE" : "PARTY_MEMBER_DISABLE", {});
        }
    }
}

// ============================================================
// Guild Handlers
// ============================================================

void SocialHandler::handleGuildInfo(network::Packet& packet) {
    GuildInfoData data;
    const size_t start = packet.getReadPos();
    if (!GuildInfoParser::parse(packet, data)) return;
    // The founding date, which the parser steps over: the packed time after
    // the name (minutes in bits 0-5, hours 6-10, weekday 11-13, day-1 14-19,
    // month-1 20-23, year-2000 24-28), read again from where it sits.
    uint32_t created = 0;
    {
        const size_t end = packet.getReadPos();
        packet.setReadPos(start + data.guildName.size() + 1);
        if (packet.hasRemaining(4)) created = packet.readUInt32();
        packet.setReadPos(end);
    }
    if (!data.guildName.empty()) {
        if (const Character* ch = owner_.getActiveCharacter(); ch && ch->hasGuild()) {
            rememberGuildName(ch->guildId, data.guildName);
            if (guildName_.empty()) guildName_ = data.guildName;
        }
    }
    // The answer to /guildinfo, and only that - nothing here asks for it
    // unprompted - said as 0x006cc440 says it: GUILD_NAME_TEMPLATE and then
    // GUILD_INFO_TEMPLATE with the founding date and the counts, two system
    // lines, every time it is asked. This printed one English line, and
    // only when a figure had changed since the last time.
    guildInfoData_ = data;
    owner_.addSystemChatMessage(interfaceFormat("GUILD_NAME_TEMPLATE", "Guild: %s", {data.guildName}));
    owner_.addSystemChatMessage(interfaceFormat("GUILD_INFO_TEMPLATE",
        "Guild created %1$d/%2$d/%3$d, %4$d players, %5$d accounts",
        {std::to_string(((created >> 20) & 0x0F) + 1), std::to_string(((created >> 14) & 0x3F) + 1),
         std::to_string(((created >> 24) & 0x1F) + 2000),
         std::to_string(data.numMembers), std::to_string(data.numAccounts)}));
}

void SocialHandler::handleGuildRoster(network::Packet& packet) {
    GuildRosterData data;
    if (!owner_.getPacketParsers()->parseGuildRoster(packet, data)) return;
    guildRoster_ = std::move(data);
    hasGuildRoster_ = true;

    // A roster is only sent to a member, so this is the server saying so.
    //
    // Joining mid-session left every record of membership untouched: the
    // character list's guild id was taken at login and the guild name is only
    // written for a character that already had one. So the social window's
    // guild tab stayed empty for a player who had just accepted an invite, and
    // stayed empty until the next login.
    const bool newlyIn = !inGuild_;
    inGuild_ = true;
    if (guildName_.empty() && !joinedGuildName_.empty()) {
        guildName_ = joinedGuildName_;
    }
    if (guildName_.empty()) {
        // The name comes from the guild's own query, which needs the id - and
        // the id is on the player now that the server has moved them.
        if (const uint32_t id = ownGuildId()) queryGuildInfo(id);
    }
    if (newlyIn && owner_.addonEventCallbackRef()) {
        // What the social window listens for to rebuild its guild tab. Without
        // it the frame has a roster and no idea it is meant to draw one.
        owner_.addonEventCallbackRef()("PLAYER_GUILD_UPDATE", {});
    }
    // False: the roster in hand is the fresh one, so nothing should ask again.
    // The argument is "you may request a roster", and every reader answers it
    // with `if arg1 then GuildRoster() end` - friendsframe, the guild bank and
    // the calendar all do exactly that. Sending true here would have each of
    // them request a roster, whose reply would fire this again, forever.
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("GUILD_ROSTER_UPDATE", {eventBool(false)});
}

// CMSG_GUILD_RANK rewrites the rank whole: id, rights, name, gold per day, then
// six pairs of bank tab rights and slots. Everything it carries has to be sent,
// which is why the caller stages from the rank's current values rather than
// from nothing - a field left at zero here is not "unchanged", it is revoked.
void SocialHandler::saveGuildRank(uint32_t rankId, uint32_t rights, const std::string& rankName,
                                  uint32_t goldLimit, const uint32_t* tabRights,
                                  const uint32_t* tabSlots) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_GUILD_RANK));
    packet.writeUInt32(rankId);
    packet.writeUInt32(rights);
    packet.writeString(rankName);
    packet.writeUInt32(goldLimit);
    for (int t = 0; t < 6; ++t) {
        packet.writeUInt32(tabRights ? tabRights[t] : 0);
        packet.writeUInt32(tabSlots ? tabSlots[t] : 0);
    }
    owner_.getSocket()->send(packet);
    requestGuildRoster();
}

uint32_t SocialHandler::getPlayerGuildRankIndex() const {
    if (!hasGuildRoster_) return 0xFFFFFFFFu;
    const uint64_t me = owner_.getPlayerGuid();
    for (const auto& m : guildRoster_.members)
        if (m.guid == me) return m.rankIndex;
    return 0xFFFFFFFFu;
}

uint32_t SocialHandler::getPlayerGuildRankRights() const {
    const uint32_t idx = getPlayerGuildRankIndex();
    if (idx >= guildRoster_.ranks.size()) return 0;
    return guildRoster_.ranks[idx].rights;
}

void SocialHandler::handleGuildQueryResponse(network::Packet& packet) {
    GuildQueryResponseData data;
    if (!owner_.getPacketParsers()->parseGuildQueryResponse(packet, data)) return;
    if (data.guildId != 0) {
        pendingGuildNameQueries_.erase(data.guildId);
        if (!data.guildName.empty()) {
            rememberGuildName(data.guildId, data.guildName);
        }
        const GuildEmblem emblem{.emblemStyle = data.emblemStyle,
                                 .emblemColor = data.emblemColor,
                                 .borderStyle = data.borderStyle,
                                 .borderColor = data.borderColor,
                                 .backgroundColor = data.backgroundColor};
        auto [it, fresh] = guildEmblems_.try_emplace(data.guildId, emblem);
        if (fresh || !(it->second == emblem)) {
            it->second = emblem;
            ++guildEmblemGeneration_;
        }
    }
    // Ours when it names the guild this character is actually in - which is
    // the player's own field once they have joined one, not the character
    // list's copy from login.
    const bool isLocalGuild = (ownGuildId() != 0 && ownGuildId() == data.guildId);
    if (isLocalGuild) {
        const bool wasUnknown = guildName_.empty();
        guildName_ = data.guildName;
        guildQueryData_ = data;
        guildRankNames_.clear();
        for (const auto& rankName : data.rankNames) guildRankNames_.push_back(rankName);
        // The name arriving is news for the interface and not for chat: the
        // original's query answer (0x006359e0) prints nothing. This said
        // "Guild: <Name>".
        if (wasUnknown && !guildName_.empty()) {
            if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("PLAYER_GUILD_UPDATE", {});
        }
    }
}

// Returns true only when the member's cached online state actually changes (and updates
// it), so callers announce genuine online/offline transitions and silently drop the
// redundant SIGNED_ON flood the server emits at login for already-online members.
bool SocialHandler::guildMemberOnlineTransition(const std::string& name, bool nowOnline) {
    for (auto& m : guildRoster_.members) {
        if (m.name == name) {
            if (m.online == nowOnline) return false;
            m.online = nowOnline;
            return true;
        }
    }
    return true;  // not in the cached roster yet - announce rather than swallow
}

void SocialHandler::handleGuildEvent(network::Packet& packet) {
    GuildEventData data;
    if (!GuildEventParser::parse(packet, data)) return;

    // What the game prints for each, as 0x006d92d0 words it - the ERR_GUILD_*
    // string with the event's strings put in, and the friend list's online
    // and offline lines - all system lines. These were English; before that
    // they went out as guild chat with no sender.
    std::string msg;
    bool guildLine = false;  // the MOTD, which is the one that is guild chat
    const auto s = [&](size_t i) { return i < data.numStrings ? data.strings[i] : std::string(); };
    switch (data.eventType) {
        case GuildEvent::PROMOTION:
            msg = interfaceFormat("ERR_GUILD_PROMOTE_SSS", "%s has promoted %s to %s.", {s(0), s(1), s(2)});
            break;
        case GuildEvent::DEMOTION:
            msg = interfaceFormat("ERR_GUILD_DEMOTE_SSS", "%s has demoted %s to %s.", {s(0), s(1), s(2)});
            break;
        case GuildEvent::MOTD:
            // The interface writes this one itself, from the GUILD_MOTD event
            // fired further down: chatframe.lua formats GUILD_MOTD_TEMPLATE
            // and adds it. Only the client's own window needs a line here.
            if (data.numStrings >= 1 && !ui::frameXmlOwns(ui::UiElement::Chat)) {
                msg = interfaceFormat("GUILD_MOTD_TEMPLATE", "Guild Message of the Day: %s", {data.strings[0]});
                guildLine = true;
            }
            break;
        case GuildEvent::JOINED:
            msg = interfaceFormat("ERR_GUILD_JOIN_S", "%s has joined the guild.", {s(0)});
            break;
        case GuildEvent::LEFT:
            msg = interfaceFormat("ERR_GUILD_LEAVE_S", "%s has left the guild.", {s(0)});
            break;
        case GuildEvent::REMOVED:
            // The member removed, then who removed them - the order the server
            // sends and ERR_GUILD_REMOVE_SS reads them in.
            msg = interfaceFormat("ERR_GUILD_REMOVE_SS", "%s has been kicked out of the guild by %s.", {s(0), s(1)});
            break;
        case GuildEvent::LEADER_IS:
            msg = interfaceFormat("ERR_GUILD_LEADER_IS_S", "%s is the leader of your guild.", {s(0)});
            break;
        case GuildEvent::LEADER_CHANGED:
            msg = interfaceFormat("ERR_GUILD_LEADER_CHANGED_SS", "%s has made %s the new Guild Master.", {s(0), s(1)});
            break;
        case GuildEvent::DISBANDED:
            msg = interfaceString("ERR_GUILD_DISBANDED", "Guild has been disbanded.");
            guildName_.clear();
            guildRankNames_.clear();
            guildRoster_ = GuildRosterData{};
            hasGuildRoster_ = false;
            if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("PLAYER_GUILD_UPDATE", {});
            break;
        case GuildEvent::SIGNED_ON:
        case GuildEvent::SIGNED_OFF: {
            // 0x006d92d0: ERR_FRIEND_ONLINE_SS / ERR_FRIEND_OFFLINE_S for every
            // one, while Guild Member Notify is on, leaving out the player and
            // anyone on the friends list, whose own notice says it already.
            // The client keeps no idea of a "transition" to hold one back by;
            // the roster's online state is still kept in step.
            const bool on = data.eventType == GuildEvent::SIGNED_ON;
            if (data.numStrings >= 1) (void)guildMemberOnlineTransition(data.strings[0], on);
            if (data.numStrings >= 1 && addons::storedCVarValue("guildMemberNotify", "1") != "0" &&
                data.strings[0] != ownName(owner_) && !owner_.friendsCacheRef().count(data.strings[0])) {
                msg = on ? interfaceFormat("ERR_FRIEND_ONLINE_SS", "|Hplayer:%s|h[%s]|h has come online.",
                                           {data.strings[0], data.strings[0]})
                         : interfaceFormat("ERR_FRIEND_OFFLINE_S", "%s has gone offline.", {data.strings[0]});
            }
            break;
        }
        // The bank's half of the event list. The server broadcasts these to
        // every member whenever anyone touches the bank, so they are frequent,
        // and not one of them is a chat line - they carry the figures the bank
        // panel reads back through its own API. Unhandled, a guild with an
        // active bank filled guild chat with "Guild event 17: 000000000051451B".
        case GuildEvent::BANK_MONEY_SET:
            // 0x006d92d0 case 0x11: the balance, then a fresh ask for what the
            // player may still take (MSG_GUILD_BANK_MONEY_WITHDRAWN).
            if (data.numStrings >= 1)
                owner_.setGuildBankMoney(guildEventMoney(data.strings[0]));
            owner_.requestGuildBankWithdrawMoney();
            break;
        case GuildEvent::BANK_TAB_AND_MONEY:
            // 0x006d92d0 case 0x12 reads nothing out of this one: it asks
            // again for the tab the open vault is showing (0x005a5800, which
            // does nothing with no banker) and sends
            // MSG_GUILD_BANK_MONEY_WITHDRAWN. This used to take a balance out
            // of its first string and raise GUILDBANK_UPDATE_TABS, neither of
            // which the client does.
            owner_.queryGuildBankTab(owner_.getGuildBankActiveTab());
            owner_.requestGuildBankWithdrawMoney();
            break;
        case GuildEvent::BANK_BAG_SLOTS_CHANGED:
        case GuildEvent::BANK_TAB_PURCHASED:
        case GuildEvent::BANK_TAB_UPDATED:
        case GuildEvent::BANK_TEXT_CHANGED:
        case GuildEvent::RANK_UPDATED:
        case GuildEvent::RANK_DELETED:
            // Silent here; the interface events they mean are fired below.
            break;
        default:
            // Any other type - the tabard event (9) among them - is
            // ERR_GUILD_INTERNAL with the event's strings, as 0x006d92d0's
            // default says it.
            msg = interfaceFormat("ERR_GUILD_INTERNAL", "Internal guild error.", {s(0), s(1), s(2)});
            break;
    }

    if (!msg.empty()) {
        MessageChatData chatMsg;
        chatMsg.type = guildLine ? ChatType::GUILD : ChatType::SYSTEM;
        chatMsg.language = ChatLanguage::UNIVERSAL;
        chatMsg.message = msg;
        owner_.addLocalChatMessage(chatMsg);
    }

    // Whether this client is about to ask for a fresh roster itself, which is
    // the switch at the end of this function. The event's argument is the
    // negation: "nobody has asked, so you may". Working it out once and using
    // it in both places is the point - FrameXML requesting when the client
    // already has costs a second roster for a large guild, and FrameXML not
    // requesting when the client has not leaves every guild panel drawing from
    // a copy that no longer says who is online.
    const bool clientWillRequest = hasGuildRoster_ && (
        data.eventType == GuildEvent::PROMOTION ||
        data.eventType == GuildEvent::DEMOTION ||
        data.eventType == GuildEvent::JOINED ||
        data.eventType == GuildEvent::LEFT ||
        data.eventType == GuildEvent::REMOVED ||
        data.eventType == GuildEvent::LEADER_CHANGED ||
        // A rank added, renamed or deleted only reaches the client in the
        // roster: SMSG_GUILD_ROSTER is what carries the rank list, and every
        // panel that names a rank reads it from there.
        data.eventType == GuildEvent::RANK_UPDATED ||
        data.eventType == GuildEvent::RANK_DELETED);

    if (owner_.addonEventCallbackRef()) {
        switch (data.eventType) {
            case GuildEvent::MOTD:
                owner_.addonEventCallbackRef()("GUILD_MOTD", {data.numStrings >= 1 ? data.strings[0] : ""});
                break;
            case GuildEvent::SIGNED_ON: case GuildEvent::SIGNED_OFF:
            case GuildEvent::PROMOTION: case GuildEvent::DEMOTION:
            case GuildEvent::JOINED: case GuildEvent::LEFT:
            case GuildEvent::REMOVED: case GuildEvent::LEADER_CHANGED:
            case GuildEvent::DISBANDED:
            case GuildEvent::RANK_UPDATED: case GuildEvent::RANK_DELETED:
                // Somebody joined, left, was promoted or signed on, and the
                // roster this client holds no longer says so. The server sends
                // the event and not a new roster, so the request has to come
                // from somewhere. Signing on and off is the case nothing
                // covered: the switch below does not request for those, so a
                // member who had just come online stayed grey until something
                // else asked.
                owner_.addonEventCallbackRef()("GUILD_ROSTER_UPDATE",
                                               {eventBool(!clientWillRequest)});
                break;
            // The bank panel's own events, which are what these types mean.
            // GuildBankFrame_OnEvent returns early unless the frame is
            // visible, so firing them while it is shut costs nothing.
            case GuildEvent::BANK_BAG_SLOTS_CHANGED:
                owner_.addonEventCallbackRef()("GUILDBANKBAGSLOTS_CHANGED", {});
                break;
            case GuildEvent::BANK_TAB_PURCHASED:
            case GuildEvent::BANK_TAB_UPDATED:
                owner_.addonEventCallbackRef()("GUILDBANK_UPDATE_TABS", {});
                break;
            case GuildEvent::BANK_TEXT_CHANGED:
                // The tab, as the server counts them from zero and the panel
                // counts them from one. It answers by querying the new text.
                if (data.numStrings >= 1)
                    owner_.addonEventCallbackRef()(
                        "GUILDBANK_TEXT_CHANGED",
                        {std::to_string(std::strtol(data.strings[0].c_str(), nullptr, 10) + 1)});
                break;
            default: break;
        }
    }

    if (clientWillRequest) requestGuildRoster();
}

void SocialHandler::handleGuildInvite(network::Packet& packet) {
    GuildInviteResponseData data;
    if (!GuildInviteResponseParser::parse(packet, data)) return;
    pendingGuildInvite_ = true;
    pendingGuildInviterName_ = data.inviterName;
    pendingGuildInviteGuildName_ = data.guildName;
    // ERR_INVITED_TO_GUILD_SSS, a system line with the inviter linked, beside
    // GUILD_INVITE_REQUEST (0x006cc3b0). This was English.
    gameError(owner_, ErrTo::Chat, "ERR_INVITED_TO_GUILD_SSS",
              "|Hplayer:%s|h[%s]|h has invited you to join the guild, %s.",
              {data.inviterName, data.inviterName, data.guildName});
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("GUILD_INVITE_REQUEST", {data.inviterName, data.guildName});
}

void SocialHandler::handleGuildCommandResult(network::Packet& packet) {
    GuildCommandResultData data;
    if (!GuildCommandResultParser::parse(packet, data)) return;
    if (data.errorCode == 0) {
        // Whether the player's own membership changed, which is a different
        // question from whether the command worked: inviting someone succeeds
        // without altering anything the panels read.
        //
        // Said as 0x006cc590 says it, system lines naming the guild or the
        // player: ERR_GUILD_CREATE_S, ERR_GUILD_INVITE_S, ERR_GUILD_QUIT_S
        // (the server's quit is 3), ERR_GUILD_FOUNDER_S. These were English.
        bool membershipChanged = false;
        const std::vector<std::string> name = {data.name};
        switch (data.command) {
            case 0:
                gameError(owner_, ErrTo::Chat, "ERR_GUILD_CREATE_S", "%s created.", name);
                membershipChanged = true;
                break;
            case 1:
                gameError(owner_, ErrTo::Chat, "ERR_GUILD_INVITE_S", "You have invited %s to join your guild.", name);
                break;
            case 2:
            case 3:
                if (data.command == 3)
                    gameError(owner_, ErrTo::Chat, "ERR_GUILD_QUIT_S", "You are no longer a member of %s.", name);
                guildName_.clear(); guildRankNames_.clear();
                guildRoster_ = GuildRosterData{}; hasGuildRoster_ = false;
                membershipChanged = true;
                break;
            case 0x0E:
                gameError(owner_, ErrTo::Chat, "ERR_GUILD_FOUNDER_S",
                          "Congratulations, you are a founding member of %s!", name);
                break;
            default: break;
        }
        // Leaving a guild emptied the name, the ranks and the whole roster and
        // told nobody, so the interface's guild tab kept showing the guild the
        // player had just left - every row of it, until something else
        // happened to fire a roster update. PLAYER_GUILD_UPDATE is what
        // friendsframe and the paperdoll re-check IsInGuild on.
        //
        // The roster event carries false, which is the established contract
        // here: every reader answers a true with `if arg1 then GuildRoster()
        // end`, and the reply to that would fire this again without end.
        if (membershipChanged && owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("PLAYER_GUILD_UPDATE", {});
            owner_.addonEventCallbackRef()("GUILD_ROSTER_UPDATE", {eventBool(false)});
        }
        return;
    }
    // The refusals by the server's numbering, as 0x006cc590 words them -
    // system lines but for the four the error table puts on screen. This had
    // seven English sentences under the wrong numbers and an English "Guild
    // command failed" for the rest, each said on screen and in chat.
    const std::vector<std::string> name = {data.name};
    switch (data.errorCode) {
        case 1:  gameError(owner_, ErrTo::Chat, "ERR_GUILD_INTERNAL", "Internal guild error."); break;
        case 2:  gameError(owner_, ErrTo::Chat, "ERR_ALREADY_IN_GUILD", "You are already in a guild."); break;
        case 3:  gameError(owner_, ErrTo::Chat, "ERR_ALREADY_IN_GUILD_S", "%s is already in a guild.", name); break;
        case 4:  gameError(owner_, ErrTo::Chat, "ERR_INVITED_TO_GUILD", "You have already been invited into a guild."); break;
        case 5:  gameError(owner_, ErrTo::Chat, "ERR_ALREADY_INVITED_TO_GUILD_S", "%s has already been invited into a guild.", name); break;
        case 6:  gameError(owner_, ErrTo::Error, "ERR_NAME_INVALID", "Invalid name"); break;
        case 7:  gameError(owner_, ErrTo::Error, "ERR_GUILD_NAME_EXISTS_S", "There is already a guild named \"%s\".", name); break;
        case 8:
            if (data.command == 3)
                gameError(owner_, ErrTo::Chat, "ERR_GUILD_LEADER_LEAVE", "You must promote a new Guild Master using /gleader before leaving the guild.");
            else
                gameError(owner_, ErrTo::Chat, "ERR_GUILD_PERMISSIONS", "You don't have permission to do that.");
            break;
        case 9:  gameError(owner_, ErrTo::Chat, "ERR_GUILD_PLAYER_NOT_IN_GUILD", "You are not in a guild."); break;
        case 10: gameError(owner_, ErrTo::Chat, "ERR_GUILD_PLAYER_NOT_IN_GUILD_S", "%s is not in your guild.", name); break;
        case 11: gameError(owner_, ErrTo::Chat, "ERR_GUILD_PLAYER_NOT_FOUND_S", "\"%s\" not found.", name); break;
        case 12: gameError(owner_, ErrTo::Chat, "ERR_GUILD_NOT_ALLIED", "You cannot invite players from the opposing alliance"); break;
        case 13: gameError(owner_, ErrTo::Chat, "ERR_GUILD_RANK_TOO_HIGH_S", "%s's rank is too high", name); break;
        case 14: gameError(owner_, ErrTo::Chat, "ERR_GUILD_RANK_TOO_LOW_S", "%s is already at the lowest rank", name); break;
        case 17: gameError(owner_, ErrTo::Chat, "ERR_GUILD_RANKS_LOCKED", "Temporary guild error.  Please try again!"); break;
        case 18: gameError(owner_, ErrTo::Chat, "ERR_GUILD_RANK_IN_USE", "That guild rank is currently in use."); break;
        case 19: gameError(owner_, ErrTo::Chat, "ERR_IGNORING_YOU_S", "%s is ignoring you.", name); break;
        case 25: gameError(owner_, ErrTo::Error, "ERR_GUILD_WITHDRAW_LIMIT", "You cannot withdraw that much from the guild bank."); break;
        case 26: gameError(owner_, ErrTo::Error, "ERR_GUILD_NOT_ENOUGH_MONEY", "The guild bank does not have enough money"); break;
        case 28: gameError(owner_, ErrTo::Error, "ERR_GUILD_BANK_FULL", "This guild bank tab is full"); break;
        case 29: gameError(owner_, ErrTo::Error, "ERR_ITEM_NOT_FOUND", "The item was not found."); break;
        default: break;
    }
}

void SocialHandler::handlePetitionShowlist(network::Packet& packet) {
    PetitionShowlistData data;
    if (!PetitionShowlistParser::parse(packet, data)) return;
    // Either registrar's window opens with it (0x006d69f0 -> 0x00599e00 or
    // 0x005a22a0), set as the interaction object before the window takes it
    // (0x0051fce0): one open with that registrar already closes first.
    owner_.setInteractionObject(data.npcGuid);
    petitionNpcGuid_ = data.npcGuid;
    petitionCost_ = data.cost;
    // The whole offer, not just the first charter's price. An arena registrar
    // offers three and the panel prices whichever tab is open.
    petitionCharters_ = data.charters;
    showPetitionDialog_ = true;
    // Which window by the registrar's own flags, as 0x006d69f0 picks it: a
    // petitioner that is also a tabard designer (NPC flags 0x40000 and
    // 0x80000) is the guild registrar, any other the arena one. The charter
    // on offer only stands in when the unit is not to hand.
    petitionIsGuildCharter_ = data.isGuildCharter();
    if (auto unit = std::dynamic_pointer_cast<Unit>(owner_.getEntityManager().getEntity(data.npcGuid)))
        petitionIsGuildCharter_ = (unit->getNpcFlags() & 0xC0000u) == 0xC0000u;
    // The vendor panel opens on this. showPetitionDialog_ is what this
    // client's own dialog reads, and the interface's version was told
    // nothing, so walking up to a guild master or an arena registrar opened
    // one of the two windows and never the other.
    //
    // Two panels, one opcode: the guild registrar and the arena registrar are
    // told apart by which charter is on offer, not by what was sent. Firing
    // the arena event at a tabard designer put the wrong window on screen.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()(
            petitionIsGuildCharter_ ? "GUILD_REGISTRAR_SHOW" : "PETITION_VENDOR_SHOW", {});
    }
}

void SocialHandler::closePetitionVendor() {
    if (!showPetitionDialog_) return;
    showPetitionDialog_ = false;
    petitionNpcGuid_ = 0;
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()(
            petitionIsGuildCharter_ ? "GUILD_REGISTRAR_CLOSED" : "PETITION_VENDOR_CLOSED", {});
    }
}

void SocialHandler::handlePetitionQueryResponse(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 12) return;
    /*uint32_t entry =*/ packet.readUInt32();
    uint64_t petGuid = packet.readUInt64();
    std::string guildName = packet.readString();
    /*std::string body =*/ packet.readString();

    // Three words the reply carries that used to be skipped along with the
    // rest of it. The first is how many signatures the charter needs - nothing
    // else on the wire says, so signaturesRequired sat at its default of nine
    // for every charter, and the counter under the signature list read "x / 9"
    // whatever the realm was configured for. CanSignPetition compares against
    // the same number, so the Sign button stayed live past the real
    // requirement.
    //
    // The third tells a guild charter from an arena one: AzerothCore writes
    // zero for a guild and the team size otherwise. GetPetitionInfo answered
    // "guild" for everything, which is the wrong heading and the wrong
    // confirmation on all three arena charters.
    uint32_t needed = 0, arenaType = 0;
    if (rem() >= 12) {
        needed = packet.readUInt32();
        /*uint32_t neededAgain =*/ packet.readUInt32();
        arenaType = packet.readUInt32();
    }
    packet.skipAll();

    if (petitionInfo_.petitionGuid != petGuid) return;
    petitionInfo_.guildName = guildName;
    if (needed > 0) petitionInfo_.signaturesRequired = needed;
    petitionInfo_.isArena = arenaType != 0;

    // Said again, because the frame was drawn when the signatures arrived and
    // everything above lands after it. Without this the charter keeps the
    // blank name and the placeholder count it opened with - the shape that
    // reads as "only right after reopening it".
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("PETITION_SHOW", {});
}

void SocialHandler::handlePetitionShowSignatures(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 21) return;
    petitionInfo_ = PetitionInfo{};
    petitionInfo_.petitionGuid = packet.readUInt64();
    petitionInfo_.ownerGuid    = packet.readUInt64();
    const uint32_t petEntry    = packet.readUInt32();
    uint8_t sigCount           = packet.readUInt8();
    petitionInfo_.signatureCount = sigCount;
    petitionInfo_.signatures.reserve(sigCount);
    for (uint8_t i = 0; i < sigCount; ++i) {
        if (rem() < 12) break;
        PetitionSignature sig;
        sig.playerGuid = packet.readUInt64();
        /*uint32_t unk =*/ packet.readUInt32();
        petitionInfo_.signatures.push_back(sig);
    }
    petitionInfo_.showUI = true;

    // Ask what this charter actually is. Everything the frame needs beyond the
    // signatures - the name, the number of signatures required, and whether it
    // is a guild or an arena charter - comes only from the query reply, and
    // this client never sent the query. The opcode was in the table and the
    // reply had a handler waiting for it; nothing ever asked, so the handler
    // never ran and the frame opened on a nameless charter needing nine
    // signatures whatever it was.
    if (owner_.getSocket()) {
        auto query = PetitionQueryPacket::build(petEntry, petitionInfo_.petitionGuid);
        owner_.getSocket()->send(query);
    }

    // The signature list frame opens on this. The petition is parsed and
    // stored above - showUI is what this client's own window reads - and the
    // interface's version was never told, so a charter could not be signed
    // through it.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("PETITION_SHOW", {});
}

void SocialHandler::handlePetitionSignResults(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 20) return;
    uint64_t petGuid    = packet.readUInt64();
    uint64_t playerGuid = packet.readUInt64();
    uint32_t result     = packet.readUInt32();
    if (result == 0 && petitionInfo_.petitionGuid == petGuid) {
        petitionInfo_.signatureCount++;
        PetitionSignature sig; sig.playerGuid = playerGuid;
        petitionInfo_.signatures.push_back(sig);
    }
    // As 0x006cefd0 says it. Someone else's signature on this player's
    // charter is ERR_PETITION_SIGNED_S naming them, whatever the result; the
    // player's own is ERR_PETITION_SIGNED, a system line, or the refusal on
    // screen, and nothing for a result it has no words for. These were
    // English.
    if (playerGuid != owner_.getPlayerGuid()) {
        const std::string signer = owner_.lookupName(playerGuid);
        if (!signer.empty())
            gameError(owner_, ErrTo::Chat, "ERR_PETITION_SIGNED_S", "%s has signed your charter.", {signer});
        return;
    }
    switch (result) {
        case 0:
            gameError(owner_, ErrTo::Chat, "ERR_PETITION_SIGNED", "Charter signed");
            if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("PETITION_CLOSED", {});
            break;
        case 1:  gameError(owner_, ErrTo::Error, "ERR_PETITION_ALREADY_SIGNED", "You have already signed that charter"); break;
        case 2:  gameError(owner_, ErrTo::Error, "ERR_PETITION_IN_GUILD", "That player is already in a guild"); break;
        case 3:  gameError(owner_, ErrTo::Error, "ERR_PETITION_CREATOR", "You can't sign your own charter"); break;
        case 5:  gameError(owner_, ErrTo::Error, "ERR_PETITION_NOT_SAME_SERVER", "That player is not from your server"); break;
        case 8:  gameError(owner_, ErrTo::Error, "ERR_PETITION_FULL", "That charter is full"); break;
        case 10: gameError(owner_, ErrTo::Error, "ERR_PETITION_ALREADY_SIGNED_OTHER", "You've already signed another guild charter"); break;
        case 11: gameError(owner_, ErrTo::Error, "ERR_PETITION_RESTRICTED_ACCOUNT", "Trial accounts cannot sign guild charters"); break;
        default: break;
    }
}

void SocialHandler::handleTurnInPetitionResults(network::Packet& packet) {
    uint32_t result = 0;
    if (!TurnInPetitionResultsParser::parse(packet, result)) return;
    // As 0x006cf1a0 says it: a charter turned in closes the registrar and
    // says nothing (the guild's own events announce it); 2, 4 and 11 are
    // refusals - ERR_PETITION_IN_GUILD and ERR_PETITION_NOT_ENOUGH_SIGNATURES
    // on screen, ERR_GUILD_PERMISSIONS a system line. This had English
    // sentences under other numbers.
    switch (result) {
        case 0:
            if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("GUILD_REGISTRAR_CLOSED", {});
            break;
        case 2:  gameError(owner_, ErrTo::Error, "ERR_PETITION_IN_GUILD", "That player is already in a guild"); break;
        case 4:  gameError(owner_, ErrTo::Error, "ERR_PETITION_NOT_ENOUGH_SIGNATURES", "You need more signatures"); break;
        case 11: gameError(owner_, ErrTo::Chat, "ERR_GUILD_PERMISSIONS", "You don't have permission to do that."); break;
        default: break;
    }
}

// ============================================================
// Server Info Handlers
// ============================================================

void SocialHandler::handleQueryTimeResponse(network::Packet& packet) {
    QueryTimeResponseData data;
    if (!QueryTimeResponseParser::parse(packet, data)) return;

    // The second word is how long until the daily quests reset -
    // GetNextDailyQuestsResetTime() minus now, in AzerothCore's own terms. It
    // was parsed and dropped, and GetQuestResetTime answered the time until
    // local midnight instead, with a comment saying this was a realm setting
    // nothing sends. It is sent, in the reply to a packet this client already
    // knew how to ask for.
    dailyResetOffset_ = data.timeOffset;
    dailyResetReceivedAt_ = time(nullptr);

    // Only /time says it out loud. The login-time ask is for the reset figure
    // and would otherwise greet every player with the clock.
    if (!announceServerTime_) return;
    announceServerTime_ = false;
    time_t serverTime = static_cast<time_t>(data.serverTime);
    const std::tm timeInfo = core::localTime(serverTime);
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeInfo);
    owner_.addSystemChatMessage("Server time: " + std::string(timeStr));
}

uint32_t SocialHandler::getSecondsUntilDailyReset() const {
    if (dailyResetOffset_ == 0 || dailyResetReceivedAt_ == 0) return 0;
    const auto elapsed = static_cast<int64_t>(time(nullptr) - dailyResetReceivedAt_);
    const int64_t left = static_cast<int64_t>(dailyResetOffset_) - elapsed;
    // Past the reset the answer is stale rather than negative; the next login
    // asks again. Zero reads as "not known", which is the honest answer.
    return left > 0 ? static_cast<uint32_t>(left) : 0;
}

void SocialHandler::handlePlayedTime(network::Packet& packet) {
    PlayedTimeData data;
    if (!PlayedTimeParser::parse(packet, data)) return;
    totalTimePlayed_ = data.totalTimePlayed;
    levelTimePlayed_ = data.levelTimePlayed;
    if (data.triggerMessage) {
        // TIME_PLAYED_MSG(totalTime, levelTime), in seconds, which is what
        // ChatFrame_SystemEventHandler dispatches to ChatFrame_DisplayTimePlayed
        // - the real client's own answer to /played, down to the localised
        // "days, hours, minutes, seconds" string. The server also sends played
        // time unasked at login, and triggerMessage is what tells the two
        // apart, so this stays inside it.
        owner_.fireAddonEvent("TIME_PLAYED_MSG",
                              {std::to_string(data.totalTimePlayed),
                               std::to_string(data.levelTimePlayed)});
        // ...and the hand-built lines below are what this client says when it
        // is drawing its own chat. With FrameXML drawing it they would be a
        // second, differently worded copy printed beside the real one - the
        // same shape as the gossip binder in quest_handler, where doing the
        // work here as well as letting the interface do it asked twice.
        if (ui::frameXmlOwns(ui::UiElement::Chat)) return;

        //
        // Worded as ChatFrame_DisplayTimePlayed words them: TIME_PLAYED_TOTAL
        // and TIME_PLAYED_LEVEL around TIME_DAYHOURMINUTESECOND. These were
        // English.
        const auto breakdown = [](uint32_t t) {
            return interfaceFormat("TIME_DAYHOURMINUTESECOND",
                "%d |4day:days;, %d |4hour:hours;, %d |4minute:minutes;, %d |4second:seconds;",
                {std::to_string(t / 86400), std::to_string((t % 86400) / 3600),
                 std::to_string((t % 3600) / 60), std::to_string(t % 60)});
        };
        owner_.addSystemChatMessage(interfaceFormat("TIME_PLAYED_TOTAL", "Total time played: %s",
                                                    {breakdown(data.totalTimePlayed)}));
        owner_.addSystemChatMessage(interfaceFormat("TIME_PLAYED_LEVEL", "Time played this level: %s",
                                                    {breakdown(data.levelTimePlayed)}));
    }
}

void SocialHandler::handleWho(network::Packet& packet) {
    const bool hasGender = isActiveExpansion("wotlk");
    uint32_t displayCount = packet.readUInt32();
    uint32_t onlineCount = packet.readUInt32();
    whoResults_.clear();
    whoOnlineCount_ = onlineCount;
    // No refusal for an empty answer: the count line below says "0 players
    // total" (WHO_NUM_RESULTS, 0x006b86b0). This raised an English "No
    // players found." on screen and told the who panel nothing.
    for (uint32_t i = 0; i < displayCount; ++i) {
        if (packet.getReadPos() >= packet.getSize()) break;
        std::string playerName = packet.readString();
        std::string guildName = packet.readString();
        if (!packet.hasRemaining(12)) break;
        uint32_t level   = packet.readUInt32();
        uint32_t classId = packet.readUInt32();
        uint32_t raceId  = packet.readUInt32();
        if (hasGender && packet.hasRemaining(1)) packet.readUInt8();
        uint32_t zoneId = 0;
        if (packet.hasRemaining(4)) zoneId = packet.readUInt32();
        WhoEntry entry;
        entry.name = playerName; entry.guildName = guildName;
        entry.level = level; entry.classId = classId;
        entry.raceId = raceId; entry.zoneId = zoneId;
        whoResults_.push_back(std::move(entry));
    }
    // To the chat when nobody is showing a panel, which is what a stock client
    // does and what SetWhoToUI is for. FriendsFrame turns that on while its Who
    // tab is up and off again on the way out, so a /who typed into chat with
    // the panel closed printed nothing at all here: the rows were parsed,
    // stored, and kept.
    //
    // The formats are WoW's own, from globalstrings - and they are the
    // client's to print rather than the interface's, which is why no FrameXML
    // file formats them. The count line carries a |4 escape, which resolves on
    // the way to the screen.
    //
    // Each row is WHO_LIST_FORMAT, or WHO_LIST_GUILD_FORMAT with a guild, and
    // the count WHO_NUM_RESULTS (0x006b5030, 0x006b86b0); the zone is always
    // there, UNKNOWN where it has no name. These were English templates.
    if (!whoToUI_) {
        for (const auto& e : whoResults_) {
            std::string zone = owner_.getWhoAreaName(e.zoneId);
            if (zone.empty()) zone = interfaceString("UNKNOWN", "Unknown");
            const std::string level = std::to_string(e.level);
            const std::string race = getRaceName(static_cast<Race>(e.raceId));
            const std::string cls = getClassName(static_cast<Class>(e.classId));
            owner_.addSystemChatMessage(e.guildName.empty()
                ? interfaceFormat("WHO_LIST_FORMAT", "|Hplayer:%s|h[%s]|h: Level %d %s %s - %s",
                                  {e.name, e.name, level, race, cls, zone})
                : interfaceFormat("WHO_LIST_GUILD_FORMAT", "|Hplayer:%s|h[%s]|h: Level %d %s %s <%s> - %s",
                                  {e.name, e.name, level, race, cls, e.guildName, zone}));
        }
        owner_.addSystemChatMessage(interfaceFormat("WHO_NUM_RESULTS", "%d |4player:players; total",
                                                    {std::to_string(whoOnlineCount_)}));
    }

    // The who panel asked for this and redraws its rows on the answer coming
    // back. Every row was parsed and stored and only this client's own window
    // ever read them, so a /who through the interface filled a list nobody was
    // told about and the panel kept showing the previous search.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("WHO_LIST_UPDATE", {});
}

// ============================================================
// Social (Friend/Ignore/Random Roll) Handlers
// ============================================================

void SocialHandler::handleFriendList(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 1) return;
    uint8_t count = packet.readUInt8();
    owner_.contactsRef().erase(std::remove_if(owner_.contactsRef().begin(), owner_.contactsRef().end(),
        [](const ContactEntry& e){ return e.isFriend(); }), owner_.contactsRef().end());
    for (uint8_t i = 0; i < count && rem() >= 9; ++i) {
        uint64_t guid   = packet.readUInt64();
        uint8_t  status = packet.readUInt8();
        uint32_t area = 0, level = 0, classId = 0;
        if (status != 0 && rem() >= 12) {
            area    = packet.readUInt32();
            level   = packet.readUInt32();
            classId = packet.readUInt32();
        }
        owner_.friendGuidsRef().insert(guid);
        auto nit = owner_.getPlayerNameCache().find(guid);
        std::string name;
        if (nit != owner_.getPlayerNameCache().end()) {
            name = nit->second;
            owner_.friendsCacheRef()[name] = guid;
        } else {
            owner_.queryPlayerName(guid);
        }
        ContactEntry entry;
        entry.guid = guid; entry.name = name; entry.flags = 0x1;
        entry.status = status; entry.areaId = area; entry.level = level; entry.classId = classId;
        owner_.contactsRef().push_back(std::move(entry));
    }
    // The full list arrives in reply to this client asking for it, so it is the
    // "show" case as well as the "changed" case. SHOW additionally re-selects
    // the tab; UPDATE only refreshes the rows. A single friend going online
    // sends the status packet instead, and fires UPDATE alone.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("FRIENDLIST_UPDATE", {});
        owner_.addonEventCallbackRef()("FRIENDLIST_SHOW", {});
    }
}

void SocialHandler::handleContactList(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 8) { packet.skipAll(); return; }
    owner_.lastContactListMaskRef()  = packet.readUInt32();
    owner_.lastContactListCountRef() = packet.readUInt32();
    owner_.contactsRef().clear();
    for (uint32_t i = 0; i < owner_.lastContactListCountRef() && rem() >= 8; ++i) {
        uint64_t guid  = packet.readUInt64();
        if (rem() < 4) break;
        uint32_t flags = packet.readUInt32();
        std::string note = packet.readString();
        uint8_t status = 0; uint32_t areaId = 0, level = 0, classId = 0;
        if (flags & 0x1) {
            if (rem() < 1) break;
            status = packet.readUInt8();
            if (status != 0 && rem() >= 12) {
                areaId = packet.readUInt32(); level = packet.readUInt32(); classId = packet.readUInt32();
            }
            owner_.friendGuidsRef().insert(guid);
            auto nit = owner_.getPlayerNameCache().find(guid);
            if (nit != owner_.getPlayerNameCache().end()) owner_.friendsCacheRef()[nit->second] = guid;
            else owner_.queryPlayerName(guid);
        }
        ContactEntry entry;
        entry.guid = guid; entry.flags = flags; entry.note = std::move(note);
        entry.status = status; entry.areaId = areaId; entry.level = level; entry.classId = classId;
        auto nit = owner_.getPlayerNameCache().find(guid);
        if (nit != owner_.getPlayerNameCache().end()) entry.name = nit->second;
        owner_.contactsRef().push_back(std::move(entry));
    }
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("FRIENDLIST_UPDATE", {});
        owner_.addonEventCallbackRef()("FRIENDLIST_SHOW", {});
        if (owner_.lastContactListMaskRef() & 0x2) owner_.addonEventCallbackRef()("IGNORELIST_UPDATE", {});
    }
}

void SocialHandler::handleFriendStatus(network::Packet& packet) {
    FriendStatusData data;
    if (!FriendStatusParser::parse(packet, data)) return;

    // Single lookup - reuse iterator for name resolution and update/erase below
    auto cit = std::find_if(owner_.contactsRef().begin(), owner_.contactsRef().end(),
        [&](const ContactEntry& e){ return e.guid == data.guid; });

    // Look up player name: contacts_ (populated by SMSG_FRIEND_LIST) > playerNameCache
    std::string playerName;
    if (cit != owner_.contactsRef().end() && !cit->name.empty()) {
        playerName = cit->name;
    } else {
        auto it = owner_.getPlayerNameCache().find(data.guid);
        if (it != owner_.getPlayerNameCache().end()) playerName = it->second;
    }

    // Only update friendsCache when we have a resolved name - inserting an empty
    // key creates a phantom entry that masks the real one when the name arrives.
    if (!playerName.empty()) {
        if (data.status == 1 || data.status == 2) owner_.friendsCacheRef()[playerName] = data.guid;
        else if (data.status == 0) owner_.friendsCacheRef().erase(playerName);
    }

    // AzerothCore's FriendsResult, read off SocialMgr.h rather than guessed.
    // The numbering here was five places out: zero is a database error and not
    // a removal, one is a full list and not an addition, and the three results
    // that matter most - removed, added-online, added-offline - are 5, 6 and 7
    // where this had "already in your list", "list is full" and "is ignoring
    // you". So adding a friend announced that the list was full, and a
    // database error deleted them from it.
    enum : uint8_t {
        kDbError = 0, kListFull = 1, kOnline = 2, kOffline = 3, kNotFound = 4,
        kRemoved = 5, kAddedOnline = 6, kAddedOffline = 7, kAlready = 8,
        kSelf = 9, kEnemy = 10,
    };
    if (data.status == kRemoved) {
        if (cit != owner_.contactsRef().end())
            owner_.contactsRef().erase(cit);
    } else if (data.status == kOnline || data.status == kOffline ||
               data.status == kAddedOnline || data.status == kAddedOffline) {
        const bool nowOnline =
            (data.status == kOnline || data.status == kAddedOnline);
        // chatFlag is the FriendStatus bitmask the packet carries, and it is
        // what the friends list reads for away and busy. Offline results carry
        // no body, so the flags are cleared rather than left as they were.
        const uint8_t flags = nowOnline ? data.chatFlag : 0;
        if (cit != owner_.contactsRef().end()) {
            if (!playerName.empty() && playerName != "Unknown") cit->name = playerName;
            cit->status = flags;
            if (nowOnline) {
                cit->areaId = data.areaId;
                cit->level = data.level;
                cit->classId = data.classId;
            }
            if (!data.note.empty()) cit->note = data.note;
        } else {
            ContactEntry entry;
            entry.guid = data.guid; entry.name = playerName; entry.flags = 0x1;
            entry.status = flags;
            entry.note = data.note;
            entry.areaId = data.areaId; entry.level = data.level;
            entry.classId = data.classId;
            owner_.contactsRef().push_back(std::move(entry));
        }
    }
    // Each result as 0x006b73a0 words it, system lines all: the friend
    // list's ERR_FRIEND_* - a database error reads as ERR_FRIEND_NOT_FOUND
    // there too, and the online notice links the name - and the ignore
    // list's ERR_IGNORE_*, which share this packet and said nothing here.
    // The lines that name a player wait for the name, as the original's do.
    // These were English.
    const std::vector<std::string> who = {playerName};
    const bool named = !playerName.empty();
    switch (data.status) {
        case kDbError:
        case kNotFound:     gameError(owner_, ErrTo::Chat, "ERR_FRIEND_NOT_FOUND", "Player not found."); break;
        case kListFull:     gameError(owner_, ErrTo::Chat, "ERR_FRIEND_LIST_FULL", "Your friends list is full."); break;
        case kOnline:
            if (named) gameError(owner_, ErrTo::Chat, "ERR_FRIEND_ONLINE_SS", "|Hplayer:%s|h[%s]|h has come online.",
                                 {playerName, playerName});
            break;
        case kOffline:      if (named) gameError(owner_, ErrTo::Chat, "ERR_FRIEND_OFFLINE_S", "%s has gone offline.", who); break;
        case kRemoved:      if (named) gameError(owner_, ErrTo::Chat, "ERR_FRIEND_REMOVED_S", "%s removed from friends list.", who); break;
        case kAddedOnline:
        case kAddedOffline: if (named) gameError(owner_, ErrTo::Chat, "ERR_FRIEND_ADDED_S", "%s added to friends.", who); break;
        case kAlready:      if (named) gameError(owner_, ErrTo::Chat, "ERR_FRIEND_ALREADY_S", "%s is already your friend.", who); break;
        case kSelf:         gameError(owner_, ErrTo::Chat, "ERR_FRIEND_SELF", "You can't put yourself on your friend list."); break;
        case kEnemy:        gameError(owner_, ErrTo::Chat, "ERR_FRIEND_WRONG_FACTION", "Friends must be part of your alliance."); break;
        case 0x0B:          gameError(owner_, ErrTo::Chat, "ERR_IGNORE_FULL", "You can't ignore any more players."); break;
        case 0x0C:          gameError(owner_, ErrTo::Chat, "ERR_IGNORE_SELF", "You can't ignore yourself."); break;
        case 0x0D:          gameError(owner_, ErrTo::Chat, "ERR_IGNORE_NOT_FOUND", "Player not found."); break;
        case 0x0E:          if (named) gameError(owner_, ErrTo::Chat, "ERR_IGNORE_ALREADY_S", "%s is already being ignored.", who); break;
        case 0x0F:          if (named) gameError(owner_, ErrTo::Chat, "ERR_IGNORE_ADDED_S", "%s is now being ignored.", who); break;
        case 0x10:          if (named) gameError(owner_, ErrTo::Chat, "ERR_IGNORE_REMOVED_S", "%s is no longer being ignored.", who); break;
        case 0x11:          gameError(owner_, ErrTo::Chat, "ERR_IGNORE_AMBIGUOUS", "That name is ambiguous, type more of the player's server name."); break;
        default: break;
    }
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("FRIENDLIST_UPDATE", {});
}

void SocialHandler::handleRandomRoll(network::Packet& packet) {
    RandomRollData data;
    if (!RandomRollParser::parse(packet, data)) return;
    // RANDOM_ROLL_RESULT with the roller's name - the player's own too - the
    // number and the range, a system line (0x006e2e90, MSG_RANDOM_ROLL); none
    // for a roller whose name is not known. This printed "You roll" /
    // "Someone rolls" in English.
    std::string rollerName = owner_.lookupName(data.rollerGuid);
    if (rollerName.empty() && data.rollerGuid == owner_.getPlayerGuid()) rollerName = ownName(owner_);
    if (rollerName.empty()) return;
    owner_.addSystemChatMessage(interfaceFormat("RANDOM_ROLL_RESULT", "%s rolls %d (%d-%d)",
        {rollerName, std::to_string(data.result), std::to_string(data.minRoll), std::to_string(data.maxRoll)}));
}

// ============================================================
// Logout Handlers
// ============================================================

void SocialHandler::handleLogoutResponse(network::Packet& packet) {
    LogoutResponseData data;
    if (!LogoutResponseParser::parse(packet, data)) return;
    if (data.result == 0) {
        // No line: the countdown is the CAMP or QUIT popup below (0x006b08b0
        // fires PLAYER_CAMPING or PLAYER_QUITING and says nothing). This
        // printed "Logging out..." / "Logging out in 20 seconds...".
        logoutCountdown_ = data.instant ? 0.0f : 20.0f;
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("PLAYER_LOGOUT", {});
            // The timer the server just imposed, announced so the interface can
            // show it. UIParent answers PLAYER_CAMPING with the CAMP popup and
            // PLAYER_QUITING with QUIT, both of which count down and offer to
            // cancel - and it already answers LOGOUT_CANCEL by hiding them, so
            // half of this conversation was wired and the half that starts it
            // was not. Nothing could show the twenty seconds but a line of chat.
            //
            // Which of the two is the difference between /logout and /quit,
            // which this handler already keeps in exitAfterLogout_.
            if (!data.instant) {
                owner_.addonEventCallbackRef()(
                    exitAfterLogout_ ? "PLAYER_QUITING" : "PLAYER_CAMPING", {});
            }
        }
    } else {
        // The server said no, and what that means depends on what was asked.
        //
        // For /logout it is the whole answer: the player stays in the world and
        // is told why. For Exit Game it is not. That button asks the program to
        // close, and a refusal governs whether the character leaves the world
        // tidily rather than whether this process keeps running - so printing
        // the error and staying put left a quit button that says no, which is
        // the one thing a quit button must never do.
        const bool exiting = exitAfterLogout_;
        loggingOut_ = false; exitAfterLogout_ = false; logoutCountdown_ = 0.0f;
        if (exiting) {
            LOG_WARNING("Logout refused, but Exit Game was asked for - closing anyway");
            if (owner_.logoutCompleteCallbackRef()) owner_.logoutCompleteCallbackRef()(true);
        } else {
            // ERR_LOGOUT_FAILED on screen (0x006b08b0); this was English.
            gameError(owner_, ErrTo::Error, "ERR_LOGOUT_FAILED", "You can't logout now.");
        }
    }
}

void SocialHandler::handleLogoutComplete(network::Packet& /*packet*/) {
    // The countdown finishing is not the end of it: the server says when the
    // character is actually out of the world, and only then can the client leave -
    // to character select, or out of the game entirely for /quit and /exit.
    // Nothing said - the client leaves the world. This printed "Logout
    // complete." on the way out.
    const bool exiting = exitAfterLogout_;
    loggingOut_ = false; exitAfterLogout_ = false; logoutCountdown_ = 0.0f;
    LOG_WARNING("Logout complete (exiting=", exiting ? "yes" : "no", ")");  // temporary: visible at default log level
    if (owner_.logoutCompleteCallbackRef()) owner_.logoutCompleteCallbackRef()(exiting);
}

// ============================================================
// Battleground Handlers
// ============================================================

void SocialHandler::handleBattlefieldStatus(network::Packet& packet) {
    BattlefieldStatusData status;
    if (!BattlefieldStatusPacket::parse(packet, status, isClassicLikeExpansion(),
                                        !isPreWotlk())) {
        return;
    }
    const uint32_t queueSlot = status.queueSlot;
    // 0x0054ae40 drops a status for any slot past the second.
    if (!isPreWotlk() && queueSlot > 1) return;
    bgJoinPendingType_ = 0;  // answered
    const uint8_t arenaType = status.arenaType;
    const uint32_t bgTypeId = status.bgTypeId;
    const uint32_t statusId = status.statusId;

    // The name GetBattlefieldStatus answers with, from where 0x0054bc30 takes
    // it: the map's own (Map.dbc) once a place is offered or entered, and the
    // battleground's (BattlemasterList.dbc) while queued - both in the
    // client's language. This was an English table of its own, "2v2 Arena
    // (Nagrand Arena)" for an arena, which the interface showed as it was.
    std::string bgName;
    if ((statusId == 2 || statusId == 3) && status.mapId != 0) bgName = owner_.getMapName(status.mapId);
    if (bgName.empty())
        if (const GameHandler::BattlemasterEntry* bg = owner_.getBattlemasterInfo(bgTypeId)) bgName = bg->name;

    // An invitation with no timeout in it still has to count down from
    // something, and eighty seconds is what the server gives one.
    const uint32_t inviteTimeout =
        status.inviteTimeoutMs ? status.inviteTimeoutMs / 1000 : 80;
    const uint32_t avgWaitSec = status.avgWaitMs / 1000;
    const uint32_t timeInQueueSec = status.timeInQueueMs / 1000;

    // Server pushes SMSG_BATTLEFIELD_STATUS periodically (~30s ticks while queued)
    // and also for each queue slot at zone change / login. Only emit a chat line
    // when the slot's statusId actually transitions; otherwise the same "Queued
    // for X." or "Entered X." spams chat every tick.
    bool statusChanged = false;
    if (queueSlot < bgQueues_.size()) {
        uint32_t prevStatus = bgQueues_[queueSlot].statusId;
        bool wasInvite = (prevStatus == 2);
        statusChanged = (prevStatus != statusId) || (bgQueues_[queueSlot].bgTypeId != bgTypeId);
        bgQueues_[queueSlot].queueSlot = queueSlot;
        bgQueues_[queueSlot].bgTypeId = bgTypeId;
        bgQueues_[queueSlot].arenaType = arenaType;
        bgQueues_[queueSlot].statusId = statusId;
        bgQueues_[queueSlot].bgName = bgName;
        bgQueues_[queueSlot].minLevel = status.minLevel;
        bgQueues_[queueSlot].maxLevel = status.maxLevel;
        bgQueues_[queueSlot].instanceId = status.instanceId;
        bgQueues_[queueSlot].isRated = status.isRated;
        if (statusId == 1) {
            bgQueues_[queueSlot].avgWaitTimeSec = avgWaitSec;
            bgQueues_[queueSlot].timeInQueueSec = timeInQueueSec;
            bgQueues_[queueSlot].queueTimeReceivedAt = std::chrono::steady_clock::now();
        }
        if (statusId == 2 && !wasInvite) { bgQueues_[queueSlot].inviteTimeout = inviteTimeout; bgQueues_[queueSlot].inviteReceivedTime = std::chrono::steady_clock::now(); }
    } else {
        statusChanged = true;
    }

    // Said in the log only. The original answers a status with
    // UPDATE_BATTLEFIELD_STATUS and no line (0x0054ae40) - the queue icon and
    // the "enter battle" popup are the interface's. This printed "Queued for
    // X.", "X is ready!" and "Entered X." in English.
    if (statusChanged) {
        LOG_WARNING("Battleground queue ", queueSlot + 1, ": status ", statusId,
                    " for type ", bgTypeId, " (", bgName, "), average wait ",
                    avgWaitSec, " s, waited ", timeInQueueSec, " s");
    }
    // The argument is which queue changed, counted from one, not what it
    // changed to. battlefieldframe.lua takes it as the index and raises the
    // "enter battleground" prompt only for the queue it names - handed the
    // status, an invitation (2) to the first queue named the second, and the
    // prompt never came up however long the player had waited for it.
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("UPDATE_BATTLEFIELD_STATUS", {std::to_string(queueSlot + 1)});
}

void SocialHandler::handleBattlefieldList(network::Packet& packet) {
    AvailableBgInfo info;
    if (!isPreWotlk()) {
        // 0x0054e390. This read the type out of the battlemaster's guid and
        // everything after it from the wrong place, so the instance list the
        // panel offers was noise.
        BattlefieldListData data;
        if (!BattlefieldListParser::parse(packet, data)) return;
        info.bgTypeId = data.bgTypeId;
        info.minLevel = data.minLevel;
        info.maxLevel = data.maxLevel;
        info.instanceIds = std::move(data.instanceIds);
        bool updated = false;
        for (auto& existing : availableBgs_) { if (existing.bgTypeId == info.bgTypeId) { existing = std::move(info); updated = true; break; } }
        if (!updated) availableBgs_.push_back(std::move(info));
        // The window it opens, as the client picks it: the random
        // battleground from a battlemaster is NPC_PVPQUEUE_ANYWHERE, a list
        // asked for from elsewhere PVPQUEUE_ANYWHERE_SHOW, and otherwise the
        // battlemaster's own list - BATTLEFIELDS_SHOW, the battlemaster the
        // interaction object (0x005496d0).
        constexpr uint32_t kRandomBattleground = 32;
        if (data.bgTypeId == kRandomBattleground && data.battlemasterGuid != 0) {
            owner_.setInteractionObject(data.battlemasterGuid);
            owner_.fireAddonEvent("NPC_PVPQUEUE_ANYWHERE", {});
        } else if (data.fromWhere != 0) {
            owner_.fireAddonEvent("PVPQUEUE_ANYWHERE_SHOW", {});
        } else if (data.battlemasterGuid != 0) {
            owner_.setInteractionObject(data.battlemasterGuid);
            owner_.fireAddonEvent("BATTLEFIELDS_SHOW", {});
        } else {
            owner_.fireAddonEvent("BATTLEFIELDS_CLOSED", {});
            owner_.fireAddonEvent("BATTLEFIELDS_SHOW", {});
        }
        return;
    }
    if (!packet.hasRemaining(5)) return;
    info.bgTypeId = packet.readUInt32();
    info.isRegistered = packet.readUInt8() != 0;
    if (isActiveExpansion("tbc")) { if (!packet.hasRemaining(1)) return; info.isHoliday = packet.readUInt8() != 0; }
    if (!packet.hasRemaining(4)) return;
    uint32_t count = std::min(packet.readUInt32(), 256u);
    info.instanceIds.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!packet.hasRemaining(4)) break;
        info.instanceIds.push_back(packet.readUInt32());
    }
    bool updated = false;
    for (auto& existing : availableBgs_) { if (existing.bgTypeId == info.bgTypeId) { existing = std::move(info); updated = true; break; } }
    if (!updated) availableBgs_.push_back(std::move(info));
}

bool SocialHandler::hasPendingBgInvite() const {
    for (const auto& slot : bgQueues_) { if (slot.statusId == 2) return true; }
    return false;
}

void SocialHandler::acceptBattlefield(uint32_t queueSlot) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    const BgQueueSlot* slot = nullptr;
    if (queueSlot == 0xFFFFFFFF) { for (const auto& s : bgQueues_) { if (s.statusId == 2) { slot = &s; break; } } }
    else if (queueSlot < bgQueues_.size() && bgQueues_[queueSlot].statusId == 2) slot = &bgQueues_[queueSlot];
    // AcceptBattlefieldPort sends the answer and says nothing, and nothing
    // for want of an invitation either. These were English lines.
    if (!slot) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_BATTLEFIELD_PORT));
    pkt.writeUInt8(slot->arenaType); pkt.writeUInt8(0x00); pkt.writeUInt32(slot->bgTypeId);
    pkt.writeUInt16(0x0000); pkt.writeUInt8(1);
    owner_.getSocket()->send(pkt);
    uint32_t clearSlot = slot->queueSlot;
    if (clearSlot < bgQueues_.size()) bgQueues_[clearSlot].statusId = 3;
}

// Leave the battleground the player is standing in, which is a different thing
// from declining an invitation to one - that is CMSG_BATTLEFIELD_PORT with a
// refusal, this is the player walking out of a match already joined.
// CMSG_BATTLEMASTER_JOIN: which battlemaster is being spoken to, which
// battleground, which instance of it, and whether the whole group is coming.
// An instance of zero means "first available", which is what the top row of
// the battlefield list is.
void SocialHandler::joinBattlefield(uint64_t battlemasterGuid, uint32_t bgTypeId,
                                    uint32_t instanceId, bool asGroup) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_BATTLEMASTER_JOIN));
    packet.writeUInt64(battlemasterGuid);
    packet.writeUInt32(bgTypeId);
    packet.writeUInt32(instanceId);
    packet.writeUInt8(asGroup ? 1 : 0);
    owner_.getSocket()->send(packet);
    bgJoinPendingType_ = bgTypeId;
    bgJoinWaitSec_ = 5.0f;
}

// CMSG_BATTLEFIELD_LIST: ask which instances of one battleground are running.
// The reply is SMSG_BATTLEFIELD_LIST, which this client already handles - it
// just had no way to ask, because nothing reached the frame that asks.
void SocialHandler::requestBattlefieldList(uint32_t bgTypeId) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_BATTLEFIELD_LIST));
    packet.writeUInt32(bgTypeId);
    // WotLK added two bytes: where the request came from, and whether the
    // player wants experience from it. Classic and TBC read the type id alone,
    // and trailing bytes there would be read as the next packet.
    if (!isPreWotlk()) {
        packet.writeUInt8(0);  // fromWhere: 0 = the battlemaster list frame
        packet.writeUInt8(1);  // canGainXP
    }
    owner_.getSocket()->send(packet);
}

// CMSG_REPORT_PVP_AFK: this player is not participating. The reply,
// SMSG_REPORT_PVP_AFK_RESULT, was already handled - the request was the half
// that had no way to be sent.
void SocialHandler::reportPvpAfk(uint64_t playerGuid) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (playerGuid == 0) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_REPORT_PVP_AFK));
    pkt.writeUInt64(playerGuid);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::leaveBattlefield() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_LEAVE_BATTLEFIELD));
    owner_.getSocket()->send(pkt);
}

void SocialHandler::declineBattlefield(uint32_t queueSlot) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    const BgQueueSlot* slot = nullptr;
    if (queueSlot == 0xFFFFFFFF) { for (const auto& s : bgQueues_) { if (s.statusId == 2) { slot = &s; break; } } }
    // A named queue is left whether it has popped or is still waiting: the
    // server takes the same refusal for both.
    else if (queueSlot < bgQueues_.size() &&
             (bgQueues_[queueSlot].statusId == 1 || bgQueues_[queueSlot].statusId == 2))
        slot = &bgQueues_[queueSlot];
    // Silent, as declining or leaving a queue is in the original - this
    // printed "No battleground invitation pending.", "Battleground
    // invitation declined." and "Left the queue for X.".
    if (!slot) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_BATTLEFIELD_PORT));
    pkt.writeUInt8(slot->arenaType); pkt.writeUInt8(0x00); pkt.writeUInt32(slot->bgTypeId);
    pkt.writeUInt16(0x0000); pkt.writeUInt8(0);
    owner_.getSocket()->send(pkt);
    uint32_t clearSlot = slot->queueSlot;
    if (clearSlot < bgQueues_.size()) bgQueues_[clearSlot] = BgQueueSlot{};
    // The minimap icon and the panel read the queues on this event; without
    // it they kept showing the queue until the server's next word.
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("UPDATE_BATTLEFIELD_STATUS", {std::to_string(clearSlot + 1)});
}

void SocialHandler::requestPvpLog() {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::MSG_PVP_LOG_DATA));
    owner_.getSocket()->send(pkt);
}

// ============================================================
// Instance Handlers
// ============================================================

void SocialHandler::handleRaidInstanceInfo(network::Packet& packet) {
    const bool isTbc = isActiveExpansion("tbc");
    const bool isClassic = isClassicLikeExpansion();
    const bool useTbcFormat = isTbc || isClassic;
    instanceLockouts_.clear();
    if (!useTbcFormat) {
        // 0x00501030: map, difficulty, the u64 instance id, locked, extended,
        // then the u32 seconds left. This took the instance id for the reset
        // time and left the seconds unread, so every save after the first
        // was read four bytes out and each reset was an instance id.
        std::vector<RaidInstanceSave> saves;
        RaidInstanceInfoParser::parse(packet, saves);
        const auto now = static_cast<uint64_t>(std::time(nullptr));
        instanceLockouts_.reserve(saves.size());
        for (const RaidInstanceSave& save : saves) {
            InstanceLockout lo;
            lo.mapId = save.mapId;
            lo.difficulty = save.difficulty;
            lo.instanceId = save.instanceId;
            lo.resetTime = now + save.resetSeconds;
            lo.locked = save.locked;
            lo.extended = save.extended;
            instanceLockouts_.push_back(lo);
        }
        return;
    }
    if (!packet.hasRemaining(4)) return;
    uint32_t count = packet.readUInt32();
    instanceLockouts_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (packet.getRemainingSize() < 13) break;
        InstanceLockout lo;
        lo.mapId = packet.readUInt32(); lo.difficulty = packet.readUInt32();
        lo.resetTime = packet.readUInt32(); lo.locked = packet.readUInt8() != 0; lo.extended = false;
        instanceLockouts_.push_back(lo);
    }
}

void SocialHandler::handleInstanceDifficulty(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 4) return;
    uint32_t prevDifficulty = instanceDifficulty_;
    instanceDifficulty_ = packet.readUInt32();
    if (rem() >= 4) {
        uint32_t secondField = packet.readUInt32();
        // The player's difficulty (0x00526530 -> 0xbd1980).
        owner_.playerDifficultyRef() = secondField;
        if (rem() >= 4) instanceIsHeroic_ = (instanceDifficulty_ == 1);
        else instanceIsHeroic_ = (secondField != 0);
    } else {
        instanceIsHeroic_ = (instanceDifficulty_ == 1);
    }
    inInstance_ = true;
    if (instanceDifficulty_ != prevDifficulty) {
        // ERR_DUNGEON_DIFFICULTY_CHANGED_S naming DUNGEON_DIFFICULTY<n>, a
        // system line - for the difficulty being set (MSG_SET_DUNGEON_DIFFICULTY,
        // 0x00525530 -> 0x00524720) and not for the instance's own
        // (SMSG_INSTANCE_DIFFICULTY, which 0x00526530 only stores). This was
        // English, for either.
        if (packet.getOpcode() == wireOpcode(Opcode::MSG_SET_DUNGEON_DIFFICULTY)) {
            const std::string key = "DUNGEON_DIFFICULTY" + std::to_string(instanceDifficulty_ + 1);
            gameError(owner_, ErrTo::Chat, "ERR_DUNGEON_DIFFICULTY_CHANGED_S",
                      "Dungeon difficulty set to %s.", {interfaceString(key.c_str(), key.c_str())});
        }
        // Already noticed and already said - in chat, to the player. The
        // minimap hangs a difficulty banner off its own corner and rereads
        // GetInstanceInfo on this, which answers from the value just stored,
        // so the banner was the one thing not told the difficulty had changed.
        if (owner_.addonEventCallbackRef()) {
            owner_.addonEventCallbackRef()("PLAYER_DIFFICULTY_CHANGED", {});
            owner_.addonEventCallbackRef()("UPDATE_INSTANCE_INFO", {});
        }
    }
}

// ============================================================
// LFG Handlers
// ============================================================

void SocialHandler::handleLfgJoinResult(network::Packet& packet) {
    // Two uint32s, not two bytes. Reading a byte each left the result right by
    // accident - every value in that enum fits in one - and the state wrong
    // always: it came from the second byte of the result, which is zero. So a
    // queue joined successfully set the state to None, and the dungeon finder
    // never knew it was in a queue.
    if (!packet.hasRemaining(8)) return;
    const uint32_t result = packet.readUInt32();
    const uint32_t state  = packet.readUInt32();
    // A join is said by the queue update that follows it (ERR_LFG_JOINED_QUEUE)
    // and a refusal here, as 0x0055bdc0 picks it from its table at
    // 0x00acdda8 - system lines but for ERR_LFG_CANT_USE_DUNGEONS on screen,
    // and a failed role check named by its reason. These were English
    // ("Dungeon Finder: ..."), refusals said twice.
    if (result == 0) {
        lfgState_ = static_cast<LfgState>(state);
    } else if (result == 1 && state == 3) {
        gameError(owner_, ErrTo::Chat, "ERR_LFG_ROLE_CHECK_FAILED_TIMEOUT", "The Role Check has failed because a group member did not respond.");
    } else if (result == 1 && state == 4) {
        gameError(owner_, ErrTo::Chat, "ERR_LFG_ROLE_CHECK_FAILED_NOT_VIABLE", "The Role Check has failed because your group is not viable.");
    } else {
        switch (result) {
            case 1: case 18:
                    gameError(owner_, ErrTo::Chat, "ERR_LFG_ROLE_CHECK_FAILED", "The Role Check has failed."); break;
            case 2: gameError(owner_, ErrTo::Chat, "ERR_LFG_GROUP_FULL", "Your group is already full."); break;
            case 4: gameError(owner_, ErrTo::Chat, "ERR_LFG_NO_LFG_OBJECT", "Internal LFG Error."); break;
            case 5: gameError(owner_, ErrTo::Chat, "ERR_LFG_NO_SLOTS_PLAYER", "You do not meet the requirements for the chosen dungeons."); break;
            case 6: gameError(owner_, ErrTo::Chat, "ERR_LFG_NO_SLOTS_PARTY", "One or more party members do not meet the requirements for the chosen dungeons."); break;
            case 7: gameError(owner_, ErrTo::Chat, "ERR_LFG_MISMATCHED_SLOTS", "You cannot mix dungeons, raids, and random when picking dungeons."); break;
            case 8: gameError(owner_, ErrTo::Chat, "ERR_LFG_PARTY_PLAYERS_FROM_DIFFERENT_REALMS", "The dungeon you chose does not support players from multiple realms."); break;
            case 9: gameError(owner_, ErrTo::Chat, "ERR_LFG_MEMBERS_NOT_PRESENT", "One or more party members are pending invites or disconnected."); break;
            case 10: gameError(owner_, ErrTo::Chat, "ERR_LFG_GET_INFO_TIMEOUT", "Could not retrieve information about some party members."); break;
            case 11: gameError(owner_, ErrTo::Chat, "ERR_LFG_INVALID_SLOT", "One or more dungeons was not valid."); break;
            case 12: gameError(owner_, ErrTo::Chat, "ERR_LFG_DESERTER_PLAYER", "You can not queue for dungeons until your deserter debuff wears off."); break;
            case 13: gameError(owner_, ErrTo::Chat, "ERR_LFG_DESERTER_PARTY", "One or more party members has a deserter debuff."); break;
            case 14: gameError(owner_, ErrTo::Chat, "ERR_LFG_RANDOM_COOLDOWN_PLAYER", "You can not queue for random dungeons while on random dungeon cooldown."); break;
            case 15: gameError(owner_, ErrTo::Chat, "ERR_LFG_RANDOM_COOLDOWN_PARTY", "One or more party members are on random dungeon cooldown."); break;
            case 16: gameError(owner_, ErrTo::Chat, "ERR_LFG_TOO_MANY_MEMBERS", "You have too many group members to queue for that."); break;
            case 17: gameError(owner_, ErrTo::Error, "ERR_LFG_CANT_USE_DUNGEONS", "You cannot queue for a dungeon or raid while using battlegrounds or arenas."); break;
            default: break;
        }
    }
    // The queue state moved, either into a queue or back out of the attempt,
    // and LFG_UPDATE is how the panel is told to re-read it - the same event
    // handleLfgUpdate fires for the same reason two handlers down. Without it
    // the state was right in this client and stale in the interface until some
    // other LFG packet happened along.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("LFG_UPDATE", {});
}

// SMSG_LFG_PLAYER_INFO - what the random dungeons pay out, and what this
// character is not allowed to queue for.
//
// The reward block has to be walked even though none of it is kept, because
// the locks are behind it and its length depends on how many items each
// random dungeon rewards. Skipping to the end was not an option; the whole
// packet was being skipped instead.
void SocialHandler::handleLfgPlayerInfo(network::Packet& packet) {
    if (!packet.hasRemaining(1)) { packet.skipAll(); return; }
    const uint8_t randomCount = packet.readUInt8();
    lfgRewards_.clear();
    for (uint8_t i = 0; i < randomCount; ++i) {
        if (!packet.hasRemaining(4 + 1 + 4 * 4 + 1)) { packet.skipAll(); return; }
        LfgReward r;
        r.dungeonId  = packet.readUInt32() & 0x00FFFFFFu;
        r.done       = packet.readUInt8() != 0;
        r.money      = packet.readUInt32();
        r.experience = packet.readUInt32();
        packet.readUInt32();          // the variable parts, which the server
        packet.readUInt32();          // sends as zero
        const uint8_t items = packet.readUInt8();
        for (uint8_t j = 0; j < items; ++j) {
            if (!packet.hasRemaining(12)) { packet.skipAll(); return; }
            LfgRewardItem item;
            item.itemId        = packet.readUInt32();
            item.displayInfoId = packet.readUInt32();
            item.count         = packet.readUInt32();
            r.items.push_back(item);
        }
        lfgRewards_.push_back(std::move(r));
    }

    if (!packet.hasRemaining(4)) { packet.skipAll(); return; }
    const uint32_t lockCount = packet.readUInt32();
    lfgLocks_.clear();
    for (uint32_t i = 0; i < lockCount && packet.hasRemaining(8); ++i) {
        // The entry packs the dungeon and its type; the list is keyed by the
        // dungeon alone, which is what the interface asks with.
        const uint32_t entry = packet.readUInt32();
        const uint32_t status = packet.readUInt32();
        lfgLocks_[entry & 0x00FFFFFFu] = status;
    }
    packet.skipAll();

    LOG_INFO("SMSG_LFG_PLAYER_INFO: ", randomCount, " random dungeons, ",
             lfgLocks_.size(), " locked");
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("LFG_LOCK_INFO_RECEIVED", {});
        owner_.addonEventCallbackRef()("LFG_UPDATE_RANDOM_INFO", {});
    }
}

// SMSG_LFG_PARTY_INFO - what each party member may not queue for. Skipped
// until now; 0x0055bdc0 (case 0x372, 0x0055b080) reads a count, then per
// member a guid and the same lock list SMSG_LFG_PLAYER_INFO ends with, and
// raises the two events that one does.
void SocialHandler::handleLfgPartyInfo(network::Packet& packet) {
    lfgPartyLocks_.clear();
    if (!packet.hasRemaining(1)) { packet.skipAll(); return; }
    const uint8_t members = packet.readUInt8();
    for (uint8_t i = 0; i < members; ++i) {
        if (!packet.hasRemaining(8 + 4)) break;
        const uint64_t guid = packet.readUInt64();
        const uint32_t lockCount = packet.readUInt32();
        auto& locks = lfgPartyLocks_[guid];
        for (uint32_t j = 0; j < lockCount && packet.hasRemaining(8); ++j) {
            const uint32_t entry = packet.readUInt32();
            locks[entry & 0x00FFFFFFu] = packet.readUInt32();
        }
    }
    packet.skipAll();
    owner_.fireAddonEvent("LFG_LOCK_INFO_RECEIVED");
    owner_.fireAddonEvent("LFG_UPDATE_RANDOM_INFO");
}

void SocialHandler::handleLfgQueueStatus(network::Packet& packet) {
    // Thirty-one bytes, not thirty-three: dungeon, five wait times, three
    // one-byte "needed" counts, and the time queued. The old length check was
    // longer than the packet the server sends, so this returned before reading
    // anything and the queue status never updated at all - no wait times, no
    // time in queue.
    if (!packet.hasRemaining(31)) return;
    lfgDungeonId_ = packet.readUInt32();
    // Two different numbers, and the panel shows them in two different places:
    // the average across everyone queued for this dungeon, and the estimate for
    // this player in the role they picked. They were being collapsed into one.
    lfgAvgWaitSec_ = static_cast<int32_t>(packet.readUInt32());
    lfgMyWaitSec_  = static_cast<int32_t>(packet.readUInt32());
    lfgWaitTank_   = static_cast<int32_t>(packet.readUInt32());
    lfgWaitHealer_ = static_cast<int32_t>(packet.readUInt32());
    lfgWaitDps_    = static_cast<int32_t>(packet.readUInt32());
    // Three of these, one per role, and only one was being read - so the time
    // in queue came from two bytes of the needed-counts and two of its own.
    lfgNeedTank_   = packet.readUInt8();
    lfgNeedHealer_ = packet.readUInt8();
    lfgNeedDps_    = packet.readUInt8();
    // Seconds, not milliseconds. Every time on this packet is a difference of
    // two time_t values on the server, and all three were being divided by a
    // thousand on the way in - so the queue timer read 0:00 for the whole wait
    // and the average wait time came out as zero, which the interface prints
    // through SecondsToTime as an empty string. A wait estimate that is missing
    // rather than wrong is this division.
    //
    // -1 is the server saying it does not know yet, and it is kept: the panel
    // tests for it and says so, where a zero claims the group is instant.
    lfgQueuedSeconds_ = packet.readUInt32();
    lfgState_ = LfgState::Queued;
    // Read and then kept to itself. The wait time is what the queue window
    // counts up, and LFDSearchStatus_Update is reached from this event and no
    // other - so the numbers above changed every few seconds and the display
    // built on them never moved.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("LFG_QUEUE_STATUS_UPDATE", {});
        // The queue state is set two lines above, and LFG_QUEUE_STATUS_UPDATE
        // is not the event that re-reads it: the minimap's dungeon eye is shown
        // by MiniMapLFG_UpdateIsShown, which asks GetLFGMode - which asks
        // GetLFGInfoServer - and is reached from LFG_UPDATE and nowhere else.
        //
        // SMSG_LFG_UPDATE_PLAYER usually arrives alongside this and does fire
        // it, which is why the eye appears at all. The case that has nothing
        // else behind it is a login or a reconnect already in the queue, where
        // this packet is what says so: the state was right and the eye was
        // never shown.
        owner_.addonEventCallbackRef()("LFG_UPDATE", {});
    }
}

namespace {

// 3.3.5 LfgProposalState. Nought is the proposal arriving, not the proposal
// failing: the three were read one place along, so the packet that offers the
// group announced "Group proposal failed" and closed the dialog before it was
// ever shown, and the one that says everyone accepted announced the offer.
// Reported as a group found and then a proposal failed, which is those two
// lines in the order the server sends them.
enum : uint32_t {
    kProposalInitiating = 0,
    kProposalFailed     = 1,
    kProposalSuccess    = 2,
};

}  // namespace

void SocialHandler::handleLfgProposalUpdate(network::Packet& packet) {
    // dungeon(4), state(1), id(4), encounters(4), silent(1), size(1), and a
    // row per member - see parseLfgProposal. This once read three uint32s for
    // the header, so the state came out of the id's bytes.
    LfgProposal proposal;
    if (!parseLfgProposal(packet, proposal)) return;
    packet.skipAll();

    // The entry packs the dungeon and its type, the same way
    // LFGDungeonData::Entry builds it.
    const uint32_t dungeonId = proposal.dungeonEntry & 0x00FFFFFFu;
    lfgProposalMembers_.clear();
    for (const auto& r : proposal.members) {
        LfgProposalMember m;
        m.role = r.roles;
        m.isSelf = r.isSelf;
        m.inDungeon = r.inDungeon;
        m.sameGroup = r.sameGroup;
        m.answered = r.answered;
        m.accepted = r.accepted;
        lfgProposalMembers_.push_back(m);
    }
    lfgDungeonId_ = dungeonId;
    lfgProposalId_ = proposal.id;
    // Held until a decline below or a queue update ends it (0x0055bdc0,
    // cases 0x361, 0x367, 0x368).
    lfgProposalValid_ = proposal.dungeonEntry != 0;
    switch (proposal.state) {
        case kProposalInitiating: lfgState_ = LfgState::Proposal;  break;
        case kProposalFailed:     lfgState_ = LfgState::Queued;    break;
        case kProposalSuccess:    lfgState_ = LfgState::InDungeon; break;
        default: break;
    }
    // The state itself announces nothing: LFG_PROPOSAL_FAILED and _SUCCEEDED
    // come from the queue update that ends the proposal (applyLfgUpdate),
    // and from here only on a decline. This fired them off the state, which
    // the client never looks at.
    const LfgProposalOutcome outcome = decideLfgProposal(proposal, shownProposalId_);
    owner_.fireAddonEvent("LFG_PROPOSAL_UPDATE");
    if (outcome.show) owner_.fireAddonEvent("LFG_PROPOSAL_SHOW");
    if (outcome.declinedLine) {
        if (std::string_view(outcome.declinedLine) == "ERR_LFG_PROPOSAL_DECLINED_PARTY")
            gameError(owner_, ErrTo::Chat, "ERR_LFG_PROPOSAL_DECLINED_PARTY", "A member of your group has declined the proposal.");
        else
            gameError(owner_, ErrTo::Chat, "ERR_LFG_PROPOSAL_DECLINED_SELF", "You have declined the group invitation.");
    }
    if (outcome.failed) {
        clearLfgProposal();
        owner_.fireAddonEvent("LFG_PROPOSAL_FAILED");
    }
}

void SocialHandler::clearLfgProposal() {
    lfgProposalValid_ = false;
    lfgProposalId_ = 0;
    lfgProposalMembers_.clear();
}

void SocialHandler::handleLfgRoleCheckUpdate(network::Packet& packet) {
    // state is the uint32; the uint8 after it says whether the check is only
    // starting. This read the state and threw it away, then took the starting
    // flag for the state - so the branches below ran on 0 or 1 and never saw
    // the values they test for.
    if (!packet.hasRemaining(6)) return;
    const uint32_t roleCheckState = packet.readUInt32();
    const bool initiating = packet.readUInt8() != 0;

    // The dungeons being checked for, and how many are in the group. Both were
    // stopped at: the count was read and the list behind it left on the wire.
    //
    // That is not merely a blank. GetLFGRoleUpdate answered nil for the slot
    // count, and the popup's own tooltip opens with `if ( slots <= 1 )` -
    // comparing nil to a number, which raises. Hovering the role-check popup
    // took the file down.
    //
    // Each dungeon arrives as its LFGDungeons entry, which packs the type into
    // the top byte the same way the queue list does above.
    lfgRoleCheckDungeons_.clear();
    const uint8_t dungeonCount = packet.readUInt8();
    for (uint8_t i = 0; i < dungeonCount && packet.hasRemaining(4); ++i) {
        const uint32_t entry = packet.readUInt32();
        lfgRoleCheckDungeons_.push_back({.dungeonId = entry & 0x00FFFFFFu,
                                         .typeId = static_cast<uint8_t>((entry >> 24) & 0xFFu)});
    }
    lfgRoleCheckMembers_ = packet.hasRemaining(1) ? packet.readUInt8() : 0;
    // Each member: guid, whether they have chosen, their roles and level.
    // The popup is for a player who has not chosen yet (0x0055bdc0, case
    // 0x363) - the server restates the check as each member picks, and
    // showing it whatever the player's own answer raised it again over a
    // choice already made. The first member stands in when the player is
    // not listed, as there.
    bool selfChosen = false;
    for (uint8_t i = 0; i < lfgRoleCheckMembers_ && packet.hasRemaining(14); ++i) {
        const uint64_t guid = packet.readUInt64();
        const bool chosen = packet.readUInt8() != 0;
        packet.readUInt32();  // roles
        packet.readUInt8();   // level
        if (i == 0 || guid == owner_.getPlayerGuid()) selfChosen = chosen;
    }

    if (roleCheckState == 1) lfgState_ = LfgState::Queued;
    else if (roleCheckState == 3) lfgState_ = LfgState::None;
    else if (roleCheckState == 2) lfgState_ = LfgState::RoleCheck;
    // ERR_LFG_ROLE_CHECK_INITIATED when the check starts and
    // ERR_LFG_ROLE_CHECK_ABORTED for state 5, system lines (0x0055bdc0); a
    // failed check is said by the join result. These were English, the
    // missing-role one on screen as well.
    if (initiating)
        gameError(owner_, ErrTo::Chat, "ERR_LFG_ROLE_CHECK_INITIATED", "A role check has been initiated. Your group will be queued when all members have selected a role.");
    if (roleCheckState == 5)
        gameError(owner_, ErrTo::Chat, "ERR_LFG_ROLE_CHECK_ABORTED", "The role check has been aborted.");
    // Likewise GetLFGRoleUpdate, which reports the check in progress.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("LFG_ROLE_CHECK_UPDATE", {});
        // The role-check popup shows on one event and hides on another, and
        // neither was ever fired - so the interface's own popup could not
        // appear at all, whichever side is drawing it.
        //
        // State 2 is LFG_ROLECHECK_INITIALITING, where the check begins.
        // Everything else is an ending: finished, missing a role, the wrong
        // roles between them, aborted, or someone picking none.
        constexpr uint32_t kRoleCheckBeginning = 2;
        owner_.addonEventCallbackRef()(
            roleCheckState == kRoleCheckBeginning && !selfChosen ? "LFG_ROLE_CHECK_SHOW"
                                                                 : "LFG_ROLE_CHECK_HIDE", {});
    }
}

/// SMSG_LFG_UPDATE_PLAYER and SMSG_LFG_UPDATE_PARTY - read and decided in
/// game/lfg_update, applied here.
bool SocialHandler::lfgBodyHasRaid(const LfgUpdateBody& body) const {
    if (!body.extraInfo) return false;
    for (uint32_t entry : body.dungeons) {
        const uint32_t id = entry & 0x00FFFFFFu;
        for (const auto& d : owner_.getLfgDungeons()) {
            if (d.id == id && d.typeId == static_cast<uint32_t>(LfgTypeId::Raid)) return true;
        }
    }
    return false;
}

void SocialHandler::handleLfgUpdatePlayer(network::Packet& packet) {
    uint8_t updateType = 0;
    LfgUpdateBody body;
    if (!parseLfgUpdate(packet, /*party=*/false, updateType, body)) return;
    applyLfgUpdate(updateType, body, /*party=*/false);
}

void SocialHandler::handleLfgUpdateParty(network::Packet& packet) {
    uint8_t updateType = 0;
    LfgUpdateBody body;
    if (!parseLfgUpdate(packet, /*party=*/true, updateType, body)) return;
    applyLfgUpdate(updateType, body, /*party=*/true);
}

void SocialHandler::applyLfgUpdate(uint8_t updateType, const LfgUpdateBody& body, bool party) {
    using namespace lfg_update_type;
    LfgUpdateBody& stored = party ? lfgPartyUpdate_ : lfgPlayerUpdate_;
    const LfgUpdateOutcome outcome = decideLfgUpdate(
        updateType, body, stored, party, isInGroup(), lfgProposalValid_,
        [this](const LfgUpdateBody& b) { return lfgBodyHasRaid(b); });

    if (outcome.proposalEnds) {
        clearLfgProposal();
        if (outcome.proposalReturnedLine)
            gameError(owner_, ErrTo::Chat, "ERR_LFG_PROPOSAL_FAILED", "Your group has been returned to the queue.");
        owner_.fireAddonEvent(outcome.proposalEvent);
    }
    if (!outcome.changed) return;

    if (outcome.line) {
        struct Line { const char* key; const char* enUS; };
        static constexpr Line kLines[] = {
            {"ERR_LFG_JOINED_QUEUE", "You are now queued in the Dungeon Finder."},
            {"ERR_LFG_JOINED_LIST", "You are now listed in the Raid Browser."},
            {"ERR_LFG_LEFT_QUEUE", "You have left the Dungeon Finder queue."},
            {"ERR_LFG_LEFT_LIST", "You have been removed from the Raid Browser."},
        };
        for (const auto& l : kLines)
            if (std::string_view(l.key) == outcome.line) gameError(owner_, ErrTo::Chat, l.key, l.enUS);
    }
    stored = body;
    if (!body.dungeons.empty()) lfgDungeonId_ = body.dungeons.front() & 0x00FFFFFFu;

    // This client's own summary of the queue, which GetLFGMode reads. A
    // proposal is the one state the flags do not tell apart: the server sends
    // it with the queue flag still set. The types are the client's numbering
    // (lfg_update_type); the list this used before ran one high from 5 up,
    // so 14 - the routine status restatement - read as a proposal starting.
    if (updateType == kGroupFound || updateType == kProposalBegin) {
        lfgState_ = LfgState::Proposal;
    } else if (!body.extraInfo) {
        // No body means no queue - except for the types that are not a
        // statement about the queue at all.
        if (updateType != kDefault && updateType != kLeaderUnk1 &&
            updateType != kGroupMemberOffline)
            lfgState_ = LfgState::None;
    } else if (body.queued) {
        lfgState_ = LfgState::Queued;
    } else if (party && body.joined) {
        // The party's join flag covers being in the dungeon as well as being
        // in the queue, so it only means "in a dungeon" when not queued.
        lfgState_ = LfgState::InDungeon;
    }

    // LFG_UPDATE, false, from the side the update is for (0x0055bdc0).
    if (outcome.fireUpdate) owner_.fireAddonEvent("LFG_UPDATE", {eventBool(false)});
}

void SocialHandler::handleLfgPlayerReward(network::Packet& packet) {
    // SMSG_LFG_PLAYER_REWARD, against WorldSession::SendLfgPlayerReward:
    //
    //   uint32 rdungeonEntry   the random entry queued for
    //   uint32 sdungeonEntry   the dungeon actually finished
    //   uint8  done
    //   uint32 1               a constant the server writes and nothing reads
    //   uint32 money           GetRewOrReqMoney(level), in copper
    //   uint32 xp              XPValue(level)
    //   uint32 0
    //   uint32 0
    //   uint8  itemNum
    //   itemNum x { uint32 itemId, uint32 displayId, uint32 count }
    //
    // This read the constant as the money, the money as the XP and the XP as
    // the item count - so the line it printed was always "1c" followed by the
    // real money labelled XP, and then it looped tens of thousands of times
    // over whatever came next looking for items.
    //
    // packet_layout_check does cover this opcode and did not catch it, for a
    // reason worth keeping: it compares field *widths*, and every field misread
    // here is four bytes wide. Delete one of them and it reports the shape
    // change immediately; shift the meaning of five fields that are all uint32
    // and the two readings line up byte for byte. A same-width misalignment is
    // the one shape that sweep cannot see.
    if (!packet.hasRemaining(4 + 4 + 1 + 4 + 4 + 4 + 4 + 4 + 1)) return;
    LfgCompletionReward reward;
    reward.randomDungeonId = packet.readUInt32();
    reward.dungeonId       = packet.readUInt32();
    reward.done            = packet.readUInt8() != 0;
    // 0x0055bdc0 names the four after it: strangers, base money, base XP,
    // variable money, variable XP - the variable parts paid per stranger.
    const uint32_t strangers = packet.readUInt32();
    reward.money           = packet.readUInt32();
    reward.xp              = packet.readUInt32();
    const uint32_t varMoney = packet.readUInt32();
    const uint32_t varXp    = packet.readUInt32();
    const uint8_t itemNum = packet.readUInt8();
    for (uint8_t i = 0; i < itemNum && packet.hasRemaining(12); ++i) {
        LfgCompletionReward::Item item;
        item.itemId    = packet.readUInt32();
        item.displayId = packet.readUInt32();
        item.count     = packet.readUInt32();
        reward.items.push_back(item);
    }
    reward.valid = true;
    lfgCompletionReward_ = reward;

    lfgState_ = LfgState::FinishedDungeon;
    // The experience and the money as a quest reward's are said,
    // ERR_QUEST_REWARD_EXP_I and ERR_QUEST_REWARD_MONEY_S, system lines
    // (0x0055bdc0); the items are announced by their own pushes. This built
    // an English "Dungeon Finder reward: ..." it never printed.
    //
    // Each figure on its own line when above zero, in the client's order: the
    // base XP, the variable XP times the strangers, the base money, the
    // variable money times the strangers. Signed, as it compares them.
    const auto sayXp = [&](int32_t xp) {
        if (xp > 0)
            gameError(owner_, ErrTo::Chat, "ERR_QUEST_REWARD_EXP_I", "Experience gained: %d.", {std::to_string(xp)});
    };
    const auto sayMoney = [&](int32_t money) {
        if (money > 0)
            gameError(owner_, ErrTo::Chat, "ERR_QUEST_REWARD_MONEY_S", "Received %s.",
                      {interface_text::moneyText(static_cast<uint32_t>(money),
                                                 [](const char* k, const char* e) { return interfaceString(k, e); })});
    };
    sayXp(static_cast<int32_t>(reward.xp));
    sayXp(static_cast<int32_t>(strangers * varXp));
    sayMoney(static_cast<int32_t>(reward.money));
    sayMoney(static_cast<int32_t>(strangers * varMoney));

    // The reward toast. It carries no arguments - the frame reads the nine
    // values back through GetLFGCompletionReward - so the store above has to
    // be written before this fires.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("LFG_COMPLETION_REWARD", {});

    // No hand-built chat line: FrameXML announces the reward its own way, in
    // a toast rather than a line, and printing both is two announcements of
    // one reward.
}

void SocialHandler::handleLfgBootProposalUpdate(network::Packet& packet) {
    // The victim's GUID sits between the three flags and the counts, and was
    // not being read - so every number after it came from eight bytes earlier
    // than it should have.
    if (!packet.hasRemaining(27)) return;
    bool inProgress = packet.readUInt8() != 0;
    // Both were read and dropped. The vote dialog needs them: didVote decides
    // whether it shows the buttons or the tally, and myVote which of the two
    // is lit.
    lfgBootDidVote_ = packet.readUInt8() != 0;
    lfgBootMyVote_  = packet.readUInt8() != 0;
    lfgBootInProgress_ = inProgress;
    lfgBootVictimGuid_ = packet.readUInt64();
    uint32_t totalVotes = packet.readUInt32();
    uint32_t bootVotes = packet.readUInt32();
    uint32_t timeLeft = packet.readUInt32();
    uint32_t votesNeeded = packet.readUInt32();
    lfgBootVotes_ = bootVotes; lfgBootTotal_ = totalVotes;
    lfgBootTimeLeft_ = timeLeft; lfgBootNeeded_ = votesNeeded;
    if (packet.getReadPos() < packet.getSize()) lfgBootReason_ = packet.readString();
    // The packet carries one string and it is the reason. The victim is the
    // guid above - AzerothCore writes `data << boot.victim` and then the
    // reason, and nothing else - so a second string was being looked for that
    // is never there, and the name it was meant to fill stayed empty. Resolved
    // from the guid instead, which is how every other name here is found.
    lfgBootTargetName_ = owner_.lookupName(lfgBootVictimGuid_);
    if (lfgBootTargetName_.empty() && lfgBootVictimGuid_ != 0) {
        owner_.queryPlayerName(lfgBootVictimGuid_);
    }
    if (inProgress) { lfgState_ = LfgState::Boot; }
    else {
        const bool bootPassed = (bootVotes >= votesNeeded);
        const std::string victimName = lfgBootTargetName_;
        lfgBootVotes_ = lfgBootTotal_ = lfgBootTimeLeft_ = lfgBootNeeded_ = 0;
        lfgBootTargetName_.clear(); lfgBootReason_.clear();
        lfgBootDidVote_ = lfgBootMyVote_ = lfgBootInProgress_ = false;
        lfgBootVictimGuid_ = 0;
        lfgState_ = LfgState::InDungeon;
        // ERR_PARTY_LFG_BOOT_VOTE_SUCCEEDED or _FAILED naming the player, a
        // system line, when the name is known (0x0055bdc0). This was English.
        if (!victimName.empty()) {
            if (bootPassed)
                gameError(owner_, ErrTo::Chat, "ERR_PARTY_LFG_BOOT_VOTE_SUCCEEDED",
                          "The vote to kick %s has passed.", {victimName});
            else
                gameError(owner_, ErrTo::Chat, "ERR_PARTY_LFG_BOOT_VOTE_FAILED",
                          "The vote to kick %s has failed.", {victimName});
        }
    }
    // The vote-to-kick dialog, whose countdown starts on this.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("LFG_BOOT_PROPOSAL_UPDATE", {});
}

void SocialHandler::handleLfgTeleportDenied(network::Packet& packet) {
    // A uint32, read as a byte. Little-endian kept it working - every reason
    // is a small number - but the read was the wrong width and the next edit
    // to this handler would have inherited it.
    if (!packet.hasRemaining(4)) return;
    const uint32_t reason = packet.readUInt32();
    // On screen, through 0x0055bdc0's jump table at 0x0055d148: dead,
    // falling, locked out, fatigued, an invalid location - which the table
    // follows with "locked out" as well, falling through - and "locked out"
    // for any reason past seven; nothing for 0, 5 and 7. This printed
    // "Dungeon Finder: " and an English reason of its own numbering.
    switch (reason) {
        case 1: gameError(owner_, ErrTo::Error, "ERR_PLAYER_DEAD", "You can't do that when you're dead."); break;
        case 2: gameError(owner_, ErrTo::Error, "ERR_NOT_WHILE_FALLING", "You can't do that while jumping or falling"); break;
        case 4: gameError(owner_, ErrTo::Error, "ERR_NOT_WHILE_FATIGUED", "You can't do that while fatigued"); break;
        case 6:
            gameError(owner_, ErrTo::Error, "ERR_INVALID_TELEPORT_LOCATION", "Invalid teleport location");
            [[fallthrough]];
        case 3: gameError(owner_, ErrTo::Error, "ERR_CLIENT_LOCKED_OUT", "You can't do that right now."); break;
        case 0: case 5: case 7: break;
        default: gameError(owner_, ErrTo::Error, "ERR_CLIENT_LOCKED_OUT", "You can't do that right now."); break;
    }
}

// ============================================================
// LFG Outgoing Packets
// ============================================================

void SocialHandler::lfgJoin(const std::vector<uint32_t>& dungeonIds, uint8_t roles) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (dungeonIds.empty()) return;   // the server drops a join with no slots
    // The dungeon finder is Wrath's. Classic and TBC have no CMSG_LFG_JOIN at
    // all - TBC's 0x35C is CMSG_LFG_SET_AUTOJOIN, an unrelated packet - so the
    // logical opcode is unmapped there and wireOpcode answers 0xFFFF. Sending
    // that would put a packet the server has no handler for on the wire.
    if (wireOpcode(Opcode::CMSG_LFG_JOIN) == 0xFFFF) return;
    owner_.getSocket()->send(LfgJoinPacket::build(dungeonIds, roles));
}

void SocialHandler::lfgLeave() {
    if (!owner_.getSocket()) return;
    lfgState_ = LfgState::None;
    const uint16_t wire = wireOpcode(Opcode::CMSG_LFG_LEAVE);
    if (wire == 0xFFFF) return;   // no dungeon finder before Wrath
    network::Packet pkt(wire);
    pkt.writeUInt32(0); pkt.writeUInt32(0); pkt.writeUInt32(0);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::lfgSetRoles(uint8_t roles) {
    lfgOfferedRoles_ = roles;
    // Three frames draw the same three role checkboxes - the dungeon queue, the
    // raid browser queue and the role-check popup - and every one of them ticks
    // from GetLFGRoles when this event arrives. Setting the roles without
    // firing it left whichever frame was clicked correct and the other two
    // showing what they were last drawn with, which is the shape that reads as
    // "it only takes after a relog".
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("LFG_ROLE_UPDATE", {});
    }
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    const uint32_t wire = wireOpcode(Opcode::CMSG_LFG_SET_ROLES);
    if (wire == 0xFFFF) return;
    network::Packet pkt(static_cast<uint16_t>(wire));
    pkt.writeUInt8(roles);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::lfgAcceptProposal(uint32_t proposalId, bool accept) {
    if (!owner_.getSocket()) return;
    const uint16_t wire = wireOpcode(Opcode::CMSG_LFG_PROPOSAL_RESULT);
    if (wire == 0xFFFF) return;   // no dungeon finder before Wrath
    network::Packet pkt(wire);
    pkt.writeUInt32(proposalId); pkt.writeUInt8(accept ? 1 : 0);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::lfgTeleport(bool toLfgDungeon) {
    if (!owner_.getSocket()) return;
    const uint16_t wire = wireOpcode(Opcode::CMSG_LFG_TELEPORT);
    if (wire == 0xFFFF) return;   // no dungeon finder before Wrath
    network::Packet pkt(wire);
    pkt.writeUInt8(toLfgDungeon ? 0 : 1);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::lfgSetBootVote(bool vote) {
    if (!owner_.getSocket()) return;
    uint16_t wireOp = wireOpcode(Opcode::CMSG_LFG_SET_BOOT_VOTE);
    if (wireOp == 0xFFFF) return;
    network::Packet pkt(wireOp);
    pkt.writeUInt8(vote ? 1 : 0);
    owner_.getSocket()->send(pkt);
}

// ============================================================
// Arena Handlers
// ============================================================

// command(4) + team name + player name + error(4).
//
// The player name was never read, so the error came out of the first four
// bytes of it - a name, so never zero, and every command result read as a
// failure however well it had gone. Where the server sends no player name the
// error slid one byte instead, which is the same fault with a different value.
// Both strings are needed anyway: the server names the team and the player
// separately because most of these results are about one acting on the other.
void SocialHandler::handleArenaTeamCommandResult(network::Packet& packet) {
    if (!packet.hasRemaining(10)) return;
    const uint32_t command = packet.readUInt32();
    const std::string teamName = packet.readString();
    const std::string playerName = packet.readString();
    if (!packet.hasRemaining(4)) return;
    const uint32_t error = packet.readUInt32();

    // ArenaTeamCommandTypes (0 create, 1 invite, 3 quit, 0x0E founder) and
    // the refusals, each as 0x006ccb20 words it from the ERR_ARENA_TEAM_*
    // strings with the team or the player put in - system lines all, and
    // nothing for a code it has no words for. This printed "arena team invite
    // succeeded: ..." and English reasons.
    const std::vector<std::string> team = {teamName};
    const std::vector<std::string> player = {playerName};
    if (error == 0) {
        switch (command) {
            case 0x00: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_CREATE_S", "%s created.  To disband, use /teamdisband [2v2, 3v3, 5v5].", team); break;
            case 0x01: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_INVITE_SS", "You have invited %s to join %s", {playerName, teamName}); break;
            case 0x03: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_QUIT_S", "You are no longer a member of %s", team); break;
            case 0x0E: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_FOUNDER_S", "Congratulations, you are a founding member of %s!  To leave, use /teamquit [2v2, 3v3, 5v5].", team); break;
            default: break;
        }
        return;
    }
    switch (error) {
        case 0x01: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_INTERNAL", "Internal arena team error"); break;
        case 0x02: gameError(owner_, ErrTo::Chat, "ERR_ALREADY_IN_ARENA_TEAM", "You are already in an arena team of that size"); break;
        case 0x03: gameError(owner_, ErrTo::Chat, "ERR_ALREADY_IN_ARENA_TEAM_S", "%s is already in an arena team of that size", player); break;
        case 0x04: gameError(owner_, ErrTo::Chat, "ERR_INVITED_TO_ARENA_TEAM", "You have already been invited into an arena team"); break;
        case 0x05: gameError(owner_, ErrTo::Chat, "ERR_ALREADY_INVITED_TO_ARENA_TEAM_S", "%s has already been invited to an arena team", player); break;
        case 0x06: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_NAME_INVALID", "That name contains invalid characters, please enter a new name"); break;
        case 0x07: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_NAME_EXISTS_S", "There is already an arena team named \"%s\"", team); break;
        case 0x08:
            if (command == 0x03)
                gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_LEADER_LEAVE_S", "You must promote a new team captain using /teamcaptain before leaving the team", team);
            else
                gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_PERMISSIONS", "You don't have permission to do that");
            break;
        case 0x09: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_PLAYER_NOT_IN_TEAM", "You are not in an arena team of that size"); break;
        case 0x0A: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_PLAYER_NOT_IN_TEAM_SS", "%s is not in %s", {playerName, teamName}); break;
        case 0x0B: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_PLAYER_NOT_FOUND_S", "\"%s\" not found", player); break;
        case 0x0C: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_NOT_ALLIED", "You cannot invite players from the opposing alliance"); break;
        case 0x13: gameError(owner_, ErrTo::Chat, "ERR_IGNORING_YOU_S", "%s is ignoring you.", player); break;
        case 0x15: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_TARGET_TOO_LOW_S", "%s is not high enough level to join your team", player); break;
        case 0x16: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_TARGET_TOO_HIGH_S", "%s is too high level to join your team", player); break;
        case 0x17: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_TOO_MANY_MEMBERS_S", "%s is full", team); break;
        case 0x1B: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_NOT_FOUND", "Arena team not found"); break;
        case 0x1E: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAMS_LOCKED", "Arena teams are locked"); break;
        default: break;
    }
}

void SocialHandler::handleArenaTeamQueryResponse(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    uint32_t teamId = packet.readUInt32();
    std::string teamName = packet.readString();
    uint32_t teamType = 0;
    if (packet.hasRemaining(4)) teamType = packet.readUInt32();
    bool known = false;
    for (auto& s : arenaTeamStats_) { if (s.teamId == teamId) { s.teamName = teamName; s.teamType = teamType; known = true; break; } }
    if (!known) {
        ArenaTeamStats stub; stub.teamId = teamId; stub.teamName = teamName; stub.teamType = teamType;
        arenaTeamStats_.push_back(std::move(stub));
    }
    // A team's name arriving is what the PvP panel redraws on. Without this the
    // arena tabs kept whatever they were built with, so a team queried after
    // the panel opened stayed nameless until it was closed and opened again.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("ARENA_TEAM_UPDATE", {});
    }
}

void SocialHandler::handleArenaTeamRoster(network::Packet& packet) {
    // teamId(4) + unk308(1) + memberCount(4) + teamType(4), then the members.
    //
    // The team type was never read, so the first member's guid was taken from
    // four bytes earlier - the type plus the guid's low half - and every
    // member after it followed from there. The roster was wrong from its first
    // row, and nothing about it looked like a length fault: the packet is long
    // enough either way.
    if (!packet.hasRemaining(13)) return;
    const uint32_t teamId = packet.readUInt32();
    // 3.0.8 added this, and it does more than mark a version: the server writes
    // the two trailing floats per member only when it is set.
    const bool hasTrailingFloats = packet.readUInt8() != 0;
    const uint32_t memberCount = std::min(packet.readUInt32(), 100u);
    /*uint32_t teamType =*/ packet.readUInt32();
    ArenaTeamRoster roster; roster.teamId = teamId; roster.members.reserve(memberCount);
    for (uint32_t i = 0; i < memberCount; ++i) {
        if (!packet.hasRemaining(12)) break;
        ArenaTeamMember m;
        m.guid = packet.readUInt64(); m.online = (packet.readUInt8() != 0); m.name = packet.readString();
        // Captain flag, level and class sit between the name and the record -
        // six bytes this went straight past, so the games played were the
        // captain flag, the wins were the level and class with two bytes of the
        // real figure behind them, and so on down the row.
        if (!packet.hasRemaining(6)) break;
        m.isCaptain = (packet.readUInt32() == 0);   // 0 captain, 1 member
        m.level     = packet.readUInt8();
        m.classId   = packet.readUInt8();
        if (!packet.hasRemaining(20)) break;
        m.weekGames = packet.readUInt32(); m.weekWins = packet.readUInt32();
        m.seasonGames = packet.readUInt32(); m.seasonWins = packet.readUInt32(); m.personalRating = packet.readUInt32();
        // Only when the header said so. Taking them whenever eight bytes were
        // left ate the next member's guid on every roster of more than one.
        if (hasTrailingFloats) {
            if (!packet.hasRemaining(8)) break;
            packet.readFloat(); packet.readFloat();
        }
        roster.members.push_back(std::move(m));
    }
    bool replaced = false;
    for (auto& r : arenaTeamRosters_) { if (r.teamId == teamId) { r = std::move(roster); replaced = true; break; } }
    if (!replaced) arenaTeamRosters_.push_back(std::move(roster));

    // The roster is stored and the arena panel is told, which it was not: the
    // team detail window redraws on this event and on nothing else, so a
    // roster that arrived after the window opened was held here and never
    // shown.
    //
    // Fired with no arguments, and that is the whole contract. pvpframe reads
    // arg1 as "the roster you hold is stale, ask for it again" and only
    // redraws from what arrived when arg1 is absent - so any argument at all
    // would answer this roster by requesting another one, forever. A zero
    // would do it too, zero being true in Lua.
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("ARENA_TEAM_ROSTER_UPDATE", {});
    }
}

void SocialHandler::handleArenaTeamInvite(network::Packet& packet) {
    std::string playerName = packet.readString();
    std::string teamName = packet.readString();
    // ARENA_TEAM_INVITE_REQUEST for the popup, and a system line in the
    // guild invitation's words, ERR_INVITED_TO_GUILD_SSS - which is what
    // 0x006cc910 prints for this. The line was English and the popup never
    // asked.
    if (owner_.addonEventCallbackRef())
        owner_.addonEventCallbackRef()("ARENA_TEAM_INVITE_REQUEST", {playerName, teamName});
    gameError(owner_, ErrTo::Chat, "ERR_INVITED_TO_GUILD_SSS",
              "|Hplayer:%s|h[%s]|h has invited you to join the guild, %s.",
              {playerName, playerName, teamName});
}

void SocialHandler::handleArenaTeamEvent(network::Packet& packet) {
    if (!packet.hasRemaining(1)) return;
    uint8_t event = packet.readUInt8();
    uint8_t strCount = 0;
    if (packet.hasRemaining(1)) strCount = packet.readUInt8();
    std::vector<std::string> args;
    for (uint8_t i = 0; i < strCount && i < 3 && packet.getSize() > packet.getReadPos(); ++i)
        args.push_back(packet.readString());
    // ArenaTeamEvents, as 0x006cc980 words them, with the event's strings put
    // in - system lines: 3 joined (ERR_ARENA_TEAM_YOU_JOIN_S when it is the
    // player), 4 left, 5 removed, 6 the captain, 7 a new captain, 8
    // disbanded, and ERR_GUILD_INTERNAL for anything else. These were
    // English, numbered from 0.
    const auto a = [&](size_t i) { return i < args.size() ? args[i] : std::string(); };
    switch (event) {
        case 3:
            if (!args.empty() && args[0] == ownName(owner_))
                gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_YOU_JOIN_S",
                          "You have joined %s.  To leave, use /teamquit [2v2, 3v3, 5v5].", {a(1)});
            else
                gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_JOIN_SS", "%s has joined %s", args);
            break;
        case 4: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_LEAVE_SS", "%s has left %s", args); break;
        case 5: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_REMOVE_SSS", "%s has been kicked out of %s by %s.", args); break;
        case 6: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_LEADER_IS_SS", "%s is the captain of %s", args); break;
        case 7: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_LEADER_CHANGED_SSS", "%s has made %s the new captain of %s.", args); break;
        case 8: gameError(owner_, ErrTo::Chat, "ERR_ARENA_TEAM_DISBANDED_S", "%s has disbanded %s.", args); break;
        default: gameError(owner_, ErrTo::Chat, "ERR_GUILD_INTERNAL", "Internal guild error."); break;
    }
}

void SocialHandler::handleArenaTeamStats(network::Packet& packet) {
    if (!packet.hasRemaining(28)) return;
    ArenaTeamStats stats;
    stats.teamId = packet.readUInt32(); stats.rating = packet.readUInt32();
    stats.weekGames = packet.readUInt32(); stats.weekWins = packet.readUInt32();
    stats.seasonGames = packet.readUInt32(); stats.seasonWins = packet.readUInt32();
    stats.rank = packet.readUInt32();
    // ARENA_TEAM_UPDATE, as 0x005a2d50 fires it once the figures are in;
    // the PvP panel's ratings and games redraw on it and on nothing else.
    bool known = false;
    for (auto& s : arenaTeamStats_) {
        if (s.teamId == stats.teamId) { stats.teamName = std::move(s.teamName); stats.teamType = s.teamType; s = std::move(stats); known = true; break; }
    }
    if (!known) arenaTeamStats_.push_back(std::move(stats));
    owner_.fireAddonEvent("ARENA_TEAM_UPDATE", {});
}

void SocialHandler::requestArenaTeamRoster(uint32_t teamId) {
    if (!owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_ARENA_TEAM_ROSTER));
    pkt.writeUInt32(teamId);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::handleArenaError(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    uint32_t error = packet.readUInt32();
    // On screen, as 0x006cce90 says it: 0 is ERR_ARENA_NO_TEAM_II with the
    // team size after it (twice, "2v2"), 1 ERR_ARENA_EXPIRED_CAIS, 2
    // ERR_LFG_CANT_USE_BATTLEGROUND, and nothing for the rest. These were
    // English reasons of another numbering, in chat.
    switch (error) {
        case 0: {
            const std::string size = packet.hasRemaining(1) ? std::to_string(packet.readUInt8()) : std::string("0");
            gameError(owner_, ErrTo::Error, "ERR_ARENA_NO_TEAM_II", "You are not in a %dv%d arena team", {size, size});
            break;
        }
        case 1: gameError(owner_, ErrTo::Error, "ERR_ARENA_EXPIRED_CAIS", "You may not queue while one or more of your team members is under the effect of restricted play."); break;
        case 2: gameError(owner_, ErrTo::Error, "ERR_LFG_CANT_USE_BATTLEGROUND", "You cannot queue for a battleground or arena while using the dungeon system."); break;
        default: break;
    }
}

void SocialHandler::handlePvpLogData(network::Packet& packet) {
    if (!PvpLogDataParser::parse(packet, bgScoreboard_)) return;
    for (auto& ps : bgScoreboard_.players) {
        auto ent = owner_.getEntityManager().getEntity(ps.guid);
        if (ent && (ent->getType() == game::ObjectType::PLAYER || ent->getType() == game::ObjectType::UNIT)) {
            auto u = std::static_pointer_cast<game::Unit>(ent);
            if (!u->getName().empty()) ps.name = u->getName();
        }
    }
    // The scoreboard is drawn from this and asked for it by name -
    // RequestBattlefieldScoreData sends the query, UPDATE_BATTLEFIELD_SCORE is
    // the answer arriving. Every row, every column and the winner were parsed
    // and stored, and the frame that displays them was never told.
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("UPDATE_BATTLEFIELD_SCORE", {});
}

void SocialHandler::updateLogoutCountdown(float deltaTime) {
    // A queue request the server let pass without a word. A realm that keeps
    // a battleground back - ChromieCraft opens the random one at level sixty,
    // and only through a command of its own - drops the join unanswered, and
    // the panel then did nothing at all when its button was pressed.
    if (bgJoinPendingType_ != 0) {
        bgJoinWaitSec_ -= deltaTime;
        if (bgJoinWaitSec_ <= 0.0f) {
            // In the log only: the original has no such notice, and this
            // said an English sentence of its own on screen and in chat.
            LOG_WARNING("Battleground join unanswered: type ", bgJoinPendingType_);
            bgJoinPendingType_ = 0;
        }
    }
    if (loggingOut_ && logoutCountdown_ > 0.0f) {
        logoutCountdown_ -= deltaTime;
        if (logoutCountdown_ < 0.0f) logoutCountdown_ = 0.0f;
    }
    // The bind prompt expires on its own; the server stops asking and the
    // pending bind lapses, so the dialog has to stop offering an answer.
    if (instanceLock_.active) {
        instanceLock_.secondsLeft -= deltaTime;
        if (instanceLock_.secondsLeft <= 0.0f) {
            instanceLock_.secondsLeft = 0.0f;
            instanceLock_.active = false;
            owner_.fireAddonEvent("INSTANCE_LOCK_STOP", {});
        }
    }
}

void SocialHandler::respondInstanceLock(bool accept) {
    // Accepting saves the player to the instance; declining sends them to the
    // graveyard. The server acts on whichever arrives and clears the pending
    // bind, so sending twice is worse than not sending at all.
    if (!instanceLock_.active || !owner_.getSocket()) return;
    instanceLock_.active = false;

    network::Packet resp(wireOpcode(Opcode::CMSG_INSTANCE_LOCK_RESPONSE));
    resp.writeUInt8(accept ? 1 : 0);
    owner_.getSocket()->send(resp);

    LOG_INFO("CMSG_INSTANCE_LOCK_RESPONSE: ", accept ? "accepted" : "declined");
    owner_.fireAddonEvent("INSTANCE_LOCK_STOP", {});
}

void SocialHandler::resetTransferState() {
    instanceLock_ = InstanceLockPrompt{};
    encounterUnitGuids_.fill(0);
    raidTargetGuids_.fill(0);
}

// ============================================================
// Moved opcode handlers (from GameHandler::registerOpcodeHandlers)
// ============================================================

void SocialHandler::handleInitializeFactions(network::Packet& packet) {
    if (!packet.hasRemaining(4)) return;
    uint32_t count = packet.readUInt32();
    size_t needed = static_cast<size_t>(count) * 5;
    if (!packet.hasRemaining(needed)) { packet.skipAll(); return; }
    owner_.initialFactionsRef().clear();
    owner_.initialFactionsRef().reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        GameHandler::FactionStandingInit fs{};
        fs.flags = packet.readUInt8();
        fs.standing = static_cast<int32_t>(packet.readUInt32());
        owner_.initialFactionsRef().push_back(fs);
    }
    // Also keyed by faction, which is how everything that draws a standing
    // asks for one. This packet is indexed by ReputationListID and the map is
    // indexed by faction id, and only SMSG_SET_FACTION_STANDING wrote to the
    // map - so a standing was known from login and unreadable until it next
    // moved. The reputation tab drew every bar empty at Neutral however much
    // the character had earned, and so did this client's own panel and the
    // watched-faction bar, all three reading the same empty map.
    //
    // And each made whole: the packet carries what has been earned, and the
    // standing is that on top of where the faction starts for this race and
    // class. See factionBaseReputation.
    owner_.loadFactionNameCache();
    for (size_t repListId = 0; repListId < owner_.initialFactionsRef().size(); ++repListId) {
        const uint32_t factionId =
            owner_.getFactionIdByRepListId(static_cast<uint32_t>(repListId));
        if (factionId == 0) continue;
        auto& standing = owner_.initialFactionsRef()[repListId].standing;
        standing += owner_.factionBaseReputation(factionId);
        owner_.factionStandingsRef()[factionId] = standing;
    }
    LOG_INFO("Reputation: ", owner_.initialFactionsRef().size(),
             " factions initialised, ", owner_.factionStandingsRef().size(),
             " resolved to a faction id");
    // Whether each faction with a standing is hostile is the at-war flag this
    // packet carries, so units judged before it arrived are judged again.
    owner_.refreshUnitHostility();
}

void SocialHandler::handleSetFactionStanding(network::Packet& packet) {
    // Expansion wire layouts differ before the shared count + entries payload:
    //   Classic: count
    //   TBC:     RAF bonus (float), count
    //   WotLK:   RAF bonus (float), showVisual (uint8), count
    // Treating WotLK's first float byte as showVisual shifted the entire packet,
    // producing bogus counts, faction names, standings, and chat messages.
    float rafBonus = 0.0f;
    if (isActiveExpansion("wotlk")) {
        if (!packet.hasRemaining(9)) return;
        rafBonus = packet.readFloat();
        /*uint8_t showVisual =*/ packet.readUInt8();
    } else if (isActiveExpansion("tbc")) {
        if (!packet.hasRemaining(8)) return;
        rafBonus = packet.readFloat();
    } else if (!packet.hasRemaining(4)) {
        return;
    }

    uint32_t count = packet.readUInt32();
    count = std::min(count, 128u);
    owner_.loadFactionNameCache();
    for (uint32_t i = 0; i < count && packet.hasRemaining(8); ++i) {
        // The packet carries Faction.dbc ReputationListID, not the faction ID.
        uint32_t repListId = packet.readUInt32();
        int32_t  standing  = static_cast<int32_t>(packet.readUInt32());

        uint32_t factionId = owner_.getFactionIdByRepListId(repListId);
        if (factionId == 0) {
            LOG_WARNING("SMSG_SET_FACTION_STANDING: unknown repListId=", repListId,
                        " standing=", standing);
            continue;
        }
        // Earned only, as at login: the whole is this on top of the start.
        standing += owner_.factionBaseReputation(factionId);

        int32_t  oldStanding = 0;
        // SMSG_INITIALIZE_FACTIONS is indexed by ReputationListID and supplies the
        // standing before incremental updates. Without using it, the first message
        // reports the character's entire accumulated reputation as the gained amount.
        if (repListId < owner_.initialFactionsRef().size()) {
            oldStanding = owner_.initialFactionsRef()[repListId].standing;
            owner_.initialFactionsRef()[repListId].standing = standing;
        } else {
            auto it = owner_.factionStandingsRef().find(factionId);
            if (it != owner_.factionStandingsRef().end()) oldStanding = it->second;
        }
        owner_.factionStandingsRef()[factionId] = standing;
        int32_t delta = standing - oldStanding;
        if (delta != 0) {
            std::string name = owner_.getFactionName(factionId);
            // FACTION_STANDING_INCREASED or _DECREASED with the faction and
            // the amount, _INCREASED_BONUS with a Recruit-A-Friend bonus, as
            // CHAT_MSG_COMBAT_FACTION_CHANGE and nothing else (0x0050b8c0).
            // This was English, said twice - once as a system line besides.
            std::string line;
            const std::string amount = std::to_string(std::abs(delta));
            if (delta < 0) {
                line = interfaceFormat("FACTION_STANDING_DECREASED",
                    "Your %s reputation has decreased by %d.", {name, amount});
            } else if (rafBonus > 0.0f) {
                // The one format with a %.1f, which luaFormat does not take:
                // the bonus goes in first.
                std::string fmt = interfaceString("FACTION_STANDING_INCREASED_BONUS",
                    "Reputation with %s increased by %d. (+%.1f Refer-A-Friend bonus)");
                char bonus[32];
                std::snprintf(bonus, sizeof(bonus), "%.1f", static_cast<double>(rafBonus));
                if (const auto at = fmt.find("%.1f"); at != std::string::npos) fmt.replace(at, 4, bonus);
                line = chat_channels::luaFormat(fmt, {name, amount});
            } else {
                line = interfaceFormat("FACTION_STANDING_INCREASED",
                    "Reputation with %s increased by %d.", {name, amount});
            }
            owner_.watchedFactionIdRef() = factionId;
            if (owner_.repChangeCallbackRef()) owner_.repChangeCallbackRef()(name, delta, standing);
            // These events fire unconditionally on any rep change (not gated by callback).
            owner_.fireAddonEvent("UPDATE_FACTION", {});
            owner_.addLocalChatLine(ChatType::COMBAT_FACTION_CHANGE, line);
        }
    }
}

void SocialHandler::handleSetFactionAtWar(network::Packet& packet) {
    if (!packet.hasRemaining(5)) { packet.skipAll(); return; }
    uint32_t repListId = packet.readUInt32();
    uint8_t  setAtWar  = packet.readUInt8();
    if (repListId < owner_.initialFactionsRef().size()) {
        if (setAtWar)
            owner_.initialFactionsRef()[repListId].flags |=  GameHandler::FACTION_FLAG_AT_WAR;
        else
            owner_.initialFactionsRef()[repListId].flags &= ~GameHandler::FACTION_FLAG_AT_WAR;
        owner_.refreshUnitHostility();
    }
}

void SocialHandler::handleSetFactionVisible(network::Packet& packet) {
    if (!packet.hasRemaining(5)) { packet.skipAll(); return; }
    uint32_t repListId = packet.readUInt32();
    uint8_t  visible   = packet.readUInt8();
    if (repListId < owner_.initialFactionsRef().size()) {
        if (visible)
            owner_.initialFactionsRef()[repListId].flags |=  GameHandler::FACTION_FLAG_VISIBLE;
        else
            owner_.initialFactionsRef()[repListId].flags &= ~GameHandler::FACTION_FLAG_VISIBLE;
    }
}

void SocialHandler::handleGroupSetLeader(network::Packet& packet) {
    if (!packet.hasData()) return;
    std::string leaderName = packet.readString();
    auto& pd = mutablePartyData();
    for (const auto& m : pd.members) {
        if (m.name == leaderName) { pd.leaderGuid = m.guid; break; }
    }
    // ERR_NEW_LEADER_YOU for the player and ERR_NEW_LEADER_S for anyone
    // else, system lines (0x006cbd70). This was English.
    if (!leaderName.empty()) {
        if (leaderName == ownName(owner_))
            gameError(owner_, ErrTo::Chat, "ERR_NEW_LEADER_YOU", "You are now the group leader.");
        else
            gameError(owner_, ErrTo::Chat, "ERR_NEW_LEADER_S", "%s is now the group leader.", {leaderName});
    }
    owner_.fireAddonEvent("PARTY_LEADER_CHANGED", {});
    owner_.fireAddonEvent("GROUP_ROSTER_UPDATE", {});
}

// ============================================================
// Minimap Ping
// ============================================================

void SocialHandler::sendMinimapPing(float wowX, float wowY) {
    if (owner_.getState() != WorldState::IN_WORLD) return;

    // MSG_MINIMAP_PING (CMSG direction): float posX + float posY
    // Server convention: posX = east/west axis = canonical Y (west)
    //                    posY = north/south axis = canonical X (north)
    const float serverX = wowY;  // canonical Y (west) → server posX
    const float serverY = wowX;  // canonical X (north) → server posY

    network::Packet pkt(wireOpcode(Opcode::MSG_MINIMAP_PING));
    pkt.writeFloat(serverX);
    pkt.writeFloat(serverY);
    owner_.getSocket()->send(pkt);

    // Add ping locally so the sender sees their own ping immediately
    GameHandler::MinimapPing localPing;
    localPing.senderGuid = owner_.activeCharacterGuidRef();
    localPing.wowX       = wowX;
    localPing.wowY       = wowY;
    localPing.age        = 0.0f;
    owner_.minimapPingsRef().push_back(localPing);
}

// ============================================================
// Summon Request
// ============================================================

void SocialHandler::handleSummonRequest(network::Packet& packet) {
    if (!packet.hasRemaining(16)) return;

    owner_.summonerGuidRef()        = packet.readUInt64();
    uint32_t zoneId             = packet.readUInt32();
    uint32_t timeoutMs          = packet.readUInt32();
    owner_.summonTimeoutSecRef()    = timeoutMs / 1000.0f;
    owner_.pendingSummonRequestRef()= true;

    owner_.summonerNameRef().clear();
    if (auto* unit = owner_.getUnitByGuid(owner_.summonerGuidRef())) {
        owner_.summonerNameRef() = unit->getName();
    }
    if (owner_.summonerNameRef().empty()) {
        owner_.summonerNameRef() = owner_.lookupName(owner_.summonerGuidRef());
    }
    if (owner_.summonerNameRef().empty()) {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "0x%llX",
                      static_cast<unsigned long long>(owner_.summonerGuidRef()));
        owner_.summonerNameRef() = tmp;
    }

    // CONFIRM_SUMMON and its popup, which names the summoner and the place;
    // no chat line (0x006d8680). This printed "X is summoning you to Y.".
    LOG_INFO("SMSG_SUMMON_REQUEST: summoner=", owner_.summonerNameRef(),
             " zoneId=", zoneId, " timeout=", owner_.summonTimeoutSecRef(), "s");
    owner_.fireAddonEvent("CONFIRM_SUMMON", {});
}

/// Both answers to a summon: who summoned, then yes or no.
///
/// HandleSummonResponseOpcode reads `ObjectGuid summoner_guid` and then a bool,
/// nine bytes. Both answers were a single byte, so the server had one byte
/// where it wanted eight, ran off the end of the buffer and dropped the packet
/// - accepting a summon has never once reached it, and neither has declining
/// one. The same mistake as the battlefield answers below, made next door.
///
/// The guid is the one SMSG_SUMMON_REQUEST arrived with, which is already kept
/// so the message can name whoever cast it.
void SocialHandler::sendSummonResponse(bool accept) {
    if (!owner_.getSocket()) return;
    owner_.pendingSummonRequestRef() = false;
    network::Packet pkt(wireOpcode(Opcode::CMSG_SUMMON_RESPONSE));
    pkt.writeUInt64(owner_.summonerGuidRef());
    pkt.writeUInt8(accept ? 1 : 0);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::acceptSummon() {
    if (!owner_.pendingSummonRequestRef()) return;
    // ConfirmSummon and CancelSummon answer and say nothing (0x0051b050,
    // 0x0051b110); these printed "Accepting summon..." and "Summon declined."
    sendSummonResponse(true);
    LOG_INFO("Accepted summon from ", owner_.summonerNameRef());
}

void SocialHandler::declineSummon() {
    sendSummonResponse(false);
}

// ============================================================
// Battlefield Manager
// ============================================================

/// All three battlefield answers carry the battle id first.
///
/// HandleBfEntryInviteResponse reads `uint32 BattleId >> uint8 Accepted` and
/// the exit request reads a battle id alone. Both answers used to be a single
/// byte, so the server read the accept flag as the low byte of a battle id,
/// ran out of buffer and dropped the packet - accepting a Wintergrasp
/// invitation had never once reached it.
void SocialHandler::sendBfMgrResponse(Opcode op, uint32_t battleId, bool accept,
                                      bool withFlag) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(op));
    pkt.writeUInt32(battleId);
    if (withFlag) pkt.writeUInt8(accept ? 1 : 0);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::acceptBfMgrInvite() {
    if (!owner_.bfMgrInvitePendingRef()) return;
    sendBfMgrResponse(Opcode::CMSG_BATTLEFIELD_MGR_ENTRY_INVITE_RESPONSE,
                      owner_.getBfMgrBattleId(), true, true);
    owner_.bfMgrInvitePendingRef() = false;
    LOG_INFO("acceptBfMgrInvite: battle ", owner_.getBfMgrBattleId());
}

void SocialHandler::declineBfMgrInvite() {
    if (!owner_.bfMgrInvitePendingRef()) return;
    sendBfMgrResponse(Opcode::CMSG_BATTLEFIELD_MGR_ENTRY_INVITE_RESPONSE,
                      owner_.getBfMgrBattleId(), false, true);
    owner_.bfMgrInvitePendingRef() = false;
    LOG_INFO("declineBfMgrInvite: battle ", owner_.getBfMgrBattleId());
}

void SocialHandler::respondBfMgrQueueInvite(uint32_t battleId, bool accept) {
    sendBfMgrResponse(Opcode::CMSG_BATTLEFIELD_MGR_QUEUE_INVITE_RESPONSE,
                      battleId, accept, true);
    owner_.bfMgrInvitePendingRef() = false;
    LOG_INFO("respondBfMgrQueueInvite: battle ", battleId,
             accept ? " accepted" : " declined");
}

void SocialHandler::requestBfMgrExit(uint32_t battleId) {
    sendBfMgrResponse(Opcode::CMSG_BATTLEFIELD_MGR_EXIT_REQUEST,
                      battleId, false, false);
    LOG_INFO("requestBfMgrExit: battle ", battleId);
}

// ============================================================
// Calendar
// ============================================================

void SocialHandler::inviteToCalendarEvent(uint64_t eventId, uint64_t inviteId,
                                          const std::string& name,
                                          bool isPreInvite, bool isGuildEvent) {
    if (!owner_.isInWorld()) return;
    // eventId, inviteId, name, isPreInvite, isGuildEvent - CalendarHandler.cpp:515.
    // The two flags are ByteBuffer bools, which are one byte each on the wire.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_EVENT_INVITE));
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeString(name);
    pkt.writeUInt8(isPreInvite ? 1 : 0);
    pkt.writeUInt8(isGuildEvent ? 1 : 0);
    owner_.getSocket()->send(pkt);
    LOG_INFO("inviteToCalendarEvent: '", name, "' to event ", eventId,
             isPreInvite ? " (before it is created)" : "");
}

void SocialHandler::massInviteGuildToCalendarEvent(uint32_t minLevel,
                                                   uint32_t maxLevel,
                                                   uint32_t minRank) {
    if (!owner_.isInWorld()) return;
    // Its own opcode, not an invite with the name left out: the server picks
    // the members itself from these three bounds and never sees a name
    // (CalendarHandler.cpp:198, and Guild::MassInviteToEvent behind it).
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_GUILD_FILTER));
    pkt.writeUInt32(minLevel);
    pkt.writeUInt32(maxLevel);
    pkt.writeUInt32(minRank);
    owner_.getSocket()->send(pkt);
    LOG_INFO("massInviteGuildToCalendarEvent: levels ", minLevel, "-", maxLevel,
             ", rank ", minRank, " and above");
}

void SocialHandler::updateCalendarEvent(uint64_t eventId, uint64_t inviteId,
                                        const CalendarEventDraft& draft) {
    if (!owner_.isInWorld()) return;
    // The same body as adding, with the event and invite ids in front of it
    // (CalendarHandler.cpp:360). Written out rather than shared with
    // createCalendarEvent: the two are the same shape today and are two
    // different opcodes, and folding them together would make the next change
    // to one silently change the other.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_UPDATE_EVENT));
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeString(draft.title);
    pkt.writeString(draft.description);
    pkt.writeUInt8(draft.type);
    pkt.writeUInt8(draft.repeatOption);
    pkt.writeUInt32(draft.maxInvites);
    pkt.writeUInt32(static_cast<uint32_t>(draft.dungeonId));
    pkt.writeUInt32(packWowPackedTime(draft.eventTime));
    pkt.writeUInt32(packWowPackedTime(draft.eventTime));
    pkt.writeUInt32(draft.flags);
    owner_.getSocket()->send(pkt);
    LOG_INFO("updateCalendarEvent: ", eventId, " '", draft.title, "'");
}

void SocialHandler::removeCalendarEvent(uint64_t eventId, uint64_t inviteId) {
    if (!owner_.isInWorld()) return;
    // The server reads the id and discards the rest, but the rest is still
    // written: it calls rfinish() rather than stopping, so a short packet is
    // a short read on its side rather than a tidy one.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_REMOVE_EVENT));
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeUInt32(0);   // flags
    owner_.getSocket()->send(pkt);
    LOG_INFO("removeCalendarEvent: ", eventId);
}

void SocialHandler::setCalendarInviteStatus(uint64_t inviteeGuid,
                                            uint64_t eventId, uint64_t inviteId,
                                            uint8_t status) {
    if (!owner_.isInWorld()) return;
    // packedGuid invitee, eventId, inviteId, ownerInviteId, status
    // (CalendarHandler.cpp:695). Setting someone *else's* status, which is what
    // an event's owner does to confirm or bench them - the player's own answer
    // is CMSG_CALENDAR_EVENT_RSVP and a different packet entirely.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_EVENT_STATUS));
    pkt.writePackedGuid(inviteeGuid);
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeUInt64(0);   // ownerInviteId
    pkt.writeUInt8(status);
    owner_.getSocket()->send(pkt);
    LOG_INFO("setCalendarInviteStatus: invitee ", inviteeGuid, " status ", status);
}

void SocialHandler::setCalendarInviteModerator(uint64_t inviteeGuid,
                                               uint64_t eventId,
                                               uint64_t inviteId, uint8_t rank) {
    if (!owner_.isInWorld()) return;
    // The same shape with a rank in place of the status
    // (CalendarHandler.cpp:727).
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_EVENT_MODERATOR_STATUS));
    pkt.writePackedGuid(inviteeGuid);
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeUInt64(0);   // ownerInviteId
    pkt.writeUInt8(rank);
    owner_.getSocket()->send(pkt);
    LOG_INFO("setCalendarInviteModerator: invitee ", inviteeGuid, " rank ", rank);
}

void SocialHandler::requestCalendarEvent(uint64_t eventId) {
    if (!owner_.isInWorld()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_GET_EVENT));
    pkt.writeUInt64(eventId);
    owner_.getSocket()->send(pkt);
    LOG_INFO("requestCalendarEvent: ", eventId);
}

void SocialHandler::createCalendarEvent(const CalendarEventDraft& draft) {
    if (!owner_.isInWorld()) return;
    // title, description, type, repeatable, maxInvites, dungeonId, then two
    // packed times and the flags - CalendarHandler.cpp:228.
    //
    // The second time is unused by the server (it is stored and never read),
    // but it is read off the wire, so leaving it out shifts the flags into it
    // and the flags off the end.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_ADD_EVENT));
    pkt.writeString(draft.title);
    pkt.writeString(draft.description);
    pkt.writeUInt8(draft.type);
    pkt.writeUInt8(draft.repeatOption);
    pkt.writeUInt32(draft.maxInvites);
    pkt.writeUInt32(static_cast<uint32_t>(draft.dungeonId));
    pkt.writeUInt32(packWowPackedTime(draft.eventTime));
    pkt.writeUInt32(packWowPackedTime(draft.eventTime));
    pkt.writeUInt32(draft.flags);
    owner_.getSocket()->send(pkt);
    LOG_INFO("createCalendarEvent: '", draft.title, "' type ", draft.type,
             " on ", draft.eventTime.day, "/", draft.eventTime.month, "/",
             draft.eventTime.fullYear());
}

void SocialHandler::respondToCalendarInvite(uint64_t eventId, uint64_t inviteId,
                                            uint32_t status) {
    if (!owner_.isInWorld()) return;
    // uint64 eventId, uint64 inviteId, uint32 status - CalendarHandler.cpp:630.
    // The status is a CalendarInviteStatus: 1 accepted, 2 declined, 8
    // tentative, 9 removed. The server answers with an event-status alert and
    // clears the pending action, so nothing is assumed here.
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_EVENT_RSVP));
    pkt.writeUInt64(eventId);
    pkt.writeUInt64(inviteId);
    pkt.writeUInt32(status);
    owner_.getSocket()->send(pkt);
    LOG_INFO("respondToCalendarInvite: event ", eventId, " invite ", inviteId,
             " status ", status);
}

void SocialHandler::requestCalendar() {
    if (!owner_.isInWorld()) return;
    // CMSG_CALENDAR_GET_CALENDAR has no payload
    network::Packet pkt(wireOpcode(Opcode::CMSG_CALENDAR_GET_CALENDAR));
    owner_.getSocket()->send(pkt);
    LOG_INFO("requestCalendar: sent CMSG_CALENDAR_GET_CALENDAR");
    // Also request pending invite count
    network::Packet numPkt(wireOpcode(Opcode::CMSG_CALENDAR_GET_NUM_PENDING));
    owner_.getSocket()->send(numPkt);
}

// ============================================================
// Methods moved from GameHandler
// ============================================================

void SocialHandler::sendSetDifficulty(uint32_t difficulty, bool raid) {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot change difficulty: not in world");
        return;
    }

    // MSG_SET_DUNGEON_DIFFICULTY, not CMSG_CHANGEPLAYER_DIFFICULTY. The latter
    // is a real opcode number and the server registers it with Handle_NULL and
    // STATUS_NEVER - it is read off the wire and discarded - so every
    // difficulty change ever sent from here went nowhere. Both of the real ones
    // carry a single uint32 mode.
    const Opcode op = raid ? Opcode::MSG_SET_RAID_DIFFICULTY
                           : Opcode::MSG_SET_DUNGEON_DIFFICULTY;
    network::Packet packet(wireOpcode(op));
    packet.writeUInt32(difficulty);
    owner_.getSocket()->send(packet);
    LOG_INFO(raid ? "MSG_SET_RAID_DIFFICULTY sent: difficulty="
                  : "MSG_SET_DUNGEON_DIFFICULTY sent: difficulty=", difficulty);
}

void SocialHandler::toggleHelm() {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot toggle helm: not in world or not connected");
        return;
    }

    owner_.helmVisibleRef() = !owner_.helmVisibleRef();
    auto packet = ShowingHelmPacket::build(owner_.helmVisibleRef());
    owner_.getSocket()->send(packet);
    // ShowHelm (0x0051c0b0) says nothing; this printed "Helm is now
    // visible/hidden."
    // The flag alone changed nothing on screen: it was set, sent to the server
    // and never read. Mark equipment dirty so the helm model and the hair under
    // it are both rebuilt from it.
    owner_.markOnlineEquipmentDirty();
    LOG_INFO("Helm visibility toggled: ", owner_.helmVisibleRef());
}

void SocialHandler::toggleCloak() {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot toggle cloak: not in world or not connected");
        return;
    }

    owner_.cloakVisibleRef() = !owner_.cloakVisibleRef();
    auto packet = ShowingCloakPacket::build(owner_.cloakVisibleRef());
    owner_.getSocket()->send(packet);
    // ShowCloak (0x0051c100) likewise; this printed "Cloak is now
    // visible/hidden."
    LOG_INFO("Cloak visibility toggled: ", owner_.cloakVisibleRef());
}

void SocialHandler::setStandState(uint8_t standState) {
    if (!owner_.isInWorld()) {
        LOG_WARNING("Cannot change stand state: not in world or not connected");
        return;
    }

    auto packet = StandStateChangePacket::build(standState);
    owner_.getSocket()->send(packet);
    LOG_INFO("Changed stand state to: ", static_cast<int>(standState));
}

void SocialHandler::sendAlterAppearance(uint32_t hairStyleEntry, uint32_t hairColor,
                                        uint32_t facialHairEntry, uint32_t skinColorEntry) {
    if (!owner_.isInWorld()) return;
    auto pkt = AlterAppearancePacket::build(hairStyleEntry, hairColor,
                                            facialHairEntry, skinColorEntry);
    owner_.getSocket()->send(pkt);
    LOG_INFO("sendAlterAppearance: hairEntry=", hairStyleEntry,
             " color=", hairColor, " facialEntry=", facialHairEntry,
             " skinEntry=", skinColorEntry);
}

void SocialHandler::deleteGmTicket() {
    if (!owner_.isInWorld()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_DELETETICKET));
    owner_.getSocket()->send(pkt);
    owner_.gmTicketActiveRef() = false;
    owner_.gmTicketTextRef().clear();
    LOG_INFO("Deleting GM ticket");
}

void SocialHandler::setGroupAssistant(uint64_t guid, bool apply) {
    if (owner_.getState() != WorldState::IN_WORLD || !owner_.getSocket()) return;
    if (guid == 0) return;
    network::Packet packet(wireOpcode(Opcode::CMSG_GROUP_ASSISTANT_LEADER));
    packet.writeUInt64(guid);
    packet.writeUInt8(apply ? 1 : 0);
    owner_.getSocket()->send(packet);
}

void SocialHandler::requestContactList() {
    if (!owner_.isInWorld() || !owner_.getSocket()) return;
    // Once a second. FriendsFrame_Update calls ShowFriends every time it
    // redraws, and it redraws on every friend status line - so an unthrottled
    // send answers each one with a request for the whole list again.
    const auto now = std::chrono::steady_clock::now();
    if (now - lastContactListRequest_ < std::chrono::seconds(1)) return;
    lastContactListRequest_ = now;
    // Two names for one request. Vanilla calls it CMSG_FRIEND_LIST and sends
    // it empty; TBC renamed it CMSG_CONTACT_LIST and gave it a flags word.
    // Checked against the opcode maps: classic names only the first, TBC and
    // WotLK only the second.
    if (isClassicLikeExpansion()) {
        const uint32_t wire = wireOpcode(Opcode::CMSG_FRIEND_LIST);
        if (wire == 0xFFFF) return;
        network::Packet pkt(static_cast<uint16_t>(wire));
        owner_.getSocket()->send(pkt);
        return;
    }
    const uint32_t wire = wireOpcode(Opcode::CMSG_CONTACT_LIST);
    if (wire == 0xFFFF) return;
    network::Packet pkt(static_cast<uint16_t>(wire));
    // Which lists to send back. AzerothCore reads one uint32 and hands it
    // straight to SendSocialList; SOCIAL_FLAG_ALL is friend, ignored and muted
    // together, which is what the panel shows across its three tabs.
    constexpr uint32_t kSocialFlagAll = 0x07;
    pkt.writeUInt32(kSocialFlagAll);
    owner_.getSocket()->send(pkt);
}

void SocialHandler::requestBattlefieldPositions() {
    if (!owner_.isInWorld() || !owner_.getSocket()) return;
    // Only in one. AzerothCore drops the request outright when the player has
    // no battleground, so asking anywhere else is bytes for nothing.
    if (!owner_.isBattlegroundMap(owner_.getCurrentMapId())) return;
    // Once a second. The interface asks from WorldMapFrame_OnUpdate, so an
    // unthrottled send is one packet per frame for as long as the map is open.
    const auto now = std::chrono::steady_clock::now();
    if (now - lastBgPositionRequest_ < std::chrono::seconds(1)) return;
    lastBgPositionRequest_ = now;
    network::Packet pkt(wireOpcode(Opcode::MSG_BATTLEGROUND_PLAYER_POSITIONS));
    owner_.getSocket()->send(pkt);
}

// MSG_GUILD_EVENT_LOG_QUERY: ask, with no payload. The reply is the same
// opcode and is parsed below.
void SocialHandler::requestGuildBankLog(uint8_t tab) {
    if (!owner_.isInWorld() || !owner_.getSocket()) return;
    network::Packet pkt(wireOpcode(Opcode::MSG_GUILD_BANK_LOG_QUERY));
    pkt.writeUInt8(tab);
    owner_.getSocket()->send(pkt);
    LOG_INFO("MSG_GUILD_BANK_LOG_QUERY: tab ", static_cast<int>(tab));
}

// The reply, which shares its opcode with the request.
//
//   uint8 tab
//   uint8 count
//   per entry:
//     int8   type
//     uint64 playerGuid
//     the payload the type calls for - an item and a count, those plus a
//     destination tab for a move, or a sum of money for everything else
//     uint32 secondsAgo
//
// Nothing sent the query, so nothing ever arrived to parse: the guild bank's
// log tab counted zero transactions and drew an empty page over a log the
// server keeps.
void SocialHandler::handleGuildBankLog(network::Packet& packet) {
    if (!packet.hasRemaining(2)) { packet.skipAll(); return; }
    const uint8_t tab = packet.readUInt8();
    const uint8_t count = packet.readUInt8();
    if (tab >= guildBankLogs_.size()) { packet.skipAll(); return; }

    auto& log = guildBankLogs_[tab];
    log.clear();
    log.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        if (!packet.hasRemaining(9)) break;
        GuildBankLogEntry e;
        e.type = packet.readUInt8();
        e.playerGuid = packet.readUInt64();
        switch (e.type) {
            case 1: case 2:                 // deposit / withdraw an item
                if (!packet.hasRemaining(8)) { packet.skipAll(); return; }
                e.itemId = packet.readUInt32();
                e.count  = packet.readUInt32();
                break;
            case 3: case 7:                 // moved between tabs
                if (!packet.hasRemaining(9)) { packet.skipAll(); return; }
                e.itemId   = packet.readUInt32();
                e.count    = packet.readUInt32();
                e.otherTab = packet.readUInt8();
                break;
            default:                        // money, in copper
                if (!packet.hasRemaining(4)) { packet.skipAll(); return; }
                e.count = packet.readUInt32();
                break;
        }
        if (!packet.hasRemaining(4)) break;
        e.secondsAgo = packet.readUInt32();
        if (e.itemId) owner_.ensureItemInfo(e.itemId);
        log.push_back(e);
    }
    packet.skipAll();
    LOG_INFO("MSG_GUILD_BANK_LOG_QUERY reply: tab ", static_cast<int>(tab),
             ", ", log.size(), " entries");
    if (owner_.addonEventCallbackRef()) {
        owner_.addonEventCallbackRef()("GUILDBANKLOG_UPDATE", {});
    }
}

void SocialHandler::requestGuildEventLog() {
    if (!owner_.isInWorld()) return;
    network::Packet pkt(wireOpcode(Opcode::MSG_GUILD_EVENT_LOG_QUERY));
    owner_.getSocket()->send(pkt);
}

// The reply. Two of the five fields are conditional, which is the whole
// difficulty: a reader that always takes the second guid loses eight bytes on
// every join and leave, and those are most of a guild log.
//
// Types, from AzerothCore's GuildEventLogTypes: 1 invite, 2 join, 3 promote,
// 4 demote, 5 remove, 6 leave.
void SocialHandler::handleGuildEventLog(network::Packet& packet) {
    auto rem = [&]() { return packet.getRemainingSize(); };
    if (rem() < 1) return;
    const uint8_t count = packet.readUInt8();
    guildEventLog_.clear();
    guildEventLog_.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        if (rem() < 9) break;                 // type + one guid at minimum
        GuildEventLogEntry e;
        e.type = packet.readUInt8();
        e.playerGuid = packet.readUInt64();
        // Everything but join (2) and leave (6) names a second player.
        if (e.type != 2 && e.type != 6) {
            if (rem() < 8) break;
            e.otherGuid = packet.readUInt64();
        }
        // Promote (3) and demote (4) carry the new rank.
        if (e.type == 3 || e.type == 4) {
            if (rem() < 1) break;
            e.newRank = packet.readUInt8();
        }
        if (rem() < 4) break;
        e.secondsAgo = packet.readUInt32();
        guildEventLog_.push_back(e);
    }
    LOG_INFO("Guild event log: ", guildEventLog_.size(), " entries");
    if (owner_.addonEventCallbackRef()) owner_.addonEventCallbackRef()("GUILD_EVENT_LOG_UPDATE", {});
}

// CMSG_GMTICKET_SYSTEMSTATUS: is the ticket queue open at all. The reply,
// SMSG_GMTICKET_SYSTEMSTATUS, was already parsed - asking was the missing half,
// and the help frame asks every time it is shown.
void SocialHandler::requestGmSystemStatus() {
    if (!owner_.isInWorld()) return;
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_SYSTEMSTATUS));
    owner_.getSocket()->send(pkt);
}

void SocialHandler::requestGmTicket() {
    if (!owner_.isInWorld()) return;
    // CMSG_GMTICKET_GETTICKET has no payload - server responds with SMSG_GMTICKET_GETTICKET
    network::Packet pkt(wireOpcode(Opcode::CMSG_GMTICKET_GETTICKET));
    owner_.getSocket()->send(pkt);
    LOG_DEBUG("Sent CMSG_GMTICKET_GETTICKET - querying open ticket status");
}

} // namespace game
} // namespace wowee
